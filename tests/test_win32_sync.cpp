#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <metalsharp/ExtraShims.h>
#include <metalsharp/Win32SyncContext.h>
#include <metalsharp/Win32Types.h>
#include <thread>

using metalsharp::ShimLibrary;
using namespace metalsharp::win32;

namespace {

struct alignas(8) GuestSyncObject {
    std::array<unsigned char, 8> bytes{};
};

using InitializeFn = void(MSABI*)(void*);
using AcquireFn = void(MSABI*)(void*);
using TryAcquireFn = BOOL(MSABI*)(void*);
using SleepFn = BOOL(MSABI*)(void*, void*, DWORD, ULONG);
using WakeFn = void(MSABI*)(void*);

template <typename Function> Function getFunction(ShimLibrary& library, const char* name) {
    auto it = library.functions.find(name);
    if (it == library.functions.end()) {
        return nullptr;
    }
    return reinterpret_cast<Function>(it->second());
}

bool isZero(const GuestSyncObject& object) {
    for (unsigned char byte : object.bytes) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

bool test_srw_exports_and_static_initialization(ShimLibrary& library) {
    const char* names[] = {
        "InitializeSRWLock",          "AcquireSRWLockExclusive",     "ReleaseSRWLockExclusive",
        "TryAcquireSRWLockExclusive", "AcquireSRWLockShared",        "ReleaseSRWLockShared",
        "TryAcquireSRWLockShared",    "InitializeConditionVariable", "SleepConditionVariableSRW",
        "WakeConditionVariable",      "WakeAllConditionVariable",
    };
    for (const char* name : names) {
        if (!library.functions.contains(name)) {
            return false;
        }
    }

    auto initializeLock = getFunction<InitializeFn>(library, "InitializeSRWLock");
    auto acquireExclusive = getFunction<AcquireFn>(library, "AcquireSRWLockExclusive");
    auto releaseExclusive = getFunction<AcquireFn>(library, "ReleaseSRWLockExclusive");
    auto tryExclusive = getFunction<TryAcquireFn>(library, "TryAcquireSRWLockExclusive");

    GuestSyncObject lock;
    acquireExclusive(&lock);
    if (tryExclusive(&lock)) {
        return false;
    }
    releaseExclusive(&lock);
    if (!tryExclusive(&lock)) {
        return false;
    }
    releaseExclusive(&lock);
    if (!isZero(lock)) {
        return false;
    }

    GuestSyncObject explicitlyInitialized;
    initializeLock(&explicitlyInitialized);
    return isZero(explicitlyInitialized);
}

bool test_shared_and_exclusive_modes(ShimLibrary& library) {
    auto acquireExclusive = getFunction<AcquireFn>(library, "AcquireSRWLockExclusive");
    auto releaseExclusive = getFunction<AcquireFn>(library, "ReleaseSRWLockExclusive");
    auto tryExclusive = getFunction<TryAcquireFn>(library, "TryAcquireSRWLockExclusive");
    auto acquireShared = getFunction<AcquireFn>(library, "AcquireSRWLockShared");
    auto releaseShared = getFunction<AcquireFn>(library, "ReleaseSRWLockShared");
    auto tryShared = getFunction<TryAcquireFn>(library, "TryAcquireSRWLockShared");

    GuestSyncObject lock;
    acquireShared(&lock);

    std::atomic<BOOL> secondReader{0};
    std::thread reader([&] {
        secondReader.store(tryShared(&lock));
        if (secondReader.load()) {
            releaseShared(&lock);
        }
    });
    reader.join();

    const BOOL writerWhileReading = tryExclusive(&lock);
    releaseShared(&lock);
    const BOOL releasedReader = tryExclusive(&lock);
    if (!secondReader.load() || writerWhileReading || !releasedReader) {
        return false;
    }
    releaseExclusive(&lock);

    if (!tryExclusive(&lock)) {
        return false;
    }
    const BOOL readerWhileWriting = tryShared(&lock);
    releaseExclusive(&lock);
    return !readerWhileWriting;
}

bool test_condition_variable_wake_and_timeout(ShimLibrary& library) {
    auto acquireExclusive = getFunction<AcquireFn>(library, "AcquireSRWLockExclusive");
    auto releaseExclusive = getFunction<AcquireFn>(library, "ReleaseSRWLockExclusive");
    auto tryExclusive = getFunction<TryAcquireFn>(library, "TryAcquireSRWLockExclusive");
    auto initializeCondition = getFunction<InitializeFn>(library, "InitializeConditionVariable");
    auto sleepCondition = getFunction<SleepFn>(library, "SleepConditionVariableSRW");
    auto wakeCondition = getFunction<WakeFn>(library, "WakeConditionVariable");
    auto wakeAll = getFunction<WakeFn>(library, "WakeAllConditionVariable");

    GuestSyncObject lock;
    GuestSyncObject condition;
    initializeCondition(&condition);

    std::atomic<bool> entered{false};
    std::atomic<BOOL> woke{0};
    std::thread waiter([&] {
        acquireExclusive(&lock);
        entered.store(true, std::memory_order_release);
        woke.store(sleepCondition(&condition, &lock, 2000, 0), std::memory_order_release);
        releaseExclusive(&lock);
    });

    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Acquiring the lock proves that SleepConditionVariableSRW released it
    // before the signal. The condition-variable wait mutex prevents the
    // signal from being lost between those two operations.
    while (!tryExclusive(&lock)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Windows permits waking while the associated lock is held. The waiter
    // must reacquire the lock only after this thread releases it.
    wakeCondition(&condition);
    releaseExclusive(&lock);
    waiter.join();

    if (!woke.load(std::memory_order_acquire)) {
        return false;
    }

    acquireExclusive(&lock);
    const auto start = std::chrono::steady_clock::now();
    const BOOL timedOut = sleepCondition(&condition, &lock, 10, 0);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    releaseExclusive(&lock);
    const BOOL releasedAfterTimeout = tryExclusive(&lock);
    if (releasedAfterTimeout) {
        releaseExclusive(&lock);
    }

    // A condition wait must reacquire the lock even when it returns FALSE for
    // a timeout. WakeAll is also safe when no waiter is currently present.
    wakeAll(&condition);
    return !timedOut && elapsed.count() < 1000 && releasedAfterTimeout && isZero(lock) && isZero(condition);
}

bool test_shared_condition_mode(ShimLibrary& library) {
    auto acquireShared = getFunction<AcquireFn>(library, "AcquireSRWLockShared");
    auto releaseShared = getFunction<AcquireFn>(library, "ReleaseSRWLockShared");
    auto sleepCondition = getFunction<SleepFn>(library, "SleepConditionVariableSRW");

    GuestSyncObject lock;
    GuestSyncObject condition;
    acquireShared(&lock);
    const BOOL timedOut = sleepCondition(&condition, &lock, 1, CONDITION_VARIABLE_LOCKMODE_SHARED);
    releaseShared(&lock);
    return !timedOut && isZero(lock) && isZero(condition);
}

} // namespace

int main() {
    ShimLibrary library;
    addMissingKernel32(library);

    const bool exportsAndStaticInitialization = test_srw_exports_and_static_initialization(library);
    const bool sharedAndExclusiveModes = test_shared_and_exclusive_modes(library);
    const bool conditionWakeAndTimeout = test_condition_variable_wake_and_timeout(library);
    const bool sharedConditionMode = test_shared_condition_mode(library);

    std::printf("SRW/condition-variable tests: %s\n", exportsAndStaticInitialization ? "PASS" : "FAIL");
    std::printf("shared/exclusive mode tests: %s\n", sharedAndExclusiveModes ? "PASS" : "FAIL");
    std::printf("condition wake/timeout tests: %s\n", conditionWakeAndTimeout ? "PASS" : "FAIL");
    std::printf("shared condition mode tests: %s\n", sharedConditionMode ? "PASS" : "FAIL");

    return exportsAndStaticInitialization && sharedAndExclusiveModes && conditionWakeAndTimeout && sharedConditionMode
               ? 0
               : 1;
}
