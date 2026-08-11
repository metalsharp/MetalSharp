#include "metalsharp/DarwinSyncMap.h"

#include <cassert>

using namespace metalsharp;

int main() {
    std::size_t count = 0;
    const auto* mappings = darwinSyncMap(&count);
    assert(mappings != nullptr);
    assert(count >= 8);

    const auto* event = darwinSyncMapping(DarwinSyncPrimitive::Event);
    assert(event != nullptr);
    assert(event->strategy == DarwinSyncStrategy::PThreadCondvar);
    assert(event->shippingReady);
    assert(!event->kernelRequired);

    const auto* srwLock = darwinSyncMapping(DarwinSyncPrimitive::SrwLock);
    assert(srwLock != nullptr);
    assert(srwLock->strategy == DarwinSyncStrategy::PThreadRWLock);
    assert(srwLock->shippingReady);
    assert(!srwLock->kernelRequired);

    const auto* conditionVariable = darwinSyncMapping(DarwinSyncPrimitive::ConditionVariable);
    assert(conditionVariable != nullptr);
    assert(conditionVariable->strategy == DarwinSyncStrategy::PThreadCondvar);
    assert(conditionVariable->shippingReady);
    assert(!conditionVariable->kernelRequired);

    const auto* waitAll = darwinSyncMapping(DarwinSyncPrimitive::WaitAll);
    assert(waitAll != nullptr);
    assert(waitAll->strategy == DarwinSyncStrategy::NeedsResearch);
    assert(!waitAll->shippingReady);

    const auto* ntsync = darwinSyncMapping(DarwinSyncPrimitive::NtSyncDevice);
    assert(ntsync != nullptr);
    assert(ntsync->strategy == DarwinSyncStrategy::UnsupportedLinuxSpecific);
    assert(ntsync->kernelRequired);
    assert(!ntsync->shippingReady);

    return 0;
}
