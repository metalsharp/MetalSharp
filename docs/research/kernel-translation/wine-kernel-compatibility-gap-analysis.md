# Wine Kernel Compatibility Gap Analysis
**Updated:** 2026-08-11

## Windows NT → macOS XNU Mapping for Anti-Cheat Game Compatibility
### macOS 26.6 (Tahoe) / arm64 (Apple Silicon) vs Windows 10/11 (NT 10.0+)

> Purpose: Identify every kernel API surface that anti-cheat software touches, map it to the macOS XNU equivalent, and classify gaps where Wine cannot bridge.

---

## Table of Contents
1. [Executive Summary](#1-executive-summary)
2. [Architecture Comparison](#2-architecture-comparison)
3. [Gap Classification System](#3-gap-classification-system)
4. [Process & Thread Lifecycle](#4-process--thread-lifecycle)
5. [Virtual Memory Management](#5-virtual-memory-management)
6. [Handle / Object Manager](#6-handle--object-manager)
7. [Debugging Interface](#7-debugging-interface)
8. [Security & Code Integrity](#8-security--code-integrity)
9. [Registry](#9-registry)
10. [Synchronization Primitives](#10-synchronization-primitives)
11. [IPC / LPC / ALPC](#11-ipc--lpc--alpc)
12. [I/O & Filesystem](#12-io--filesystem)
13. [Driver Loading](#13-driver-loading)
14. [Windowing & Input (Win32k)](#14-windowing--input-win32k)
15. [Network Stack](#15-network-stack)
16. [Power Management](#16-power-management)
17. [Kernel Callback Infrastructure](#17-kernel-callback-infrastructure)
18. [Anti-Cheat Specific Touch Points](#18-anti-cheat-specific-touch-points)
19. [Critical Gaps Summary](#19-critical-gaps-summary)
20. [Wine Implementation Feasibility Matrix](#20-wine-implementation-feasibility-matrix)

---

## 1. Executive Summary

This document maps every Windows NT kernel API surface used by game anti-cheat systems (EasyAntiCheat, BattlEye, Vanguard, FACEIT, ESEA) to their macOS XNU equivalents. The goal is to determine what Wine must implement, emulate, or stub for anti-cheat-protected games to run on macOS.

### Key Findings

| Category | Total NT APIs | Mapped to XNU | Partial Match | No Equivalent | Gap % |
|----------|--------------|---------------|---------------|---------------|-------|
| Process/Thread | 30 | 20 | 4 | 6 | 20% |
| Virtual Memory | 24 | 18 | 2 | 4 | 17% |
| Object Manager | 15 | 3 | 4 | 8 | 53% |
| Debugging | 7 | 4 | 1 | 2 | 29% |
| Security/Token | 39 | 6 | 5 | 28 | 72% |
| Registry | 42 | 0 | 0 | 42 | 100% |
| Synchronization | 35 | 18 | 8 | 9 | 26% |
| IPC/ALPC | 38 | 12 | 6 | 20 | 53% |
| I/O & FS | 25 | 18 | 3 | 4 | 16% |
| Driver Loading | 25 | 4 | 2 | 19 | 76% |
| Win32k | 200+ | ~40 | ~30 | ~130 | ~65% |
| Kernel Callbacks | 12 | 6 | 2 | 4 | 33% |
| **TOTAL** | **~492** | **~149** | **~67** | **~276** | **~56%** |

**Bottom line:** 56% of Windows NT kernel APIs have no direct macOS XNU equivalent. Most of these (Registry, Object Manager, Token/Security, Driver, Win32k) are emulated in Wine's userspace layer. The critical anti-cheat gaps are in handle enumeration, debug objects, code integrity verification, and kernel-mode callbacks.

---

## 2. Architecture Comparison

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Windows NT                                            │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐    │
│  │ NtXxx    │ │ Object   │ │ Handle   │ │ Token    │ │ Registry │    │
│  │ Syscalls │ │ Manager  │ │ Table    │ │ (Se)     │ │ (Cm)     │    │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────┘    │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐                 │
│  │ Process  │ │ Memory   │ │ I/O Mgr  │ │ Win32k   │                 │
│  │ (Ps)     │ │ (Mm)     │ │ (Io)     │ │ GDI/Win  │                 │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐                              │
│  │ Callback │ │ Driver   │ │ ALPC     │                              │
│  │ (Ob/Ps)  │ │ (WDM)    │ │ Port     │                              │
│  └──────────┘ └──────────┘ └──────────┘                              │
└─────────────────────────────────────────────────────────────────────────┘

                              ↕ Wine Translation Layer ↕

┌─────────────────────────────────────────────────────────────────────────┐
│                    macOS XNU                                            │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐                 │
│  │ BSD      │ │ Mach     │ │ IOKit    │ │ MACF     │                 │
│  │ Syscalls │ │ IPC/Traps│ │ Drivers  │ │ Security │                 │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐                 │
│  │ Mach VM  │ │ Mach     │ │ BSD VFS  │ │ CoreGra- │                 │
│  │ (vm_map) │ │ Task/    │ │ /Network │ │ phics /  │                 │
│  │          │ │ Thread   │ │          │ │ Skywalk  │                 │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘                 │
└─────────────────────────────────────────────────────────────────────────┘
```

### Structural Mismatches

| NT Concept | XNU Equivalent | Gap Nature |
|------------|---------------|------------|
| Object Manager namespace | No equivalent — XNU uses Mach port names + file descriptors | Structural — Wine must maintain its own handle table |
| Unified handle table (HANDLE) | Mach port namespace (per-task) + fd table (per-process) | Structural — handles are per-process on NT, ports are per-task on Mach |
| Token-based security | POSIX uid/gid + Mach task credentials + MACF labels | Semantic — NT tokens are rich objects, POSIX uids are integers |
| Registry hive tree | No equivalent | Complete — Wine uses on-disk file-backed registry |
| Kernel-mode drivers | IOKit C++ drivers + kexts | Structural — completely different driver models |
| APC delivery | Mach exceptions + Unix signals | Semantic — different delivery mechanisms |
| IRP-based I/O | BSD devfs + IOKit user clients | Structural — no IRP equivalent |
| Waitable timer objects | mk_timer_create (Mach) | Close — mk_timer is similar but Mach-native |
| Process jobs | Coalitions (partial) | Partial — coalitions exist but lack job-level resource limits |

---

## 3. Gap Classification System

| Classification | Symbol | Meaning | Wine Strategy |
|---------------|--------|---------|---------------|
| **Direct Map** | ✅ | 1:1 API exists on XNU | Thin wrapper around XNU syscall/trap |
| **Close Match** | ≈ | Similar API, different semantics | Wrapper with semantic translation |
| **Userspace Emulation** | 🔄 | No kernel equivalent, but can emulate in userspace | Wine implements in ntdll/kernel32 |
| **Partial / Stub** | ⚠️ | Subset implementable, rest returns fake data | Implement critical paths, stub rest |
| **Structural Gap** | ❌ | Fundamentally different, no Wine bridge possible | Major compatibility blocker |
| **Not Needed** | — | Anti-cheat doesn't touch this API | No action required |

---

## 4. Process & Thread Lifecycle

### 4.1 Full Mapping

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtCreateProcess | fork (2) + execve (59) | ✅ | Wine uses fork+exec or posix_spawn |
| NtCreateProcessEx | posix_spawn (244) | ✅ | Direct equivalent |
| NtCreateUserProcess | posix_spawn (244) | ✅ | Wine maps to posix_spawn with fd/actions |
| NtCreateThread | bsdthread_create (360) | ✅ | Different API, same result |
| NtCreateThreadEx | bsdthread_create (360) | ≈ | Missing TEB initializations — Wine handles in userspace |
| NtTerminateProcess | exit (1) / kill (37) / terminate_with_payload (520) | ✅ | |
| NtTerminateThread | thread_terminate (mach) | ✅ | |
| NtSuspendProcess | pid_suspend (433) | ✅ | |
| NtSuspendThread | thread_suspend (mach) | ✅ | |
| NtResumeProcess | pid_resume (434) | ✅ | |
| NtResumeThread | thread_resume (mach) | ✅ | |
| NtYieldExecution | swtch (mach trap 41) | ✅ | |
| NtDelayExecution | nanosleep / clock_sleep | ✅ | |
| NtContinue | sigreturn (184) | ≈ | Works for exception continuation |
| NtGetContextThread | thread_get_state (mach) | ✅ | ARM_CONTEXT flavor on Apple Silicon |
| NtSetContextThread | thread_set_state (mach) | ✅ | ARM_CONTEXT flavor on Apple Silicon |
| NtQueueApcThread | thread_set_state + ARM context manipulation | ⚠️ | No native APC mechanism — Wine must inject via signal or Mach exception |
| NtQueueApcThreadEx/Ex2 | (same as above) | ⚠️ | Extended APC — same gap |
| NtAlertThread | — | 🔄 | Wine emulates with eventfd/ulock |
| NtAlertResumeThread | — | 🔄 | Wine emulates |
| NtAlertThreadByThreadId | — | 🔄 | Wine emulates with ulock |
| NtTestAlert | — | 🔄 | Wine stubs |
| NtCreateProcessStateChange | pid_suspend (433) | ≈ | Different model — no state change objects |
| NtCreateThreadStateChange | — | ⚠️ | No thread state change objects on XNU |
| NtTerminateEnclave | — | — | SGX/ enclave — not on macOS |
| NtTerminateJobObject | kill (37) | ≈ | See Jobs section |

### 4.2 Process Information

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtOpenProcess | proc_info (336) + task_for_pid (mach trap 50) | ✅ | Requires entitlement or root |
| NtGetNextProcess | proc_info (336) | ✅ | Enumerate PIDs |
| NtGetNextThread | task_threads (mach) | ✅ | Enumerate threads in task |
| NtQueryInformationProcess | proc_info (336) | ≈ | Different info classes — Wine translates |
| NtSetInformationProcess | process_policy (323) | ≈ | Subset supported |
| NtQueryInformationThread | thread_info (mach) | ✅ | |
| NtSetInformationThread | thread_policy_set (mach) | ✅ | |
| NtQuerySystemInformation | sysctl (202) + proc_info (336) | ≈ | Many info classes — Wine maps critical ones |

---

## 5. Virtual Memory Management

### 5.1 Full Mapping

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtAllocateVirtualMemory | mmap (197) / mach_vm_allocate (mach trap 17) | ✅ | |
| NtAllocateVirtualMemoryEx | mmap (197) | ✅ | Extended attributes mapped |
| NtFreeVirtualMemory | munmap (73) / mach_vm_deallocate (mach trap 18) | ✅ | |
| NtProtectVirtualMemory | mprotect (74) / mach_vm_protect (mach trap 19) | ✅ | |
| NtQueryVirtualMemory | mach_vm_region / mach_vm_region_recurse | ✅ | |
| NtLockVirtualMemory | mlock (203) | ✅ | |
| NtUnlockVirtualMemory | munlock (204) | ✅ | |
| NtFlushVirtualMemory | msync (65) | ✅ | |
| NtReadVirtualMemory | mach_vm_read (mach IPC) | ✅ | Cross-process read via task port |
| NtWriteVirtualMemory | mach_vm_write (mach IPC) | ✅ | Cross-process write via task port |
| NtGetWriteWatch | mincore (78) | ≈ | Partial — mincore gives residency, not dirty tracking |
| NtResetWriteWatch | — | ⚠️ | No write-watch reset on XNU |
| NtCreateSection | shm_open (266) + mmap (197) | ✅ | Wine creates file-backed or anonymous mappings |
| NtOpenSection | shm_open (266) | ✅ | |
| NtMapViewOfSection | mmap (197) | ✅ | |
| NtUnmapViewOfSection | munmap (73) | ✅ | |
| NtExtendSection | — | 🔄 | Wine emulates by remapping |
| NtAreMappedFilesTheSame | — | 🔄 | Wine compares inode/device |
| NtSetInformationVirtualMemory | madvise (75) | ≈ | Subset of info classes |
| NtAllocateUserPhysicalPages | — | ❌ | No physical page allocation API on XNU |
| NtFreeUserPhysicalPages | — | ❌ | |
| NtMapUserPhysicalPages | — | ❌ | |
| NtCreateSectionEx | — | 🔄 | Extended section — Wine stubs |
| NtMapViewOfSectionEx | — | 🔄 | Extended mapping — Wine stubs |

### 5.2 Anti-Cheat VM Patterns

| Anti-Cheat Technique | NT API Pattern | XNU Translation | Feasibility |
|---------------------|----------------|-----------------|-------------|
| Memory scanning | NtReadVirtualMemory in loop | mach_vm_region + mach_vm_read | ✅ Works — needs task port |
| Code injection | NtAllocateVirtualMemory + NtWriteVirtualMemory + NtCreateThreadEx | mmap + mach_vm_write + bsdthread_create | ✅ Works — needs task port |
| Memory protection monitoring | NtProtectVirtualMemory + NtQueryVirtualMemory | mprotect + mach_vm_region | ✅ Works |
| Process hollowing | NtUnmapViewOfSection + NtWriteVirtualMemory + NtSetContextThread | munmap + mach_vm_write + thread_set_state | ✅ Works — needs task port |
| DLL injection | NtCreateSection + NtMapViewOfSection (remote) + NtQueueApcThread | mmap (remote via mach_vm_map) + thread_set_state | ⚠️ APC gap — must use Mach exceptions instead |

---

## 6. Handle / Object Manager

### 6.1 The Handle Table Gap

This is the single largest structural gap. Windows NT has a unified object manager where every kernel object (process, thread, file, mutex, event, registry key, port, etc.) is accessed through a `HANDLE` in a per-process handle table. macOS XNU has no equivalent.

| NT Concept | macOS Equivalent | Gap Level |
|------------|-----------------|-----------|
| Per-process handle table | Mach port namespace + fd table | ❌ Structural — Wine maintains its own handle table in ntdll |
| Object types (30+ types) | No unified type system | ❌ Wine implements each object type separately |
| Handle inheritance | fork inherits fds + mach ports | ≈ Close enough for most cases |
| Handle duplication (NtDuplicateObject) | dup/dup2 (41/90) for fds, mach_port_insert_right for ports | ✅ |
| Object namespace (\??\, \BaseNamedObjects\) | No equivalent | ❌ Wine implements named objects via files in ~/.wine/ |
| Object security (DACL/SACL) | POSIX mode bits + ACLs + MACF | ⚠️ Very different model |

### 6.2 Object Manager APIs

| Windows NT API | macOS Equivalent | Gap | Notes |
|----------------|-----------------|-----|-------|
| NtCreateDirectoryObject | — | 🔄 | Wine uses directory in its prefix |
| NtOpenDirectoryObject | — | 🔄 | |
| NtCreateSymbolicLinkObject | symlink (57) | ✅ | |
| NtOpenSymbolicLinkObject | readlink (58) | ✅ | |
| NtQueryObject | — | ❌ | No way to query arbitrary kernel object info |
| NtSetInformationObject | — | ❌ | |
| NtDuplicateObject | dup (41) / mach_port_insert_right | ≈ | Works for fds/ports, not Wine handles |
| NtMakePermanentObject | — | ❌ | No concept |
| NtCompareObjects | — | ❌ | |
| NtQuerySecurityObject | fstat + acl_get_fd | ≈ | Very different security model |
| NtSetSecurityObject | fchmod + acl_set_fd | ≈ | |

### 6.3 Handle Enumeration (Anti-Cheat Critical)

| What Anti-Cheat Does | NT Mechanism | macOS Equivalent | Gap |
|----------------------|-------------|-----------------|-----|
| Enumerate all open handles | NtQuerySystemInformation(SystemHandleInformation) | — | ❌ **CRITICAL GAP** — no way to enumerate another process's Mach ports or fds |
| Check handle count | NtQueryInformationProcess(ProcessHandleCount) | proc_info | ⚠️ Partial |
| Find open file handles | NtQuerySystemInformation(SystemHandleInformation) + NtQueryObject | lsof (via proc_info) | ⚠️ Can enumerate fds via proc_info but not Mach ports |
| Detect handle leaks | Handle enumeration | — | ❌ |
| Validate handle access rights | NtQueryObject(ObjectTypeInformation) | — | ❌ No metadata on fd/port access rights |

**Impact on Anti-Cheat:** BattlEye and Vanguard enumerate the process handle table to detect suspicious open handles (e.g., open handles to the game process from a cheat). Wine cannot provide this information because macOS does not expose another process's Mach port namespace.

---

## 7. Debugging Interface

### 7.1 Full Mapping

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtCreateDebugObject | — | ❌ | No debug object concept — Wine emulates with wait status |
| NtDebugActiveProcess | ptrace (PT_ATTACH) (26) | ✅ | |
| NtWaitForDebugEvent | waitid (173) + Mach exceptions | ≈ | Wine translates Mach exceptions → DEBUG_EVENT |
| NtDebugContinue | ptrace (PT_CONTINUE) (26) | ✅ | |
| NtRemoveProcessDebug | — | ❌ | Wine must track internally |
| NtSetInformationDebugObject | — | ❌ | |
| DbgUiConnectToDbg | ptrace + task_for_pid | ✅ | |
| DbgkpQueueMessage | Mach exception port | ≈ | Wine routes Mach exceptions to debug handlers |

### 7.2 Anti-Debug Detection

| Anti-Debug Technique | NT Mechanism | macOS Mechanism | Wine Feasibility |
|---------------------|-------------|-----------------|-----------------|
| IsDebuggerPresent | PEB.BeingDebugged | ptrace P_TRACED flag | ✅ Wine sets PEB.BeingDebugged |
| NtQueryInformationProcess(ProcessDebugPort) | Debug object port | P_TRACED check | ✅ Wine emulates |
| NtQueryInformationProcess(ProcessDebugObjectHandle) | Debug object | — | ⚠️ Wine stubs — returns STATUS_PORT_NOT_SET |
| CheckRemoteDebuggerPresent | NtQueryInformationProcess | proc_info + P_TRACED | ✅ |
| Hardware breakpoints (DR0-DR3) | CONTEXT.Dr0-Dr3 | ARM debug registers (MDSCR_EL1) via thread_set_state | ⚠️ ARM has different debug register model |
| Software breakpoints | INT3 / int3 instruction | Software breakpoint via ptrace | ✅ |
| Anti-attach timing | NtQuerySystemTime | mach_absolute_time | ✅ |

---

## 8. Security & Code Integrity

### 8.1 Token / Security

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtCreateToken | — | ❌ | No token creation API |
| NtDuplicateToken | — | ❌ | |
| NtOpenProcessToken | — | ❌ | No token objects — Wine emulates with uid/gid |
| NtOpenThreadToken | — | ❌ | |
| NtAdjustPrivilegesToken | — | ❌ | No privilege tokens — POSIX capabilities are different |
| NtAccessCheck | mac_vnode_check_access | ≈ | MACF hooks provide access checks but different API |
| NtPrivilegeCheck | mac_priv_check | ≈ | |
| NtQueryInformationToken | — | ❌ | |

**Impact:** Low for anti-cheat. Most anti-cheat software doesn't deeply inspect tokens. Wine stubs the token APIs sufficiently.

### 8.2 Code Integrity

| NT Mechanism | macOS Mechanism | Gap | Anti-Cheat Impact |
|-------------|-----------------|-----|-------------------|
| NtGetCachedSigningLevel | csops (169) | ✅ | csops CS_OPS_GETSIGNINGINFO returns equivalent data |
| NtSetCachedSigningLevel | — (SIP protects) | ❌ | Cannot set signing level — SIP enforces |
| Authenticode signature check | codesign + csops | ✅ | Different format but equivalent function |
| CI.dll (Code Integrity module) | AMFI + MACF mac_vnode_check_signature | ≈ | AMFI is the Apple equivalent of CI.dll |
| Kernel-mode code signing (KMCI) | kext signing requirement | ✅ | Apple requires kext signing |
| Image load validation | mac_vnode_check_exec + AMFI | ✅ | |
| Process trust level | csops + entitlements | ✅ | csops CS_OPS_PIDINFO |
| NtQueryInformationProcess(ProcessSigningLevel) | csops | ✅ | |

### 8.3 Code Signing: The Critical Path

```
Windows Anti-Cheat Code Integrity Check:
  1. NtQueryInformationProcess(ProcessSigningLevel) → verify process is signed
  2. NtGetCachedSigningLevel(handle) → check DLL signing level
  3. NtQueryVirtualMemory(MEM_IMAGE) → enumerate loaded modules
  4. For each module: verify Authenticode signature
  5. Report: trusted / untrusted / tampered

macOS Equivalent:
  1. csops(CS_OPS_PIDINFO, pid) → get code signing info for process
  2. csops(CS_OPS_GETSIGNINGINFO) → get signing info for Mach-O
  3. proc_info(PROC_PIDREGIONINFO) → enumerate memory regions
  4. For each region: getattrlist → check code signature
  5. mac_vnode_check_signature → MACF validates signature

Wine Translation:
  1. NtQueryInformationProcess → Wine calls csops internally
  2. NtGetCachedSigningLevel → Wine maps to csops or fakes result
  3. NtQueryVirtualMemory → Wine maps to mach_vm_region + proc_info
  4. Signature verification → Wine returns "signed" for all Wine DLLs
```

**Key Risk:** Anti-cheat that calls `NtGetCachedSigningLevel` on every loaded DLL will see Wine's PE modules as unsigned. Wine must either fake signing results or ensure the anti-cheat doesn't inspect Wine's internal modules.

---

## 9. Registry

### 9.1 Complete Gap

**42 NT Registry APIs → 0 macOS equivalents**

The Windows Registry has absolutely no analog in macOS XNU. This is a complete structural gap.

| NT API Count | Wine Strategy |
|-------------|---------------|
| 42 registry syscalls (NtCreateKey through NtThawRegistry) | Wine implements a file-backed registry in `~/.wine/*.reg` files |

**Anti-Cheat Impact:**
- Most anti-cheat reads registry keys for: installed software, hardware info, previous bans, driver settings
- Wine provides a virtual registry — anti-cheat reads will return Wine's fabricated data
- **Risk:** Sophisticated anti-cheat may detect that registry data doesn't match expected real hardware values

---

## 10. Synchronization Primitives

### 10.1 Full Mapping

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtCreateEvent | kevent (363) | ≈ | Different model — Wine uses eventfd or futex-like |
| NtSetEvent | kevent / write(eventfd) | ≈ | |
| NtResetEvent | kevent | ≈ | |
| NtClearEvent | kevent | ≈ | |
| NtPulseEvent | — | 🔄 | Wine emulates |
| NtCreateMutant | psynch_mutexwait (301) / psynch_mutexdrop (302) | ✅ | |
| NtReleaseMutant | psynch_mutexdrop (302) | ✅ | |
| NtCreateSemaphore | sem_open (268) / semaphore_create (mach) | ✅ | |
| NtReleaseSemaphore | sem_post (273) | ✅ | |
| NtCreateTimer | mk_timer_create (mach) | ✅ | |
| NtSetTimer | mk_timer_arm (mach) | ✅ | |
| NtCancelTimer | mk_timer_cancel (mach) | ✅ | |
| NtCreateTimer2 | mk_timer_create + mk_timer_arm_leeway | ≈ | |
| NtCreateKeyedEvent | ulock (515/516) | ✅ | |
| NtWaitForKeyedEvent | ulock_wait (515) | ✅ | |
| NtReleaseKeyedEvent | ulock_wake (516) | ✅ | |
| NtCreateIoCompletion | kqueue (362) | ✅ | |
| NtWaitForSingleObject | kevent (363) / select (93) / poll (230) | ✅ | |
| NtWaitForMultipleObjects | kevent (363) | ✅ | |
| NtSignalAndWaitForSingleObject | semaphore_wait_signal (mach trap 13) | ✅ | |
| NtCreateEventPair | — | 🔄 | Wine emulates with two events |
| NtSetHighEventPair / NtSetLowEventPair | — | 🔄 | |
| NtWaitHighEventPair / NtWaitLowEventPair | — | 🔄 | |
| NtSetHighWaitLowEventPair / NtSetLowWaitHighEventPair | — | 🔄 | |
| NtCreateIRTimer | — | 🔄 | Wine stubs |
| NtSetIRTimer | — | 🔄 | |
| NtCreateWaitablePort | mach_port_allocate | ≈ | |
| NtCreateWaitCompletionPacket | — | 🔄 | |
| NtAssociateWaitCompletionPacket | — | 🔄 | |
| NtCancelWaitCompletionPacket | — | 🔄 | |
| NtSetEventBoostPriority | — | 🔄 | Wine emulates |
| NtSetEventEx | — | 🔄 | |

### 10.2 Wine Synchronization Architecture

Wine uses `fast_sync_*` (futex-like primitives) on Linux. On macOS, the mapping is:

| Wine Primitive | macOS Backend |
|---------------|---------------|
| Event | ulock / eventfd |
| Mutex | os_unfair_lock / psynch_mutex |
| Semaphore | dispatch_semaphore / semaphore_create |
| Critical Section | pthread_mutex / os_unfair_lock |
| SRWLock | out-of-line pthread_rwlock_t keyed by guest address |
| Condition Variable | out-of-line pthread_cond plus wait mutex keyed by guest address |
| Timer | mk_timer_create / dispatch_after |

---

## 11. IPC / LPC / ALPC

### 11.1 Full Mapping

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtCreatePort | mach_port_allocate (mach trap 22) | ✅ | |
| NtConnectPort | mach_msg (mach trap 6) | ✅ | |
| NtSecureConnectPort | mach_msg + bootstrap_check_in | ≈ | |
| NtListenPort | mach_msg (receive) | ✅ | |
| NtAcceptConnectPort | mach_msg | ≈ | |
| NtRequestPort | mach_msg (send) | ✅ | |
| NtRequestWaitReplyPort | mach_msg (send+receive) | ✅ | |
| NtReplyPort | mach_msg (send) | ✅ | |
| NtReplyWaitReceivePort | mach_msg | ✅ | |
| NtAlpcCreatePort | mach_port_allocate | ≈ | ALPC has more features |
| NtAlpcConnectPort | mach_msg | ≈ | |
| NtAlpcSendWaitReceivePort | mach_msg | ≈ | |
| NtCompleteConnectPort | — | 🔄 | Wine stubs |
| NtReplyWaitReceivePortEx | — | 🔄 | |
| NtReplyWaitReplyPort | — | 🔄 | |
| NtReadRequestData | — | 🔄 | Wine embeds in message |
| NtWriteRequestData | — | 🔄 | |
| NtImpersonateClientOfPort | — | ❌ | No impersonation on macOS |
| NtAlpcCreatePortSection | — | 🔄 | Wine uses shared memory |
| NtAlpcCreateSectionView | — | 🔄 | |
| NtAlpcCreateResourceReserve | — | 🔄 | |
| NtAlpcCreateSecurityContext | — | ❌ | |
| NtAlpcDeletePortSection | — | 🔄 | |
| NtAlpcDeleteSectionView | — | 🔄 | |
| NtAlpcDeleteResourceReserve | — | 🔄 | |
| NtAlpcDeleteSecurityContext | — | ❌ | |
| NtAlpcImpersonateClientOfPort | — | ❌ | |
| NtAlpcOpenSenderProcess | task_for_pid | ≈ | Can get sender's task |
| NtAlpcOpenSenderThread | thread_act | ≈ | |
| NtAlpcQueryInformation | — | 🔄 | |
| NtAlpcQueryInformationMessage | — | 🔄 | |
| NtAlpcSetInformation | — | 🔄 | |
| NtAlpcCancelMessage | — | 🔄 | |
| NtRegisterThreadTerminatePort | — | 🔄 | Wine emulates |

**Impact:** Moderate. Anti-cheat uses ALPC for communicating with its service/driver. Wine can map basic LPC to Mach IPC. ALPC-specific features (security contexts, port sections, impersonation) are stubbed.

---

## 12. I/O & Filesystem

### 12.1 Full Mapping

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtCreateFile | open (5) / openat (463) | ✅ | |
| NtOpenFile | open (5) | ✅ | |
| NtReadFile | read (3) / pread (153) | ✅ | |
| NtWriteFile | write (4) / pwrite (154) | ✅ | |
| NtClose | close (6) | ✅ | |
| NtDeleteFile | unlink (10) | ✅ | |
| NtFlushBuffersFile | fsync (95) | ✅ | |
| NtDeviceIoControlFile | ioctl (54) | ✅ | Wine translates IOCTL codes |
| NtQueryInformationFile | fstat (189) / getattrlist (220) | ✅ | |
| NtQueryAttributesFile | stat (188) | ✅ | |
| NtQueryDirectoryFile | getdirentries (196) / getdirentries64 (344) | ✅ | |
| NtQueryEaFile | getxattr (234) | ✅ | Extended attributes → xattrs |
| NtSetEaFile | setxattr (236) | ✅ | |
| NtQueryVolumeInformationFile | statfs (157) | ✅ | |
| NtLockFile | flock (131) | ✅ | |
| NtNotifyChangeDirectoryFile | kqueue (362) + EVFILT_VNODE | ✅ | |
| NtReadFileScatter | readv (120) | ✅ | |
| NtWriteFileGather | writev (121) | ✅ | |
| NtSetInformationFile | chflags / chmod / utimes | ≈ | Subset of info classes |
| NtSetVolumeInformationFile | — | ⚠️ | Limited support |
| NtFsControlFile | fsctl (242) | ⚠️ | Some FSCTLs mapped |
| NtQueryQuotaInformationFile | quotactl (165) | ≈ | |
| NtCancelIoFile | — | 🔄 | Wine cancels via fd closure |
| NtCopyFileChunk | sendfile (337) / copyfile (227) | ✅ | |
| NtQueryFullAttributesFile | stat | ✅ | |
| NtFlushBuffersFileEx | fdatasync (187) | ✅ | |

**Impact:** Low for anti-cheat. File I/O maps well.

---

## 13. Driver Loading

### 13.1 Full Mapping

| Windows NT API | macOS XNU Equivalent | Gap | Notes |
|----------------|---------------------|-----|-------|
| NtLoadDriver | kext_load (no direct syscall) | ≈ | IOKit matching + kext loading — very different model |
| NtUnloadDriver | kext_unload | ≈ | |
| NtAddBootEntry through NtFilterBootOption | — | ❌ | No boot entry management on macOS |
| NtLoadHotPatch / NtManageHotPatch | — | ❌ | No hot patching support |
| NtLoadEnclaveData / NtInitializeEnclave / NtCreateEnclave / NtCallEnclave / NtTerminateEnclave | — | ❌ | No SGX/enclave support on Apple Silicon |

### 13.2 Driver Model Comparison

| NT Driver Model | macOS Equivalent | Gap |
|----------------|-----------------|-----|
| WDM (Windows Driver Model) | IOKit (C++ framework) | ❌ Completely different |
| KMDF (Kernel-Mode Driver Framework) | IOKit | ❌ |
| UMDF (User-Mode Driver Framework) | IOKit user clients | ≈ Closest match |
| Minifilter (filesystem) | MACF mac_vnode_check_* hooks | ≈ Different but similar function |
| NDIS (network driver) | NetworkExtension / skywalk | ❌ |
| WFP (Windows Filtering Platform) | NetworkExtension (content filter) | ≈ |
| DirectX graphics driver | Metal Performance Shaders / IOKit GPU | ❌ |

**Anti-Cheat Impact:** **CRITICAL** for kernel-level anti-cheat (Vanguard, FACEIT). These anti-cheats load kernel drivers on Windows. On macOS:
1. Apple requires kexts to be notarized and signed
2. System extensions (DEXTs) are the modern replacement but have limited kernel access
3. EndpointSecurity framework provides some monitoring but requires Apple entitlements
4. **Vanguard-style kernel drivers cannot run on macOS without Apple's explicit approval**

---

## 14. Windowing & Input (Win32k)

### 14.1 Mapping Summary

| Win32k Category | API Count | macOS Equivalent | Gap % |
|----------------|-----------|-----------------|-------|
| Window management | ~30 | CoreGraphics / NSWindow via Wine | ~30% |
| Input (keyboard/mouse) | ~20 | CGEvent / IOKit HID | ~40% |
| GDI drawing | ~20 | CoreGraphics / Cairo (Wine) | ~70% |
| GDI objects | ~15 | CoreGraphics objects | ~60% |
| Text / fonts | ~10 | CoreText | ~50% |
| Clipboard | ~10 | NSPasteboard | ~20% |
| Desktop/station | ~10 | — | ~80% |
| Menu | ~10 | — | ~70% |
| Misc | ~30 | Various | ~50% |

### 14.2 Anti-Cheat Windowing APIs

| What Anti-Cheat Does | NT API | macOS Equivalent | Gap |
|----------------------|--------|-----------------|-----|
| Detect overlay windows | NtUserFindWindowEx | CGWindowListCopyWindowInfo | ✅ |
| Detect input hooks | NtUserSetWindowsHookEx | — | ❌ No global hook mechanism |
| Read keyboard state | NtUserGetAsyncKeyState | CGEventSourceKeyState | ✅ |
| Detect mouse injection | NtUserSendInput detection | mac_iokit_check_hid_control | ≈ |
| Screenshot detection | NtGdiBitBlt | CGWindowListCreateImage | ✅ |
| Window enumeration | NtUserBuildHwndList | CGWindowListCopyWindowInfo | ✅ |
| Raw input detection | NtUserGetRawInputData | IOKit HID | ≈ |
| Block input | NtUserBlockInput | CGEventTap | ≈ |

---

## 15. Network Stack

| NT Mechanism | macOS XNU Equivalent | Gap | Notes |
|-------------|---------------------|-----|-------|
| Winsock (ws2_32) | BSD socket layer | ✅ | Direct mapping |
| WSAIoctl | setsockopt / ioctl | ✅ | |
| Raw sockets | Raw IP sockets | ⚠️ | Restricted on macOS without root |
| Packet capture | NDIS / WFP | NetworkExtension / BPF | ≈ | Different API, same capability |
| DNS resolution | getaddrinfo / res_9 | ✅ | |
| Firewall interaction | WFP | NetworkExtension (content filter) | ≈ |

**Anti-Cheat Impact:** Moderate. Some anti-cheat monitors network traffic for cheat servers. Wine maps sockets well; packet filtering requires NetworkExtension.

---

## 16. Power Management

| NT API Group | macOS Equivalent | Gap | Anti-Cheat Relevance |
|-------------|-----------------|-----|---------------------|
| PoXxx (47 functions) | IOKit power management | ❌ | Low — anti-cheat doesn't touch power APIs |
| PoFx (18 functions) | — | ❌ | Low |
| HalXxx (24 functions) | — | ❌ | Low |

---

## 17. Kernel Callback Infrastructure

This is critical for kernel-level anti-cheat.

### 17.1 NT Kernel Callbacks

| NT Callback | macOS XNU Equivalent | Gap | Anti-Cheat Usage |
|------------|---------------------|-----|-----------------|
| PsSetCreateProcessNotifyRoutineEx2 | mac_proc_check_fork / EndpointSecurity | ≈ | Process creation monitoring (all kernel AC) |
| PsSetCreateThreadNotifyRoutineEx | — | ❌ | Thread creation monitoring |
| PsSetLoadImageNotifyRoutineEx | mac_vnode_check_exec | ≈ | DLL load monitoring (all AC) |
| ObRegisterCallbacks | mac_proc_check_get_task | ≈ | Handle operation filtering (Vanguard) |
| CmRegisterCallbackEx | — | ❌ | Registry operation monitoring |
| FltRegisterFilter | mac_vnode_check_* hooks | ≈ | Filesystem filtering (BE, EAC) |
| SeRegisterImageVerificationCallback | mac_vnode_check_signature | ≈ | Image signing verification |
| KeRegisterProcessorChangeCallback | — | — | Not anti-cheat relevant |

### 17.2 macOS EndpointSecurity Framework

macOS provides EndpointSecurity as the supported way to do what kernel callbacks do on Windows:

| EndpointSecurity Event | NT Equivalent | Available in Wine? |
|----------------------|--------------|-------------------|
| ES_EVENT_TYPE_NOTIFY_EXEC | PsSetLoadImageNotifyRoutine | ✅ Can be used by Wine's anti-cheat bridge |
| ES_EVENT_TYPE_NOTIFY_FORK | PsSetCreateProcessNotifyRoutine | ✅ |
| ES_EVENT_TYPE_NOTIFY_EXIT | — | ✅ |
| ES_EVENT_TYPE_NOTIFY_MMAP | FltRegisterFilter (mem map) | ✅ |
| ES_EVENT_TYPE_NOTIFY_MPROTECT | — | ✅ |
| ES_EVENT_TYPE_NOTIFY_KEXT_LOAD | PsSetLoadImageNotifyRoutine | ✅ |
| ES_EVENT_TYPE_NOTIFY_SIGNAL | — | ✅ |
| ES_EVENT_TYPE_NOTIFY_FILE | FltRegisterFilter | ✅ |

**Requirement:** EndpointSecurity requires Apple entitlement + notarization. Wine cannot use it directly — a companion system extension would be needed.

---

## 18. Anti-Cheat Specific Touch Points

### 18.1 EasyAntiCheat (EAC)

| EAC Technique | NT APIs Used | macOS Feasibility | Blocker? |
|--------------|-------------|-------------------|----------|
| Process enumeration | NtQuerySystemInformation | ✅ proc_info | No |
| Module enumeration | NtQueryVirtualMemory (MEM_IMAGE) | ✅ mach_vm_region + proc_info | No |
| Memory scanning | NtReadVirtualMemory | ✅ mach_vm_read | No |
| Code integrity check | NtGetCachedSigningLevel | ⚠️ csops (may need fake results) | Maybe |
| Handle enumeration | NtQuerySystemInformation(SystemHandleInformation) | ❌ No equivalent | **YES** |
| Thread context validation | NtGetContextThread | ✅ thread_get_state | No |
| Anti-debug | IsDebuggerPresent + NtQueryInformationProcess | ✅ Wine emulates | No |
| Driver communication | DeviceIoControl → kernel driver | ❌ Cannot load EAC driver | **YES** |
| File integrity | NtQueryInformationFile + hash | ✅ stat + read | No |
| Input detection | NtUserGetAsyncKeyState | ✅ CGEventSourceKeyState | No |

**EAC Verdict:** Blocked by (1) handle enumeration gap, (2) kernel driver requirement.

### 18.2 BattlEye (BE)

| BE Technique | NT APIs Used | macOS Feasibility | Blocker? |
|-------------|-------------|-------------------|----------|
| Process scan | NtQuerySystemInformation | ✅ | No |
| Memory scan | NtReadVirtualMemory | ✅ | No |
| Handle scan | NtQuerySystemInformation(SystemHandleInformation) | ❌ | **YES** |
| Window detection | NtUserFindWindowEx | ✅ CGWindowListCopyWindowInfo | No |
| Module scan | NtQueryVirtualMemory | ✅ | No |
| Code integrity | NtGetCachedSigningLevel | ⚠️ | Maybe |
| Driver communication | DeviceIoControl | ❌ | **YES** |
| Network monitoring | WFP | ≈ NetworkExtension | Maybe |
| Registry checks | NtQueryValueKey | 🔄 Wine virtual registry | No |

**BE Verdict:** Blocked by (1) handle enumeration gap, (2) kernel driver requirement.

### 18.3 Vanguard (Riot)

| Vanguard Technique | NT APIs Used | macOS Feasibility | Blocker? |
|-------------------|-------------|-------------------|----------|
| Kernel driver (vgk.sys) | NtLoadDriver | ❌ Cannot load kext | **YES** |
| Process callbacks | PsSetCreateProcessNotifyRoutineEx2 | ≈ EndpointSecurity | **YES** |
| Thread callbacks | PsSetCreateThreadNotifyRoutineEx | ❌ No thread creation callback | **YES** |
| Image load callbacks | PsSetLoadImageNotifyRoutineEx | ≈ EndpointSecurity | **YES** |
| Handle callbacks | ObRegisterCallbacks | ❌ No handle callback | **YES** |
| Memory integrity | MmCopyVirtualMemory | ✅ mach_vm_read | No |
| Code signing enforcement | CI.dll + NtSetCachedSigningLevel | ❌ SIP enforces, cannot customize | **YES** |
| Process protection | PS_PROTECTED_LEVEL | ≈ csops + entitlements | Maybe |
| ETW tracing | NtTraceEvent | ⚠️ kdebug_trace (179/180) | Maybe |

**Vanguard Verdict:** Fundamentally blocked. Vanguard requires a kernel-mode driver with process/thread/handle/image load callbacks. macOS has no equivalent to ObRegisterCallbacks or PsSetCreateThreadNotifyRoutineEx. EndpointSecurity covers ~40% of what Vanguard needs but requires Apple approval.

### 18.4 FACEIT / ESEA

Similar to EAC/BE — user-mode anti-cheat with optional kernel driver.

| Technique | Blocker? |
|-----------|----------|
| User-mode memory scan | No |
| User-mode process enum | No |
| Kernel driver | **YES** |
| Handle enumeration | **YES** |

---

## 19. Critical Gaps Summary

### 19.1 Gaps by Severity

#### BLOCKER (cannot be bridged by Wine)

| # | Gap | Why It Blocks | Affected Anti-Cheat |
|---|-----|---------------|-------------------|
| 1 | Kernel driver loading | Apple requires notarized kexts; anti-cheat drivers are proprietary | Vanguard, EAC, BE |
| 2 | Handle table enumeration | No API to enumerate another process's Mach ports/fds | EAC, BE, Vanguard |
| 3 | Process/thread creation callbacks | EndpointSecurity exists but needs Apple entitlement | Vanguard |
| 4 | Handle operation callbacks (ObRegisterCallbacks) | No Mach port access callback | Vanguard |
| 5 | Thread creation notification | No per-thread creation callback | Vanguard |
| 6 | Code integrity customization (NtSetCachedSigningLevel) | SIP prevents modifying signing levels | Vanguard |
| 7 | Object Manager namespace | No unified kernel object namespace | All (indirect) |

#### HIGH (can be partially worked around)

| # | Gap | Workaround | Affected Anti-Cheat |
|---|-----|-----------|-------------------|
| 8 | APC delivery (NtQueueApcThread) | Use Mach exceptions or signals | EAC, BE |
| 9 | Debug object model | Wine emulates with wait status | All |
| 10 | Token/security model | Wine stubs with uid/gid | Low impact |
| 11 | ALPC advanced features | Mach IPC covers basics | Some |
| 12 | Win32k GDI/drawing | Wine implements in userspace | Low impact |
| 13 | Hardware debug registers (DR0-3) | ARM debug registers differ | Some |

#### MEDIUM (Wine already handles or can stub)

| # | Gap | Wine Status |
|---|-----|------------|
| 14 | Registry | File-backed virtual registry — works |
| 15 | Event pairs | Emulated with two events |
| 16 | I/O cancellation | fd closure |
| 17 | Boot entry management | Stubbed |
| 18 | Physical page allocation | Stubbed |
| 19 | Write-watch | Stubbed/mincore |
| 20 | Enclave/SGX | Stubbed |

### 19.2 Gap Heat Map

```
NT Subsystem            ████████████████████ Gap Level
Object Manager          ████████████████████ 100%  ❌ BLOCKER
Registry                ████████████████████ 100%  — Wine Emulated
Driver Loading          █████████████████░░░  85%  ❌ BLOCKER
Security/Token          ████████████████░░░░  72%  ⚠️ HIGH
Win32k (GDI/Win)        █████████████░░░░░░░  65%  🔄 STUB
IPC/ALPC                ██████████░░░░░░░░░░  53%  🔄 STUB
Kernel Callbacks        ███████░░░░░░░░░░░░░  33%  ❌ BLOCKER
Debugging               ██████░░░░░░░░░░░░░░  29%  ⚠️ HIGH
Synchronization         █████░░░░░░░░░░░░░░░  26%  🔄 STUB
Process/Thread          ████░░░░░░░░░░░░░░░░  20%  ✅ MOSTLY WORKS
Virtual Memory          ████░░░░░░░░░░░░░░░░  17%  ✅ MOSTLY WORKS
I/O & Filesystem        ████░░░░░░░░░░░░░░░░  16%  ✅ MOSTLY WORKS
```

---

## 20. Wine Implementation Feasibility Matrix

### 20.1 What Wine Can Do Today

| Category | Status | Implementation |
|----------|--------|---------------|
| Process/thread create/terminate | ✅ Working | fork/exec/posix_spawn + bsdthread_create |
| Virtual memory operations | ✅ Working | mmap/munmap/mprotect + mach_vm_* |
| File I/O | ✅ Working | BSD syscalls |
| Basic synchronization | ✅ Working | ulock/kevent/dispatch |
| Registry | ✅ Working | File-backed virtual registry |
| Basic LPC/IPC | ✅ Working | Mach IPC |
| Window management | ✅ Working | Wine X11/Mac driver |
| Debugging | ⚠️ Partial | ptrace + Mach exceptions |
| Code integrity queries | ⚠️ Partial | csops + faked results |
| Module enumeration | ⚠️ Partial | proc_info + dyld info |

### 20.2 What Wine Cannot Do (Requires macOS Changes)

| Category | Blocker | Possible Solution |
|----------|---------|-------------------|
| Kernel driver loading | SIP + kext signing | Apple-approved system extension (not feasible for proprietary AC) |
| Handle enumeration | No Mach port enumeration API | Apple would need to add proc_info for port names |
| Thread creation callbacks | No per-thread notification | EndpointSecurity could add this (Apple decision) |
| Handle access callbacks | No Mach port access hook | Would require new MACF hook |
| Custom code integrity | SIP prevents | By design — Apple controls signing |
| ObRegisterCallbacks equivalent | No port-level access control | Would require new Mach IPC hook |

### 20.3 Path Forward for Anti-Cheat on macOS via Wine

**Tier 1: User-mode anti-cheat (EAC "user-mode", FACEIT client)**
- Feasible if anti-cheat vendor provides a macOS-compatible user-mode client
- Wine handles most NT API translation
- Remaining gaps: handle enumeration (stub), code signing (fake)
- **Probability: 60-70%** — depends on vendor cooperation

**Tier 2: Kernel-assisted anti-cheat (EAC full, BattlEye)**
- Requires either:
  (a) Anti-cheat vendor ships macOS system extension (requires Apple developer account + notarization)
  (b) Wine ships a signed EndpointSecurity system extension that mediates between Wine and the anti-cheat
  (c) Apple adds anti-cheat-specific APIs to macOS
- Handle enumeration remains a hard gap
- **Probability: 20-30%** — requires vendor + Apple cooperation

**Tier 3: Full kernel anti-cheat (Vanguard)**
- Blocked at a fundamental level — Vanguard requires kernel driver with process/thread/handle/image-load callbacks
- macOS architecture (Mach microkernel + SIP) is designed to prevent exactly this kind of kernel access
- The only path is Riot shipping a notarized macOS system extension, which they have not done
- **Probability: <5%** — architectural incompatibility

### 20.4 Recommendations for MetalSharp / Wine on macOS

1. **Focus on Tier 1 games first** — games with user-mode anti-cheat or no anti-cheat
2. **Stub handle enumeration** — return fake handle data that looks normal to anti-cheat
3. **Fake code signing results** — return "signed, trusted" for all Wine-loaded modules
4. **Implement APC via Mach exceptions** — use task_set_exception_ports to deliver APC-like callbacks
5. **Explore EndpointSecurity companion** — a signed system extension could provide process/thread/file monitoring that bridges ~40% of the kernel callback gap
6. **For kernel AC vendors** — the path is: ship a NetworkExtension + EndpointSecurity system extension, not a kext
7. **Handle enumeration workaround** — Wine could maintain its own virtual handle table and expose it through a custom proc_info extension or IOKit user client

---

## Appendix A: Complete Syscall Number Reference

### macOS XNU BSD Syscalls (Anti-Cheat Relevant)

| Number | Name | Anti-Cheat Use |
|--------|------|---------------|
| 1 | exit | Process termination |
| 2 | fork | Process creation |
| 26 | ptrace | Debug attach |
| 37 | kill | Process kill |
| 54 | ioctl | Device communication |
| 74 | mprotect | Memory protection |
| 169 | csops | Code signing ops |
| 184 | sigreturn | Exception continuation |
| 197 | mmap | Memory mapping |
| 202 | sysctl | System information |
| 220 | getattrlist | File attributes |
| 244 | posix_spawn | Process creation |
| 301 | psynch_mutexwait | Mutex wait |
| 302 | psynch_mutexdrop | Mutex release |
| 336 | proc_info | Process information |
| 360 | bsdthread_create | Thread creation |
| 362 | kqueue | Event queue |
| 363 | kevent | Event notification |
| 433 | pid_suspend | Process suspend |
| 434 | pid_resume | Process resume |
| 463 | openat | File open |
| 515 | ulock_wait | Userspace lock wait |
| 516 | ulock_wake | Userspace lock wake |

### macOS XNU Mach Traps (Anti-Cheat Relevant)

| Number | Name | Anti-Cheat Use |
|--------|------|---------------|
| 3 | mach_reply_port | IPC reply |
| 6 | mach_msg | IPC messaging |
| 11 | mach_port_allocate | Port allocation |
| 13 | semaphore_wait_signal | Sync wait+signal |
| 16 | clock_sleep | Sleep |
| 17 | mach_vm_allocate | VM allocation |
| 18 | mach_vm_deallocate | VM deallocation |
| 19 | mach_vm_protect | VM protection |
| 22 | mach_port_allocate | Port alloc |
| 41 | swtch | Yield |
| 50 | task_for_pid | Get task port (CRITICAL) |
| 85 | mach_vm_read | Read process memory |
| 86 | mach_vm_write | Write process memory |
| 87 | mach_vm_region | Query VM region |

---

## Appendix B: Anti-Cheat Vendor Contact Points

| Vendor | Kernel Driver? | macOS Support? | Wine Attitude |
|--------|---------------|---------------|--------------|
| EasyAntiCheat (Epic) | Yes (EAC.sys) | No official support | Has Proton support on Linux — could extend to macOS |
| BattlEye (BOHemia) | Yes (BEDaisy.sys) | No official support | Has Proton support on Linux |
| Vanguard (Riot) | Yes (vgk.sys) | No official support | No Linux/macOS support announced |
| FACEIT | Optional kernel driver | No | No official Wine stance |
| ESEA (ESL) | Yes (ESEA anti-cheat) | No | No Wine support |

---

## Appendix C: File Cross-Reference

| This Document Section | Source File | Section |
|----------------------|-----------|---------|
| Process/Thread APIs | xnu-kernel-reference.md | §2.1, §6 |
| Virtual Memory APIs | xnu-kernel-reference.md | §5 |
| Mach IPC | xnu-kernel-reference.md | §4 |
| MACF Security | xnu-kernel-reference.md | §8 |
| NT Process/Thread | windows-nt-kernel-reference.md | §2.1 |
| NT Virtual Memory | windows-nt-kernel-reference.md | §2.2 |
| NT Security | windows-nt-kernel-reference.md | §2.6, §11 |
| NT Registry | windows-nt-kernel-reference.md | §2.8 |
| NT Object Manager | windows-nt-kernel-reference.md | §2.9, §8 |
| NT Driver | windows-nt-kernel-reference.md | §2.10 |
| Win32k | windows-nt-kernel-reference.md | §17 |
| NT Anti-Cheat Matrix | windows-nt-kernel-reference.md | §20 |
