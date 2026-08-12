/// @file Win32SyncContext.cpp
/// @brief Out-of-line pthread state for Win32 SRW locks and condition variables.

#include <metalsharp/Win32SyncContext.h>
#include <sys/time.h>

namespace metalsharp {
namespace win32 {
namespace {

constexpr uint32_t kInfinite = 0xFFFFFFFF;

void setTimeout(timespec* timeout, uint32_t milliseconds) {
    clock_gettime(CLOCK_REALTIME, timeout);
    timeout->tv_sec += milliseconds / 1000;
    timeout->tv_nsec += static_cast<long>(milliseconds % 1000) * 1000000L;
    if (timeout->tv_nsec >= 1000000000L) {
        timeout->tv_sec++;
        timeout->tv_nsec -= 1000000000L;
    }
}

} // namespace

Win32SyncContext::SrwLockState::SrwLockState() {
    initialized = pthread_rwlock_init(&lock, nullptr) == 0;
}

Win32SyncContext::SrwLockState::~SrwLockState() {
    if (initialized) {
        pthread_rwlock_destroy(&lock);
    }
}

Win32SyncContext::ConditionVariableState::ConditionVariableState() {
    mutexInitialized = pthread_mutex_init(&waitMutex, nullptr) == 0;
    if (mutexInitialized) {
        conditionInitialized = pthread_cond_init(&condition, nullptr) == 0;
    }
}

Win32SyncContext::ConditionVariableState::~ConditionVariableState() {
    if (conditionInitialized) {
        pthread_cond_destroy(&condition);
    }
    if (mutexInitialized) {
        pthread_mutex_destroy(&waitMutex);
    }
}

Win32SyncContext& Win32SyncContext::instance() {
    // SRW locks and condition variables have no destruction APIs. Leaking the
    // registry for the process lifetime also prevents static destruction from
    // racing a guest thread that is still unwinding during process teardown.
    static auto* context = new Win32SyncContext();
    return *context;
}

Win32SyncContext::SrwLockState* Win32SyncContext::getSrwLock(void* address) {
    if (!address) {
        return nullptr;
    }

    const auto key = reinterpret_cast<uintptr_t>(address);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_srwLocks.find(key);
    if (it != m_srwLocks.end()) {
        return it->second->initialized ? it->second.get() : nullptr;
    }

    auto state = std::make_unique<SrwLockState>();
    if (!state->initialized) {
        return nullptr;
    }

    auto* result = state.get();
    m_srwLocks.emplace(key, std::move(state));
    return result;
}

Win32SyncContext::ConditionVariableState* Win32SyncContext::getConditionVariable(void* address) {
    if (!address) {
        return nullptr;
    }

    const auto key = reinterpret_cast<uintptr_t>(address);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_conditionVariables.find(key);
    if (it != m_conditionVariables.end()) {
        const auto* state = it->second.get();
        return state->conditionInitialized && state->mutexInitialized ? it->second.get() : nullptr;
    }

    auto state = std::make_unique<ConditionVariableState>();
    if (!state->conditionInitialized || !state->mutexInitialized) {
        return nullptr;
    }

    auto* result = state.get();
    m_conditionVariables.emplace(key, std::move(state));
    return result;
}

void Win32SyncContext::initializeSrwLock(void* address) {
    (void)getSrwLock(address);
}

void Win32SyncContext::initializeConditionVariable(void* address) {
    (void)getConditionVariable(address);
}

bool Win32SyncContext::acquireSrwLockExclusive(void* address) {
    auto* state = getSrwLock(address);
    return state && pthread_rwlock_wrlock(&state->lock) == 0;
}

bool Win32SyncContext::releaseSrwLockExclusive(void* address) {
    auto* state = getSrwLock(address);
    return state && pthread_rwlock_unlock(&state->lock) == 0;
}

bool Win32SyncContext::tryAcquireSrwLockExclusive(void* address) {
    auto* state = getSrwLock(address);
    return state && pthread_rwlock_trywrlock(&state->lock) == 0;
}

bool Win32SyncContext::acquireSrwLockShared(void* address) {
    auto* state = getSrwLock(address);
    return state && pthread_rwlock_rdlock(&state->lock) == 0;
}

bool Win32SyncContext::releaseSrwLockShared(void* address) {
    auto* state = getSrwLock(address);
    return state && pthread_rwlock_unlock(&state->lock) == 0;
}

bool Win32SyncContext::tryAcquireSrwLockShared(void* address) {
    auto* state = getSrwLock(address);
    return state && pthread_rwlock_tryrdlock(&state->lock) == 0;
}

bool Win32SyncContext::sleepConditionVariableSrw(void* conditionVariable, void* srwLock, uint32_t milliseconds,
                                                 uint32_t flags) {
    auto* condition = getConditionVariable(conditionVariable);
    auto* lock = getSrwLock(srwLock);
    if (!condition || !lock || pthread_mutex_lock(&condition->waitMutex) != 0) {
        return false;
    }

    const bool shared = (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0;
    if (pthread_rwlock_unlock(&lock->lock) != 0) {
        pthread_mutex_unlock(&condition->waitMutex);
        return false;
    }

    int waitResult = 0;
    if (milliseconds == kInfinite) {
        waitResult = pthread_cond_wait(&condition->condition, &condition->waitMutex);
    } else {
        timespec timeout{};
        setTimeout(&timeout, milliseconds);
        waitResult = pthread_cond_timedwait(&condition->condition, &condition->waitMutex, &timeout);
    }

    // pthread_cond_wait/timedwait return with waitMutex held. Release that
    // internal mutex before reacquiring the SRW lock: a waker is allowed to
    // hold the SRW lock while calling WakeConditionVariable, and keeping the
    // two locks held in opposite order would deadlock that valid pattern.
    const int unlockResult = pthread_mutex_unlock(&condition->waitMutex);
    const int reacquireResult = shared ? pthread_rwlock_rdlock(&lock->lock) : pthread_rwlock_wrlock(&lock->lock);
    return waitResult == 0 && reacquireResult == 0 && unlockResult == 0;
}

bool Win32SyncContext::wakeConditionVariable(void* conditionVariable) {
    auto* condition = getConditionVariable(conditionVariable);
    if (!condition || pthread_mutex_lock(&condition->waitMutex) != 0) {
        return false;
    }

    const int signalResult = pthread_cond_signal(&condition->condition);
    const int unlockResult = pthread_mutex_unlock(&condition->waitMutex);
    return signalResult == 0 && unlockResult == 0;
}

bool Win32SyncContext::wakeAllConditionVariable(void* conditionVariable) {
    auto* condition = getConditionVariable(conditionVariable);
    if (!condition || pthread_mutex_lock(&condition->waitMutex) != 0) {
        return false;
    }

    const int broadcastResult = pthread_cond_broadcast(&condition->condition);
    const int unlockResult = pthread_mutex_unlock(&condition->waitMutex);
    return broadcastResult == 0 && unlockResult == 0;
}

} // namespace win32
} // namespace metalsharp
