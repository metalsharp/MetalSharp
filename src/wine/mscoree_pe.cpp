/// @file mscoree_pe.cpp
/// @brief Wine mscoree PE module for .NET CLR hosting shim.
///
/// Implements CorBindToRuntimeEx and CLRCreateInstance to satisfy .NET executable loading. Routes actual CLR
/// initialization to the unixlib backend which loads the system Mono runtime for running managed game assemblies.
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef long NTSTATUS;
typedef unsigned long long unixlib_handle_t;
typedef NTSTATUS(WINAPI* unix_call_dispatcher_t)(unixlib_handle_t, unsigned int, void*);

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif

#include "mscoree_path.h"
#include "mscoree_unix.h"

static unixlib_handle_t g_unixlib_handle = 0;
static unix_call_dispatcher_t g_unix_call_dispatcher = NULL;
static HMODULE g_hModule = NULL;
static int g_unix_initialized = 0;

static NTSTATUS unix_call(unsigned int code, void* args) {
    if (!g_unix_call_dispatcher || !g_unixlib_handle)
        return (NTSTATUS)0xC000000E;
    return g_unix_call_dispatcher(g_unixlib_handle, code, args);
}

static int init_unix_call(void) {
    if (g_unix_initialized)
        return 1;
    fprintf(stderr, "[mscoree] init_unix_call starting...\n");

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        fprintf(stderr, "[mscoree] no ntdll\n");
        return 0;
    }

    typedef NTSTATUS(NTAPI * pNtQVM)(HANDLE, const void*, ULONG, void*, SIZE_T, SIZE_T*);
    pNtQVM pNtQueryVirtualMemory = (pNtQVM)GetProcAddress(ntdll, "NtQueryVirtualMemory");
    if (!pNtQueryVirtualMemory) {
        fprintf(stderr, "[mscoree] no NtQueryVirtualMemory\n");
        return 0;
    }

    unixlib_handle_t handle = 0;
    NTSTATUS status =
        pNtQueryVirtualMemory((HANDLE)(LONG_PTR)-1, (const void*)g_hModule, 1000, &handle, sizeof(handle), NULL);
    fprintf(stderr, "[mscoree] NtQueryVirtualMemory(1000) status=%ld handle=%llu\n", status,
            (unsigned long long)handle);
    if (status < 0)
        return 0;
    g_unixlib_handle = handle;

    void** ptr = (void**)GetProcAddress(ntdll, "__wine_unix_call_dispatcher");
    fprintf(stderr, "[mscoree] __wine_unix_call_dispatcher ptr=%p val=%p\n", ptr, ptr ? *ptr : NULL);
    if (ptr && *ptr)
        g_unix_call_dispatcher = (unix_call_dispatcher_t)*ptr;
    else
        g_unix_call_dispatcher = (unix_call_dispatcher_t)GetProcAddress(ntdll, "__wine_unix_call_dispatcher");

    if (!g_unix_call_dispatcher) {
        fprintf(stderr, "[mscoree] no unix_call_dispatcher\n");
        return 0;
    }
    fprintf(stderr, "[mscoree] unix bridge initialized: dispatcher=%p handle=%llu\n", (void*)g_unix_call_dispatcher,
            (unsigned long long)handle);
    g_unix_initialized = 1;
    return 1;
}

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoAssembly MonoAssembly;
typedef struct _MonoImage MonoImage;
typedef int MonoImageOpenStatus;

static HMODULE g_mono_handle = NULL;

static MonoDomain* (*p_mono_jit_init_version)(const char*, const char*);
static int (*p_mono_jit_exec)(MonoDomain*, MonoAssembly*, int, char**);
static MonoAssembly* (*p_mono_assembly_open)(const char*, MonoImageOpenStatus*);
static MonoImage* (*p_mono_assembly_get_image)(MonoAssembly*);
static MonoAssembly* (*p_mono_assembly_load_from)(MonoImage*, const char*, MonoImageOpenStatus*);
static MonoImage* (*p_mono_image_open)(const char*, MonoImageOpenStatus*);
static void (*p_mono_set_dirs)(const char*, const char*);
static void (*p_mono_config_parse)(const char*);
static void (*p_mono_thread_manage)(void);
static const char* (*p_mono_get_version)(void);
static void (*p_mono_set_assemblies_path)(const char*);
static void (*p_mono_runtime_quit)(void);
static void (*p_mono_domain_set_config)(MonoDomain*, const char*, const char*);
static void (*p_mono_thread_attach)(MonoDomain*);
static void (*p_mono_thread_detach)(void);

#define LOAD_MONO_FUNC(name)                                                                                           \
    p_##name = (decltype(p_##name))GetProcAddress(g_mono_handle, #name);                                               \
    if (!p_##name) {                                                                                                   \
        fprintf(stderr, "[mscoree] FAILED to resolve " #name "\n");                                                    \
    }

static int load_wine_mono(void) {
    if (g_mono_handle)
        return 1;

    char mono_dll_path[MAX_PATH];
    GetWindowsDirectoryA(mono_dll_path, MAX_PATH);
    strcat(mono_dll_path, "\\mono\\mono-2.0\\bin\\libmono-2.0-x86.dll");

    fprintf(stderr, "[mscoree] Loading wine-mono: %s\n", mono_dll_path);
    g_mono_handle = LoadLibraryA(mono_dll_path);
    if (!g_mono_handle) {
        fprintf(stderr, "[mscoree] FAILED to load wine-mono: %lu\n", GetLastError());
        return 0;
    }
    fprintf(stderr, "[mscoree] wine-mono loaded at %p\n", (void*)g_mono_handle);

    LOAD_MONO_FUNC(mono_get_version);
    LOAD_MONO_FUNC(mono_set_assemblies_path);

    const char* ver = p_mono_get_version ? p_mono_get_version() : "unknown";
    fprintf(stderr, "[mscoree] mono version: %s\n", ver);

    LOAD_MONO_FUNC(mono_jit_init_version);
    LOAD_MONO_FUNC(mono_jit_exec);
    LOAD_MONO_FUNC(mono_assembly_open);
    LOAD_MONO_FUNC(mono_assembly_get_image);
    LOAD_MONO_FUNC(mono_assembly_load_from);
    LOAD_MONO_FUNC(mono_image_open);
    LOAD_MONO_FUNC(mono_set_dirs);
    LOAD_MONO_FUNC(mono_config_parse);
    LOAD_MONO_FUNC(mono_thread_manage);
    LOAD_MONO_FUNC(mono_runtime_quit);
    LOAD_MONO_FUNC(mono_domain_set_config);
    LOAD_MONO_FUNC(mono_thread_attach);
    LOAD_MONO_FUNC(mono_thread_detach);

    fprintf(stderr, "[mscoree] All mono functions resolved\n");

    if (!p_mono_jit_init_version || !p_mono_set_dirs || !p_mono_config_parse) {
        fprintf(stderr, "[mscoree] Missing critical: jit_init=%p set_dirs=%p config_parse=%p\n",
                (void*)p_mono_jit_init_version, (void*)p_mono_set_dirs, (void*)p_mono_config_parse);
        return 0;
    }
    return 1;
}

#undef LOAD_MONO_FUNC

extern "C" {

static int launch_embedded_mono(void) {
    if (!load_wine_mono()) {
        fprintf(stderr, "[mscoree] Cannot load mono runtime\n");
        return -1;
    }

    char exe_path[1024] = {0};
    char exe_dir[1024] = {0};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    mscoree_terminate_path(exe_path, sizeof(exe_path));
    mscoree_copy_path(exe_dir, sizeof(exe_dir), exe_path);
    char* last_slash = strrchr(exe_dir, '\\');
    if (last_slash)
        *last_slash = 0;

    char unix_exe[1024];
    char unix_dir[1024];
    const char* real_path = exe_path;
    if (real_path[0] && real_path[1] == ':')
        real_path += 2;
    const char* real_dir = exe_dir;
    if (real_dir[0] && real_dir[1] == ':')
        real_dir += 2;

    const char* src = real_path;
    char* dst = unix_exe;
    while (*src && dst < unix_exe + sizeof(unix_exe) - 1) {
        *dst++ = (*src == '\\') ? '/' : *src;
        src++;
    }
    *dst = 0;
    src = real_dir;
    dst = unix_dir;
    while (*src && dst < unix_dir + sizeof(unix_dir) - 1) {
        *dst++ = (*src == '\\') ? '/' : *src;
        src++;
    }
    *dst = 0;

    fprintf(stderr, "[mscoree] _CorExeMain: exe=%s dir=%s\n", unix_exe, unix_dir);
    fprintf(stderr, "[mscoree] mono version: %s\n", p_mono_get_version ? p_mono_get_version() : "null");

    fprintf(stderr, "[mscoree] [1] about to try mono_jit_init...\n");
    SetEnvironmentVariableA("MONO_ENV_OPTIONS", "--interpreter");
    SetEnvironmentVariableA("MONO_GC_PARAMS", "major=marksweep");
    fprintf(stderr, "[mscoree] [1b] set MONO_ENV_OPTIONS=--interpreter\n");
    typedef MonoDomain* (*mono_jit_init_t)(const char*);
    fprintf(stderr, "[mscoree] [2] typedef done\n");
    mono_jit_init_t p_jit_init = (mono_jit_init_t)GetProcAddress(g_mono_handle, "mono_jit_init");
    fprintf(stderr, "[mscoree] [3] GetProcAddress returned %p\n", (void*)p_jit_init);
    MonoDomain* domain = NULL;
    if (p_jit_init) {
        fprintf(stderr, "[mscoree] [4] calling mono_jit_init...\n");
        domain = p_jit_init("metalsharp");
        fprintf(stderr, "[mscoree] [5] mono_jit_init returned %p\n", (void*)domain);
    }
    if (!domain) {
        fprintf(stderr, "[mscoree] mono_jit_init_version failed\n");
        return -1;
    }
    fprintf(stderr, "[mscoree] mono runtime initialized\n");

    char config_file[1024];
    if (!mscoree_build_config_path(config_file, sizeof(config_file), exe_path)) {
        fprintf(stderr, "[mscoree] executable path is too long for its config path\n");
        return -1;
    }
    fprintf(stderr, "[mscoree] mono_domain_set_config...\n");
    p_mono_domain_set_config(domain, unix_dir, config_file);

    fprintf(stderr, "[mscoree] mono_image_open: %s\n", unix_exe);
    MonoImageOpenStatus status;
    MonoImage* image = p_mono_image_open(unix_exe, &status);
    if (!image) {
        fprintf(stderr, "[mscoree] mono_image_open failed: %d\n", status);
        return -1;
    }

    MonoAssembly* assembly = p_mono_assembly_load_from(image, unix_exe, &status);
    if (!assembly) {
        fprintf(stderr, "[mscoree] mono_assembly_load_from failed: %d\n", status);
        return -1;
    }
    fprintf(stderr, "[mscoree] assembly loaded, calling mono_jit_exec\n");

    LPWSTR cmdLineW = GetCommandLineW();
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(cmdLineW, &argc);

    char** argv = (char**)malloc(argc * sizeof(char*));
    for (int i = 0; i < argc; i++) {
        argv[i] = (char*)malloc(512);
        WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, argv[i], 512, NULL, NULL);
    }
    LocalFree(argvW);

    int exit_code = p_mono_jit_exec(domain, assembly, argc, argv);
    fprintf(stderr, "[mscoree] mono_jit_exec returned %d\n", exit_code);

    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);

    p_mono_thread_manage();
    p_mono_runtime_quit();

    ExitProcess(exit_code);
    return exit_code;
}

void WINAPI _CorExeMain(void) {
    fprintf(stderr, "[mscoree] _CorExeMain called\n");
    if (!init_unix_call()) {
        fprintf(stderr, "[mscoree] unix bridge init failed\n");
        ExitProcess(1);
    }

    char exe_path[1024] = {0};
    char exe_dir[1024] = {0};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    mscoree_terminate_path(exe_path, sizeof(exe_path));
    mscoree_copy_path(exe_dir, sizeof(exe_dir), exe_path);
    char* last_slash = strrchr(exe_dir, '\\');
    if (last_slash)
        *last_slash = 0;

    const char* real_path = exe_path;
    if (real_path[0] && real_path[1] == ':')
        real_path += 2;
    const char* real_dir = exe_dir;
    if (real_dir[0] && real_dir[1] == ':')
        real_dir += 2;

    char unix_exe[1024];
    char unix_dir[1024];
    const char* src = real_path;
    char* dst = unix_exe;
    while (*src && dst < unix_exe + sizeof(unix_exe) - 1) {
        *dst++ = (*src == '\\') ? '/' : *src;
        src++;
    }
    *dst = 0;
    src = real_dir;
    dst = unix_dir;
    while (*src && dst < unix_dir + sizeof(unix_dir) - 1) {
        *dst++ = (*src == '\\') ? '/' : *src;
        src++;
    }
    *dst = 0;

    struct mscoree_cor_exe_main_params params;
    memset(&params, 0, sizeof(params));
    strncpy(params.exe_path, unix_exe, sizeof(params.exe_path) - 1);
    strncpy(params.exe_dir, unix_dir, sizeof(params.exe_dir) - 1);
    params.argc = 0;
    params.exit_code = 0;

    fprintf(stderr, "[mscoree] calling unix bridge INIT...\n");
    NTSTATUS init_status = unix_call(MSCOREE_FUNC_INIT, NULL);
    fprintf(stderr, "[mscoree] INIT returned %ld\n", (long)init_status);

    fprintf(stderr, "[mscoree] calling unix bridge _CorExeMain (no args)...\n");
    NTSTATUS cor_status = unix_call(MSCOREE_FUNC_COR_EXE_MAIN, NULL);
    fprintf(stderr, "[mscoree] _CorExeMain returned %ld\n", (long)cor_status);

    ExitProcess(params.exit_code);
}

__int32 WINAPI _CorExeMain2(PBYTE, DWORD, LPWSTR, LPWSTR, LPWSTR) {
    launch_embedded_mono();
    return -1;
}

BOOL WINAPI _CorDllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
void WINAPI CorExitProcess(int exitCode) {
    ExitProcess(exitCode);
}
VOID WINAPI _CorImageUnloading(PVOID) {}
HRESULT WINAPI _CorValidateImage(PVOID*, LPCWSTR) {
    return E_FAIL;
}

HRESULT WINAPI CorBindToRuntimeEx(LPWSTR, LPWSTR, DWORD, REFCLSID, REFIID, LPVOID* ppv) {
    *ppv = NULL;
    return E_FAIL;
}
HRESULT WINAPI CorBindToRuntime(LPWSTR, LPWSTR, DWORD, REFCLSID, REFIID, LPVOID* ppv) {
    *ppv = NULL;
    return E_FAIL;
}
HRESULT WINAPI CorBindToRuntimeHost(LPCWSTR, LPCWSTR, LPCWSTR, VOID*, DWORD, REFCLSID, REFIID, LPVOID* ppv) {
    *ppv = NULL;
    return E_FAIL;
}
HRESULT WINAPI CorBindToCurrentRuntime(LPCWSTR, REFCLSID, REFIID, LPVOID* ppv) {
    *ppv = NULL;
    return E_FAIL;
}
HRESULT WINAPI CLRCreateInstance(REFCLSID, REFIID, LPVOID*) {
    return CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI CreateInterface(REFCLSID, REFIID, LPVOID*) {
    return CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID*) {
    return CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI DllCanUnloadNow() {
    return S_FALSE;
}
HRESULT WINAPI DllRegisterServer() {
    return S_OK;
}
HRESULT WINAPI DllUnregisterServer() {
    return S_OK;
}

HRESULT WINAPI GetCORVersion(LPWSTR pBuffer, DWORD cchBuffer, DWORD* dwLength) {
    const char* ver = "v4.0.30319";
    if (dwLength)
        *dwLength = 10;
    if (pBuffer && cchBuffer > 10)
        MultiByteToWideChar(CP_ACP, 0, ver, -1, pBuffer, cchBuffer);
    return S_OK;
}

HRESULT WINAPI GetCORSystemDirectory(LPWSTR pBuffer, DWORD cchBuffer, DWORD* dwLength) {
    const char* dir = "C:\\windows\\Microsoft.NET\\Framework\\v4.0.30319";
    if (dwLength)
        *dwLength = (DWORD)strlen(dir);
    if (pBuffer && cchBuffer > (DWORD)strlen(dir))
        MultiByteToWideChar(CP_ACP, 0, dir, -1, pBuffer, cchBuffer);
    return S_OK;
}

HRESULT WINAPI GetFileVersion(LPCWSTR, LPWSTR, DWORD, DWORD* dwLength) {
    if (dwLength)
        *dwLength = 0;
    return S_OK;
}
HRESULT WINAPI CoInitializeCor(DWORD) {
    return S_OK;
}
void WINAPI CoUninitializeCor(DWORD) {}
HRESULT WINAPI CoInitializeEE(DWORD) {
    return E_FAIL;
}
void WINAPI CoUninitializeEE(BOOL) {}
void WINAPI CoEEShutDownCOM(void) {}
void WINAPI CorMarkThreadInThreadPool(void) {}
HRESULT WINAPI CorGetSvc(void) {
    return E_NOTIMPL;
}
HRESULT WINAPI CorIsLatestSvc(int* a, int* b) {
    if (a)
        *a = 1;
    if (b)
        *b = 1;
    return S_OK;
}
HRESULT WINAPI GetRealProcAddress(LPCSTR, void**) {
    return 0x80131073;
}
HRESULT WINAPI LockClrVersion(void*, void**, void**) {
    return S_OK;
}
HRESULT WINAPI ClrCreateManagedInstance(LPCWSTR, REFIID, void**) {
    return E_FAIL;
}
void WINAPI CorDllMainWorker(void*) {}
HRESULT WINAPI LoadLibraryShim(LPCWSTR, LPCWSTR, LPVOID, HMODULE* ph) {
    if (ph)
        *ph = NULL;
    return E_HANDLE;
}
HRESULT WINAPI GetRequestedRuntimeInfo(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, DWORD, LPWSTR, DWORD, DWORD*, LPWSTR, DWORD,
                                       DWORD*) {
    return E_NOTIMPL;
}
HRESULT WINAPI GetRequestedRuntimeVersion(LPWSTR, LPWSTR, DWORD, DWORD*) {
    return E_NOTIMPL;
}
void WINAPI CallFunctionShim(void*, char*, void*, void**) {}
HRESULT WINAPI CloseCtrs() {
    return S_OK;
}
HRESULT WINAPI CollectCtrs() {
    return S_OK;
}
HRESULT WINAPI CorBindToRuntimeByCfg(void*, DWORD, DWORD, REFCLSID, REFIID, LPVOID*) {
    return E_FAIL;
}
HRESULT WINAPI CorBindToRuntimeByPath(LPCWSTR, LPCWSTR, DWORD, REFCLSID, REFIID, LPVOID*) {
    return E_FAIL;
}
HRESULT WINAPI CorBindToRuntimeByPathEx(LPCWSTR, DWORD, DWORD, REFCLSID, REFIID, LPVOID*) {
    return E_FAIL;
}
HRESULT WINAPI CorTickleSvc() {
    return S_OK;
}
HRESULT WINAPI CreateConfigStream(LPCWSTR, void**) {
    return E_NOTIMPL;
}
HRESULT WINAPI CreateDebuggingInterfaceFromVersion(DWORD, LPCWSTR, void**) {
    return E_NOTIMPL;
}
HRESULT WINAPI GetCORRequiredVersion(LPWSTR, DWORD, DWORD*) {
    return S_OK;
}
HRESULT WINAPI GetCORRootDirectory(LPWSTR, DWORD, DWORD*) {
    return S_OK;
}
HRESULT WINAPI GetCompileInfo(DWORD*, DWORD*) {
    return S_OK;
}
HRESULT WINAPI MetahostGetRuntime(LPCWSTR, REFCLSID, REFIID, LPVOID*) {
    return E_FAIL;
}
HRESULT WINAPI RunDll32Shim() {
    return S_OK;
}
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = (HMODULE)hinstDLL;
        fprintf(stderr, "[mscoree] DllMain - metalsharp embedded mono shim\n");
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}
