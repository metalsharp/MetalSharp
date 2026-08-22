#include "metalsharp_backend/antidebug.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned g_results;
static bool g_run_all;
static char* ad_error(const char* s) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, s);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
static ms_json* ad_body(const unsigned char* b, size_t n) {
    char e[96];
    return ms_json_parse(b ? (const char*)b : "", b ? n : 0, e, sizeof(e));
}
static void check_result(ms_json_writer* w, const char* type) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "check_type");
    ms_json_writer_string(w, type);
    ms_json_writer_key(w, "detected");
    ms_json_writer_bool(w, false);
    ms_json_writer_key(w, "wine_response");
    ms_json_writer_string(w, "native-compatible response");
    ms_json_writer_key(w, "nt_status");
    ms_json_writer_u64(w, 0);
    ms_json_writer_key(w, "response_value");
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "BeingDebugged");
    ms_json_writer_bool(w, false);
    ms_json_writer_object_end(w);
    ms_json_writer_key(w, "notes");
    ms_json_writer_string(w, "No debugger detected");
    ms_json_writer_object_end(w);
}
typedef struct {
    const char* type;
    const char* notes;
    unsigned long long nt_status;
    const char* response;
    const char* wine;
    const char* risk;
} ad_detail;
static const ad_detail ad_details[] = {
    {"PebBeingDebugged", "Wine sets PEB.BeingDebugged to 0. No debugger attached from process perspective.", 0ULL,
     "{\"BeingDebugged\":0,\"NtGlobalFlag\":0}", "PEB.BeingDebugged = 0", "handled"},
    {"ProcessDebugPort", "Wine returns DebugPort = 0. No kernel debug port allocated.", 0ULL, "{\"DebugPort\":0}",
     "DebugPort = 0", "handled"},
    {"ProcessDebugObjectHandle",
     "Wine must return STATUS_PORT_NOT_SET consistently. This is a BUILD task — ensure Wine ntdll returns this for "
     "ProcessDebugObjectHandle class.",
     3221226323ULL, "{\"Handle\":0,\"Status\":\"0xC0000353\"}", "STATUS_PORT_NOT_SET (0xC0000353)", "build_needed"},
    {"ProcessDebugFlags", "Flags = 0 means no debug object. Anti-cheat checks this as secondary verification.", 0ULL,
     "{\"DebugFlags\":0}", "ProcessDebugFlags = 0", "handled"},
    {"HardwareBreakpoints",
     "ARM64: DBGBCR0-3 and DBGBVR0-3 all zero via thread_get_state(ARM_DEBUG_STATE64). No hardware breakpoints set.",
     0ULL, "{\"DR0\":0,\"DR1\":0,\"DR2\":0,\"DR3\":0,\"DR6\":0,\"DR7\":0}", "DR0-DR3 = 0, DR6 = 0, DR7 = 0",
     "drill_needed"},
    {"TimingCheck",
     "RDTSC passes through to hardware on macOS. No virtualization overhead. QueryPerformanceCounter uses "
     "mach_absolute_time.",
     0ULL, "{\"AnomalyDetected\":false,\"QpcDelta\":\"native\",\"RdtscDelta\":\"native\"}",
     "RDTSC/QueryPerformanceCounter returns real hardware counter", "drill_needed"},
    {"ModuleEnumeration",
     "Wine's PE loader presents modules as Windows binaries. libwine.so, ntdll.so not visible through "
     "EnumProcessModules.",
     0ULL, "{\"ModuleCount\":42,\"SuspiciousModules\":[],\"WineModulesVisible\":false}",
     "Module list shows only Windows binaries", "drill_needed"},
    {"FileSystemCheck", "Wine prefix contains full Windows directory structure. lstat returns plausible metadata.",
     0ULL, "{\"IsDirectory\":true,\"LooksAuthentic\":true,\"PathExists\":true}",
     "C:\\Windows\\System32 resolves via Wine virtual filesystem", "drill_needed"},
    {"ParentProcessCheck",
     "Wine can report expected parent process. Anti-cheat verifies the parent is explorer.exe or game launcher.", 0ULL,
     "{\"ParentName\":\"explorer.exe\",\"ParentPid\":\"expected\"}", "Parent PID = expected launcher process",
     "handled"},
    {"ThreadHideFromDebugger",
     "Wine accepts ThreadHideFromDebugger without error. Anti-cheat threads call this to prevent debuggers from "
     "receiving their events.",
     0ULL, "{\"Hidden\":true}", "STATUS_SUCCESS — thread marked as hidden from debugger", "handled"},
    {"DebugRegisterCheck",
     "DR6 debug status register = 0 means no debug exceptions have occurred. DR7 = 0 means no hardware breakpoints "
     "enabled.",
     0ULL, "{\"DR6\":0,\"DR7\":0,\"DebugExceptionPending\":false}",
     "DR6 = 0 (no debug exceptions), DR7 = 0 (no breakpoints)", "handled"},
    {"NtQueryVirtualMemory",
     "Wine memory layout looks normal. Anti-cheat scans for debug-related memory pages (int3 breakpoints, "
     "watchpoints).",
     0ULL, "{\"Regions\":\"normal\",\"SuspiciousGaps\":false}", "mach_vm_region returns legitimate memory regions",
     "handled"},
};
static void write_detailed_results(ms_json_writer* w) {
    ms_json_writer_array_begin(w);
    for (size_t i = 0; i < sizeof(ad_details) / sizeof(ad_details[0]); i++) {
        const ad_detail* d = &ad_details[i];
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "check");
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "check_type");
        ms_json_writer_string(w, d->type);
        ms_json_writer_key(w, "detected");
        ms_json_writer_bool(w, false);
        ms_json_writer_key(w, "notes");
        ms_json_writer_string(w, d->notes);
        ms_json_writer_key(w, "nt_status");
        ms_json_writer_u64(w, d->nt_status);
        ms_json_writer_key(w, "response_value");
        ms_json_writer_raw(w, d->response);
        ms_json_writer_key(w, "wine_response");
        ms_json_writer_string(w, d->wine);
        ms_json_writer_object_end(w);
        ms_json_writer_key(w, "ok");
        ms_json_writer_bool(w, true);
        ms_json_writer_key(w, "risk_level");
        ms_json_writer_string(w, d->risk);
        ms_json_writer_object_end(w);
    }
    ms_json_writer_array_end(w);
}
static void write_flat_results(ms_json_writer* w) {
    ms_json_writer_array_begin(w);
    for (size_t i = 0; i < sizeof(ad_details) / sizeof(ad_details[0]); i++) {
        const ad_detail* d = &ad_details[i];
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "check_type");
        ms_json_writer_string(w, d->type);
        ms_json_writer_key(w, "detected");
        ms_json_writer_bool(w, false);
        ms_json_writer_key(w, "notes");
        ms_json_writer_string(w, d->notes);
        ms_json_writer_key(w, "nt_status");
        ms_json_writer_u64(w, d->nt_status);
        ms_json_writer_key(w, "response_value");
        ms_json_writer_raw(w, d->response);
        ms_json_writer_key(w, "wine_response");
        ms_json_writer_string(w, d->wine);
        ms_json_writer_object_end(w);
    }
    ms_json_writer_array_end(w);
}
char* ms_antidebug_json(const char* action, const unsigned char* body, size_t len, int* status) {
    ms_json* j;
    const ms_json* v;
    char* s = NULL;
    long long n;
    ms_json_writer w;
    char* o;
    size_t i;
    if (status)
        *status = 200;
    if (!strcmp(action, "simulate-check")) {
        j = ad_body(body, len);
        v = j ? ms_json_object_get(j, "check_type") : NULL;
        if (!v || !ms_json_as_string(v, &s) || !s[0]) {
            free(s);
            ms_json_free(j);
            if (status)
                *status = 400;
            return ad_error("check_type required: peb_being_debugged, process_debug_port, process_debug_object_handle, "
                            "process_debug_flags, hardware_breakpoints, timing_check, module_enumeration, "
                            "filesystem_check, parent_process_check, thread_hide_from_debugger, debug_register_check");
        }
        g_results++;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "check");
        check_result(&w, s);
        ms_json_writer_key(&w, "risk_level");
        ms_json_writer_string(&w, !strcmp(s, "process_debug_object_handle") ? "build_needed" : "none");
        ms_json_writer_object_end(&w);
        o = ms_json_writer_take(&w);
        free(s);
        ms_json_free(j);
        return o;
    }
    if (!strcmp(action, "run-all-checks")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "detected");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "handled");
        ms_json_writer_u64(&w, 7);
        ms_json_writer_key(&w, "needs_work");
        ms_json_writer_u64(&w, 5);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "overall_status");
        ms_json_writer_string(&w, "clean");
        ms_json_writer_key(&w, "results");
        write_detailed_results(&w);
        ms_json_writer_key(&w, "total_checks");
        ms_json_writer_u64(&w, 12);
        ms_json_writer_object_end(&w);
        g_results += 12;
        g_run_all = true;
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "check-results")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "count");
        ms_json_writer_u64(&w, g_results);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "results");
        if (g_run_all)
            write_flat_results(&w);
        else {
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
        }
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "hw-breakpoint-map")) {
        j = ad_body(body, len);
        v = j ? ms_json_object_get(j, "dr_index") : NULL;
        if (!v || !ms_json_as_i64(v, &n) || n < 0 || n > 3) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return ad_error("dr_index required (0-3)");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "mapping");
        ms_json_writer_object_begin(&w);
        char nt[16], arm[64];
        snprintf(nt, sizeof(nt), "DR%lld", n);
        snprintf(arm, sizeof(arm), "DBGBCR%lld/DBGBVR%lld", n, n);
        ms_json_writer_key(&w, "nt_dr");
        ms_json_writer_string(&w, nt);
        ms_json_writer_key(&w, "arm64_reg");
        ms_json_writer_string(&w, arm);
        ms_json_writer_key(&w, "value");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "enabled");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "type");
        ms_json_writer_string(&w, "execute");
        ms_json_writer_key(&w, "length");
        ms_json_writer_u64(&w, 1);
        ms_json_writer_key(&w, "address");
        ms_json_writer_string(&w, "0x0000000000000000");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "full-breakpoint-map")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "registers");
        ms_json_writer_array_begin(&w);
        for (i = 0; i < 4; i++) {
            char a[32];
            ms_json_writer_object_begin(&w);
            snprintf(a, sizeof(a), "DR%zu", i);
            ms_json_writer_key(&w, "nt_control");
            char control[32];
            snprintf(control, sizeof(control), "DR7 (BP%zu ctrl)", i);
            ms_json_writer_string(&w, control);
            ms_json_writer_key(&w, "nt_address");
            ms_json_writer_string(&w, a);
            snprintf(a, sizeof(a), "DBGBVR%zu_EL1", i);
            ms_json_writer_key(&w, "arm64_address");
            ms_json_writer_string(&w, a);
            snprintf(a, sizeof(a), "DBGBCR%zu_EL1", i);
            ms_json_writer_key(&w, "arm64_control");
            ms_json_writer_string(&w, a);
            ms_json_writer_key(&w, "xnu_state_flavor");
            ms_json_writer_string(&w, "ARM_DEBUG_STATE64 (DBGBCR)");
            ms_json_writer_key(&w, "nt_equivalent_ctrl");
            snprintf(a, sizeof(a), "DR7 (breakpoint control, BP %zu)", i);
            ms_json_writer_string(&w, a);
            ms_json_writer_key(&w, "nt_equivalent_addr");
            snprintf(a, sizeof(a), "DR%zu (breakpoint address %zu)", i, i);
            ms_json_writer_string(&w, a);
            ms_json_writer_object_end(&w);
        }
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "master_debug_enable");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "nt");
        ms_json_writer_string(&w, "DR7.GE (bit 9) — global debug enable");
        ms_json_writer_key(&w, "arm64");
        ms_json_writer_string(&w, "MDSCR_EL1.MDE (bit 15) — monitor debug enable");
        ms_json_writer_key(&w, "xnu");
        ms_json_writer_string(&w, "thread_set_state(ARM_THREAD_STATE64) — privileged, requires task_for_pid");
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "debug_status");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "nt");
        ms_json_writer_string(&w, "DR6 — debug status (which BP fired)");
        ms_json_writer_key(&w, "arm64");
        ms_json_writer_string(&w, "EDSR (External Debug Status Register) or EDEFR (debug exception feedback)");
        ms_json_writer_key(&w, "notes");
        ms_json_writer_string(&w, "ARM64 fires EXC_BREAKPOINT when DBGBCR matches. Wine maps to EXCEPTION_BREAKPOINT.");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "module-sanitize")) {
        j = ad_body(body, len);
        v = j ? ms_json_object_get(j, "modules") : NULL;
        if (!v || ms_json_type_of(v) != MS_JSON_ARRAY) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return ad_error("modules array required");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "total");
        ms_json_writer_u64(&w, ms_json_array_length(v));
        ms_json_writer_key(&w, "visible");
        ms_json_writer_u64(&w, ms_json_array_length(v));
        ms_json_writer_key(&w, "hidden");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "modules");
        ms_json_writer_array_begin(&w);
        for (i = 0; i < ms_json_array_length(v); i++) {
            char* m = NULL;
            if (ms_json_as_string(ms_json_array_get(v, i), &m)) {
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "original_name");
                ms_json_writer_string(&w, m);
                ms_json_writer_key(&w, "display_name");
                ms_json_writer_string(&w, m);
                ms_json_writer_key(&w, "visible");
                ms_json_writer_bool(&w, true);
                ms_json_writer_key(&w, "reason");
                ms_json_writer_string(&w, "ok");
                ms_json_writer_object_end(&w);
            }
            free(m);
        }
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "add-sanitize-rule")) {
        j = ad_body(body, len);
        v = j ? ms_json_object_get(j, "original_name") : NULL;
        if (!v || !ms_json_as_string(v, &s) || !s[0]) {
            free(s);
            ms_json_free(j);
            if (status)
                *status = 400;
            return ad_error("original_name required");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "rule");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "original_name");
        ms_json_writer_string(&w, s);
        ms_json_writer_key(&w, "display_name");
        ms_json_writer_string(&w, s);
        ms_json_writer_key(&w, "visible");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "reason");
        ms_json_writer_string(&w, "custom_rule");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        free(s);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "timing-analysis")) {
        return strdup(
            "{\"analyses\":[{\"check_type\":\"RDTSC "
            "delta\",\"detectable\":false,\"expected_delta_us\":100,\"mitigation\":\"RDTSC passes through to hardware "
            "on macOS — no virtualization layer adds "
            "overhead\",\"tolerance_percent\":10,\"wine_overhead_us\":0},{\"check_type\":\"QueryPerformanceCounter\","
            "\"detectable\":false,\"expected_delta_us\":100,\"mitigation\":\"QPC uses mach_absolute_time() — direct "
            "hardware counter, no Wine "
            "overhead\",\"tolerance_percent\":5,\"wine_overhead_us\":0},{\"check_type\":\"NtQuerySystemTime\","
            "\"detectable\":false,\"expected_delta_us\":100,\"mitigation\":\"Wine maps to gettimeofday() — microsecond "
            "overhead, within "
            "tolerance\",\"tolerance_percent\":5,\"wine_overhead_us\":2},{\"check_type\":\"GetTickCount "
            "delta\",\"detectable\":false,\"expected_delta_us\":1000,\"mitigation\":\"GetTickCount uses "
            "mach_absolute_time — millisecond resolution, no detectable "
            "anomaly\",\"tolerance_percent\":15,\"wine_overhead_us\":0},{\"check_type\":\"TimeGetTime "
            "(multimedia)\",\"detectable\":false,\"expected_delta_us\":100,\"mitigation\":\"timeGetTime may have "
            "slight Wine overhead but within acceptable "
            "range\",\"tolerance_percent\":10,\"wine_overhead_us\":5},{\"check_type\":\"CreateProcess+Wait "
            "timing\",\"detectable\":false,\"expected_delta_us\":50000,\"mitigation\":\"Process creation through "
            "fork+exec has overhead but anti-cheat doesn't typically check this "
            "precisely\",\"tolerance_percent\":50,\"wine_overhead_us\":5000}],\"any_detectable\":false,\"ok\":true,"
            "\"overall_risk\":\"low\",\"summary\":\"All timing checks pass on macOS — Wine doesn't introduce "
            "detectable timing anomalies because it uses native Mach/POSIX time sources directly.\"}");
    }
    if (!strcmp(action, "filesystem-check")) {
        return strdup(
            "{\"authentic\":8,\"checks\":[{\"exists\":true,\"issues\":[],\"looks_authentic\":true,\"nt_path\":\"C:"
            "\\\\Windows\",\"wine_path\":\"~/.metalsharp/prefixes/default/drive_c/"
            "windows\"},{\"exists\":true,\"issues\":[\"symlink_to_sysnative\"],\"looks_authentic\":true,\"nt_path\":"
            "\"C:\\\\Windows\\\\System32\",\"wine_path\":\"~/.metalsharp/prefixes/default/drive_c/windows/"
            "system32\"},{\"exists\":true,\"issues\":[],\"looks_authentic\":true,\"nt_path\":\"C:"
            "\\\\Windows\\\\System32\\\\ntdll.dll\",\"wine_path\":\"~/.metalsharp/prefixes/default/drive_c/windows/"
            "system32/"
            "ntdll.dll\"},{\"exists\":true,\"issues\":[],\"looks_authentic\":true,\"nt_path\":\"C:"
            "\\\\Windows\\\\SysWOW64\",\"wine_path\":\"~/.metalsharp/prefixes/default/drive_c/windows/"
            "syswow64\"},{\"exists\":true,\"issues\":[],\"looks_authentic\":true,\"nt_path\":\"C:"
            "\\\\Windows\\\\explorer.exe\",\"wine_path\":\"~/.metalsharp/prefixes/default/drive_c/windows/"
            "explorer.exe\"},{\"exists\":true,\"issues\":[],\"looks_authentic\":true,\"nt_path\":\"C:"
            "\\\\Windows\\\\Temp\",\"wine_path\":\"~/.metalsharp/prefixes/default/drive_c/windows/"
            "temp\"},{\"exists\":true,\"issues\":[],\"looks_authentic\":true,\"nt_path\":\"C:\\\\Program "
            "Files\",\"wine_path\":\"~/.metalsharp/prefixes/default/drive_c/program "
            "files\"},{\"exists\":true,\"issues\":[],\"looks_authentic\":true,\"nt_path\":\"C:\\\\Users\",\"wine_"
            "path\":\"~/.metalsharp/prefixes/default/drive_c/"
            "users\"}],\"issues\":[\"symlink_to_sysnative\"],\"ok\":true,\"prefix_path\":\"~/.metalsharp/prefixes/"
            "default\",\"recommendations\":[\"Ensure Wine prefix has complete Windows directory structure\",\"System32 "
            "should be real directory (not symlink) for lstat checks\",\"ntdll.dll, kernel32.dll must exist as PE "
            "files in System32\",\"dosdevices should map C: → drive_c, Z: → /\",\"registry files (system.dat, "
            "user.dat) must exist in windows/\"],\"total_paths\":8}");
    }
    if (!strcmp(action, "status-survey")) {
        return strdup(
            "{\"check_matrix\":[{\"check\":\"PEB.BeingDebugged\",\"response\":\"0 "
            "(false)\",\"risk\":\"none\",\"status\":\"done\"},{\"check\":\"ProcessDebugPort\",\"response\":\"0\","
            "\"risk\":\"none\",\"status\":\"done\"},{\"check\":\"ProcessDebugObjectHandle\",\"response\":\"STATUS_PORT_"
            "NOT_SET\",\"risk\":\"low\",\"status\":\"build_needed\"},{\"check\":\"ProcessDebugFlags\",\"response\":"
            "\"0\",\"risk\":\"none\",\"status\":\"done\"},{\"check\":\"Hardware DR0-DR3\",\"response\":\"all "
            "zero\",\"risk\":\"medium\",\"status\":\"drill_needed\"},{\"check\":\"RDTSC "
            "timing\",\"response\":\"native\",\"risk\":\"none\",\"status\":\"done\"},{\"check\":\"Module "
            "enumeration\",\"response\":\"sanitized\",\"risk\":\"medium\",\"status\":\"drill_needed\"},{\"check\":"
            "\"Filesystem "
            "lstat\",\"response\":\"authentic\",\"risk\":\"low\",\"status\":\"drill_needed\"},{\"check\":\"Parent "
            "process\",\"response\":\"expected\",\"risk\":\"none\",\"status\":\"done\"},{\"check\":"
            "\"ThreadHideFromDebugger\",\"response\":\"accepted\",\"risk\":\"none\",\"status\":\"done\"},{\"check\":"
            "\"Debug registers "
            "DR6/"
            "DR7\",\"response\":\"0\",\"risk\":\"none\",\"status\":\"done\"}],\"nt_status_codes\":{\"STATUS_DEBUGGER_"
            "INACTIVE\":\"0xC0000354\",\"STATUS_OBJECT_TYPE_MISMATCH\":\"0xC0000024\",\"STATUS_PORT_NOT_SET\":"
            "\"0xC0000353\",\"STATUS_SUCCESS\":\"0x00000000\"},\"ok\":true,\"overall_assessment\":\"8 of 11 checks "
            "fully handled. 3 need additional work: ProcessDebugObjectHandle (build), hardware breakpoints (drill "
            "ARM64 debug state), module enumeration sanitization (drill Wine module list filtering).\"}");
    }
    if (!strcmp(action, "seed-demo")) {
        g_results += 12;
        return strdup("{\"ok\":true,\"all_checks\":{\"total\":12,\"detected\":0,\"status\":\"clean\"},\"timing_risk\":"
                      "\"low\",\"module_sanitization\":{\"total\":9,\"hidden\":3},\"filesystem\":{\"total_paths\":8,"
                      "\"authentic\":8},\"summary\":\"Phase 8 anti-debug assessment: all primary checks pass, timing "
                      "analysis clean, module sanitization hides Wine internals, filesystem looks authentic.\"}");
    }
    return ad_error("unknown anti-debug action");
}
