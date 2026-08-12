/// @file Win32SyncContext.h
/// @brief Out-of-line state for zero-initialized Win32 synchronization objects.
///
/// SRW locks and condition variables are pointer-sized Windows objects whose
/// static initializers are all zero. Their Darwin counterparts are larger
/// pthread objects and cannot be placed in the guest-owned storage. This
/// context keeps the host objects in process-lifetime maps keyed by the guest
/// object address.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <unordered_map>

namespace metalsharp {
namespace win32 {

class Win32SyncContext {
  public:
    static Win32SyncContext& instance();

    void initializeSrwLock(void* address);
    void initializeConditionVariable(void* address);

    bool acquireSrwLockExclusive(void* address);
    bool releaseSrwLockExclusive(void* address);
    bool tryAcquireSrwLockExclusive(void* address);

    bool acquireSrwLockShared(void* address);
    bool releaseSrwLockShared(void* address);
    bool tryAcquireSrwLockShared(void* address);

    bool sleepConditionVariableSrw(void* conditionVariable, void* srwLock, uint32_t milliseconds, uint32_t flags);
    bool wakeConditionVariable(void* conditionVariable);
    bool wakeAllConditionVariable(void* conditionVariable);

  private:
    struct SrwLockState {
        pthread_rwlock_t lock{};
        bool initialized = false;

        SrwLockState();
        ~SrwLockState();
    };

    struct ConditionVariableState {
        pthread_cond_t condition{};
        pthread_mutex_t waitMutex{};
        bool conditionInitialized = false;
        bool mutexInitialized = false;

        ConditionVariableState();
        ~ConditionVariableState();
    };

    Win32SyncContext() = default;

    SrwLockState* getSrwLock(void* address);
    ConditionVariableState* getConditionVariable(void* address);

    std::mutex m_mutex;
    std::unordered_map<uintptr_t, std::unique_ptr<SrwLockState>> m_srwLocks;
    std::unordered_map<uintptr_t, std::unique_ptr<ConditionVariableState>> m_conditionVariables;
};

constexpr uint32_t CONDITION_VARIABLE_LOCKMODE_SHARED = 0x00000001;

} // namespace win32
} // namespace metalsharp
