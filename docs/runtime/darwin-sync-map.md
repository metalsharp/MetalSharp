# Darwin Sync Map
**Updated:** 2026-09-22

MetalSharp maps Windows synchronization behavior onto Darwin primitives. Source surface:

- `include/metalsharp/DarwinSyncMap.h`
- `src/runtime/host/DarwinSyncMap.cpp`
- `tests/test_darwin_sync_map.cpp`

## Classification

| Primitive | Strategy | Shipping-ready | Notes |
|---|---|---:|---|
| Event | `pthread_mutex_t + pthread_cond_t` | yes | Tracked in-process manual/auto reset events |
| Semaphore | dispatch/pthread counted semaphore | yes | Current use covered; Mach benchmarking continues |
| Mutex | `pthread_mutex_t` | yes | In-process ownership semantics covered |
| CriticalSection | `pthread_mutex_t` | yes | Represented by existing kernel32/ntdll shims |
| WaitAny | condition-variable wakeups over tracked handles | yes | In-process; cross-process semantics need work |
| WaitAll | coordinated wait over tracked handles | no | Needs correctness tests for mixed waits |
| Futex | Darwin `ulock` candidate | no | Research candidate |
| NtSyncDevice | Linux-specific device | no | macOS has no `/dev/ntsync` equivalent |

This map is a compatibility inventory. Evidence from games that need cross-process NT object semantics or lower-latency waits decides whether to prototype a user-space `ulock` strategy, a Mach-backed strategy, or a system extension.

## Remaining Work

- Add mixed-object WaitAll correctness tests.
- Benchmark pthread condition variables against Mach semaphores and `ulock`.
- Identify games that stress synchronization enough to justify deeper work.
- Document entitlement, signing, notarization, install, and user-consent requirements before any system extension prototype.
