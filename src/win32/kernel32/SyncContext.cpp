/// @file SyncContext.cpp
/// @brief Win32 synchronization primitives via pthreads.
///
/// Implements Win32 Events, Mutexes, Semaphores, and critical sections using pthreads and POSIX synchronization
/// primitives. Each Win32 sync object is tracked by handle for CreateEvent/OpenMutex style access.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <metalsharp/Logger.h>
#include <metalsharp/SyncContext.h>
#include <sys/time.h>
#include <unistd.h>

namespace metalsharp {
namespace win32 {

namespace {

struct ThreadStartContext {
    SyncThreadState* state;
    void* startAddress;
    void* parameter;
};

static void* threadTrampoline(void* rawContext) {
    auto* context = static_cast<ThreadStartContext*>(rawContext);
    auto entry = reinterpret_cast<DWORD(MSABI*)(void*)>(context->startAddress);
    DWORD exitCode = entry ? entry(context->parameter) : 0;

    context->state->exitCode.store(exitCode);
    pthread_mutex_lock(&context->state->joinMutex);
    context->state->finished.store(true);
    pthread_cond_broadcast(&context->state->joinCond);
    pthread_mutex_unlock(&context->state->joinMutex);

    delete context;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(exitCode));
}

} // namespace

SyncContext& SyncContext::instance() {
    static SyncContext ctx;
    return ctx;
}

void SyncContext::initialize() {
    MS_INFO("SyncContext: initializing");
}

void* SyncContext::createEvent(bool manualReset, bool initialState, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!name.empty()) {
        auto it = m_namedHandles.find(name);
        if (it != m_namedHandles.end()) {
            return reinterpret_cast<void*>(it->second);
        }
    }

    int handle = m_nextHandle++;

    auto* evt = new SyncEventState();
    pthread_mutex_init(&evt->mutex, nullptr);
    pthread_cond_init(&evt->cond, nullptr);
    evt->signaled = initialState;
    evt->manualReset = manualReset;

    SyncHandle sh;
    sh.type = SyncHandleType::Event;
    sh.data = evt;
    sh.name = name;
    m_handles[handle] = sh;

    if (!name.empty())
        m_namedHandles[name] = handle;

    return reinterpret_cast<void*>(static_cast<intptr_t>(handle));
}

bool SyncContext::setEvent(void* handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find((intptr_t)handle);
    if (it == m_handles.end() || it->second.type != SyncHandleType::Event)
        return false;

    auto* evt = static_cast<SyncEventState*>(it->second.data);
    pthread_mutex_lock(&evt->mutex);
    evt->signaled = true;
    if (evt->manualReset) {
        pthread_cond_broadcast(&evt->cond);
    } else {
        pthread_cond_signal(&evt->cond);
    }
    pthread_mutex_unlock(&evt->mutex);
    return true;
}

bool SyncContext::resetEvent(void* handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find((intptr_t)handle);
    if (it == m_handles.end() || it->second.type != SyncHandleType::Event)
        return false;

    auto* evt = static_cast<SyncEventState*>(it->second.data);
    pthread_mutex_lock(&evt->mutex);
    evt->signaled = false;
    pthread_mutex_unlock(&evt->mutex);
    return true;
}

void* SyncContext::createMutex(bool initialOwner, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!name.empty()) {
        auto it = m_namedHandles.find(name);
        if (it != m_namedHandles.end()) {
            return reinterpret_cast<void*>(it->second);
        }
    }

    int handle = m_nextHandle++;

    auto* mtx = new SyncMutexState();
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mtx->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    mtx->owningThread = 0;
    mtx->recursionCount = 0;

    if (initialOwner) {
        pthread_mutex_lock(&mtx->mutex);
        mtx->owningThread = (DWORD)(uintptr_t)pthread_self();
        mtx->recursionCount = 1;
    }

    SyncHandle sh;
    sh.type = SyncHandleType::Mutex;
    sh.data = mtx;
    sh.name = name;
    m_handles[handle] = sh;

    if (!name.empty())
        m_namedHandles[name] = handle;

    return reinterpret_cast<void*>(static_cast<intptr_t>(handle));
}

bool SyncContext::releaseMutex(void* handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find((intptr_t)handle);
    if (it == m_handles.end() || it->second.type != SyncHandleType::Mutex)
        return false;

    auto* mtx = static_cast<SyncMutexState*>(it->second.data);
    if (mtx->owningThread != (DWORD)(uintptr_t)pthread_self())
        return false;

    mtx->recursionCount--;
    if (mtx->recursionCount == 0) {
        mtx->owningThread = 0;
    }
    pthread_mutex_unlock(&mtx->mutex);
    return true;
}

void* SyncContext::createSemaphore(int32_t initialCount, int32_t maxCount, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!name.empty()) {
        auto it = m_namedHandles.find(name);
        if (it != m_namedHandles.end()) {
            return reinterpret_cast<void*>(it->second);
        }
    }

    int handle = m_nextHandle++;

    auto* sem = new SyncSemaphoreState();
    pthread_mutex_init(&sem->mutex, nullptr);
    pthread_cond_init(&sem->cond, nullptr);
    sem->count = initialCount;
    sem->maxCount = maxCount;

    SyncHandle sh;
    sh.type = SyncHandleType::Semaphore;
    sh.data = sem;
    sh.name = name;
    m_handles[handle] = sh;

    if (!name.empty())
        m_namedHandles[name] = handle;

    return reinterpret_cast<void*>(static_cast<intptr_t>(handle));
}

bool SyncContext::releaseSemaphore(void* handle, int32_t releaseCount, int32_t* prevCount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find((intptr_t)handle);
    if (it == m_handles.end() || it->second.type != SyncHandleType::Semaphore)
        return false;

    auto* sem = static_cast<SyncSemaphoreState*>(it->second.data);
    pthread_mutex_lock(&sem->mutex);
    if (prevCount)
        *prevCount = sem->count;
    sem->count += releaseCount;
    if (sem->count > sem->maxCount)
        sem->count = sem->maxCount;
    for (int32_t i = 0; i < releaseCount; i++) {
        pthread_cond_signal(&sem->cond);
    }
    pthread_mutex_unlock(&sem->mutex);
    return true;
}

void* SyncContext::createThread(void* startAddress, void* parameter, DWORD* threadId, int* errorCode) {
    if (errorCode)
        *errorCode = 0;

    auto* thr = new SyncThreadState();
    thr->exitCode.store(259);
    thr->joined.store(false);
    thr->finished.store(false);
    pthread_mutex_init(&thr->joinMutex, nullptr);
    pthread_cond_init(&thr->joinCond, nullptr);

    auto* context = new ThreadStartContext{thr, startAddress, parameter};
    int result = pthread_create(&thr->thread, nullptr, threadTrampoline, context);
    if (result != 0) {
        if (errorCode)
            *errorCode = result;
        delete context;
        pthread_mutex_destroy(&thr->joinMutex);
        pthread_cond_destroy(&thr->joinCond);
        delete thr;
        return nullptr;
    }

    if (threadId)
        *threadId = static_cast<DWORD>(reinterpret_cast<uintptr_t>(thr->thread));

    std::lock_guard<std::mutex> lock(m_mutex);
    int handle = m_nextHandle++;

    SyncHandle sh;
    sh.type = SyncHandleType::Thread;
    sh.data = thr;
    m_handles[handle] = sh;

    return reinterpret_cast<void*>(static_cast<intptr_t>(handle));
}

SyncThreadState* SyncContext::getThreadState(void* handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find((intptr_t)handle);
    if (it == m_handles.end() || it->second.type != SyncHandleType::Thread)
        return nullptr;
    return static_cast<SyncThreadState*>(it->second.data);
}

static void timespecFromMs(timespec& ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
}

uint32_t SyncContext::waitForEvent(SyncEventState* evt, uint32_t ms) {
    pthread_mutex_lock(&evt->mutex);

    if (evt->signaled) {
        if (!evt->manualReset) {
            evt->signaled = false;
        }
        pthread_mutex_unlock(&evt->mutex);
        return 0;
    }

    if (ms == 0) {
        pthread_mutex_unlock(&evt->mutex);
        return 258;
    }

    if (ms == 0xFFFFFFFF) {
        while (!evt->signaled) {
            pthread_cond_wait(&evt->cond, &evt->mutex);
        }
        if (!evt->manualReset) {
            evt->signaled = false;
        }
        pthread_mutex_unlock(&evt->mutex);
        return 0;
    }

    timespec ts;
    timespecFromMs(ts, ms);
    int rc = 0;
    while (!evt->signaled && rc == 0) {
        rc = pthread_cond_timedwait(&evt->cond, &evt->mutex, &ts);
    }

    if (evt->signaled) {
        if (!evt->manualReset) {
            evt->signaled = false;
        }
        pthread_mutex_unlock(&evt->mutex);
        return 0;
    }

    pthread_mutex_unlock(&evt->mutex);
    return 258;
}

uint32_t SyncContext::waitForMutex(SyncMutexState* mtx, uint32_t ms) {
    if (ms == 0xFFFFFFFF) {
        pthread_mutex_lock(&mtx->mutex);
        mtx->owningThread = (DWORD)(uintptr_t)pthread_self();
        mtx->recursionCount++;
        return 0;
    }

    if (ms == 0) {
        if (pthread_mutex_trylock(&mtx->mutex) == 0) {
            mtx->owningThread = (DWORD)(uintptr_t)pthread_self();
            mtx->recursionCount++;
            return 0;
        }
        return 258;
    }

    timespec ts;
    timespecFromMs(ts, ms);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (pthread_mutex_trylock(&mtx->mutex) == 0) {
            mtx->owningThread = (DWORD)(uintptr_t)pthread_self();
            mtx->recursionCount++;
            return 0;
        }
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if ((uint32_t)elapsed >= ms)
            break;
        usleep(1000);
    }
    return 258;
}

uint32_t SyncContext::waitForSemaphore(SyncSemaphoreState* sem, uint32_t ms) {
    pthread_mutex_lock(&sem->mutex);

    if (sem->count > 0) {
        sem->count--;
        pthread_mutex_unlock(&sem->mutex);
        return 0;
    }

    if (ms == 0) {
        pthread_mutex_unlock(&sem->mutex);
        return 258;
    }

    if (ms == 0xFFFFFFFF) {
        while (sem->count <= 0) {
            pthread_cond_wait(&sem->cond, &sem->mutex);
        }
        sem->count--;
        pthread_mutex_unlock(&sem->mutex);
        return 0;
    }

    timespec ts;
    timespecFromMs(ts, ms);
    int rc = 0;
    while (sem->count <= 0 && rc == 0) {
        rc = pthread_cond_timedwait(&sem->cond, &sem->mutex, &ts);
    }

    if (sem->count > 0) {
        sem->count--;
        pthread_mutex_unlock(&sem->mutex);
        return 0;
    }

    pthread_mutex_unlock(&sem->mutex);
    return 258;
}

uint32_t SyncContext::waitForThread(SyncThreadState* thr, uint32_t ms) {
    if (thr->joined.load())
        return 0;

    if (ms == 0xFFFFFFFF) {
        pthread_mutex_lock(&thr->joinMutex);
        while (!thr->finished.load()) {
            pthread_cond_wait(&thr->joinCond, &thr->joinMutex);
        }
        pthread_mutex_unlock(&thr->joinMutex);
        if (!thr->joined.exchange(true)) {
            pthread_join(thr->thread, nullptr);
        }
        return 0;
    }

    if (ms == 0) {
        if (thr->finished.load()) {
            if (!thr->joined.exchange(true)) {
                pthread_join(thr->thread, nullptr);
            }
            return 0;
        }
        return 258;
    }

    timespec ts;
    timespecFromMs(ts, ms);
    pthread_mutex_lock(&thr->joinMutex);
    int rc = 0;
    while (!thr->finished.load() && rc == 0) {
        rc = pthread_cond_timedwait(&thr->joinCond, &thr->joinMutex, &ts);
    }
    pthread_mutex_unlock(&thr->joinMutex);

    if (thr->finished.load()) {
        if (!thr->joined.exchange(true)) {
            pthread_join(thr->thread, nullptr);
        }
        return 0;
    }
    return 258;
}

uint32_t SyncContext::waitForSingleObject(void* handle, uint32_t milliseconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find((intptr_t)handle);
    if (it == m_handles.end())
        return 0xFFFFFFFF;

    switch (it->second.type) {
    case SyncHandleType::Event:
        return waitForEvent(static_cast<SyncEventState*>(it->second.data), milliseconds);
    case SyncHandleType::Mutex:
        return waitForMutex(static_cast<SyncMutexState*>(it->second.data), milliseconds);
    case SyncHandleType::Semaphore:
        return waitForSemaphore(static_cast<SyncSemaphoreState*>(it->second.data), milliseconds);
    case SyncHandleType::Thread:
        return waitForThread(static_cast<SyncThreadState*>(it->second.data), milliseconds);
    default:
        return 0xFFFFFFFF;
    }
}

uint32_t SyncContext::waitForMultipleObjects(uint32_t count, void** handles, bool waitAll, uint32_t milliseconds) {
    if (count == 0)
        return 0xFFFFFFFF;

    if (waitAll) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t remaining = 0xFFFFFFFF;
            if (milliseconds != 0xFFFFFFFF) {
                auto start = std::chrono::steady_clock::now();
                uint32_t result = waitForSingleObject(handles[i], milliseconds);
                if (result != 0)
                    return 0xFFFFFFFF;
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count();
                if (milliseconds > (uint32_t)elapsed) {
                    milliseconds -= (uint32_t)elapsed;
                } else {
                    milliseconds = 0;
                }
            } else {
                uint32_t result = waitForSingleObject(handles[i], 0xFFFFFFFF);
                if (result != 0)
                    return 0xFFFFFFFF;
            }
        }
        return 0;
    }

    uint32_t totalMs = milliseconds;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (uint32_t i = 0; i < count; i++) {
                auto it = m_handles.find((intptr_t)handles[i]);
                if (it == m_handles.end())
                    continue;

                bool signaled = false;
                switch (it->second.type) {
                case SyncHandleType::Event: {
                    auto* evt = static_cast<SyncEventState*>(it->second.data);
                    pthread_mutex_lock(&evt->mutex);
                    signaled = evt->signaled;
                    if (signaled && !evt->manualReset)
                        evt->signaled = false;
                    pthread_mutex_unlock(&evt->mutex);
                    break;
                }
                case SyncHandleType::Semaphore: {
                    auto* sem = static_cast<SyncSemaphoreState*>(it->second.data);
                    pthread_mutex_lock(&sem->mutex);
                    signaled = sem->count > 0;
                    if (signaled)
                        sem->count--;
                    pthread_mutex_unlock(&sem->mutex);
                    break;
                }
                case SyncHandleType::Thread: {
                    auto* thr = static_cast<SyncThreadState*>(it->second.data);
                    signaled = thr->finished.load();
                    break;
                }
                case SyncHandleType::Mutex: {
                    auto* mtx = static_cast<SyncMutexState*>(it->second.data);
                    if (pthread_mutex_trylock(&mtx->mutex) == 0) {
                        mtx->owningThread = (DWORD)(uintptr_t)pthread_self();
                        mtx->recursionCount++;
                        signaled = true;
                    }
                    break;
                }
                default:
                    break;
                }
                if (signaled)
                    return i;
            }
        }

        if (totalMs == 0)
            return 258;
        if (totalMs != 0xFFFFFFFF) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if ((uint32_t)elapsed >= totalMs)
                return 258;
            usleep(1000);
        } else {
            usleep(1000);
        }
    }
}

void SyncContext::destroyHandle(void* handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find((intptr_t)handle);
    if (it == m_handles.end())
        return;

    switch (it->second.type) {
    case SyncHandleType::Event: {
        auto* evt = static_cast<SyncEventState*>(it->second.data);
        pthread_mutex_destroy(&evt->mutex);
        pthread_cond_destroy(&evt->cond);
        delete evt;
        break;
    }
    case SyncHandleType::Mutex: {
        auto* mtx = static_cast<SyncMutexState*>(it->second.data);
        pthread_mutex_destroy(&mtx->mutex);
        delete mtx;
        break;
    }
    case SyncHandleType::Semaphore: {
        auto* sem = static_cast<SyncSemaphoreState*>(it->second.data);
        pthread_mutex_destroy(&sem->mutex);
        pthread_cond_destroy(&sem->cond);
        delete sem;
        break;
    }
    case SyncHandleType::Thread: {
        auto* thr = static_cast<SyncThreadState*>(it->second.data);
        pthread_mutex_destroy(&thr->joinMutex);
        pthread_cond_destroy(&thr->joinCond);
        delete thr;
        break;
    }
    default:
        break;
    }

    if (!it->second.name.empty()) {
        m_namedHandles.erase(it->second.name);
    }
    m_handles.erase(it);
}

} // namespace win32
} // namespace metalsharp
