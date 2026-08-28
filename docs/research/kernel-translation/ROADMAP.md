# Kernel Translation Layer — Implementation Roadmap
**Updated:** 2026-07-08


> macOS XNU ↔ Windows NT kernel translation for MetalSharp Wine anti-cheat compatibility
> No blockers — only drill-down targets. Everything below is buildable.

---

## Legend

| Symbol | Meaning |
|--------|---------|
| **DRILL** | Deep investigation required before implementation — the translation isn't obvious yet |
| **BUILD** | Implementation is clear from existing research, ready to code |
| **STUB** | Return fake data / STATUS_NOT_SUPPORTED — sufficient for now |
| **DONE** | Already working in Wine or MetalSharp |
| **EXT** | Requires Apple entitlement or system extension |

---

## Phase 0 — Foundation (Current State)

What we have:
- 4 research documents: XNU reference, NT reference, gap analysis, implementation mapping (5,400+ lines)
- C kernel translation modules: syscall/struct tables, coverage reports, host probes
- C header: compile-time mapping tables + XNU constants
- 2 API endpoints: `/kernel-translation/probe`, `/kernel-translation/host-probe`
- Draft PR #146 open

Coverage: 30 syscalls mapped, 11 structs mapped, 5 drill-down targets identified

---

## Phase 1 — Complete the Translation Tables

**Goal:** Map every relevant NT syscall to its XNU equivalent in the C backend modules.

| Task | Type | Detail |
|------|------|--------|
| Expand syscall table from 30 → 100+ entries | BUILD | Pull from `wine-macos-implementation-mapping.md` §1.1–1.13 — all 385 NtXxx syscalls that are ✅/≈/🔄 quality |
| Add remaining struct mappings | BUILD | Pull from implementation mapping Part 4 — all 28 NT structures |
| Object type table | BUILD | Pull from implementation mapping Part 5 — all 22 NT object types |
| Kernel executive function table (Mm/Ps/Io/Ob/Se/Ke/Rtl) | BUILD | Pull from implementation mapping Part 2 — summary coverage for ~500 ntoskrnl exports |
| Win32k syscall table (top 150) | BUILD | Pull from implementation mapping Part 3 — windowing/input/GDI relevant to anti-cheat overlay detection |
| EndpointSecurity event mapping table | BUILD | Pull from gap analysis §17.2 — 8 ES event types paired to NT callbacks |

**Deliverable:** `/kernel-translation/probe` returns full coverage breakdown — total APIs, per-quality counts, per-subsystem coverage percentages.

---

## Phase 2 — Handle Table Virtualization

**Goal:** Replace the "handle enumeration gap" with a virtual handle table that satisfies anti-cheat queries.

This was flagged as the #1 gap. The drill-down is: build it ourselves.

### 2A — Virtual Handle Table

| Task | Type | Detail |
|------|------|--------|
| Wine handle table struct in C | BUILD | Per-process table mapping `HANDLE` → `(type, fd_or_port, access_mask, name)` |
| Handle creation tracking | BUILD | Hook NtCreateFile/NtOpenProcess/NtCreateEvent/etc. to register every handle in the table |
| Handle destruction tracking | BUILD | Hook NtClose to remove entries |
| NtQuerySystemInformation(SystemHandleInformation) | BUILD | Return virtual handle table entries as SYSTEM_HANDLE_INFORMATION structs |
| NtQueryObject(ObjectTypeInformation) | BUILD | Return type name + usage count from virtual table |
| NtDuplicateObject | BUILD | Clone virtual table entry + dup2/mach_port_insert_right |

### 2B — Handle Enumeration Bridge

| Task | Type | Detail |
|------|------|--------|
| `/proc/{pid}/fd` enumeration | DRILL | macOS exposes open fds via `proc_info(PROC_INFO_CALL_PIDFDINFO)` — map to NT handle types |
| Mach port enumeration via `mach_port_names` | DRILL | `mach_port_names(task, &names, &count)` works for own task — test if cross-task works with task port |
| Combine fd + port enumeration into unified handle view | BUILD | Merge both sources into the virtual handle table for NtQuerySystemInformation responses |
| Fake handle data for handles we can't enumerate | BUILD | For handles anti-cheat expects but we can't see (internal Wine handles), fabricate plausible entries |

**Why this works:** Anti-cheat enumerates handles to detect suspicious open handles to the game process. Our virtual table will contain every handle Wine created — which is exactly what the anti-cheat sees on Windows. The anti-cheat doesn't need to enumerate *kernel* handles; it needs to enumerate *its own process's* handles, and Wine controls those.

---

## Phase 3 — Code Integrity Emulation

**Goal:** Make Wine-loaded modules appear "signed and trusted" to anti-cheat integrity checks.

| Task | Type | Detail |
|------|------|--------|
| NtGetCachedSigningLevel → csops bridge | BUILD | Call `csops(CS_OPS_GETSIGNINGINFO)` on the actual Mach-O, translate result to NT signing level |
| NtQueryInformationProcess(ProcessSigningLevel) | BUILD | Return `csops` data translated to NT format |
| Fake signing for PE modules | BUILD | Wine's PE DLLs are unsigned — return a plausible signing level (e.g., `SIGNING_LEVEL_MICROSOFT`) for all Wine-shipped modules |
| Module hash verification bridge | DRILL | Anti-cheat hashes loaded modules and compares to known-good — Wine needs to return correct hashes for the PE images as they exist in memory |
| csops extended queries | DRILL | Test `CS_OPS_PIDINFO`, `CS_OPS_ENTITLEMENTS_BLOB` to understand what real macOS signing data looks like — use this to craft realistic fake responses |
| NtSetCachedSigningLevel handler | STUB | Return `STATUS_SUCCESS` — anti-cheat calls this during initialization, expecting it to succeed |

**Why this works:** Anti-cheat checks if modules are signed. On macOS, Wine's Mach-O wrapper IS signed (by the Wine build). We bridge csops data for Mach-O binaries and fabricate signing data for PE binaries that Wine loads. The anti-cheat sees "signed, trusted" for everything.

---

## Phase 4 — APC Delivery via Mach Exceptions

**Goal:** Replace the "no APC mechanism" gap with Mach exception-based APC injection.

| Task | Type | Detail |
|------|------|--------|
| NtQueueApcThread → Mach exception trampoline | DRILL | Design: (1) suspend target thread, (2) save ARM_CONTEXT, (3) set PC to APC trampoline in Wine's address space, (4) trampoline calls APC function, (5) restores original context, (6) resumes |
| APC trampoline page allocation | BUILD | `mmap` a page with `PROT_READ|PROT_EXEC|PROT_WRITE`, write trampoline code (ARM64) that: calls APC function pointer, then calls `thread_set_state` to restore original context |
| Kernel APC vs User APC distinction | DRILL | NT has kernel APCs (delivered at IRQL) and user APCs (delivered at alertable wait). Wine only needs user APCs — these map to Mach exceptions delivered at user-space boundary |
| APC queue per thread | BUILD | Wine-internal APC queue per thread — enqueue at NtQueueApcThread, deliver at next alertable wait or NtTestAlert |
| NtTestAlert / alertable wait integration | BUILD | Check APC queue on `NtWaitForSingleObject(alertable=TRUE)` and `NtTestAlert` — deliver pending APCs |

**Why this works:** APC is just "call this function in that thread's context." We achieve this by manipulating the ARM64 context of the target thread — exactly what debuggers do. The trampoline pattern is well-established in Wine's existing ptrace-based debugging.

---

## Phase 5 — Process & Thread Notification Callbacks

**Goal:** Provide the equivalent of `PsSetCreateProcessNotifyRoutine` and `PsSetCreateThreadNotifyRoutine` for anti-cheat.

This was flagged as "no thread creation callback." The drill-down: we build it from EndpointSecurity + Mach task notifications.

### 5A — EndpointSecurity Bridge

| Task | Type | Detail |
|------|------|--------|
| EndpointSecurity system extension skeleton | EXT | Create a MetalSharp system extension that subscribes to `ES_EVENT_TYPE_NOTIFY_EXEC`, `ES_EVENT_TYPE_NOTIFY_FORK`, `ES_EVENT_TYPE_NOTIFY_EXIT` |
| Process creation callback → NT PsSetCreateProcessNotifyRoutine | BUILD | When ES fires `NOTIFY_EXEC`, translate to NT callback format: `(parent_pid, child_pid, create=TRUE)` |
| Process exit callback | BUILD | ES `NOTIFY_EXIT` → `(parent_pid, child_pid, create=FALSE)` |
| Image load callback → NT PsSetLoadImageNotifyRoutine | BUILD | ES `NOTIFY_EXEC` + `NOTIFY_MMAP` with `MAP_EXECUTABLE` → `(image_base, image_size, image_name, pid)` |
| Mach IPC bridge from system extension to Wine | DRILL | Design the Mach IPC channel: extension sends notification messages → Wine ntdll receives and dispatches to registered callbacks |
| Apple entitlement acquisition | EXT | Apply for `com.apple.developer.endpoint-security.client` entitlement via Apple Developer Relations |

### 5B — Thread Notification

| Task | Type | Detail |
|------|------|--------|
| Thread creation detection via `task_threads` polling | DRILL | Test if `task_threads()` can be called periodically to detect new threads — compare thread list snapshots |
| Mach exception port for thread birth | DRILL | Explore `task_set_exception_ports` with `EXC_MASK_ALL` — does it get notified on thread creation? |
| `proc_info` thread enumeration | DRILL | `proc_info(PROC_INFO_CALL_PIDTBSDINFO)` exposes thread count — delta detection for thread creation |
| Register thread list change notification | DRILL | Investigate `mach_port_request_notification` on task port with `MACH_NOTIFY_NO_SENDERS` as thread lifecycle signal |
| Build thread notification dispatcher | BUILD | Whatever mechanism works from above, wrap it in a thread that calls registered NT-style callbacks |

**Why this works:** EndpointSecurity gives us process creation/exit/image load — that's 3 of 4 critical NT callbacks. Thread creation is the hardest, but we have 4 avenues to explore. Even without thread-level callbacks, most anti-cheat (EAC, BE) primarily cares about process creation and image loads.

---

## Phase 6 — Handle Operation Callbacks (ObRegisterCallbacks Equivalent)

**Goal:** Detect when another process opens a handle to the game process — the `ObRegisterCallbacks` equivalent.

| Task | Type | Detail |
|------|------|--------|
| MACF `mac_proc_check_get_task` hook | DRILL | This MACF hook fires when a process calls `task_for_pid()` on another process — this IS the "handle open" event. Investigate if it can be hooked from a system extension or if it requires a kext |
| EndpointSecurity `NOTIFY_SIGNAL` for task_for_pid detection | DRILL | When a process calls `task_for_pid()`, does EndpointSecurity see it as a signal or syscall? If yes, we can intercept it |
| Sandbox extension for process access monitoring | DRILL | macOS sandbox can restrict `task_for_pid` — investigate if sandbox profiles can be configured to log/restrict cross-process access |
| Wine virtual handle callback | BUILD | Since Wine controls the handle table (Phase 2), add callback hooks that fire when NtOpenProcess/NtDuplicateObject creates a handle to a protected process |
| Pre-call filtering (ObRegisterCallbacks pre-operation) | BUILD | Allow registered callbacks to return `OB_PRE_OPERATION_PARAMETERS` with `DesiredAccess` modified — Wine can strip `PROCESS_VM_WRITE` from handles opened to protected processes |

**Why this works:** The real question is "can we detect when a process tries to get a handle to the game?" On macOS, the path is `task_for_pid()` → `mach_task_self()`. We intercept this either at the MACF level (if we have a system extension) or at the Wine level (since Wine controls its own handle table). Anti-cheat on Windows uses ObRegisterCallbacks to detect cross-process handle opens — we provide the same signal via a different mechanism.

---

## Phase 7 — Driver Model Translation

**Goal:** Provide the equivalent of kernel driver loading for anti-cheat that requires it.

| Task | Type | Detail |
|------|------|--------|
| System extension architecture for anti-cheat driver | DRILL | Design: anti-cheat "driver" compiles as a macOS system extension (EndpointSecurity + NetworkExtension) instead of a Windows kext |
| IOKit user client for Wine ↔ extension communication | DRILL | Design the IOKit user client that Wine opens to talk to the anti-cheat system extension — this replaces `DeviceIoControl` to the AC kernel driver |
| IOCTL translation layer | BUILD | Map anti-cheat `DeviceIoControl` codes to Mach IPC messages sent to the system extension |
| Driver entry point shim | BUILD | `DriverEntry` equivalent that: (1) registers EndpointSecurity subscription, (2) creates IOKit user client, (3) registers Mach exception server, (4) exposes NT-style callback registration |
| WDM → IOKit type mapping | BUILD | `DEVICE_OBJECT` → `IOService`, `IRP` → Mach message, `DRIVER_DISPATCH` → IOKit method |
| Build the anti-cheat extension template | BUILD | Open-source MetalSharp template that anti-cheat vendors can adapt — they implement `DriverEntry`, we provide the macOS scaffolding |

**Why this works:** We're not trying to load Windows PE drivers. We're providing a macOS-native driver model that anti-cheat vendors can target. The extension does the same thing their Windows driver does — monitor processes, threads, memory, handles — but using EndpointSecurity and MACF instead of WDM. Wine translates the communication channel so the anti-cheat user-mode component thinks it's talking to its Windows driver.

---

## Phase 8 — Anti-Debug & Anti-Tamper

**Goal:** Ensure Wine's virtualization is invisible to anti-cheat detection.

| Task | Type | Detail |
|------|------|--------|
| PEB.BeingDebugged | DONE | Wine sets this correctly |
| NtQueryInformationProcess(ProcessDebugPort) | DONE | Wine returns 0 (no debugger) |
| NtQueryInformationProcess(ProcessDebugObjectHandle) | BUILD | Return `STATUS_PORT_NOT_SET` consistently |
| Hardware breakpoint (DR0-DR3) → ARM debug registers | DRILL | Map NT debug register context to ARM64 `MDSCR_EL1`/`DBGBCR`/`DBGBVR` via `thread_set_state` |
| Anti-attach timing | STUB | `NtQuerySystemTime` returns real time — no gap |
| Wine module detection | DRILL | Anti-cheat can enumerate loaded modules and look for `ntdll.dll`, `wineboot.exe` — investigate if Wine's module list can be sanitized |
| `lstat` on Wine prefix | DRILL | Anti-cheat checks if `C:\Windows\System32` is a real directory — Wine's virtual filesystem must look authentic |
| Timing-based VM detection | DRILL | Some anti-cheat uses `RDTSC`/`QueryPerformanceCounter` to detect virtualization — ensure Wine doesn't introduce detectable timing anomalies |

---

## Phase 9 — User-Mode Anti-Cheat Validation

**Goal:** Test the complete stack against real user-mode anti-cheat.

| Task | Type | Detail |
|------|------|--------|
| EAC user-mode service test | DRILL | Run a game with EAC in user-mode only (Epic Games Launcher supports this) through MetalSharp Wine |
| BattlEye user-mode test | DRILL | Find a BE-protected game that allows user-mode only |
| FACEIT client test | DRILL | FACEIT anti-cheat client runs in user-mode |
| Hook NtQuerySystemInformation and verify handle enumeration returns plausible data | BUILD | Automated test: enumerate handles, verify count and types match expected values |
| Verify code integrity responses are realistic | BUILD | Automated test: call `NtGetCachedSigningLevel` on every loaded module, verify all return "signed" |
| Verify process enumeration works end-to-end | BUILD | Test: `NtQuerySystemInformation(SystemProcessInformation)` returns all running Wine processes |
| Memory scan bridge test | BUILD | Test: `NtReadVirtualMemory` across processes works via `task_for_pid` + `mach_vm_read` |

---

## Phase 10 — Kernel-Assisted Anti-Cheat (Full Stack)

**Goal:** Ship the EndpointSecurity system extension and validate against kernel-level anti-cheat.

| Task | Type | Detail |
|------|------|--------|
| Ship signed EndpointSecurity system extension | EXT | Submit to Apple for notarization with `com.apple.developer.endpoint-security.client` entitlement |
| Process/thread creation callbacks wired end-to-end | BUILD | ES events → Mach IPC → Wine ntdll → registered NT callbacks → anti-cheat driver handler |
| Image load callbacks wired end-to-end | BUILD | ES `NOTIFY_EXEC`/`NOTIFY_MMAP` → NT `PsSetLoadImageNotifyRoutine` callbacks |
| Handle operation callbacks wired end-to-end | BUILD | Phase 6 MACF bridge → NT `ObRegisterCallbacks` pre/post operation callbacks |
| Full driver communication channel | BUILD | Anti-cheat user-mode → `DeviceIoControl` → Mach IPC → system extension → results back |
| EAC full stack test | DRILL | Run EAC-protected game with kernel driver emulation active |
| BattlEye full stack test | DRILL | Run BE-protected game with kernel driver emulation active |
| Performance profiling | BUILD | Measure latency of ES event → Mach IPC → Wine callback path — must be <1ms for game-critical operations |
| Fallback mode | BUILD | If system extension is unavailable, fall back to user-mode stubs — document which anti-cheat features are degraded |

---

## Phase 11 — Integration with MetalSharp Infrastructure

**Goal:** Wire the kernel translation layer into MetalSharp's existing anti-cheat and Wine management systems.

| Task | Type | Detail |
|------|------|--------|
| Integrate with the C anti-cheat evidence system | BUILD | Kernel translation probe results feed into the backend anti-cheat evidence pipeline. |
| Bottle-aware kernel translation | BUILD | Per-bottle kernel translation config — each Wine prefix can have different handle/code-integrity settings |
| Runtime doctor integration | BUILD | `/steam/runtime-doctor` reports kernel translation coverage and capability status |
| Sharp Library UI — kernel translation status | BUILD | Game cards show kernel translation readiness (handle table ✓, code integrity ✓, callbacks ⏳) |
| Wine prefix auto-configuration | BUILD | When creating a Wine prefix for an anti-cheat game, automatically enable virtual handle table and code integrity emulation |
| EndpointSecurity extension lifecycle management | BUILD | Install, activate, deactivate, update the system extension via MetalSharp backend |
| CLI tools | BUILD | `metalsharp kernel-translation status`, `metalsharp kernel-translation probe`, `metalsharp kernel-translation install-extension` |

---

## Phase 12 — Hardening & Edge Cases

| Task | Type | Detail |
|------|------|--------|
| Race condition testing on handle table | DRILL | Verify handle table updates are atomic under concurrent NtCreate*/NtClose calls |
| Anti-cheat update resilience | DRILL | Anti-cheat vendors update their detection — ensure our virtualization doesn't break on new checks |
| Multiple anti-cheat simultaneously | BUILD | Some games run EAC + BattlEye — both must be able to register callbacks without conflicts |
| Wine version compatibility | BUILD | Test against Wine 8.x, 9.x — ensure translation layer works across Wine versions MetalSharp ships |
| Crash recovery | BUILD | If the system extension crashes, Wine must detect this and fall back to user-mode stubs without killing the game |
| Logging & diagnostics | BUILD | Every translated syscall logged with timing — `metalsharp kernel-translation diagnose <pid>` dumps recent translations |
| ARM64e pointer authentication | DRILL | On ARM64e, `pac_*` instructions sign pointers — ensure our context manipulation (Phase 4 APC) doesn't break PAC |

---

## Summary: Dependency Graph

```
Phase 1 (Tables) ──────────────────────────────┐
                                                 │
Phase 2 (Handle Table) ────────────────────────┤
Phase 3 (Code Integrity) ──────────────────────┤
Phase 4 (APC) ─────────────────────────────────┤
                                                 ├──→ Phase 9 (User-Mode Validation)
Phase 5 (Process/Thread Callbacks) ────────────┤
Phase 6 (Handle Callbacks) ────────────────────┤
Phase 7 (Driver Model) ────────────────────────┤
Phase 8 (Anti-Debug) ──────────────────────────┘
                                                 │
                                                 ├──→ Phase 10 (Kernel-Assisted Validation)
                                                 │
Phase 11 (MetalSharp Integration) ←────────────┘
                                                 │
Phase 12 (Hardening) ←─────────────────────────┘
```

Phases 1–8 can be worked in parallel. Phase 9 needs 1–4 complete minimum. Phase 10 needs 1–8 complete. Phase 11 can start as soon as Phase 2 lands. Phase 12 is continuous.

---

## Priority Order for Immediate Work

1. **Phase 1** — Expand tables (pure data entry, no risk)
2. **Phase 2A** — Virtual handle table (biggest impact, solves the #1 "blocker")
3. **Phase 3** — Code integrity (solves the #2 "blocker")
4. **Phase 4** — APC via Mach exceptions (solves DLL injection gap)
5. **Phase 5A** — EndpointSecurity bridge (requires Apple entitlement — start application now)
6. **Phase 8** — Anti-debug hardening (parallel with above)
7. **Phase 9** — Validation testing

Phases 5B, 6, 7 can proceed once Phase 5A proves the ES bridge works.
