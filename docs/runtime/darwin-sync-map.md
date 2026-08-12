# MetalSharp Darwin Sync Map
**Updated:** 2026-08-11


Status: Phase 6 foundation

Linux Proton can lean on Linux synchronization primitives and, where available, `/dev/ntsync`. macOS does not have Linux futexes or an `ntsync` device. MetalSharp has to map Windows synchronization behavior onto Darwin primitives without pretending macOS is Linux.

## Source Surface

The first source-backed map lives in:

- `include/metalsharp/DarwinSyncMap.h`
- `src/runtime/host/DarwinSyncMap.cpp`
- `tests/test_darwin_sync_map.cpp`

It classifies each primitive by strategy, whether a kernel/system component is required, and whether the mapping is safe to call shipping-ready today.

## Current Classification

| Primitive | Strategy | Shipping-ready | Notes |
|---|---|---:|---|
| Event | `pthread_mutex_t + pthread_cond_t` | yes | Good for tracked in-process manual/auto reset events. |
| Semaphore | dispatch/pthread counted semaphore | yes | Current use is covered; Mach semaphore benchmarking remains useful. |
| Mutex | `pthread_mutex_t` | yes | Current in-process ownership semantics are covered. |
| CriticalSection | `pthread_mutex_t` | yes | Already represented by existing kernel32/ntdll shims. |
| SRWLock | out-of-line `pthread_rwlock_t` keyed by guest address | yes | Preserves zero/static initialization and shared/exclusive modes without writing a host pthread object into the guest's pointer-sized storage. |
| ConditionVariable | out-of-line `pthread_cond_t` plus wait mutex keyed by guest address | yes | The wait mutex closes the release/wait race; `SleepConditionVariableSRW` reacquires the mapped SRW lock before returning. |
| WaitAny | condition-variable wakeups over tracked handles | yes | In-process only; cross-process semantics need more work. |
| WaitAll | coordinated wait over tracked handles | no | Needs correctness tests for mixed event/semaphore/mutex waits. |
| Futex | Darwin `ulock` candidate | no | Research candidate only; not Linux ABI compatible. |
| NtSyncDevice | unsupported Linux-specific device | no | macOS has no `/dev/ntsync` equivalent. |

## Boundary

This map is not an anti-cheat bypass and not a kernel plan. It is a compatibility inventory. If a future game proves we need cross-process NT object semantics or lower-latency wait behavior, that evidence decides whether to prototype a user-space `ulock` strategy, a Mach-backed strategy, or an Apple-approved system extension investigation.

## Remaining Work

- Add mixed-object WaitAll correctness tests.
- Benchmark pthread condition variables against Mach semaphores and `ulock` where legally usable.
- Identify which real games stress synchronization enough to justify deeper work.
- Document entitlement, signing, notarization, install, and user-consent requirements before any system extension prototype.
