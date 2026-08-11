#include "metalsharp/DarwinSyncMap.h"

namespace metalsharp {
namespace {

constexpr DarwinSyncMapping kMappings[] = {
    {
        DarwinSyncPrimitive::Event,
        DarwinSyncStrategy::PThreadCondvar,
        "pthread_mutex_t + pthread_cond_t",
        false,
        true,
        "Manual-reset and auto-reset event behavior can be modeled in user space for in-process waits.",
    },
    {
        DarwinSyncPrimitive::Semaphore,
        DarwinSyncStrategy::MachSemaphoreCandidate,
        "dispatch_semaphore_t or pthread_cond_t counted semaphore",
        false,
        true,
        "Current in-process use is covered; Mach semaphore benchmarking is a latency research candidate.",
    },
    {
        DarwinSyncPrimitive::Mutex,
        DarwinSyncStrategy::PThreadMutex,
        "pthread_mutex_t with recursive attributes where required",
        false,
        true,
        "Matches current SyncContext coverage for in-process mutex ownership.",
    },
    {
        DarwinSyncPrimitive::CriticalSection,
        DarwinSyncStrategy::PThreadMutex,
        "pthread_mutex_t",
        false,
        true,
        "Already used by kernel32/ntdll shim coverage.",
    },
    {
        DarwinSyncPrimitive::SrwLock,
        DarwinSyncStrategy::PThreadRWLock,
        "out-of-line pthread_rwlock_t keyed by guest SRWLOCK address",
        false,
        true,
        "Guest SRWLOCK storage remains zero-initialized; shared and exclusive modes use the mapped host lock.",
    },
    {
        DarwinSyncPrimitive::ConditionVariable,
        DarwinSyncStrategy::PThreadCondvar,
        "out-of-line pthread_cond_t + wait mutex keyed by guest CONDITION_VARIABLE address",
        false,
        true,
        "The wait mutex closes the release/wait race before the mapped SRW lock is reacquired.",
    },
    {
        DarwinSyncPrimitive::WaitAny,
        DarwinSyncStrategy::PThreadCondvar,
        "poll tracked handles with condition-variable wakeups",
        false,
        true,
        "Works for tracked in-process handles; cross-process handle semantics need more evidence.",
    },
    {
        DarwinSyncPrimitive::WaitAll,
        DarwinSyncStrategy::NeedsResearch,
        "coordinated wait over tracked handles",
        false,
        false,
        "Correctness under mixed event/semaphore/mutex handles needs focused tests before calling this Proton-grade.",
    },
    {
        DarwinSyncPrimitive::Futex,
        DarwinSyncStrategy::ULockCandidate,
        "Darwin ulock or pthread fallback",
        false,
        false,
        "macOS has no Linux futex ABI; ulock is a research candidate, not a drop-in contract.",
    },
    {
        DarwinSyncPrimitive::NtSyncDevice,
        DarwinSyncStrategy::UnsupportedLinuxSpecific,
        "no /dev/ntsync equivalent on macOS",
        true,
        false,
        "Linux ntsync cannot be copied to macOS without a legitimate Apple-approved system component strategy.",
    },
};

} // namespace

const DarwinSyncMapping* darwinSyncMap(std::size_t* count) {
    if (count) {
        *count = sizeof(kMappings) / sizeof(kMappings[0]);
    }
    return kMappings;
}

const DarwinSyncMapping* darwinSyncMapping(DarwinSyncPrimitive primitive) {
    for (const auto& mapping : kMappings) {
        if (mapping.primitive == primitive) {
            return &mapping;
        }
    }
    return nullptr;
}

} // namespace metalsharp
