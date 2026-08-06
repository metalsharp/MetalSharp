#include "ntdll_hook.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/* === MetalFX Spatial Upscaling live toggle (both arches) ===========
 *
 * The overlay writes <METALSHARP_HOME>/etc/metalfx.overlay.json
 * ("{"enabled":bool,"factor":num,"ts":u64}") via the MetalSharp backend.
 * This poller, resident in the Wine game process, applies the on/off to the
 * process environment with SetEnvironmentVariableW so DXMT picks it up at the
 * next CreateSwapChain (game-initiated swapchain recreate / alt-enter).
 * The factor is not live-reloadable (DXMT Config is a cached singleton); it
 * is written to dxmt.conf by the backend for the next launch.
 */

static volatile LONG g_metalfx_last_ts = 0;
static volatile LONG g_metalfx_stop = 0;

static void metalfx_build_state_path(LPWSTR out, int cap) {
    out[0] = L'\0';
    wchar_t home[1024];
    DWORD n = GetEnvironmentVariableW(L"METALSHARP_HOME", home, 1024);
    if (n == 0 || n >= 1024)
        return;
    /* METALSHARP_HOME is a unix path (/Users/x/.metalsharp); map to Z:\... */
    int wrote = _snwprintf(out, cap, L"Z:");
    for (DWORD i = 0; i < n && wrote < cap - 24; i++) {
        wchar_t c = home[i];
        if (c == L'/')
            c = L'\\';
        out[wrote++] = c;
    }
    if (wrote < cap - 24) {
        out[wrote] = L'\0';
        wcscat_s(out, cap, L"\\etc\\metalfx.overlay.json");
    } else {
        out[0] = L'\0';
    }
}

static int metalfx_read_file(const wchar_t* path, char* buf, int bufsz) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, bufsz - 1, &got, NULL);
    CloseHandle(h);
    if (!ok)
        return -1;
    buf[got] = '\0';
    return (int)got;
}

/* Minimal JSON field extractors (no parser dep). */
static int metalfx_json_bool(const char* s, const char* key, int def) {
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(s, pat);
    if (!p)
        return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (!strncmp(p, "true", 4))
        return 1;
    if (!strncmp(p, "false", 5))
        return 0;
    return def;
}
static unsigned long metalfx_json_u64(const char* s, const char* key, unsigned long def) {
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(s, pat);
    if (!p)
        return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    return strtoul(p, NULL, 10);
}

static DWORD WINAPI metalfx_poller_thread(LPVOID unused) {
    (void)unused;
    char buf[512];
    wchar_t path[1200];
    /* backoff poll: 500ms while a game is likely running; cheap file stat. */
    for (; !g_metalfx_stop; Sleep(500)) {
        metalfx_build_state_path(path, 1200);
        if (!path[0])
            continue;
        if (metalfx_read_file(path, buf, sizeof(buf)) < 0)
            continue;
        unsigned long ts = metalfx_json_u64(buf, "ts", 0);
        if (ts == 0)
            continue;
        if ((LONG)ts == g_metalfx_last_ts)
            continue; /* unchanged */
        int enabled = metalfx_json_bool(buf, "enabled", -1);
        if (enabled < 0)
            continue;
        SetEnvironmentVariableW(L"DXMT_METALFX_SPATIAL_SWAPCHAIN", enabled ? L"1" : L"0");
        g_metalfx_last_ts = (LONG)ts;
        OutputDebugStringA(enabled ? "[metalsharp_ntdll_hook] MetalFX spatial enabled (next swapchain recreate)"
                                   : "[metalsharp_ntdll_hook] MetalFX spatial disabled (next swapchain recreate)");
    }
    return 0;
}

static void metalfx_init(void) {
    HANDLE t = CreateThread(NULL, 0, metalfx_poller_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

#ifndef _WIN64
/* 32-bit games: no Nt* IPC/IAT hooks, but the MetalFX poller runs so the
 * overlay can toggle upscaling live (applies on swapchain recreate). */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        metalfx_init();
    }
    return TRUE;
}
#endif

#ifdef _WIN64

static MS_HOOK_CONTEXT g_ctx = {0};
static SOCKET g_sock = INVALID_SOCKET;
static WSADATA g_wsa = {0};

static UINT32 get_next_request_id(void) {
    return (UINT32)InterlockedIncrement((LONG*)&g_ctx.next_request_id);
}

typedef NTSTATUS(WINAPI* NtOpenProcess_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID);
typedef NTSTATUS(WINAPI* NtOpenThread_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID);
typedef NTSTATUS(WINAPI* NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(WINAPI* NtQueryInformationProcess_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(WINAPI* NtClose_t)(HANDLE);
typedef NTSTATUS(WINAPI* NtDeviceIoControlFile_t)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG,
                                                  PVOID, ULONG);
typedef NTSTATUS(WINAPI* NtQueryVirtualMemory_t)(HANDLE, PVOID, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(WINAPI* NtQueryObject_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(WINAPI* NtSetInformationThread_t)(HANDLE, ULONG, PVOID, ULONG);

static NtOpenProcess_t real_NtOpenProcess = NULL;
static NtOpenThread_t real_NtOpenThread = NULL;
static NtQuerySystemInformation_t real_NtQuerySystemInformation = NULL;
static NtQueryInformationProcess_t real_NtQueryInformationProcess = NULL;
static NtClose_t real_NtClose = NULL;
static NtDeviceIoControlFile_t real_NtDeviceIoControlFile = NULL;
static NtQueryVirtualMemory_t real_NtQueryVirtualMemory = NULL;
static NtQueryObject_t real_NtQueryObject = NULL;
static NtSetInformationThread_t real_NtSetInformationThread = NULL;

static HANDLE get_real_ntdll_fn(const char* name) {
    static HMODULE ntdll_base = NULL;
    if (!ntdll_base) {
        ntdll_base = GetModuleHandleA("ntdll.dll");
    }
    if (!ntdll_base)
        return NULL;
    return (HANDLE)GetProcAddress(ntdll_base, name);
}

static volatile LONG g_fns_loaded = 0;

static void load_real_functions(void) {
    if (InterlockedCompareExchange(&g_fns_loaded, 1, 0) != 0)
        return;
    real_NtOpenProcess = (NtOpenProcess_t)get_real_ntdll_fn("NtOpenProcess");
    real_NtOpenThread = (NtOpenThread_t)get_real_ntdll_fn("NtOpenThread");
    real_NtQuerySystemInformation = (NtQuerySystemInformation_t)get_real_ntdll_fn("NtQuerySystemInformation");
    real_NtQueryInformationProcess = (NtQueryInformationProcess_t)get_real_ntdll_fn("NtQueryInformationProcess");
    real_NtClose = (NtClose_t)get_real_ntdll_fn("NtClose");
    real_NtDeviceIoControlFile = (NtDeviceIoControlFile_t)get_real_ntdll_fn("NtDeviceIoControlFile");
    real_NtQueryVirtualMemory = (NtQueryVirtualMemory_t)get_real_ntdll_fn("NtQueryVirtualMemory");
    real_NtQueryObject = (NtQueryObject_t)get_real_ntdll_fn("NtQueryObject");
    real_NtSetInformationThread = (NtSetInformationThread_t)get_real_ntdll_fn("NtSetInformationThread");
}

static NTSTATUS ipc_call(UINT16 op, const void* req, UINT32 req_size, void* out_data, UINT32 out_data_cap,
                         PULONG out_return_length) {
    UINT8 resp_buf[4096];
    INT32 ipc_status;
    UINT32 ipc_return_len;

    NTSTATUS status = ms_ipc_transact(op, req, req_size, resp_buf, sizeof(resp_buf));
    if (status != STATUS_SUCCESS)
        return STATUS_NOT_IMPLEMENTED;

    memcpy(&ipc_status, resp_buf, 4);
    if (ipc_status != STATUS_SUCCESS)
        return ipc_status;

    memcpy(&ipc_return_len, resp_buf + 4, 4);
    if (out_return_length)
        *out_return_length = ipc_return_len;

    if (out_data && ipc_return_len > 0 && out_data_cap > 0) {
        UINT32 copy_len = ipc_return_len;
        if (copy_len > out_data_cap)
            copy_len = out_data_cap;
        if (copy_len > sizeof(resp_buf) - 8)
            copy_len = (UINT32)sizeof(resp_buf) - 8;
        if (copy_len < ipc_return_len) {
            /* Truncated: report the required length and fail the call like
             * the real Nt* API would, so the caller can retry with a larger
             * buffer instead of misparsing a partial response. */
            if (out_return_length)
                *out_return_length = ipc_return_len;
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        memcpy(out_data, resp_buf + 8, copy_len);
    }

    return STATUS_SUCCESS;
}

static BOOL send_all(SOCKET s, const void* buf, int len) {
    const char* p = (const char*)buf;
    int remaining = len;
    while (remaining > 0) {
        int sent = send(s, p, remaining, 0);
        if (sent <= 0)
            return FALSE;
        p += sent;
        remaining -= sent;
    }
    return TRUE;
}

static BOOL recv_all(SOCKET s, void* buf, int len) {
    char* p = (char*)buf;
    int remaining = len;
    while (remaining > 0) {
        int recvd = recv(s, p, remaining, 0);
        if (recvd <= 0)
            return FALSE;
        p += recvd;
        remaining -= recvd;
    }
    return TRUE;
}

BOOL ms_ipc_connect(void) {
    struct sockaddr_in addr;

    if (WSAStartup(MAKEWORD(2, 2), &g_wsa) != 0) {
        return FALSE;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) {
        WSACleanup();
        return FALSE;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MS_IPC_PORT);
    addr.sin_addr.s_addr = inet_addr(MS_IPC_HOST);

    if (connect(g_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
        return FALSE;
    }

    {
        BOOL nodelay = TRUE;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    }

    return TRUE;
}

void ms_ipc_disconnect(void) {
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

NTSTATUS ms_ipc_transact(UINT16 operation, const void* request_body, UINT32 request_size, void* response_body,
                         UINT32 response_size) {
    MS_IPC_HEADER req_hdr;
    MS_IPC_HEADER resp_hdr;
    UINT8* resp_json = NULL;

    if (g_sock == INVALID_SOCKET) {
        return STATUS_NOT_IMPLEMENTED;
    }

    EnterCriticalSection(&g_ctx.lock);

    req_hdr.magic = MS_IPC_MAGIC;
    req_hdr.version = MS_IPC_VERSION;
    req_hdr.operation = operation;
    req_hdr.request_id = get_next_request_id();
    req_hdr.body_size = request_size;

    if (!send_all(g_sock, &req_hdr, sizeof(req_hdr))) {
        LeaveCriticalSection(&g_ctx.lock);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (request_size > 0 && request_body) {
        if (!send_all(g_sock, request_body, (int)request_size)) {
            LeaveCriticalSection(&g_ctx.lock);
            return STATUS_NOT_IMPLEMENTED;
        }
    }

    if (!recv_all(g_sock, &resp_hdr, sizeof(resp_hdr))) {
        LeaveCriticalSection(&g_ctx.lock);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (resp_hdr.magic != MS_IPC_MAGIC || resp_hdr.body_size == 0) {
        LeaveCriticalSection(&g_ctx.lock);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (resp_hdr.body_size > 65536) {
        LeaveCriticalSection(&g_ctx.lock);
        return STATUS_NOT_IMPLEMENTED;
    }

    resp_json = (UINT8*)HeapAlloc(GetProcessHeap(), 0, resp_hdr.body_size);
    if (!resp_json) {
        LeaveCriticalSection(&g_ctx.lock);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!recv_all(g_sock, resp_json, (int)resp_hdr.body_size)) {
        HeapFree(GetProcessHeap(), 0, resp_json);
        LeaveCriticalSection(&g_ctx.lock);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (response_body && response_size > 0) {
        UINT32 copy_len = response_size < resp_hdr.body_size ? response_size : resp_hdr.body_size;
        memcpy(response_body, resp_json, copy_len);
    }

    HeapFree(GetProcessHeap(), 0, resp_json);
    LeaveCriticalSection(&g_ctx.lock);

    return STATUS_SUCCESS;
}

struct iat_hook_entry {
    const char* name;
    void* hook_fn;
};

static struct iat_hook_entry g_iat_hooks[] = {
    {"NtOpenProcess", NULL},
    {"NtOpenThread", NULL},
    {"NtQuerySystemInformation", NULL},
    {"NtQueryInformationProcess", NULL},
    {"NtClose", NULL},
    {"NtDeviceIoControlFile", NULL},
    {"NtQueryVirtualMemory", NULL},
    {"NtQueryObject", NULL},
    {"NtSetInformationThread", NULL},
    {NULL, NULL},
};

#define PE_SIG      0x4550
#define MAX_MODULES 512

static int str_eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void patch_iat_for_module(HMODULE mod) {
    UINT8* base = (UINT8*)mod;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    IMAGE_IMPORT_DESCRIPTOR* imports;
    UINT32 import_size = 0;
    UINT8* import_end;

    if (base == NULL)
        return;

    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != 0x5A4D)
        return;

    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != PE_SIG)
        return;

    if (nt->OptionalHeader.DataDirectory[1].VirtualAddress == 0)
        return;

    imports = (IMAGE_IMPORT_DESCRIPTOR*)(base + nt->OptionalHeader.DataDirectory[1].VirtualAddress);
    import_size = nt->OptionalHeader.DataDirectory[1].Size;
    import_end = (UINT8*)imports + import_size;

    for (; (UINT8*)imports < import_end && imports->Name != 0; imports++) {
        char* dll_name = (char*)(base + imports->Name);
        if (!str_eq_ci(dll_name, "ntdll.dll"))
            continue;

        IMAGE_THUNK_DATA64* name_thunk = (IMAGE_THUNK_DATA64*)(base + imports->OriginalFirstThunk);
        IMAGE_THUNK_DATA64* iat_thunk = (IMAGE_THUNK_DATA64*)(base + imports->FirstThunk);

        if (imports->OriginalFirstThunk == 0)
            break;

        for (; name_thunk->u1.AddressOfData != 0; name_thunk++, iat_thunk++) {
            IMAGE_IMPORT_BY_NAME* hint;
            const char* func_name;
            DWORD old_protect;
            int i;

            if (name_thunk->u1.AddressOfData & (ULONG_PTR)0x8000000000000000ULL)
                continue;

            hint = (IMAGE_IMPORT_BY_NAME*)(base + (UINT32)name_thunk->u1.AddressOfData);
            func_name = (const char*)hint->Name;

            for (i = 0; g_iat_hooks[i].name != NULL; i++) {
                if (strcmp(func_name, g_iat_hooks[i].name) == 0) {
                    if (VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old_protect)) {
                        iat_thunk->u1.Function = (ULONG_PTR)g_iat_hooks[i].hook_fn;
                        VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), old_protect, &old_protect);
                    }
                    break;
                }
            }
        }
    }
}

typedef NTSTATUS(WINAPI* LdrRegisterDllNotification_t)(ULONG, PVOID, PVOID*);
typedef NTSTATUS(WINAPI* LdrUnregisterDllNotification_t)(PVOID);

static PVOID g_notification_cookie = NULL;

static void CALLBACK dll_notification_callback(ULONG reason, const void* data, void* ctx) {
    (void)data;
    (void)ctx;
    if (reason == 1) {
        struct {
            ULONG reserved;
            PUNICODE_STRING full_dll_name;
            PUNICODE_STRING base_dll_name;
            PVOID dll_base;
            ULONG size_of_image;
        }* info = (void*)data;
        if (info && info->dll_base) {
            patch_iat_for_module((HMODULE)info->dll_base);
        }
    }
}

static void patch_all_loaded_modules(void) {
    HANDLE snap;
    MODULEENTRY32 me;
    UINT count = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (count >= MAX_MODULES)
                break;
            patch_iat_for_module(me.hModule);
            count++;
        } while (Module32Next(snap, &me));
    }

    CloseHandle(snap);
}

static void install_iat_hooks(void) {
    g_iat_hooks[0].hook_fn = (void*)ms_hook_nt_open_process;
    g_iat_hooks[1].hook_fn = (void*)ms_hook_nt_open_thread;
    g_iat_hooks[2].hook_fn = (void*)ms_hook_nt_query_system_information;
    g_iat_hooks[3].hook_fn = (void*)ms_hook_nt_query_information_process;
    g_iat_hooks[4].hook_fn = (void*)ms_hook_nt_close;
    g_iat_hooks[5].hook_fn = (void*)ms_hook_nt_device_io_control_file;
    g_iat_hooks[6].hook_fn = (void*)ms_hook_nt_query_virtual_memory;
    g_iat_hooks[7].hook_fn = (void*)ms_hook_nt_query_object;
    g_iat_hooks[8].hook_fn = (void*)ms_hook_nt_set_information_thread;

    patch_all_loaded_modules();

    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            LdrRegisterDllNotification_t reg =
                (LdrRegisterDllNotification_t)GetProcAddress(ntdll, "LdrRegisterDllNotification");
            if (reg) {
                reg(0, (PVOID)dll_notification_callback, &g_notification_cookie);
            }
        }
    }
}

NTSTATUS ms_hook_nt_open_process(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                 PCLIENT_ID ClientId) {
    if (!ClientId || !ProcessHandle) {
        if (real_NtOpenProcess)
            return real_NtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
        return STATUS_INVALID_PARAMETER;
    }

    ULONG pid = (ULONG)(ULONG_PTR)ClientId->UniqueProcess;

    if (g_ctx.ipc_connected) {
        MS_IPC_NT_OPEN_PROCESS_REQ req = {
            .desired_access = DesiredAccess,
            .inherit_handle = ObjectAttributes ? ((ObjectAttributes->Attributes & 2) ? 1 : 0) : 0,
            .process_id = pid,
        };
        MS_IPC_NT_OPEN_PROCESS_RESP resp = {0};

        NTSTATUS status = ms_ipc_transact(MS_OP_NT_OPEN_PROCESS, &req, sizeof(req), &resp, sizeof(resp));

        if (status == STATUS_SUCCESS && resp.nt_status == STATUS_SUCCESS) {
            *ProcessHandle = (HANDLE)(ULONG_PTR)resp.handle;
            return STATUS_SUCCESS;
        }
    }

    if (real_NtOpenProcess)
        return real_NtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);

    return STATUS_ACCESS_DENIED;
}

NTSTATUS ms_hook_nt_open_thread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                PCLIENT_ID ClientId) {
    if (!ClientId || !ThreadHandle) {
        if (real_NtOpenThread)
            return real_NtOpenThread(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId);
        return STATUS_INVALID_PARAMETER;
    }

    if (g_ctx.ipc_connected) {
        MS_IPC_NT_OPEN_THREAD_REQ req = {
            .desired_access = DesiredAccess,
            .inherit_handle = ObjectAttributes ? ((ObjectAttributes->Attributes & 2) ? 1 : 0) : 0,
            .thread_id = (ULONG)(ULONG_PTR)ClientId->UniqueThread,
        };
        MS_IPC_NT_OPEN_THREAD_RESP resp = {0};

        NTSTATUS status = ms_ipc_transact(MS_OP_NT_OPEN_THREAD, &req, sizeof(req), &resp, sizeof(resp));

        if (status == STATUS_SUCCESS && resp.nt_status == STATUS_SUCCESS) {
            *ThreadHandle = (HANDLE)(ULONG_PTR)resp.handle;
            return STATUS_SUCCESS;
        }
    }

    if (real_NtOpenThread)
        return real_NtOpenThread(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId);

    return STATUS_ACCESS_DENIED;
}

NTSTATUS ms_hook_nt_query_system_information(ULONG SystemInformationClass, PVOID SystemInformation,
                                             ULONG SystemInformationLength, PULONG ReturnLength) {
    if (g_ctx.ipc_connected) {
        MS_IPC_NT_QUERY_SYSTEM_INFO_REQ req = {
            .info_class = SystemInformationClass,
            .buffer_length = SystemInformationLength,
        };

        NTSTATUS status = ipc_call(MS_OP_NT_QUERY_SYSTEM_INFO, &req, sizeof(req), SystemInformation,
                                   SystemInformationLength, ReturnLength);
        if (status == STATUS_SUCCESS)
            return STATUS_SUCCESS;
    }

    if (real_NtQuerySystemInformation)
        return real_NtQuerySystemInformation(SystemInformationClass, SystemInformation, SystemInformationLength,
                                             ReturnLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ms_hook_nt_query_information_process(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                                              PVOID ProcessInformation, ULONG ProcessInformationLength,
                                              PULONG ReturnLength) {
    if (g_ctx.ipc_connected) {
        MS_IPC_NT_QUERY_INFORMATION_PROC_REQ req = {
            .handle = (UINT64)(ULONG_PTR)ProcessHandle,
            .info_class = ProcessInformationClass,
            .buffer_length = ProcessInformationLength,
        };

        NTSTATUS status = ipc_call(MS_OP_NT_QUERY_INFORMATION_PROC, &req, sizeof(req), ProcessInformation,
                                   ProcessInformationLength, ReturnLength);
        if (status == STATUS_SUCCESS)
            return STATUS_SUCCESS;
    }

    if (real_NtQueryInformationProcess)
        return real_NtQueryInformationProcess(ProcessHandle, ProcessInformationClass, ProcessInformation,
                                              ProcessInformationLength, ReturnLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ms_hook_nt_close(HANDLE Handle) {
    UINT64 handle_val = (UINT64)(ULONG_PTR)Handle;
    if (handle_val < 0x100 || (handle_val & 0x3) != 0) {
        if (real_NtClose)
            return real_NtClose(Handle);
        return STATUS_INVALID_HANDLE;
    }

    if (g_ctx.ipc_connected) {
        MS_IPC_NT_CLOSE_REQ req = {.handle = handle_val};
        MS_IPC_NT_CLOSE_RESP resp = {0};

        NTSTATUS status = ms_ipc_transact(MS_OP_NT_CLOSE, &req, sizeof(req), &resp, sizeof(resp));

        if (status == STATUS_SUCCESS && resp.nt_status == STATUS_SUCCESS) {
            return STATUS_SUCCESS;
        }
    }

    if (real_NtClose)
        return real_NtClose(Handle);

    return STATUS_INVALID_HANDLE;
}

NTSTATUS ms_hook_nt_device_io_control_file(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                           PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer,
                                           ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength) {
    if (g_ctx.ipc_connected) {
        MS_IPC_NT_DEVICE_IO_CONTROL_REQ req = {
            .handle = (UINT64)(ULONG_PTR)FileHandle,
            .io_control_code = IoControlCode,
            .input_buffer_length = InputBufferLength,
            .output_buffer_length = OutputBufferLength,
        };

        /* Reject sizes that would overflow the u32 arithmetic or exceed a
         * sane cap: a hostile/huge length must not wrap total_req/total_resp
         * into an undersized HeapAlloc followed by an oversized memcpy. */
        if (InputBufferLength > (UINT32_MAX - sizeof(MS_IPC_NT_DEVICE_IO_CONTROL_REQ)) ||
            OutputBufferLength > (UINT32_MAX - sizeof(MS_IPC_NT_DEVICE_IO_CONTROL_RESP)) ||
            InputBufferLength > 16 * 1024 * 1024 || OutputBufferLength > 16 * 1024 * 1024)
            return real_NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode,
                                              InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength);

        UINT32 total_req = sizeof(MS_IPC_NT_DEVICE_IO_CONTROL_REQ) + InputBufferLength;
        UINT8* req_buf = (UINT8*)HeapAlloc(GetProcessHeap(), 0, total_req);
        if (req_buf) {
            memcpy(req_buf, &req, sizeof(req));
            if (InputBuffer && InputBufferLength > 0)
                memcpy(req_buf + sizeof(req), InputBuffer, InputBufferLength);

            UINT32 total_resp = sizeof(MS_IPC_NT_DEVICE_IO_CONTROL_RESP) + OutputBufferLength;
            UINT8* resp_buf = (UINT8*)HeapAlloc(GetProcessHeap(), 0, total_resp);
            if (resp_buf) {
                NTSTATUS status = ms_ipc_transact(MS_OP_NT_DEVICE_IO_CONTROL, req_buf, total_req, resp_buf, total_resp);

                if (status == STATUS_SUCCESS) {
                    MS_IPC_NT_DEVICE_IO_CONTROL_RESP* dev_resp = (MS_IPC_NT_DEVICE_IO_CONTROL_RESP*)resp_buf;
                    if (dev_resp->nt_status == STATUS_SUCCESS) {
                        if (IoStatusBlock) {
                            IoStatusBlock->Status = STATUS_SUCCESS;
                            IoStatusBlock->Information = (ULONG_PTR)dev_resp->bytes_returned;
                        }
                        if (OutputBuffer && dev_resp->output_buffer_length > 0) {
                            /* The response length is not trusted: never copy
                             * more than the caller's buffer can hold. */
                            UINT32 out_len = dev_resp->output_buffer_length;
                            if (out_len > OutputBufferLength)
                                out_len = OutputBufferLength;
                            memcpy(OutputBuffer, resp_buf + sizeof(MS_IPC_NT_DEVICE_IO_CONTROL_RESP), out_len);
                        }
                        HeapFree(GetProcessHeap(), 0, resp_buf);
                        HeapFree(GetProcessHeap(), 0, req_buf);
                        return STATUS_SUCCESS;
                    }
                }
                HeapFree(GetProcessHeap(), 0, resp_buf);
            }
            HeapFree(GetProcessHeap(), 0, req_buf);
        }
    }

    if (real_NtDeviceIoControlFile)
        return real_NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode,
                                          InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ms_hook_nt_query_virtual_memory(HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass,
                                         PVOID MemoryInformation, ULONG MemoryInformationLength, PULONG ReturnLength) {
    if (g_ctx.ipc_connected) {
        MS_IPC_NT_QUERY_VIRTUAL_MEMORY_REQ req = {
            .process_handle = (UINT64)(ULONG_PTR)ProcessHandle,
            .base_address = (UINT64)(ULONG_PTR)BaseAddress,
            .info_class = MemoryInformationClass,
            .buffer_length = MemoryInformationLength,
        };

        NTSTATUS status = ipc_call(MS_OP_NT_QUERY_VIRTUAL_MEMORY, &req, sizeof(req), MemoryInformation,
                                   MemoryInformationLength, ReturnLength);
        if (status == STATUS_SUCCESS)
            return STATUS_SUCCESS;
    }

    if (real_NtQueryVirtualMemory)
        return real_NtQueryVirtualMemory(ProcessHandle, BaseAddress, MemoryInformationClass, MemoryInformation,
                                         MemoryInformationLength, ReturnLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ms_hook_nt_query_object(HANDLE Handle, ULONG ObjectInformationClass, PVOID ObjectInformation,
                                 ULONG ObjectInformationLength, PULONG ReturnLength) {
    if (g_ctx.ipc_connected) {
        MS_IPC_NT_QUERY_OBJECT_REQ req = {
            .handle = (UINT64)(ULONG_PTR)Handle,
            .info_class = ObjectInformationClass,
            .buffer_length = ObjectInformationLength,
        };

        UINT8 resp_buf[4096];
        NTSTATUS ipc_status = ms_ipc_transact(MS_OP_NT_QUERY_OBJECT, &req, sizeof(req), resp_buf, sizeof(resp_buf));
        if (ipc_status == STATUS_SUCCESS) {
            INT32 nt_status;
            UINT32 ipc_return_len;
            memcpy(&nt_status, resp_buf, 4);
            memcpy(&ipc_return_len, resp_buf + 4, 4);
            if (nt_status == STATUS_SUCCESS) {
                if (ReturnLength)
                    *ReturnLength = ipc_return_len;
                if (ObjectInformation && ObjectInformationLength > 0) {
                    if (ObjectInformationClass == 0x01 && ObjectInformationLength >= 24) {
                        UNICODE_STRING* us = (UNICODE_STRING*)ObjectInformation;
                        UINT8* str_src = resp_buf + 8;
                        UINT32 str_bytes = ipc_return_len;
                        if (str_bytes > ObjectInformationLength - 16)
                            str_bytes = ObjectInformationLength - 16;
                        us->Length = (USHORT)str_bytes;
                        us->MaximumLength = (USHORT)(str_bytes + 2);
                        us->Buffer = (PWCH)((UINT8*)ObjectInformation + 16);
                        memcpy(us->Buffer, str_src, str_bytes);
                        if (str_bytes + 2 <= ObjectInformationLength - 16)
                            ((UINT8*)us->Buffer)[str_bytes] = 0;
                    } else {
                        UINT32 copy_len = ipc_return_len;
                        if (copy_len > ObjectInformationLength)
                            copy_len = ObjectInformationLength;
                        if (copy_len > sizeof(resp_buf) - 8)
                            copy_len = (UINT32)sizeof(resp_buf) - 8;
                        memcpy(ObjectInformation, resp_buf + 8, copy_len);
                    }
                }
                return STATUS_SUCCESS;
            }
        }
    }

    if (real_NtQueryObject)
        return real_NtQueryObject(Handle, ObjectInformationClass, ObjectInformation, ObjectInformationLength,
                                  ReturnLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ms_hook_nt_set_information_thread(HANDLE ThreadHandle, ULONG ThreadInformationClass, PVOID ThreadInformation,
                                           ULONG ThreadInformationLength) {
    if (g_ctx.ipc_connected) {
        MS_IPC_NT_SET_INFORMATION_THREAD_REQ req = {
            .thread_handle = (UINT64)(ULONG_PTR)ThreadHandle,
            .info_class = ThreadInformationClass,
            .buffer_length = ThreadInformationLength,
        };
        MS_IPC_NT_SET_INFORMATION_THREAD_RESP resp = {0};

        NTSTATUS status = ms_ipc_transact(MS_OP_NT_SET_INFORMATION_THREAD, &req, sizeof(req), &resp, sizeof(resp));

        if (status == STATUS_SUCCESS && resp.nt_status == STATUS_SUCCESS) {
            return STATUS_SUCCESS;
        }
    }

    if (real_NtSetInformationThread)
        return real_NtSetInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation,
                                           ThreadInformationLength);

    return STATUS_NOT_IMPLEMENTED;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    (void)lpvReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        InitializeCriticalSection(&g_ctx.lock);
        load_real_functions();
        metalfx_init();
        g_ctx.ipc_connected = ms_ipc_connect() ? 1 : 0;
        if (!g_ctx.ipc_connected) {
            OutputDebugStringA("[metalsharp_ntdll_hook] IPC connect failed, falling back to real ntdll");
        } else {
            OutputDebugStringA("[metalsharp_ntdll_hook] IPC connected to MetalSharp backend");
            install_iat_hooks();
        }
        break;
    case DLL_PROCESS_DETACH:
        /* Stop the MetalFX poller before tearing down IPC: it may be inside
         * CreateFileW/GetEnvironmentVariableW while ntdll unloads. Signal the
         * flag and let it observe it on its next iteration (500ms); we cannot
         * WaitForSingleObject from DllMain (loader lock deadlock risk). */
        g_metalfx_stop = 1;
        ms_ipc_disconnect();
        DeleteCriticalSection(&g_ctx.lock);
        break;
    }
    return TRUE;
}

#endif
