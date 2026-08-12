#pragma once

#include <cstddef>

namespace metalsharp {

enum class DarwinSyncPrimitive {
    Event,
    Semaphore,
    Mutex,
    CriticalSection,
    SrwLock,
    ConditionVariable,
    WaitAny,
    WaitAll,
    Futex,
    NtSyncDevice,
};

enum class DarwinSyncStrategy {
    PThreadCondvar,
    PThreadMutex,
    PThreadRWLock,
    MachSemaphoreCandidate,
    ULockCandidate,
    UnsupportedLinuxSpecific,
    NeedsResearch,
};

struct DarwinSyncMapping {
    DarwinSyncPrimitive primitive;
    DarwinSyncStrategy strategy;
    const char* replacement;
    bool kernelRequired;
    bool shippingReady;
    const char* notes;
};

const DarwinSyncMapping* darwinSyncMap(std::size_t* count);
const DarwinSyncMapping* darwinSyncMapping(DarwinSyncPrimitive primitive);

} // namespace metalsharp
