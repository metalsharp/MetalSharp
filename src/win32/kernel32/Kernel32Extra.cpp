/// @file Kernel32Extra.cpp
/// @brief Extended kernel32.dll shims — process, module, TLS, CPU, and system API emulation.
///
/// This file implements the "long tail" of kernel32.dll functions that Windows games
/// call beyond the basics handled in Kernel32Shim.cpp. Coverage areas:
///
/// Module/Process Management
/// =========================
///   shim_GetModuleHandleA/W — Returns fake HMODULE for known DLLs, searches loaded DLLs
///   shim_GetModuleFileNameA/W — Returns "C:\Windows\System32\{dll}" for shims
///   shim_LoadLibraryA/W — Delegates to PELoader::loadLibrary()
///   shim_GetProcAddress — Delegates to PELoader::getProcAddress()
///   shim_CreateProcessA/W — Fork+exec with environment inheritance
///   shim_ExitProcess — Calls exit()
///   shim_TerminateProcess — No-op for game compatibility
///
/// Thread-Local Storage
/// ====================
///   shim_TlsAlloc/TlsGetValue/TlsSetValue/TlsFree — pthread_key_t based implementation
///   shim_FlsAlloc/FlsGetValue/FlsSetValue/FlsFree — Fiber-local storage (same as TLS)
///
/// CPU/System Information
/// ======================
///   shim_GetSystemInfo — Reports Apple Silicon as x86_64 (8 cores, 4KB pages)
///   shim_IsProcessorFeaturePresent — Returns TRUE for common features
///   shim_GetNativeSystemInfo — Same as GetSystemInfo
///   __cpuidex — Fake CPUID returning Intel-like results for compatibility
///
/// Environment & Debug
/// ===================
///   shim_GetEnvironmentVariableA/W — Case-insensitive Windows-side environment lookup
///   shim_SetEnvironmentVariableA/W — Updates the Windows-side environment map
///   shim_GetCommandLineA/W — Returns stored command line from launcher
///   shim_OutputDebugStringA — Forwards to MS_INFO log
///   shim_IsDebuggerPresent — Returns FALSE
///   shim_RaiseException — Minimal handling for game exception patterns
///
/// Memory & Time
/// =============
///   shim_GlobalMemoryStatusEx — Reports real macOS memory stats
///   shim_GetTickCount/64 — Returns mach_absolute_time-based uptime
///   shim_QueryPerformanceCounter/Frequency — mach_absolute_time based
///   shim_GetSystemTime/AsFileTime — Real clock via gettimeofday
///   shim_Sleep — usleep wrapper
///
/// Synchronization
/// ===============
///   SRW locks and condition variables — Guest-address keyed host pthread state
///
/// Status: ~70% of commonly-used kernel32 exports implemented. Games that call
/// obscure kernel32 functions will log "MISSING" in the import resolver.
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <metalsharp/Kernel32Shim.h>
#include <metalsharp/Logger.h>
#include <metalsharp/NetworkContext.h>
#include <metalsharp/PEHeader.h>
#include <metalsharp/PELoader.h>
#include <metalsharp/Registry.h>
#include <metalsharp/SyncContext.h>
#include <metalsharp/VirtualFileSystem.h>
#include <metalsharp/Win32Error.h>
#include <metalsharp/Win32SyncContext.h>
#include <metalsharp/Win32Types.h>
#include <mutex>
#include <pthread.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace metalsharp {
namespace win32 {

static std::atomic<int> g_callCount{0};
static thread_local int t_callDepth = 0;

static void* MSABI stub_zero() {
    return nullptr;
}
static BOOL MSABI stub_true() {
    return 1;
}
static DWORD MSABI stub_zero_dword() {
    return 0;
}
static void MSABI stub_void() {}

static void* MSABI shim_VirtualProtect(void* lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect) {
    (void)dwSize;
    (void)flNewProtect;
    if (lpflOldProtect)
        *lpflOldProtect = PAGE_EXECUTE_READWRITE;
    return reinterpret_cast<void*>(1);
}

static BOOL MSABI shim_VirtualQuery(void* lpAddress, void* lpBuffer, SIZE_T dwLength, SIZE_T* lpResultLength) {
    (void)lpAddress;
    (void)dwLength;
    auto* mbi = reinterpret_cast<MEMORY_BASIC_INFORMATION*>(lpBuffer);
    memset(mbi, 0, sizeof(MEMORY_BASIC_INFORMATION));
    mbi->BaseAddress = lpAddress;
    mbi->AllocationBase = lpAddress;
    mbi->RegionSize = 65536;
    mbi->State = 0x1000;
    mbi->Protect = PAGE_EXECUTE_READWRITE;
    mbi->Type = 0x20000;
    if (lpResultLength)
        *lpResultLength = sizeof(MEMORY_BASIC_INFORMATION);
    return 1;
}

static void MSABI shim_ExitProcess(UINT uExitCode) {
    MS_INFO("PELoader: ExitProcess(%u)", uExitCode);
    exit(uExitCode);
}

void* s_unhandledExceptionFilter = nullptr;
std::vector<std::pair<void*, bool>> s_vehHandlers;
std::mutex s_vehMutex;

static void MSABI shim_RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
                                      void* lpArguments) {
    (void)dwExceptionFlags;
    (void)nNumberOfArguments;
    (void)lpArguments;
    MS_INFO("PELoader: RaiseException(0x%08X) — dispatching to VEH chain", dwExceptionCode);

    struct FakeExceptionRecord {
        uint32_t ExceptionCode;
        uint32_t ExceptionFlags;
        void* ExceptionRecord;
        void* ExceptionAddress;
        uint32_t NumberParameters;
        void* ExceptionInformation[15];
    } record = {};
    record.ExceptionCode = dwExceptionCode;
    record.ExceptionFlags = dwExceptionFlags;

    {
        std::lock_guard<std::mutex> lock(s_vehMutex);
        for (auto& [handler, isFirst] : s_vehHandlers) {
            if (handler) {
                struct FakePointers {
                    void* ExceptionRecord;
                    void* ContextRecord;
                };
                FakePointers pointers = {&record, nullptr};
                typedef int32_t (*VEHHandler)(void*);
                auto veh = reinterpret_cast<VEHHandler>(handler);
                int32_t result = veh(&pointers);
                if (result == -1) {
                    MS_INFO("PELoader: VEH handler %p handled exception 0x%08X", handler, dwExceptionCode);
                    return;
                }
            }
        }
    }

    if (s_unhandledExceptionFilter) {
        struct FakePointers {
            void* ExceptionRecord;
            void* ContextRecord;
        };
        FakePointers pointers = {&record, nullptr};
        typedef void* (*FilterFunc)(void*);
        auto filter = reinterpret_cast<FilterFunc>(s_unhandledExceptionFilter);
        filter(&pointers);
        return;
    }

    MS_WARN("PELoader: Unhandled exception 0x%08X", dwExceptionCode);
}

static void* MSABI shim_SetUnhandledExceptionFilter(void* lpTopLevelExceptionFilter) {
    void* old = s_unhandledExceptionFilter;
    s_unhandledExceptionFilter = lpTopLevelExceptionFilter;
    return old;
}

static void* MSABI shim_UnhandledExceptionFilter(void* exceptionInfo) {
    MS_INFO("PELoader: UnhandledExceptionFilter(%p)", exceptionInfo);

    {
        std::lock_guard<std::mutex> lock(s_vehMutex);
        for (auto& [handler, isFirst] : s_vehHandlers) {
            if (handler && exceptionInfo) {
                typedef int32_t (*VEHHandler)(void*);
                auto veh = reinterpret_cast<VEHHandler>(handler);
                int32_t result = veh(exceptionInfo);
                if (result == -1) {
                    MS_INFO("PELoader: VEH handler %p handled exception", handler);
                    return nullptr;
                }
            }
        }
    }

    if (s_unhandledExceptionFilter) {
        typedef void* (*FilterFunc)(void*);
        auto filter = reinterpret_cast<FilterFunc>(s_unhandledExceptionFilter);
        return filter(exceptionInfo);
    }

    MS_INFO("PELoader: UnhandledExceptionFilter returning (no handler)");
    return nullptr;
}

static void* MSABI shim_AddVectoredExceptionHandler(uint32_t First, void* Handler) {
    MS_INFO("TRACE: AddVectoredExceptionHandler(%u, %p)", First, Handler);
    std::lock_guard<std::mutex> lock(s_vehMutex);
    if (First) {
        s_vehHandlers.insert(s_vehHandlers.begin(), {Handler, true});
    } else {
        s_vehHandlers.push_back({Handler, false});
    }
    return Handler;
}

static uint32_t MSABI shim_RemoveVectoredExceptionHandler(void* Handler) {
    MS_INFO("TRACE: RemoveVectoredExceptionHandler(%p)", Handler);
    std::lock_guard<std::mutex> lock(s_vehMutex);
    for (auto it = s_vehHandlers.begin(); it != s_vehHandlers.end(); ++it) {
        if (it->first == Handler) {
            s_vehHandlers.erase(it);
            return 0;
        }
    }
    return 1;
}

static void* MSABI shim_AddVectoredContinueHandler(uint32_t First, void* Handler) {
    (void)First;
    return Handler;
}

static uint32_t MSABI shim_RemoveVectoredContinueHandler(void* Handler) {
    (void)Handler;
    return 0;
}

static void MSABI shim_DebugBreak() {
    MS_INFO("PELoader: DebugBreak");
}

static char s_cmdLineA[4096] = "";
static wchar_t s_cmdLineW[4096] = {0};

void setCommandLine(const char* cmd) {
    strncpy(s_cmdLineA, cmd, sizeof(s_cmdLineA) - 1);
    for (size_t i = 0; i < sizeof(s_cmdLineW) / sizeof(wchar_t) - 1 && cmd[i]; i++) {
        s_cmdLineW[i] = (wchar_t)(unsigned char)cmd[i];
    }
}

static char* MSABI shim_GetCommandLineA() {
    MS_INFO("TRACE: GetCommandLineA()");
    return s_cmdLineA;
}
static wchar_t* MSABI shim_GetCommandLineW() {
    MS_INFO("TRACE: GetCommandLineW()");
    return s_cmdLineW;
}

static std::unordered_map<std::string, std::string> s_envStore;
static std::unordered_set<std::string> s_envHidden;
static bool s_envInitialized = false;

static std::string canonicalEnvironmentName(std::string_view name) {
    std::string canonical;
    canonical.reserve(name.size());
    for (char character : name) {
        canonical.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }
    return canonical;
}

static std::string narrowEnvironmentString(const wchar_t* value) {
    std::string result;
    if (!value)
        return result;

    for (size_t i = 0; value[i]; i++)
        result.push_back(static_cast<char>(value[i] & 0x7F));
    return result;
}

static char** hostEnvironment() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

// macOS keeps environment names case-sensitive even though Windows does not.
// Walk the process environment rather than calling getenv(name), so a PE
// lookup cannot miss a host variable solely because its spelling differs.
static const char* findHostEnvironmentValue(const std::string& canonicalName) {
    char** environment = hostEnvironment();
    if (!environment)
        return nullptr;

    for (char** entry = environment; *entry; entry++) {
        const char* separator = strchr(*entry, '=');
        if (!separator)
            continue;

        std::string entryName(*entry, static_cast<size_t>(separator - *entry));
        if (canonicalEnvironmentName(entryName) == canonicalName)
            return separator + 1;
    }
    return nullptr;
}

static void ensureEnvInit() {
    if (s_envInitialized)
        return;
    s_envInitialized = true;
    s_envStore["PATH"] = "C:\\Windows\\system32;C:\\Windows;C:\\Windows\\System32\\Wbem";
    s_envStore["APPDATA"] = "C:\\Users\\user\\AppData\\Roaming";
    s_envStore["USERPROFILE"] = "C:\\Users\\user";
    s_envStore["PROGRAMFILES"] = "C:\\Program Files";
    s_envStore["PROGRAMFILES(X86)"] = "C:\\Program Files (x86)";
    s_envStore["PROGRAMW6432"] = "C:\\Program Files";
    s_envStore["WINDIR"] = "C:\\Windows";
    s_envStore["SYSTEMROOT"] = "C:\\Windows";
    s_envStore["SYSTEMDRIVE"] = "C:";
    s_envStore["TEMP"] = "C:\\Users\\user\\AppData\\Local\\Temp";
    s_envStore["TMP"] = "C:\\Users\\user\\AppData\\Local\\Temp";
    s_envStore["HOMEDRIVE"] = "C:";
    s_envStore["HOMEPATH"] = "\\Users\\user";
    s_envStore["COMPUTERNAME"] = "METALSHARP";
    s_envStore["USERNAME"] = "user";
    s_envStore["USERDOMAIN"] = "METALSHARP";
    s_envStore["OS"] = "Windows_NT";
    s_envStore["PROCESSOR_ARCHITECTURE"] = "AMD64";
    s_envStore["PROCESSOR_LEVEL"] = "6";
    s_envStore["PROCESSOR_REVISION"] = "3F09";
    s_envStore["NUMBER_OF_PROCESSORS"] = std::to_string(sysconf(_SC_NPROCESSORS_ONLN));
    s_envStore["COMMONPROGRAMFILES"] = "C:\\Program Files\\Common Files";
    s_envStore["COMMONPROGRAMFILES(X86)"] = "C:\\Program Files (x86)\\Common Files";
    s_envStore["COMMONPROGRAMW6432"] = "C:\\Program Files\\Common Files";
    s_envStore["LOCALAPPDATA"] = "C:\\Users\\user\\AppData\\Local";
    s_envStore["PUBLIC"] = "C:\\Users\\Public";
    s_envStore["ALLUSERSPROFILE"] = "C:\\ProgramData";
    s_envStore["PATHEXT"] = ".COM;.EXE;.BAT;.CMD;.VBS;.JS";
    s_envStore["PSMODULEPATH"] = "C:\\Users\\user\\Documents\\WindowsPowerShell\\Modules";
    s_envStore["FPS_BROWSER_USER_PROFILE"] = "default";
    s_envStore["FPS_BROWSER_APP_PROFILE"] = "chrome";
}

static const std::string* findEnvironmentValue(const std::string& canonicalName) {
    auto cached = s_envStore.find(canonicalName);
    if (cached != s_envStore.end())
        return &cached->second;

    // A Windows-side deletion must mask an inherited host variable without
    // calling unsetenv(), which would mutate the launcher's POSIX namespace.
    if (s_envHidden.find(canonicalName) != s_envHidden.end())
        return nullptr;

    const char* hostValue = findHostEnvironmentValue(canonicalName);
    if (!hostValue)
        return nullptr;

    auto [inserted, unused] = s_envStore.emplace(canonicalName, hostValue);
    (void)unused;
    return &inserted->second;
}

static BOOL setEnvironmentValue(const std::string& name, const std::string* value) {
    const std::string canonicalName = canonicalEnvironmentName(name);
    if (canonicalName.empty()) {
        setKernel32LastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (value) {
        s_envStore[canonicalName] = *value;
        s_envHidden.erase(canonicalName);
    } else {
        s_envStore.erase(canonicalName);
        s_envHidden.insert(canonicalName);
    }
    return 1;
}

static DWORD MSABI shim_GetEnvironmentVariableW(const wchar_t* lpName, wchar_t* lpBuffer, DWORD nSize) {
    ensureEnvInit();
    if (!lpName) {
        setKernel32LastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    const std::string canonicalName = canonicalEnvironmentName(narrowEnvironmentString(lpName));
    const std::string* value = findEnvironmentValue(canonicalName);
    if (!value) {
        setKernel32LastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }

    const std::string& val = *value;
    DWORD needed = static_cast<DWORD>(val.size() + 1);
    if (nSize == 0)
        return needed;
    if (!lpBuffer || nSize < needed) {
        setKernel32LastError(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    if (lpBuffer) {
        for (size_t i = 0; i < val.size(); i++)
            lpBuffer[i] = (wchar_t)(unsigned char)val[i];
        lpBuffer[val.size()] = L'\0';
    }
    return needed - 1;
}

static DWORD MSABI shim_GetEnvironmentVariableA(const char* lpName, char* lpBuffer, DWORD nSize) {
    ensureEnvInit();
    if (!lpName) {
        setKernel32LastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    const std::string canonicalName = canonicalEnvironmentName(lpName);
    const std::string* value = findEnvironmentValue(canonicalName);
    if (!value) {
        setKernel32LastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }

    const std::string& val = *value;
    DWORD needed = static_cast<DWORD>(val.size() + 1);
    if (nSize == 0)
        return needed;
    if (!lpBuffer || nSize < needed) {
        setKernel32LastError(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    if (lpBuffer) {
        memcpy(lpBuffer, val.c_str(), needed);
    }
    return needed - 1;
}

static BOOL MSABI shim_SetEnvironmentVariableW(const wchar_t* lpName, const wchar_t* lpValue) {
    ensureEnvInit();
    if (!lpName) {
        setKernel32LastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    std::string value = narrowEnvironmentString(lpValue);
    return setEnvironmentValue(narrowEnvironmentString(lpName), lpValue ? &value : nullptr);
}

static BOOL MSABI shim_SetEnvironmentVariableA(const char* lpName, const char* lpValue) {
    ensureEnvInit();
    if (!lpName) {
        setKernel32LastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    std::string value = lpValue ? lpValue : "";
    return setEnvironmentValue(lpName, lpValue ? &value : nullptr);
}

static DWORD MSABI shim_ExpandEnvironmentStringsW(const wchar_t* lpSrc, wchar_t* lpDst, DWORD nSize) {
    ensureEnvInit();
    if (!lpSrc)
        return 0;

    std::string src;
    for (int i = 0; lpSrc[i]; i++)
        src += (char)(lpSrc[i] & 0x7F);

    std::string result;
    size_t pos = 0;
    while (pos < src.size()) {
        if (src[pos] == '%') {
            size_t end = src.find('%', pos + 1);
            if (end != std::string::npos) {
                std::string varName = src.substr(pos + 1, end - pos - 1);
                const std::string canonicalName = canonicalEnvironmentName(varName);
                const std::string* value = findEnvironmentValue(canonicalName);
                if (value)
                    result += *value;
                else
                    result += src.substr(pos, end - pos + 1);
                pos = end + 1;
            } else {
                result += src[pos++];
            }
        } else {
            result += src[pos++];
        }
    }

    DWORD needed = static_cast<DWORD>(result.size()) + 1;
    if (nSize == 0 || !lpDst)
        return needed;
    DWORD copyLen = needed < nSize ? needed : nSize;
    for (DWORD i = 0; i < copyLen; i++)
        lpDst[i] = (wchar_t)(unsigned char)result[i];
    if (copyLen > 0)
        lpDst[copyLen - 1] = 0;
    return needed;
}

static void MSABI shim_GetStartupInfoW(STARTUPINFOW* lpStartupInfo) {
    MS_INFO("TRACE: GetStartupInfoW(%p)", lpStartupInfo);
    memset(lpStartupInfo, 0, sizeof(STARTUPINFOW));
    lpStartupInfo->cb = sizeof(STARTUPINFOW);
}

static void* MSABI shim_GetStdHandle(DWORD nStdHandle) {
    MS_INFO("TRACE: GetStdHandle(0x%X)", nStdHandle);
    switch (nStdHandle) {
    case 0xFFFFFFF6:
        return reinterpret_cast<void*>(stdin);
    case 0xFFFFFFF5:
        return reinterpret_cast<void*>(stdout);
    case 0xFFFFFFF4:
        return reinterpret_cast<void*>(stderr);
    default:
        return INVALID_HANDLE_VALUE;
    }
}

static BOOL MSABI shim_SetStdHandle(DWORD nStdHandle, HANDLE hHandle) {
    (void)nStdHandle;
    (void)hHandle;
    return 1;
}

static BOOL MSABI shim_FreeLibrary(HMODULE hLibModule) {
    (void)hLibModule;
    return 1;
}

static HMODULE MSABI shim_LoadLibraryExA(const char* lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    (void)hFile;
    (void)dwFlags;
    if (!lpLibFileName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    MS_INFO("PELoader: LoadLibraryExA(\"%s\")", lpLibFileName);
    HMODULE result = PELoader::instance()->loadLibrary(std::string(lpLibFileName));
    if (!result)
        setLastErrorCode(ERROR_MOD_NOT_FOUND);
    return result;
}

static HMODULE MSABI shim_LoadLibraryExW(const wchar_t* lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    (void)hFile;
    (void)dwFlags;
    if (!lpLibFileName)
        return nullptr;
    const auto* u16 = reinterpret_cast<const uint16_t*>(lpLibFileName);
    char narrow[1024];
    int j = 0;
    for (int i = 0; u16[i] && j < 1023; i++) {
        char c = (char)(u16[i] & 0x7F);
        if (c >= 0x20)
            narrow[j++] = c;
    }
    narrow[j] = 0;
    MS_INFO("PELoader: LoadLibraryExW(\"%s\")", narrow);
    if (j == 0) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    HMODULE result = PELoader::instance()->loadLibrary(std::string(narrow));
    if (!result)
        setLastErrorCode(ERROR_MOD_NOT_FOUND);
    return result;
}

static HMODULE MSABI shim_GetModuleHandleExA(DWORD dwFlags, const char* lpModuleName, HMODULE* phModule) {
    (void)dwFlags;
    (void)lpModuleName;
    if (phModule)
        *phModule = reinterpret_cast<HMODULE>(0x2);
    return reinterpret_cast<HMODULE>(0x2);
}

static HMODULE MSABI shim_GetModuleHandleExW(DWORD dwFlags, const wchar_t* lpModuleName, HMODULE* phModule) {
    (void)dwFlags;
    (void)lpModuleName;
    if (phModule)
        *phModule = reinterpret_cast<HMODULE>(0x2);
    return reinterpret_cast<HMODULE>(0x2);
}

static void* MSABI shim_CreateEventA(void* lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
                                     const char* lpName) {
    (void)lpEventAttributes;
    std::string name = lpName ? lpName : "";
    return SyncContext::instance().createEvent(bManualReset != 0, bInitialState != 0, name);
}

static BOOL MSABI shim_ResetEvent(HANDLE hEvent) {
    return SyncContext::instance().resetEvent(hEvent) ? 1 : 0;
}
static BOOL MSABI shim_SetEvent(HANDLE hEvent) {
    return SyncContext::instance().setEvent(hEvent) ? 1 : 0;
}

static HANDLE MSABI shim_OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, const char* lpName) {
    (void)dwDesiredAccess;
    (void)bInheritHandle;
    if (!lpName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return SyncContext::instance().createEvent(true, false, lpName);
}

static void* MSABI shim_CreateIoCompletionPort(HANDLE FileHandle, HANDLE ExistingCompletionPort, void* CompletionKey,
                                               DWORD NumberOfConcurrentThreads) {
    (void)FileHandle;
    (void)ExistingCompletionPort;
    (void)CompletionKey;
    (void)NumberOfConcurrentThreads;
    return reinterpret_cast<void*>(0x101);
}

static BOOL MSABI shim_PostQueuedCompletionStatus(HANDLE CompletionPort, DWORD dwNumberOfBytesTransferred,
                                                  void* dwCompletionKey, void* lpOverlapped) {
    (void)CompletionPort;
    (void)dwNumberOfBytesTransferred;
    (void)dwCompletionKey;
    (void)lpOverlapped;
    return 1;
}

static BOOL MSABI shim_DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                                       HANDLE* lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle,
                                       DWORD dwOptions) {
    (void)hSourceProcessHandle;
    (void)hSourceHandle;
    (void)hTargetProcessHandle;
    (void)dwDesiredAccess;
    (void)bInheritHandle;
    (void)dwOptions;
    if (lpTargetHandle)
        *lpTargetHandle = hSourceHandle;
    return 1;
}

static HANDLE MSABI shim_OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId) {
    (void)dwDesiredAccess;
    (void)bInheritHandle;
    (void)dwProcessId;
    return reinterpret_cast<HANDLE>(0x200);
}

static HANDLE MSABI shim_OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId) {
    (void)dwDesiredAccess;
    (void)bInheritHandle;
    (void)dwThreadId;
    return reinterpret_cast<HANDLE>(0x201);
}

static BOOL MSABI shim_TerminateProcess(HANDLE hProcess, UINT uExitCode) {
    (void)hProcess;
    (void)uExitCode;
    return 1;
}

static BOOL MSABI shim_GetExitCodeProcess(HANDLE hProcess, DWORD* lpExitCode) {
    (void)hProcess;
    if (lpExitCode)
        *lpExitCode = 259;
    return 1;
}

static BOOL MSABI shim_GetExitCodeThread(HANDLE hThread, DWORD* lpExitCode) {
    auto* state = SyncContext::instance().getThreadState(hThread);
    if (lpExitCode) {
        if (state && state->finished.load()) {
            *lpExitCode = state->exitCode.load();
        } else {
            *lpExitCode = 259;
        }
    }
    return 1;
}

static BOOL MSABI shim_TerminateThread(HANDLE hThread, DWORD dwExitCode) {
    (void)hThread;
    (void)dwExitCode;
    return 1;
}

static DWORD MSABI shim_SuspendThread(HANDLE hThread) {
    (void)hThread;
    return 0;
}
static DWORD MSABI shim_ResumeThread(HANDLE hThread) {
    (void)hThread;
    return 0;
}
static BOOL MSABI shim_SetThreadPriority(HANDLE hThread, int nPriority) {
    (void)hThread;
    (void)nPriority;
    return 1;
}
static DWORD MSABI shim_SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask) {
    (void)hThread;
    (void)dwThreadAffinityMask;
    return 1;
}
static BOOL MSABI shim_GetProcessAffinityMask(HANDLE hProcess, DWORD_PTR* lpProcessAffinityMask,
                                              DWORD_PTR* lpSystemAffinityMask) {
    (void)hProcess;
    if (lpProcessAffinityMask)
        *lpProcessAffinityMask = 1;
    if (lpSystemAffinityMask)
        *lpSystemAffinityMask = 1;
    return 1;
}
static BOOL MSABI shim_SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask) {
    (void)hProcess;
    (void)dwProcessAffinityMask;
    return 1;
}

static BOOL MSABI shim_SwitchToThread() {
    sched_yield();
    return 1;
}

static void MSABI shim_InitializeSListHead(void* ListHead) {
    memset(ListHead, 0, 16);
}
static void* MSABI shim_InterlockedPushEntrySList(void* ListHead, void* ListEntry) {
    (void)ListHead;
    (void)ListEntry;
    return nullptr;
}

static void MSABI shim_InitializeSRWLock(void* SRWLock) {
    Win32SyncContext::instance().initializeSrwLock(SRWLock);
}

static void MSABI shim_AcquireSRWLockExclusive(void* SRWLock) {
    (void)Win32SyncContext::instance().acquireSrwLockExclusive(SRWLock);
}

static void MSABI shim_ReleaseSRWLockExclusive(void* SRWLock) {
    (void)Win32SyncContext::instance().releaseSrwLockExclusive(SRWLock);
}

static BOOL MSABI shim_TryAcquireSRWLockExclusive(void* SRWLock) {
    return Win32SyncContext::instance().tryAcquireSrwLockExclusive(SRWLock) ? 1 : 0;
}

static void MSABI shim_AcquireSRWLockShared(void* SRWLock) {
    (void)Win32SyncContext::instance().acquireSrwLockShared(SRWLock);
}

static void MSABI shim_ReleaseSRWLockShared(void* SRWLock) {
    (void)Win32SyncContext::instance().releaseSrwLockShared(SRWLock);
}

static BOOL MSABI shim_TryAcquireSRWLockShared(void* SRWLock) {
    return Win32SyncContext::instance().tryAcquireSrwLockShared(SRWLock) ? 1 : 0;
}

static void MSABI shim_InitializeConditionVariable(void* ConditionVariable) {
    Win32SyncContext::instance().initializeConditionVariable(ConditionVariable);
}

static BOOL MSABI shim_SleepConditionVariableSRW(void* ConditionVariable, void* SRWLock, DWORD dwMilliseconds,
                                                 ULONG Flags) {
    return Win32SyncContext::instance().sleepConditionVariableSrw(ConditionVariable, SRWLock, dwMilliseconds, Flags)
               ? 1
               : 0;
}

static void MSABI shim_WakeAllConditionVariable(void* ConditionVariable) {
    (void)Win32SyncContext::instance().wakeAllConditionVariable(ConditionVariable);
}

static void MSABI shim_WakeConditionVariable(void* ConditionVariable) {
    (void)Win32SyncContext::instance().wakeConditionVariable(ConditionVariable);
}

static void MSABI shim_InitializeCriticalSectionAndSpinCount(void* lpCriticalSection, DWORD dwSpinCount) {
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(lpCriticalSection);
    auto* mtx = new pthread_mutex_t();
    pthread_mutex_init(mtx, nullptr);
    cs->DebugInfo = mtx;
    cs->SpinCount = dwSpinCount;
}

static void MSABI shim_InitializeCriticalSectionEx(void* lpCriticalSection, DWORD dwSpinCount, DWORD dwFlags) {
    (void)dwFlags;
    MS_INFO("TRACE: InitializeCriticalSectionEx(%p, %u, %u)", lpCriticalSection, dwSpinCount, dwFlags);
    shim_InitializeCriticalSectionAndSpinCount(lpCriticalSection, dwSpinCount);
}

static BOOL MSABI shim_TryEnterCriticalSection(void* lpCriticalSection) {
    (void)lpCriticalSection;
    return 1;
}

static BOOL MSABI shim_InitOnceBeginInitialize(void* InitOnce, DWORD dwFlags, BOOL* fPending, void** lpContext) {
    (void)InitOnce;
    (void)dwFlags;
    (void)lpContext;
    if (fPending)
        *fPending = 0;
    return 1;
}

static BOOL MSABI shim_InitOnceComplete(void* InitOnce, DWORD dwFlags, void* lpContext) {
    (void)InitOnce;
    (void)dwFlags;
    (void)lpContext;
    return 1;
}

static thread_local void* s_tlsSlots[1088];
static DWORD s_nextTlsSlot = 64;

static DWORD MSABI shim_TlsAlloc() {
    if (s_nextTlsSlot >= 1088)
        return 0xFFFFFFFF;
    DWORD slot = s_nextTlsSlot++;
    MS_INFO("TRACE: TlsAlloc() -> %u", slot);
    return slot;
}

static BOOL MSABI shim_TlsFree(DWORD dwTlsIndex) {
    (void)dwTlsIndex;
    return 1;
}

static void* MSABI shim_TlsGetValue(DWORD dwTlsIndex) {
    if (dwTlsIndex >= 1088)
        return nullptr;
    return s_tlsSlots[dwTlsIndex];
}

static BOOL MSABI shim_TlsSetValue(DWORD dwTlsIndex, void* lpTlsValue) {
    if (dwTlsIndex >= 1088)
        return 0;
    s_tlsSlots[dwTlsIndex] = lpTlsValue;
    return 1;
}

static DWORD s_flsNext = 0;
static void* s_flsSlots[256];

static DWORD MSABI shim_FlsAlloc(void* lpCallback) {
    (void)lpCallback;
    if (s_flsNext >= 256)
        return 0xFFFFFFFF;
    s_flsSlots[s_flsNext] = nullptr;
    return s_flsNext++;
}

static BOOL MSABI shim_FlsFree(DWORD dwFlsIndex) {
    (void)dwFlsIndex;
    return 1;
}
static void* MSABI shim_FlsGetValue(DWORD dwFlsIndex) {
    if (dwFlsIndex >= 256)
        return nullptr;
    return s_flsSlots[dwFlsIndex];
}
static BOOL MSABI shim_FlsSetValue(DWORD dwFlsIndex, void* lpFlsValue) {
    if (dwFlsIndex >= 256)
        return 0;
    s_flsSlots[dwFlsIndex] = lpFlsValue;
    return 1;
}

static void* MSABI shim_ConvertThreadToFiber(void* lpParameter) {
    (void)lpParameter;
    return reinterpret_cast<void*>(0x300);
}
static BOOL MSABI shim_ConvertFiberToThread() {
    return 1;
}
static void* MSABI shim_CreateFiber(SIZE_T dwStackSize, void* lpStartAddress, void* lpParameter) {
    (void)dwStackSize;
    (void)lpStartAddress;
    (void)lpParameter;
    return reinterpret_cast<void*>(0x301);
}
static void MSABI shim_DeleteFiber(void* lpFiber) {
    (void)lpFiber;
}
static void MSABI shim_SwitchToFiber(void* lpFiber) {
    (void)lpFiber;
}

static void MSABI shim_GetSystemTime(void* lpSystemTime) {
    auto* st = reinterpret_cast<WORD*>(lpSystemTime);
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    st[0] = (WORD)(1900 + t.tm_year);
    st[1] = (WORD)(1 + t.tm_mon);
    st[2] = (WORD)t.tm_wday;
    st[3] = (WORD)t.tm_mday;
    st[4] = (WORD)t.tm_hour;
    st[5] = (WORD)t.tm_min;
    st[6] = (WORD)t.tm_sec;
    st[7] = 0;
}

static void MSABI shim_GetSystemTimeAsFileTime(void* lpFileTime) {
    MS_INFO("TRACE: GetSystemTimeAsFileTime(%p)", lpFileTime);
    auto* ft = reinterpret_cast<uint64_t*>(lpFileTime);
    auto now = std::chrono::system_clock::now();
    auto dur = now.time_since_epoch();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
    *ft = static_cast<uint64_t>((micros + 11644473600000000LL) * 10);
}

static void MSABI shim_FileTimeToSystemTime(const void* lpFileTime, void* lpSystemTime) {
    (void)lpFileTime;
    (void)lpSystemTime;
}

static void MSABI shim_SystemTimeToFileTime(const void* lpSystemTime, void* lpFileTime) {
    (void)lpSystemTime;
    (void)lpFileTime;
}

static void MSABI shim_SystemTimeToTzSpecificLocalTime(void* lpTimeZone, const void* lpUniversalTime,
                                                       void* lpLocalTime) {
    (void)lpTimeZone;
    if (lpLocalTime && lpUniversalTime)
        memcpy(lpLocalTime, lpUniversalTime, 16);
}

static DWORD MSABI shim_GetTimeZoneInformation(void* lpTimeZoneInformation) {
    (void)lpTimeZoneInformation;
    return 0;
}

static int MSABI shim_GetDateFormatW(void* Locale, DWORD dwFlags, const void* lpDate, const wchar_t* lpFormat,
                                     wchar_t* lpDateStr, int cchDate) {
    (void)Locale;
    (void)dwFlags;
    (void)lpDate;
    (void)lpFormat;
    if (lpDateStr && cchDate > 0) {
        lpDateStr[0] = 0;
        return 0;
    }
    return 0;
}

static int MSABI shim_GetTimeFormatW(void* Locale, DWORD dwFlags, const void* lpTime, const wchar_t* lpFormat,
                                     wchar_t* lpTimeStr, int cchTime) {
    (void)Locale;
    (void)dwFlags;
    (void)lpTime;
    (void)lpFormat;
    if (lpTimeStr && cchTime > 0) {
        lpTimeStr[0] = 0;
        return 0;
    }
    return 0;
}

static BOOL MSABI shim_GetVersionExA(void* lpVersionInformation) {
    auto* vi = reinterpret_cast<BYTE*>(lpVersionInformation);
    if (vi[0] < 156)
        return 0;
    memset(vi, 0, 156);
    vi[0] = 156;
    auto* dw = reinterpret_cast<DWORD*>(vi);
    dw[1] = 0x00000006;
    dw[2] = 0x00000001;
    dw[3] = 0x00000002;
    auto* sz = reinterpret_cast<char*>(vi + 20);
    strcpy(sz, "Microsoft Windows 10");
    return 1;
}

static BOOL MSABI shim_VerifyVersionInfoW(void* lpVersionInformation, DWORD dwTypeMask, uint64_t dwlConditionMask) {
    (void)lpVersionInformation;
    (void)dwTypeMask;
    (void)dwlConditionMask;
    return 1;
}

static uint64_t MSABI shim_VerSetConditionMask(uint64_t dwlConditionMask, DWORD dwTypeBitMask, BYTE dwCondition) {
    (void)dwTypeBitMask;
    (void)dwCondition;
    return dwlConditionMask;
}

static void* MSABI shim_GlobalLock(HANDLE hMem) {
    return hMem;
}
static BOOL MSABI shim_GlobalUnlock(HANDLE hMem) {
    (void)hMem;
    return 1;
}

static void MSABI shim_GlobalMemoryStatusEx(void* lpBuffer) {
    auto* ms = reinterpret_cast<DWORD*>(lpBuffer);
    memset(ms, 0, 32);
    ms[0] = 32;
    ms[1] = 100;
    ms[2] = static_cast<DWORD>(16ULL * 1024 * 1024 * 1024);
    ms[3] = static_cast<DWORD>(8ULL * 1024 * 1024 * 1024);
}

static BOOL MSABI shim_HeapLock(HANDLE hHeap) {
    (void)hHeap;
    return 1;
}
static BOOL MSABI shim_HeapUnlock(HANDLE hHeap) {
    (void)hHeap;
    return 1;
}
static BOOL MSABI shim_HeapWalk(HANDLE hHeap, void* lpEntry) {
    (void)hHeap;
    (void)lpEntry;
    return 0;
}
static BOOL MSABI shim_HeapQueryInformation(HANDLE hHeap, DWORD HeapInformationClass, void* HeapInformation,
                                            SIZE_T HeapInformationLength, SIZE_T* ReturnLength) {
    (void)hHeap;
    (void)HeapInformationClass;
    (void)HeapInformation;
    (void)HeapInformationLength;
    (void)ReturnLength;
    return 1;
}
static BOOL MSABI shim_HeapSetInformation(HANDLE hHeap, DWORD HeapInformationClass, void* HeapInformation,
                                          SIZE_T HeapInformationLength) {
    (void)hHeap;
    (void)HeapInformationClass;
    (void)HeapInformation;
    (void)HeapInformationLength;
    return 1;
}
static SIZE_T MSABI shim_HeapSize(HANDLE hHeap, DWORD dwFlags, const void* lpMem) {
    MS_INFO("TRACE: HeapSize(%p, 0x%X, %p)", hHeap, dwFlags, lpMem);
    (void)hHeap;
    (void)dwFlags;
    (void)lpMem;
    return 1024;
}
static void* MSABI shim_HeapReAlloc(HANDLE hHeap, DWORD dwFlags, void* lpMem, SIZE_T dwBytes) {
    MS_INFO("TRACE: HeapReAlloc(%p, 0x%X, %p, %zu)", hHeap, dwFlags, lpMem, dwBytes);
    (void)hHeap;
    (void)dwFlags;
    return realloc(lpMem, dwBytes);
}

static DWORD MSABI shim_GetProcessHeaps(DWORD NumberOfHeaps, HANDLE* ProcessHeaps) {
    (void)NumberOfHeaps;
    if (ProcessHeaps)
        ProcessHeaps[0] = reinterpret_cast<HANDLE>(0x1);
    return 1;
}

static BOOL MSABI shim_IsBadWritePtr(void* lp, size_t ucb) {
    (void)lp;
    (void)ucb;
    return 0;
}
static BOOL MSABI shim_DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, void* lpInBuffer, DWORD nInBufferSize,
                                       void* lpOutBuffer, DWORD nOutBufferSize, DWORD* lpBytesReturned,
                                       void* lpOverlapped) {
    (void)hDevice;
    (void)dwIoControlCode;
    (void)lpInBuffer;
    (void)nInBufferSize;
    (void)lpOutBuffer;
    (void)nOutBufferSize;
    (void)lpOverlapped;
    if (lpBytesReturned)
        *lpBytesReturned = 0;
    return 0;
}

static BOOL MSABI shim_ProcessIdToSessionId(DWORD dwProcessId, DWORD* pSessionId) {
    (void)dwProcessId;
    if (pSessionId)
        *pSessionId = 0;
    return 1;
}

static void MSABI shim_OutputDebugStringW(const wchar_t* lpOutputString) {
    (void)lpOutputString;
}

static BOOL MSABI shim_PeekNamedPipe(HANDLE hNamedPipe, void* lpBuffer, DWORD nBufferSize, DWORD* lpBytesRead,
                                     DWORD* lpTotalBytesAvail, DWORD* lpBytesLeftThisMessage) {
    (void)hNamedPipe;
    (void)lpBuffer;
    (void)nBufferSize;
    if (lpBytesRead)
        *lpBytesRead = 0;
    if (lpTotalBytesAvail)
        *lpTotalBytesAvail = 0;
    if (lpBytesLeftThisMessage)
        *lpBytesLeftThisMessage = 0;
    return 0;
}

static BOOL MSABI shim_SetConsoleCtrlHandler(void* HandlerRoutine, BOOL Add) {
    (void)HandlerRoutine;
    (void)Add;
    return 1;
}
static BOOL MSABI shim_SetConsoleMode(HANDLE hConsoleHandle, DWORD dwMode) {
    (void)hConsoleHandle;
    (void)dwMode;
    return 1;
}
static BOOL MSABI shim_GetConsoleMode(HANDLE hConsoleHandle, DWORD* lpMode) {
    (void)hConsoleHandle;
    if (lpMode)
        *lpMode = 0x1FF;
    return 1;
}
static UINT MSABI shim_GetConsoleOutputCP() {
    return 437;
}
static UINT MSABI shim_GetOEMCP() {
    return 437;
}

static BOOL MSABI shim_GetCPInfo(UINT CodePage, void* lpCPInfo) {
    (void)CodePage;
    memset(lpCPInfo, 0, 28);
    reinterpret_cast<DWORD*>(lpCPInfo)[0] = 1;
    reinterpret_cast<BYTE*>(lpCPInfo)[4] = 1;
    return 1;
}

static void* MSABI shim_GetCurrentThread() {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-2));
}

static int MSABI shim_CompareStringW(void* Locale, DWORD dwCmpFlags, const wchar_t* lpString1, int cchCount1,
                                     const wchar_t* lpString2, int cchCount2) {
    (void)Locale;
    (void)dwCmpFlags;
    int len1 = cchCount1 > 0 ? cchCount1 : (int)wcslen(lpString1);
    int len2 = cchCount2 > 0 ? cchCount2 : (int)wcslen(lpString2);
    int minLen = len1 < len2 ? len1 : len2;
    for (int i = 0; i < minLen; i++) {
        if (lpString1[i] < lpString2[i])
            return 1;
        if (lpString1[i] > lpString2[i])
            return 3;
    }
    if (len1 < len2)
        return 1;
    if (len1 > len2)
        return 3;
    return 2;
}

static int MSABI shim_LCMapStringW(void* Locale, DWORD dwMapFlags, const wchar_t* lpSrcStr, int cchSrc,
                                   wchar_t* lpDestStr, int cchDest) {
    (void)Locale;
    (void)dwMapFlags;
    if (!lpSrcStr)
        return 0;
    int len = cchSrc > 0 ? cchSrc : (int)wcslen(lpSrcStr);
    if (cchDest == 0)
        return len;
    int copyLen = len < cchDest ? len : cchDest;
    for (int i = 0; i < copyLen; i++)
        lpDestStr[i] = lpSrcStr[i];
    return copyLen;
}

static BOOL MSABI shim_GetStringTypeW(DWORD dwInfoType, const wchar_t* lpSrcStr, int cchSrc, WORD* lpCharType) {
    (void)dwInfoType;
    int len = cchSrc > 0 ? cchSrc : (int)wcslen(lpSrcStr);
    for (int i = 0; i < len; i++) {
        lpCharType[i] = 0;
        wchar_t c = lpSrcStr[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            lpCharType[i] |= 0x0001 | 0x0002;
        if (c >= '0' && c <= '9')
            lpCharType[i] |= 0x0004;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            lpCharType[i] |= 0x0008;
    }
    return 1;
}

static void* MSABI shim_FreeEnvironmentStringsW(wchar_t* lpszEnvironmentBlock) {
    free(lpszEnvironmentBlock);
    return reinterpret_cast<void*>(1);
}

static wchar_t* MSABI shim_GetEnvironmentStringsW() {
    ensureEnvInit();
    size_t totalChars = 1;
    for (auto& [key, val] : s_envStore) {
        totalChars += key.size() + 1 + val.size() + 1;
    }
    totalChars++;

    auto* block = static_cast<wchar_t*>(malloc(totalChars * sizeof(wchar_t)));
    size_t pos = 0;
    for (auto& [key, val] : s_envStore) {
        for (size_t i = 0; i < key.size(); i++)
            block[pos++] = (wchar_t)(unsigned char)key[i];
        block[pos++] = L'=';
        for (size_t i = 0; i < val.size(); i++)
            block[pos++] = (wchar_t)(unsigned char)val[i];
        block[pos++] = 0;
    }
    block[pos++] = 0;
    return block;
}

static BOOL MSABI shim_CopyFileExW(const wchar_t* lpExistingFileName, const wchar_t* lpNewFileName,
                                   void* lpProgressRoutine, void* lpData, BOOL* pbCancel, DWORD dwCopyFlags) {
    (void)lpProgressRoutine;
    (void)lpData;
    (void)pbCancel;
    (void)dwCopyFlags;
    if (!lpExistingFileName || !lpNewFileName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }

    char srcW[1024], dstW[1024];
    for (int i = 0; lpExistingFileName[i] && i < 1023; i++)
        srcW[i] = (char)(lpExistingFileName[i] & 0xFF);
    srcW[1023] = 0;
    for (int i = 0; lpNewFileName[i] && i < 1023; i++)
        dstW[i] = (char)(lpNewFileName[i] & 0xFF);
    dstW[1023] = 0;

    std::string src = VirtualFileSystem::instance().winToHost(srcW);
    std::string dst = VirtualFileSystem::instance().winToHost(dstW);

    FILE* fin = fopen(src.c_str(), "rb");
    if (!fin) {
        setLastErrorFromErrno();
        return 0;
    }
    {
        size_t pos = dst.rfind('/');
        if (pos != std::string::npos) {
            std::string dir = dst.substr(0, pos);
            struct stat st;
            if (stat(dir.c_str(), &st) != 0) {
                size_t start = 0;
                while ((start = dir.find('/', start + 1)) != std::string::npos) {
                    std::string part = dir.substr(0, start);
                    mkdir(part.c_str(), 0755);
                }
                mkdir(dir.c_str(), 0755);
            }
        }
    }
    FILE* fout = fopen(dst.c_str(), "wb");
    if (!fout) {
        fclose(fin);
        setLastErrorFromErrno();
        return 0;
    }

    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        fwrite(buf, 1, n, fout);
    }
    fclose(fin);
    fclose(fout);
    return 1;
}

static BOOL MSABI shim_CreateDirectoryW(const wchar_t* lpPathName, void* lpSecurityAttributes) {
    (void)lpSecurityAttributes;
    char path[1024];
    for (int i = 0; lpPathName[i] && i < 1023; i++)
        path[i] = (char)(lpPathName[i] & 0xFF);
    path[1023] = 0;
    if (mkdir(path, 0755) == 0)
        return 1;
    setLastErrorFromErrno();
    return 0;
}

static BOOL MSABI shim_RemoveDirectoryA(const char* lpPathName) {
    if (rmdir(lpPathName) == 0)
        return 1;
    setLastErrorFromErrno();
    return 0;
}
static BOOL MSABI shim_RemoveDirectoryW(const wchar_t* lpPathName) {
    char path[1024];
    for (int i = 0; lpPathName[i] && i < 1023; i++)
        path[i] = (char)(lpPathName[i] & 0xFF);
    path[1023] = 0;
    if (rmdir(path) == 0)
        return 1;
    setLastErrorFromErrno();
    return 0;
}
static BOOL MSABI shim_DeleteFileW(const wchar_t* lpFileName) {
    char path[1024];
    for (int i = 0; lpFileName[i] && i < 1023; i++)
        path[i] = (char)(lpFileName[i] & 0xFF);
    path[1023] = 0;
    if (unlink(path) == 0)
        return 1;
    setLastErrorFromErrno();
    return 0;
}
static BOOL MSABI shim_MoveFileExW(const wchar_t* lpExistingFileName, const wchar_t* lpNewFileName, DWORD dwFlags) {
    (void)dwFlags;
    char src[1024], dst[1024];
    for (int i = 0; lpExistingFileName[i] && i < 1023; i++)
        src[i] = (char)(lpExistingFileName[i] & 0xFF);
    src[1023] = 0;
    for (int i = 0; lpNewFileName[i] && i < 1023; i++)
        dst[i] = (char)(lpNewFileName[i] & 0xFF);
    dst[1023] = 0;
    if (rename(src, dst) == 0)
        return 1;
    setLastErrorFromErrno();
    return 0;
}
static BOOL MSABI shim_ReplaceFileW(const wchar_t* lpReplacedFileName, const wchar_t* lpReplacementFileName,
                                    const wchar_t* lpBackupFileName, DWORD dwReplaceFlags, void* lpExclude,
                                    void* lpReserved) {
    (void)lpReplacedFileName;
    (void)lpReplacementFileName;
    (void)lpBackupFileName;
    (void)dwReplaceFlags;
    (void)lpExclude;
    (void)lpReserved;
    setLastErrorCode(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}
static BOOL MSABI shim_CreateSymbolicLinkW(const wchar_t* lpSymlinkFileName, const wchar_t* lpTargetFileName,
                                           DWORD dwFlags) {
    (void)lpSymlinkFileName;
    (void)lpTargetFileName;
    (void)dwFlags;
    setLastErrorCode(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}
static BOOL MSABI shim_SetFileAttributesW(const wchar_t* lpFileName, DWORD dwFileAttributes) {
    (void)lpFileName;
    (void)dwFileAttributes;
    return 1;
}
static BOOL MSABI shim_SetCurrentDirectoryW(const wchar_t* lpPathName) {
    char path[1024];
    for (int i = 0; lpPathName[i] && i < 1023; i++)
        path[i] = (char)(lpPathName[i] & 0xFF);
    path[1023] = 0;
    if (chdir(path) == 0)
        return 1;
    setLastErrorFromErrno();
    return 0;
}

static DWORD MSABI shim_GetFullPathNameW(const wchar_t* lpFileName, DWORD nBufferLength, wchar_t* lpBuffer,
                                         wchar_t** lpFilePart) {
    if (!lpFileName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char narrow[1024];
    int j = 0;
    for (int i = 0; lpFileName[i] && j < 1023; i++)
        narrow[j++] = (char)(lpFileName[i] & 0xFF);
    narrow[j] = 0;

    std::string full = VirtualFileSystem::instance().getFullPathName(narrow);
    DWORD needed = static_cast<DWORD>(full.size());

    if (lpFilePart) {
        auto lastSlash = full.rfind('\\');
        if (lastSlash != std::string::npos) {
            std::string tmp = full;
            *lpFilePart = lpBuffer ? lpBuffer + lastSlash + 1 : nullptr;
        } else {
            *lpFilePart = lpBuffer;
        }
    }

    if (nBufferLength == 0 || !lpBuffer)
        return needed + 1;
    DWORD copyLen = needed < nBufferLength ? needed : nBufferLength - 1;
    for (DWORD i = 0; i < copyLen; i++)
        lpBuffer[i] = (wchar_t)(unsigned char)full[i];
    lpBuffer[copyLen] = 0;
    return needed;
}

static BOOL MSABI shim_CreateProcessW(const wchar_t* lpApplicationName, const wchar_t* lpCommandLine,
                                      void* lpProcessAttributes, void* lpThreadAttributes, BOOL bInheritHandles,
                                      DWORD dwCreationFlags, void* lpEnvironment, const wchar_t* lpCurrentDirectory,
                                      void* lpStartupInfo, void* lpProcessInformation) {
    (void)lpProcessAttributes;
    (void)lpThreadAttributes;
    (void)bInheritHandles;
    (void)dwCreationFlags;
    (void)lpEnvironment;
    (void)lpStartupInfo;

    char appName[1024] = {};
    char cmdLine[2048] = {};
    char workDir[1024] = {};

    if (lpApplicationName) {
        for (int i = 0; i < 1023 && lpApplicationName[i]; i++)
            appName[i] = (char)lpApplicationName[i];
    }
    if (lpCommandLine) {
        for (int i = 0; i < 2047 && lpCommandLine[i]; i++)
            cmdLine[i] = (char)lpCommandLine[i];
    }
    if (lpCurrentDirectory) {
        for (int i = 0; i < 1023 && lpCurrentDirectory[i]; i++)
            workDir[i] = (char)lpCurrentDirectory[i];
    }

    MS_INFO("PELoader: CreateProcessW(\"%s\", \"%s\", cwd=\"%s\")", appName, cmdLine, workDir);

    const char* targetExe = appName[0] ? appName : cmdLine;

    char exeArgs[4096] = {};
    if (cmdLine[0]) {
        snprintf(exeArgs, sizeof(exeArgs), "%s", cmdLine);
    } else if (appName[0]) {
        snprintf(exeArgs, sizeof(exeArgs), "%s", appName);
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (workDir[0]) {
            chdir(workDir);
        } else if (appName[0]) {
            char dirCopy[1024];
            snprintf(dirCopy, sizeof(dirCopy), "%s", appName);
            char* lastSlash = strrchr(dirCopy, '/');
            if (lastSlash) {
                *lastSlash = 0;
                chdir(dirCopy);
            }
        }

        char exePath[4096];
        const char* home = getenv("HOME");
        snprintf(exePath, sizeof(exePath), "%s/metalsharp/build/metalsharp", home ? home : "/tmp");

        setenv("METALSHARP_CHILD_PROCESS", "1", 1);
        setenv("METALSHARP_CMDLINE", exeArgs, 1);
        if (workDir[0])
            setenv("METALSHARP_CWD", workDir, 1);

        char* argv[] = {exePath, const_cast<char*>(targetExe), nullptr};
        execv(exePath, argv);
        _exit(127);
    }

    if (pid > 0 && lpProcessInformation) {
        auto* pi = reinterpret_cast<DWORD*>(lpProcessInformation);
        pi[0] = (DWORD)pid;
        pi[1] = (DWORD)0x1337;
        pi[2] = (DWORD)pid;
        pi[3] = (DWORD)0;
        return 1;
    }

    if (pid < 0) {
        setLastErrorFromErrno();
        return 0;
    }
    return 1;
}

static BOOL MSABI shim_FindFirstFileExW(const wchar_t* lpFileName, DWORD fInfoLevelId, void* lpFindFileData,
                                        DWORD fSearchOp, void* lpSearchFilter, DWORD dwAdditionalFlags) {
    (void)lpFileName;
    (void)fInfoLevelId;
    (void)lpFindFileData;
    (void)fSearchOp;
    (void)lpSearchFilter;
    (void)dwAdditionalFlags;
    return 0;
}

static void* MSABI shim_FindFirstFileW(const wchar_t* lpFileName, void* lpFindFileData) {
    if (!lpFileName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    char narrow[1024];
    int j = 0;
    for (int i = 0; lpFileName[i] && j < 1023; i++)
        narrow[j++] = (char)(lpFileName[i] & 0xFF);
    narrow[j] = 0;
    return VirtualFileSystem::instance().findFirstFileW(narrow, lpFindFileData);
}

static BOOL MSABI shim_FindNextFileW(void* hFindFile, void* lpFindFileData) {
    return VirtualFileSystem::instance().findNextFileW(hFindFile, lpFindFileData);
}

static BOOL MSABI shim_GetFileAttributesExW(const wchar_t* lpFileName, DWORD fInfoLevelId, void* lpFileInformation) {
    if (!lpFileName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char narrow[1024];
    int j = 0;
    for (int i = 0; lpFileName[i] && j < 1023; i++)
        narrow[j++] = (char)(lpFileName[i] & 0xFF);
    narrow[j] = 0;
    return VirtualFileSystem::instance().getFileAttributesEx(narrow, lpFileInformation);
}

static DWORD MSABI shim_GetFileAttributesW(const wchar_t* lpFileName) {
    if (!lpFileName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFF;
    }
    char narrow[1024];
    int j = 0;
    for (int i = 0; lpFileName[i] && j < 1023; i++)
        narrow[j++] = (char)(lpFileName[i] & 0xFF);
    narrow[j] = 0;
    return VirtualFileSystem::instance().getFileAttributes(narrow);
}

static BOOL MSABI shim_GetFileInformationByHandle(HANDLE hFile, void* lpFileInformation) {
    return VirtualFileSystem::instance().getFileInformationByHandle(hFile, lpFileInformation);
}

static BOOL MSABI shim_GetFileTime(HANDLE hFile, void* lpCreationTime, void* lpLastAccessTime, void* lpLastWriteTime) {
    auto* entry = VirtualFileSystem::instance().getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }
    auto* fs = static_cast<FileState*>(entry->data);
    struct stat st;
    if (fstat(fs->fd, &st) != 0) {
        setLastErrorFromErrno();
        return 0;
    }
    uint64_t ctime = static_cast<uint64_t>(st.st_ctime) * 10000000ULL + 116444736000000000ULL;
    uint64_t atime = static_cast<uint64_t>(st.st_atime) * 10000000ULL + 116444736000000000ULL;
    uint64_t mtime = static_cast<uint64_t>(st.st_mtime) * 10000000ULL + 116444736000000000ULL;
    if (lpCreationTime)
        memcpy(lpCreationTime, &ctime, 8);
    if (lpLastAccessTime)
        memcpy(lpLastAccessTime, &atime, 8);
    if (lpLastWriteTime)
        memcpy(lpLastWriteTime, &mtime, 8);
    return 1;
}
static BOOL MSABI shim_SetFileTime(HANDLE hFile, const void* lpCreationTime, const void* lpLastAccessTime,
                                   const void* lpLastWriteTime) {
    (void)hFile;
    (void)lpCreationTime;
    (void)lpLastAccessTime;
    (void)lpLastWriteTime;
    return 1;
}

static DWORD MSABI shim_GetFileType(HANDLE hFile) {
    return VirtualFileSystem::instance().getFileType(hFile);
}

static BOOL MSABI shim_GetDiskFreeSpaceA(const char* lpRootPathName, DWORD* lpSectorsPerCluster,
                                         DWORD* lpBytesPerSector, DWORD* lpNumberOfFreeClusters,
                                         DWORD* lpTotalNumberOfClusters) {
    (void)lpRootPathName;
    if (lpSectorsPerCluster)
        *lpSectorsPerCluster = 8;
    if (lpBytesPerSector)
        *lpBytesPerSector = 512;
    if (lpNumberOfFreeClusters)
        *lpNumberOfFreeClusters = 1000000;
    if (lpTotalNumberOfClusters)
        *lpTotalNumberOfClusters = 2000000;
    return 1;
}

static BOOL MSABI shim_GetDiskFreeSpaceExW(const wchar_t* lpDirectoryName, uint64_t* lpFreeBytesAvailable,
                                           uint64_t* lpTotalNumberOfBytes, uint64_t* lpTotalNumberOfFreeBytes) {
    (void)lpDirectoryName;
    if (lpFreeBytesAvailable)
        *lpFreeBytesAvailable = 8ULL * 1024 * 1024 * 1024 * 1024;
    if (lpTotalNumberOfBytes)
        *lpTotalNumberOfBytes = 16ULL * 1024 * 1024 * 1024 * 1024;
    if (lpTotalNumberOfFreeBytes)
        *lpTotalNumberOfFreeBytes = 8ULL * 1024 * 1024 * 1024 * 1024;
    return 1;
}

static UINT MSABI shim_GetDriveTypeW(const wchar_t* lpRootPathName) {
    (void)lpRootPathName;
    return 3;
}

static BOOL MSABI shim_FlushFileBuffers(HANDLE hFile) {
    return VirtualFileSystem::instance().flushFileBuffers(hFile);
}
static BOOL MSABI shim_SetEndOfFile(HANDLE hFile) {
    auto* entry = VirtualFileSystem::instance().getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }
    auto* fs = static_cast<FileState*>(entry->data);
    off_t pos = lseek(fs->fd, 0, SEEK_CUR);
    if (pos == (off_t)-1) {
        setLastErrorFromErrno();
        return 0;
    }
    if (ftruncate(fs->fd, pos) != 0) {
        setLastErrorFromErrno();
        return 0;
    }
    return 1;
}
static BOOL MSABI shim_SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags) {
    (void)hObject;
    (void)dwMask;
    (void)dwFlags;
    return 1;
}
static DWORD MSABI shim_SetErrorMode(DWORD uMode) {
    (void)uMode;
    return 0;
}

static bool resourceImageRangeValid(const LoadedModule& module, uint32_t rva, size_t length) {
    if (!module.base || rva > module.size)
        return false;
    return length <= static_cast<size_t>(module.size - rva);
}

static bool resourcePointerRangeValid(const LoadedModule& module, const void* pointer, size_t length) {
    if (!module.base || !pointer)
        return false;

    uintptr_t imageBase = reinterpret_cast<uintptr_t>(module.base);
    uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (address < imageBase)
        return false;

    uintptr_t rva = address - imageBase;
    return rva <= module.size && length <= static_cast<size_t>(module.size - rva);
}

static bool resourceRangeValid(const LoadedModule& module, uint32_t resourceRVA, uint32_t resourceSize, uint32_t offset,
                               size_t length) {
    if (!resourceImageRangeValid(module, resourceRVA, resourceSize) || offset > resourceSize)
        return false;
    return length <= static_cast<size_t>(resourceSize - offset);
}

static uint8_t* resourcePtr(const LoadedModule& module, uint32_t resourceRVA, uint32_t resourceSize, uint32_t offset,
                            size_t length) {
    if (!resourceRangeValid(module, resourceRVA, resourceSize, offset, length))
        return nullptr;
    return module.base + static_cast<size_t>(resourceRVA) + offset;
}

// Directory-entry offsets are relative to the resource data directory; the
// OffsetToData in a leaf IMAGE_RESOURCE_DATA_ENTRY is an image RVA.
static const IMAGE_RESOURCE_DATA_ENTRY* validatedResourceDataEntry(const LoadedModule& module,
                                                                   const void* resourceHandle) {
    if (!resourcePointerRangeValid(module, resourceHandle, sizeof(IMAGE_RESOURCE_DATA_ENTRY)))
        return nullptr;

    auto* dataEntry = static_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(resourceHandle);
    if (dataEntry->OffsetToData >= module.size ||
        !resourceImageRangeValid(module, dataEntry->OffsetToData, dataEntry->Size))
        return nullptr;

    return dataEntry;
}

static void* findResourceEntry(const LoadedModule& module, uint32_t resourceRVA, uint32_t resourceSize,
                               uint32_t directoryOffset, uint32_t id, bool expectDirectory, uint32_t* targetOffset) {
    auto* dir = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(
        resourcePtr(module, resourceRVA, resourceSize, directoryOffset, sizeof(IMAGE_RESOURCE_DIRECTORY)));
    if (!dir)
        return nullptr;

    uint32_t count = static_cast<uint32_t>(dir->NumberOfNamedEntries) + dir->NumberOfIdEntries;
    size_t entriesSize = static_cast<size_t>(count) * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY);
    if (!resourceRangeValid(module, resourceRVA, resourceSize, directoryOffset,
                            sizeof(IMAGE_RESOURCE_DIRECTORY) + entriesSize))
        return nullptr;

    auto* entries = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(reinterpret_cast<const uint8_t*>(dir) +
                                                                            sizeof(IMAGE_RESOURCE_DIRECTORY));
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].Name != id)
            continue;

        uint32_t entryOffset = entries[i].OffsetToData & 0x7FFFFFFF;
        bool isDirectory = (entries[i].OffsetToData & 0x80000000) != 0;
        if (isDirectory != expectDirectory)
            return nullptr;

        size_t targetSize = expectDirectory ? sizeof(IMAGE_RESOURCE_DIRECTORY) : sizeof(IMAGE_RESOURCE_DATA_ENTRY);
        void* entry = resourcePtr(module, resourceRVA, resourceSize, entryOffset, targetSize);
        if (!entry)
            return nullptr;

        if (!expectDirectory && !validatedResourceDataEntry(module, entry))
            return nullptr;

        if (targetOffset)
            *targetOffset = entryOffset;
        return entry;
    }
    return nullptr;
}

static void* MSABI shim_FindResourceA(HMODULE hModule, const char* lpName, const char* lpType) {
    (void)hModule;

    auto* loader = PELoader::instance();
    if (!loader)
        return nullptr;
    auto* mod = loader->getMainModule();
    if (!mod || !resourceImageRangeValid(*mod, 0, sizeof(IMAGE_DOS_HEADER)))
        return nullptr;

    auto* base = mod->base;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        return nullptr;

    uint64_t ntHeaderRVA = static_cast<uint32_t>(dos->e_lfanew);
    if (ntHeaderRVA > UINT32_MAX || !resourceImageRangeValid(*mod, static_cast<uint32_t>(ntHeaderRVA),
                                                             sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER)))
        return nullptr;

    auto* peSignature = reinterpret_cast<const uint32_t*>(base + ntHeaderRVA);
    if (*peSignature != IMAGE_PE_SIGNATURE)
        return nullptr;

    auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(peSignature + 1);
    if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
        return nullptr;

    uint64_t optionalHeaderRVA = ntHeaderRVA + sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER);
    if (optionalHeaderRVA > UINT32_MAX ||
        !resourceImageRangeValid(*mod, static_cast<uint32_t>(optionalHeaderRVA), sizeof(IMAGE_OPTIONAL_HEADER64)))
        return nullptr;

    auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(base + optionalHeaderRVA);
    if (opt->Magic != IMAGE_OPTIONAL_MAGIC_PE32PLUS || opt->NumberOfRvaAndSizes <= DIRECTORY_RESOURCE)
        return nullptr;

    const auto& resourceDirectory = opt->DataDirectory[DIRECTORY_RESOURCE];
    if (!resourceDirectory.VirtualAddress || !resourceDirectory.Size ||
        !resourceImageRangeValid(*mod, resourceDirectory.VirtualAddress, resourceDirectory.Size))
        return nullptr;

    uint32_t typeId = (reinterpret_cast<uintptr_t>(lpType) >= 0x10000)
                          ? 0
                          : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lpType));
    uint32_t typeOffset = 0;
    if (!findResourceEntry(*mod, resourceDirectory.VirtualAddress, resourceDirectory.Size, 0, typeId, true,
                           &typeOffset))
        return nullptr;

    uint32_t nameId = (reinterpret_cast<uintptr_t>(lpName) >= 0x10000)
                          ? 0
                          : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lpName));
    uint32_t nameOffset = 0;
    if (!findResourceEntry(*mod, resourceDirectory.VirtualAddress, resourceDirectory.Size, typeOffset, nameId, true,
                           &nameOffset))
        return nullptr;

    return findResourceEntry(*mod, resourceDirectory.VirtualAddress, resourceDirectory.Size, nameOffset, 0, false,
                             nullptr);
}

static void* MSABI shim_LoadResource(HMODULE hModule, void* hResInfo) {
    (void)hModule;

    auto* loader = PELoader::instance();
    if (!loader || !validatedResourceDataEntry(*loader->getMainModule(), hResInfo))
        return nullptr;
    return hResInfo;
}
static void* MSABI shim_LockResource(void* hResData) {
    auto* loader = PELoader::instance();
    if (!loader)
        return nullptr;

    auto* mod = loader->getMainModule();
    auto* dataEntry = validatedResourceDataEntry(*mod, hResData);
    if (!dataEntry)
        return nullptr;
    return mod->base + dataEntry->OffsetToData;
}
static DWORD MSABI shim_SizeofResource(HMODULE hModule, void* hResInfo) {
    (void)hModule;

    auto* loader = PELoader::instance();
    if (!loader)
        return 0;
    auto* dataEntry = validatedResourceDataEntry(*loader->getMainModule(), hResInfo);
    return dataEntry ? dataEntry->Size : 0;
}

static int MSABI shim_MulDiv(int nNumber, int nNumerator, int nDenominator) {
    if (nDenominator == 0)
        return -1;
    return (int)((int64_t)nNumber * nNumerator / nDenominator);
}

static BOOL MSABI shim_ReadConsoleA(HANDLE hConsoleInput, void* lpBuffer, DWORD nNumberOfCharsToRead,
                                    DWORD* lpNumberOfCharsRead, void* pInputControl) {
    (void)hConsoleInput;
    (void)lpBuffer;
    (void)nNumberOfCharsToRead;
    (void)pInputControl;
    if (lpNumberOfCharsRead)
        *lpNumberOfCharsRead = 0;
    return 0;
}

static BOOL MSABI shim_ReadConsoleW(HANDLE hConsoleInput, void* lpBuffer, DWORD nNumberOfCharsToRead,
                                    DWORD* lpNumberOfCharsRead, void* pInputControl) {
    (void)hConsoleInput;
    (void)lpBuffer;
    (void)nNumberOfCharsToRead;
    (void)pInputControl;
    if (lpNumberOfCharsRead)
        *lpNumberOfCharsRead = 0;
    return 0;
}

static BOOL MSABI shim_WriteConsoleW(HANDLE hConsoleOutput, const void* lpBuffer, DWORD nNumberOfCharsToWrite,
                                     DWORD* lpNumberOfCharsWritten, void* lpReserved) {
    (void)hConsoleOutput;
    (void)lpReserved;
    if (lpNumberOfCharsWritten)
        *lpNumberOfCharsWritten = nNumberOfCharsToWrite;
    return 1;
}

static BOOL MSABI shim_GetProductInfo(DWORD dwOSMajorVersion, DWORD dwOSMinorVersion, DWORD dwSpMajorVersion,
                                      DWORD dwSpMinorVersion, DWORD* pdwReturnedProductType) {
    (void)dwOSMajorVersion;
    (void)dwOSMinorVersion;
    (void)dwSpMajorVersion;
    (void)dwSpMinorVersion;
    if (pdwReturnedProductType)
        *pdwReturnedProductType = 1;
    return 1;
}

struct XMM_SAVE_AREA32 {
    uint8_t FloatRegisters[128];
    uint8_t XmmRegisters[256];
    uint8_t Reserved[96];
};

struct WIN64_CONTEXT {
    uint64_t P1Home, P2Home, P3Home, P4Home;
    uint64_t P5Home, P6Home;
    uint32_t ContextFlags;
    uint32_t MxCsr;
    uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    uint32_t EFlags;
    uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    union {
        XMM_SAVE_AREA32 FltSave;
        struct {
            uint8_t Header[16];
            uint8_t Legacy[32];
            uint8_t Xmm0[16], Xmm1[16], Xmm2[16], Xmm3[16];
            uint8_t Xmm4[16], Xmm5[16], Xmm6[16], Xmm7[16];
            uint8_t Xmm8[16], Xmm9[16], Xmm10[16], Xmm11[16];
            uint8_t Xmm12[16], Xmm13[16], Xmm14[16], Xmm15[16];
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    uint8_t VectorRegister[26][16];
    uint64_t VectorControl;
    uint64_t DebugControl;
    uint64_t LastBranchToRip;
    uint64_t LastBranchFromRip;
    uint64_t LastExceptionToRip;
    uint64_t LastExceptionFromRip;
};

static void MSABI shim_RtlCaptureContext(void* ContextRecord) {
    auto* ctx = reinterpret_cast<WIN64_CONTEXT*>(ContextRecord);
    if (ctx) {
        memset(ctx, 0, sizeof(WIN64_CONTEXT));
        ctx->ContextFlags = 0x100000;
        ctx->Rip = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        uint64_t rbp_val = 0;
#if defined(__x86_64__)
        __asm__ volatile("movq %%rbp, %0" : "=r"(rbp_val));
#endif
        ctx->Rsp = rbp_val + 16;
    }
    MS_INFO("TRACE: RtlCaptureContext(%p) -> Rip=0x%llX, Rsp=0x%llX", ContextRecord,
            (unsigned long long)(ctx ? ctx->Rip : 0), (unsigned long long)(ctx ? ctx->Rsp : 0));
}
static void* MSABI shim_RtlCaptureStackBackTrace(DWORD FramesToSkip, DWORD FramesToCapture, void** BackTrace,
                                                 DWORD* BackTraceHash) {
    (void)FramesToSkip;
    (void)FramesToCapture;
    (void)BackTrace;
    if (BackTraceHash)
        *BackTraceHash = 0;
    return nullptr;
}
static uint32_t s_fakeRuntimeFunc[3] = {0, 0, 0};
static uint8_t s_fakeUnwindInfo[8] = {1, 0, 0, 0, 0, 0, 0, 0};
static std::mutex s_fakeRuntimeFuncMutex;

static void* MSABI shim_RtlLookupFunctionEntry(uint64_t ControlPoint, uint64_t* ImageBase, void* HistoryTable) {
    MS_INFO("TRACE: RtlLookupFunctionEntry(0x%llX)", (unsigned long long)ControlPoint);
    (void)ControlPoint;
    (void)HistoryTable;
    if (ImageBase)
        *ImageBase = 0;
    void* result = PELoader::instance()->lookupFunctionEntry(ControlPoint, ImageBase);
    if (!result) {
        MS_INFO("TRACE: RtlLookupFunctionEntry -> returning fake entry");
        auto* base = ImageBase ? reinterpret_cast<uint8_t*>(*ImageBase) : nullptr;
        if (!base)
            base = reinterpret_cast<uint8_t*>(PELoader::instance()->getMainModule()->base);
        {
            std::lock_guard<std::mutex> lock(s_fakeRuntimeFuncMutex);
            s_fakeRuntimeFunc[0] = 0;
            s_fakeRuntimeFunc[1] = 0x1000;
            s_fakeRuntimeFunc[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_fakeUnwindInfo) -
                                                         reinterpret_cast<uintptr_t>(base));
            result = s_fakeRuntimeFunc;
        }
    }
    return result;
}
static void* MSABI shim_RtlPcToFileHeader(uint64_t PcValue, uint64_t* ImageBase) {
    MS_INFO("TRACE: RtlPcToFileHeader(0x%llX)", PcValue);
    (void)PcValue;
    if (ImageBase)
        *ImageBase = 0;
    return nullptr;
}
static void MSABI shim_RtlUnwind(void* TargetFrame, void* TargetIp, void* ExceptionRecord, void* ReturnValue) {
    MS_INFO("TRACE: RtlUnwind(%p, %p, %p, %p)", TargetFrame, TargetIp, ExceptionRecord, ReturnValue);
}
static void MSABI shim_RtlUnwindEx(void* TargetFrame, void* TargetIp, void* ExceptionRecord, void* ReturnValue,
                                   void* ContextRecord) {
    MS_INFO("TRACE: RtlUnwindEx(%p, %p, %p, %p, %p)", TargetFrame, TargetIp, ExceptionRecord, ReturnValue,
            ContextRecord);
}
static void MSABI shim_RtlVirtualUnwind(DWORD HandlerType, uint64_t ImageBase, uint64_t ControlPc, void* FunctionEntry,
                                        void* ContextRecord, void** HandlerData, uint64_t* EstablisherFrame,
                                        void* ContextPointers) {
    (void)HandlerType;
    (void)ControlPc;
    (void)ContextPointers;
    (void)FunctionEntry;

    auto* ctx = reinterpret_cast<WIN64_CONTEXT*>(ContextRecord);

    uint64_t frame = ctx ? ctx->Rsp : 0;
    if (EstablisherFrame)
        *EstablisherFrame = frame;
    if (HandlerData)
        *HandlerData = nullptr;

    if (ctx) {
        ctx->Rip = 0;
        ctx->Rsp = 0;
    }

    MS_INFO("TRACE: RtlVirtualUnwind -> Establisher=0x%llX", (unsigned long long)frame);
}

static void* MSABI shim_EncodePointer(void* Ptr) {
    return Ptr;
}
static void* MSABI shim_DecodePointer(void* Ptr) {
    return Ptr;
}

static BOOL MSABI stub_GetFileSizeEx(void* hFile, int64_t* size) {
    return VirtualFileSystem::instance().getFileSizeEx(hFile, size);
}
static DWORD MSABI stub_SetFilePointer(void* hFile, LONG lDistanceToMove, LONG* lpDistanceToMoveHigh,
                                       DWORD dwMoveMethod) {
    return VirtualFileSystem::instance().setFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
}
static BOOL MSABI stub_SetFilePointerEx(void* hFile, int64_t liDistanceToMove, int64_t* lpNewFilePointer,
                                        DWORD dwMoveMethod) {
    return VirtualFileSystem::instance().setFilePointerEx(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod);
}

static void* MSABI shim_CreateNamedPipeW(const wchar_t* lpName, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
                                         DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
                                         void* lpSecurityAttributes) {
    (void)dwOpenMode;
    (void)dwPipeMode;
    (void)nMaxInstances;
    (void)nOutBufferSize;
    (void)nInBufferSize;
    (void)nDefaultTimeOut;
    (void)lpSecurityAttributes;
    if (!lpName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    char nameA[256] = {0};
    for (int i = 0; lpName[i] && i < 255; i++)
        nameA[i] = (char)(lpName[i] & 0x7F);

    std::string pipeName(nameA);
    size_t pipePos = pipeName.find("pipe");
    if (pipePos != std::string::npos)
        pipeName = pipeName.substr(pipePos + 5);

    int handles[2];
    if (!NetworkContext::instance().allocPipePair(pipeName, true, handles)) {
        setLastErrorFromErrno();
        return INVALID_HANDLE_VALUE;
    }

    MS_INFO("TRACE: CreateNamedPipeW(\"%s\") -> handle %d", nameA, handles[0]);
    return reinterpret_cast<void*>(static_cast<intptr_t>(handles[0]));
}

static BOOL MSABI shim_ConnectNamedPipe(void* hNamedPipe, void* lpOverlapped) {
    (void)lpOverlapped;
    int handle = (int)(intptr_t)hNamedPipe;
    int listenFd = NetworkContext::instance().getPipeReadFd(handle);
    if (listenFd < 0) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    int clientFd = accept(listenFd, nullptr, nullptr);
    if (clientFd < 0) {
        setLastErrorFromErrno();
        return 0;
    }

    fcntl(clientFd, F_SETFL, O_NONBLOCK);
    MS_INFO("TRACE: ConnectNamedPipe(%d) -> client fd %d", handle, clientFd);

    // Wire the accepted client socket into the pipe's handle table entry so
    // subsequent reads/writes on this pipe reach the client, instead of
    // discarding it (fd leak) and plumbing an anonymous pipe that was never
    // connected to anything.
    return NetworkContext::instance().connectPipe(handle, clientFd) ? 1 : 0;
}

static BOOL MSABI shim_WaitNamedPipeW(const wchar_t* lpNamedPipeName, DWORD nTimeOut) {
    (void)nTimeOut;
    if (!lpNamedPipeName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

static BOOL MSABI shim_CallNamedPipeW(const wchar_t* lpNamedPipeName, void* lpInBuffer, DWORD nInBufferSize,
                                      void* lpOutBuffer, DWORD nOutBufferSize, DWORD* lpBytesRead, DWORD nTimeOut) {
    (void)lpNamedPipeName;
    (void)lpInBuffer;
    (void)nInBufferSize;
    (void)lpOutBuffer;
    (void)nOutBufferSize;
    (void)nTimeOut;
    if (lpBytesRead)
        *lpBytesRead = 0;
    return 0;
}

static BOOL MSABI shim_CreatePipe(void* hReadPipe, void* hWritePipe, void* lpPipeAttributes, DWORD nSize) {
    (void)lpPipeAttributes;
    (void)nSize;
    int fds[2];
    if (pipe(fds) < 0) {
        setLastErrorFromErrno();
        return 0;
    }

    auto& vfs = VirtualFileSystem::instance();
    *(reinterpret_cast<HANDLE*>(hReadPipe)) = vfs.registerPipeFd(fds[0]);
    *(reinterpret_cast<HANDLE*>(hWritePipe)) = vfs.registerPipeFd(fds[1]);

    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    return 1;
}

static BOOL MSABI shim_TransactNamedPipe(void* hNamedPipe, void* lpInBuffer, DWORD nInBufferSize, void* lpOutBuffer,
                                         DWORD nOutBufferSize, DWORD* lpBytesRead, void* lpOverlapped) {
    (void)hNamedPipe;
    (void)lpInBuffer;
    (void)nInBufferSize;
    (void)lpOutBuffer;
    (void)nOutBufferSize;
    (void)lpOverlapped;
    if (lpBytesRead)
        *lpBytesRead = 0;
    return 0;
}

static void* MSABI shim_CreateMutexA(void* lpMutexAttributes, BOOL bInitialOwner, const char* lpName) {
    (void)lpMutexAttributes;
    std::string name = lpName ? lpName : "";
    return SyncContext::instance().createMutex(bInitialOwner != 0, name);
}

static BOOL MSABI shim_ReleaseMutex(HANDLE hMutex) {
    return SyncContext::instance().releaseMutex(hMutex) ? 1 : 0;
}

static void* MSABI shim_CreateSemaphoreA(void* lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
                                         const char* lpName) {
    (void)lpSemaphoreAttributes;
    std::string name = lpName ? lpName : "";
    return SyncContext::instance().createSemaphore(lInitialCount, lMaximumCount, name);
}

static BOOL MSABI shim_ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LONG* lpPreviousCount) {
    return SyncContext::instance().releaseSemaphore(hSemaphore, lReleaseCount, lpPreviousCount) ? 1 : 0;
}

static void* MSABI shim_CreateEventW(void* lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
                                     const wchar_t* lpName) {
    (void)lpEventAttributes;
    std::string name;
    if (lpName) {
        char buf[256] = {0};
        for (int i = 0; lpName[i] && i < 255; i++)
            buf[i] = (char)(lpName[i] & 0x7F);
        name = buf;
    }
    return SyncContext::instance().createEvent(bManualReset != 0, bInitialState != 0, name);
}

static void* MSABI shim_CreateMutexW(void* lpMutexAttributes, BOOL bInitialOwner, const wchar_t* lpName) {
    (void)lpMutexAttributes;
    std::string name;
    if (lpName) {
        char buf[256] = {0};
        for (int i = 0; lpName[i] && i < 255; i++)
            buf[i] = (char)(lpName[i] & 0x7F);
        name = buf;
    }
    return SyncContext::instance().createMutex(bInitialOwner != 0, name);
}

void addMissingKernel32(ShimLibrary& lib) {
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["VirtualProtect"] = fn((void*)shim_VirtualProtect);
    lib.functions["VirtualQuery"] = fn((void*)shim_VirtualQuery);
    lib.functions["ExitProcess"] = fn((void*)shim_ExitProcess);
    lib.functions["RaiseException"] = fn((void*)shim_RaiseException);
    lib.functions["SetUnhandledExceptionFilter"] = fn((void*)shim_SetUnhandledExceptionFilter);
    lib.functions["UnhandledExceptionFilter"] = fn((void*)shim_UnhandledExceptionFilter);
    lib.functions["AddVectoredExceptionHandler"] = fn((void*)shim_AddVectoredExceptionHandler);
    lib.functions["RemoveVectoredExceptionHandler"] = fn((void*)shim_RemoveVectoredExceptionHandler);
    lib.functions["AddVectoredContinueHandler"] = fn((void*)shim_AddVectoredContinueHandler);
    lib.functions["RemoveVectoredContinueHandler"] = fn((void*)shim_RemoveVectoredContinueHandler);
    lib.functions["DebugBreak"] = fn((void*)shim_DebugBreak);
    lib.functions["GetCommandLineA"] = fn((void*)shim_GetCommandLineA);
    lib.functions["GetCommandLineW"] = fn((void*)shim_GetCommandLineW);
    lib.functions["GetEnvironmentVariableW"] = fn((void*)shim_GetEnvironmentVariableW);
    lib.functions["GetEnvironmentVariableA"] = fn((void*)shim_GetEnvironmentVariableA);
    lib.functions["SetEnvironmentVariableW"] = fn((void*)shim_SetEnvironmentVariableW);
    lib.functions["SetEnvironmentVariableA"] = fn((void*)shim_SetEnvironmentVariableA);
    lib.functions["ExpandEnvironmentStringsA"] = fn((void*)stub_zero_dword);
    lib.functions["ExpandEnvironmentStringsW"] = fn((void*)shim_ExpandEnvironmentStringsW);
    lib.functions["GetStartupInfoW"] = fn((void*)shim_GetStartupInfoW);
    lib.functions["GetStartupInfoA"] = fn((void*)shim_GetStartupInfoW);
    lib.functions["IsDBCSLeadByte"] = fn((void*)+[](UINT) -> int { return 0; });
    lib.functions["__C_specific_handler"] = fn((void*)+[]() -> void* { return nullptr; });
    lib.functions["FreeLibrary"] = fn((void*)shim_FreeLibrary);
    lib.functions["LoadLibraryExA"] = fn((void*)shim_LoadLibraryExA);
    lib.functions["LoadLibraryExW"] = fn((void*)shim_LoadLibraryExW);
    lib.functions["GetModuleHandleExA"] = fn((void*)shim_GetModuleHandleExA);
    lib.functions["GetModuleHandleExW"] = fn((void*)shim_GetModuleHandleExW);
    lib.functions["CreateEventA"] = fn((void*)shim_CreateEventA);
    lib.functions["CreateEventW"] = fn((void*)shim_CreateEventW);
    lib.functions["ResetEvent"] = fn((void*)shim_ResetEvent);
    lib.functions["SetEvent"] = fn((void*)shim_SetEvent);
    lib.functions["OpenEventA"] = fn((void*)shim_OpenEventA);
    lib.functions["CreateMutexA"] = fn((void*)shim_CreateMutexA);
    lib.functions["CreateMutexW"] = fn((void*)shim_CreateMutexW);
    lib.functions["ReleaseMutex"] = fn((void*)shim_ReleaseMutex);
    lib.functions["CreateSemaphoreA"] = fn((void*)shim_CreateSemaphoreA);
    lib.functions["ReleaseSemaphore"] = fn((void*)shim_ReleaseSemaphore);
    lib.functions["CreateIoCompletionPort"] = fn((void*)shim_CreateIoCompletionPort);
    lib.functions["PostQueuedCompletionStatus"] = fn((void*)shim_PostQueuedCompletionStatus);
    lib.functions["DuplicateHandle"] = fn((void*)shim_DuplicateHandle);
    lib.functions["OpenProcess"] = fn((void*)shim_OpenProcess);
    lib.functions["OpenThread"] = fn((void*)shim_OpenThread);
    lib.functions["TerminateProcess"] = fn((void*)shim_TerminateProcess);
    lib.functions["GetExitCodeProcess"] = fn((void*)shim_GetExitCodeProcess);
    lib.functions["GetExitCodeThread"] = fn((void*)shim_GetExitCodeThread);
    lib.functions["TerminateThread"] = fn((void*)shim_TerminateThread);
    lib.functions["SuspendThread"] = fn((void*)shim_SuspendThread);
    lib.functions["ResumeThread"] = fn((void*)shim_ResumeThread);
    lib.functions["SetThreadPriority"] = fn((void*)shim_SetThreadPriority);
    lib.functions["SetThreadAffinityMask"] = fn((void*)shim_SetThreadAffinityMask);
    lib.functions["GetProcessAffinityMask"] = fn((void*)shim_GetProcessAffinityMask);
    lib.functions["SetProcessAffinityMask"] = fn((void*)shim_SetProcessAffinityMask);
    lib.functions["SwitchToThread"] = fn((void*)shim_SwitchToThread);
    lib.functions["InitializeSListHead"] = fn((void*)shim_InitializeSListHead);
    lib.functions["InterlockedPushEntrySList"] = fn((void*)shim_InterlockedPushEntrySList);
    lib.functions["InitializeSRWLock"] = fn((void*)shim_InitializeSRWLock);
    lib.functions["AcquireSRWLockExclusive"] = fn((void*)shim_AcquireSRWLockExclusive);
    lib.functions["ReleaseSRWLockExclusive"] = fn((void*)shim_ReleaseSRWLockExclusive);
    lib.functions["TryAcquireSRWLockExclusive"] = fn((void*)shim_TryAcquireSRWLockExclusive);
    lib.functions["AcquireSRWLockShared"] = fn((void*)shim_AcquireSRWLockShared);
    lib.functions["ReleaseSRWLockShared"] = fn((void*)shim_ReleaseSRWLockShared);
    lib.functions["TryAcquireSRWLockShared"] = fn((void*)shim_TryAcquireSRWLockShared);
    lib.functions["InitializeConditionVariable"] = fn((void*)shim_InitializeConditionVariable);
    lib.functions["SleepConditionVariableSRW"] = fn((void*)shim_SleepConditionVariableSRW);
    lib.functions["WakeAllConditionVariable"] = fn((void*)shim_WakeAllConditionVariable);
    lib.functions["WakeConditionVariable"] = fn((void*)shim_WakeConditionVariable);
    lib.functions["InitializeCriticalSectionAndSpinCount"] = fn((void*)shim_InitializeCriticalSectionAndSpinCount);
    lib.functions["InitializeCriticalSectionEx"] = fn((void*)shim_InitializeCriticalSectionEx);
    lib.functions["TryEnterCriticalSection"] = fn((void*)shim_TryEnterCriticalSection);
    lib.functions["InitOnceBeginInitialize"] = fn((void*)shim_InitOnceBeginInitialize);
    lib.functions["InitOnceComplete"] = fn((void*)shim_InitOnceComplete);
    lib.functions["TlsAlloc"] = fn((void*)shim_TlsAlloc);
    lib.functions["TlsFree"] = fn((void*)shim_TlsFree);
    lib.functions["TlsGetValue"] = fn((void*)shim_TlsGetValue);
    lib.functions["TlsSetValue"] = fn((void*)shim_TlsSetValue);
    lib.functions["FlsAlloc"] = fn((void*)shim_FlsAlloc);
    lib.functions["FlsFree"] = fn((void*)shim_FlsFree);
    lib.functions["FlsGetValue"] = fn((void*)shim_FlsGetValue);
    lib.functions["FlsSetValue"] = fn((void*)shim_FlsSetValue);
    lib.functions["ConvertThreadToFiber"] = fn((void*)shim_ConvertThreadToFiber);
    lib.functions["ConvertFiberToThread"] = fn((void*)shim_ConvertFiberToThread);
    lib.functions["CreateFiber"] = fn((void*)shim_CreateFiber);
    lib.functions["DeleteFiber"] = fn((void*)shim_DeleteFiber);
    lib.functions["SwitchToFiber"] = fn((void*)shim_SwitchToFiber);
    lib.functions["GetSystemTime"] = fn((void*)shim_GetSystemTime);
    lib.functions["GetSystemTimeAsFileTime"] = fn((void*)shim_GetSystemTimeAsFileTime);
    lib.functions["FileTimeToSystemTime"] = fn((void*)shim_FileTimeToSystemTime);
    lib.functions["SystemTimeToFileTime"] = fn((void*)shim_SystemTimeToFileTime);
    lib.functions["SystemTimeToTzSpecificLocalTime"] = fn((void*)shim_SystemTimeToTzSpecificLocalTime);
    lib.functions["GetTimeZoneInformation"] = fn((void*)shim_GetTimeZoneInformation);
    lib.functions["GetDateFormatW"] = fn((void*)shim_GetDateFormatW);
    lib.functions["GetTimeFormatW"] = fn((void*)shim_GetTimeFormatW);
    lib.functions["GetVersionExA"] = fn((void*)shim_GetVersionExA);
    lib.functions["GetVersionExW"] = fn((void*)shim_GetVersionExA);
    lib.functions["VerifyVersionInfoW"] = fn((void*)shim_VerifyVersionInfoW);
    lib.functions["VerSetConditionMask"] = fn((void*)shim_VerSetConditionMask);
    lib.functions["GlobalLock"] = fn((void*)shim_GlobalLock);
    lib.functions["GlobalUnlock"] = fn((void*)shim_GlobalUnlock);
    lib.functions["GlobalMemoryStatusEx"] = fn((void*)shim_GlobalMemoryStatusEx);
    lib.functions["HeapLock"] = fn((void*)shim_HeapLock);
    lib.functions["HeapUnlock"] = fn((void*)shim_HeapUnlock);
    lib.functions["HeapWalk"] = fn((void*)shim_HeapWalk);
    lib.functions["HeapQueryInformation"] = fn((void*)shim_HeapQueryInformation);
    lib.functions["HeapSetInformation"] = fn((void*)shim_HeapSetInformation);
    lib.functions["HeapSize"] = fn((void*)shim_HeapSize);
    lib.functions["HeapReAlloc"] = fn((void*)shim_HeapReAlloc);
    lib.functions["GetProcessHeaps"] = fn((void*)shim_GetProcessHeaps);
    lib.functions["IsBadWritePtr"] = fn((void*)shim_IsBadWritePtr);
    lib.functions["DeviceIoControl"] = fn((void*)shim_DeviceIoControl);
    lib.functions["ProcessIdToSessionId"] = fn((void*)shim_ProcessIdToSessionId);
    lib.functions["OutputDebugStringW"] = fn((void*)shim_OutputDebugStringW);
    lib.functions["PeekNamedPipe"] = fn((void*)shim_PeekNamedPipe);
    lib.functions["SetConsoleCtrlHandler"] = fn((void*)shim_SetConsoleCtrlHandler);
    lib.functions["SetConsoleMode"] = fn((void*)shim_SetConsoleMode);
    lib.functions["GetConsoleMode"] = fn((void*)shim_GetConsoleMode);
    lib.functions["GetConsoleOutputCP"] = fn((void*)shim_GetConsoleOutputCP);
    lib.functions["GetOEMCP"] = fn((void*)shim_GetOEMCP);
    lib.functions["GetCPInfo"] = fn((void*)shim_GetCPInfo);
    lib.functions["GetCurrentThread"] = fn((void*)shim_GetCurrentThread);
    lib.functions["CompareStringW"] = fn((void*)shim_CompareStringW);
    lib.functions["LCMapStringW"] = fn((void*)shim_LCMapStringW);
    lib.functions["GetStringTypeW"] = fn((void*)shim_GetStringTypeW);
    lib.functions["FreeEnvironmentStringsW"] = fn((void*)shim_FreeEnvironmentStringsW);
    lib.functions["GetEnvironmentStringsW"] = fn((void*)shim_GetEnvironmentStringsW);
    lib.functions["CopyFileExW"] = fn((void*)shim_CopyFileExW);
    lib.functions["CreateDirectoryW"] = fn((void*)shim_CreateDirectoryW);
    lib.functions["RemoveDirectoryA"] = fn((void*)shim_RemoveDirectoryA);
    lib.functions["RemoveDirectoryW"] = fn((void*)shim_RemoveDirectoryW);
    lib.functions["DeleteFileW"] = fn((void*)shim_DeleteFileW);
    lib.functions["MoveFileExW"] = fn((void*)shim_MoveFileExW);
    lib.functions["ReplaceFileW"] = fn((void*)shim_ReplaceFileW);
    lib.functions["CreateSymbolicLinkW"] = fn((void*)shim_CreateSymbolicLinkW);
    lib.functions["SetFileAttributesW"] = fn((void*)shim_SetFileAttributesW);
    lib.functions["SetCurrentDirectoryW"] = fn((void*)shim_SetCurrentDirectoryW);
    lib.functions["GetFullPathNameW"] = fn((void*)shim_GetFullPathNameW);
    lib.functions["CreateProcessW"] = fn((void*)shim_CreateProcessW);
    lib.functions["FindFirstFileExW"] = fn((void*)shim_FindFirstFileExW);
    lib.functions["FindFirstFileW"] = fn((void*)shim_FindFirstFileW);
    lib.functions["FindNextFileW"] = fn((void*)shim_FindNextFileW);
    lib.functions["GetFileAttributesExW"] = fn((void*)shim_GetFileAttributesExW);
    lib.functions["GetFileAttributesW"] = fn((void*)shim_GetFileAttributesW);
    lib.functions["GetFileInformationByHandle"] = fn((void*)shim_GetFileInformationByHandle);
    lib.functions["GetFileTime"] = fn((void*)shim_GetFileTime);
    lib.functions["SetFileTime"] = fn((void*)shim_SetFileTime);
    lib.functions["GetFileType"] = fn((void*)shim_GetFileType);
    lib.functions["GetDiskFreeSpaceA"] = fn((void*)shim_GetDiskFreeSpaceA);
    lib.functions["GetDiskFreeSpaceExW"] = fn((void*)shim_GetDiskFreeSpaceExW);
    lib.functions["GetDriveTypeW"] = fn((void*)shim_GetDriveTypeW);
    lib.functions["FlushFileBuffers"] = fn((void*)shim_FlushFileBuffers);
    lib.functions["SetEndOfFile"] = fn((void*)shim_SetEndOfFile);
    lib.functions["SetHandleInformation"] = fn((void*)shim_SetHandleInformation);
    lib.functions["SetErrorMode"] = fn((void*)shim_SetErrorMode);
    lib.functions["FindResourceA"] = fn((void*)shim_FindResourceA);
    lib.functions["LoadResource"] = fn((void*)shim_LoadResource);
    lib.functions["LockResource"] = fn((void*)shim_LockResource);
    lib.functions["SizeofResource"] = fn((void*)shim_SizeofResource);
    lib.functions["MulDiv"] = fn((void*)shim_MulDiv);
    lib.functions["ReadConsoleA"] = fn((void*)shim_ReadConsoleA);
    lib.functions["ReadConsoleW"] = fn((void*)shim_ReadConsoleW);
    lib.functions["WriteConsoleW"] = fn((void*)shim_WriteConsoleW);
    lib.functions["GetProductInfo"] = fn((void*)shim_GetProductInfo);
    lib.functions["RtlCaptureContext"] = fn((void*)shim_RtlCaptureContext);
    lib.functions["RtlCaptureStackBackTrace"] = fn((void*)shim_RtlCaptureStackBackTrace);
    lib.functions["RtlLookupFunctionEntry"] = fn((void*)shim_RtlLookupFunctionEntry);
    lib.functions["RtlPcToFileHeader"] = fn((void*)shim_RtlPcToFileHeader);
    lib.functions["RtlUnwind"] = fn((void*)shim_RtlUnwind);
    lib.functions["RtlUnwindEx"] = fn((void*)shim_RtlUnwindEx);
    lib.functions["RtlVirtualUnwind"] = fn((void*)shim_RtlVirtualUnwind);
    lib.functions["EncodePointer"] = fn((void*)shim_EncodePointer);
    lib.functions["DecodePointer"] = fn((void*)shim_DecodePointer);
    lib.functions["SetStdHandle"] = fn((void*)shim_SetStdHandle);
    lib.functions["GetFileSizeEx"] = fn((void*)stub_GetFileSizeEx);
    lib.functions["SetFilePointer"] = fn((void*)stub_SetFilePointer);
    lib.functions["SetFilePointerEx"] = fn((void*)stub_SetFilePointerEx);
    lib.functions["CreateNamedPipeW"] = fn((void*)shim_CreateNamedPipeW);
    lib.functions["ConnectNamedPipe"] = fn((void*)shim_ConnectNamedPipe);
    lib.functions["WaitNamedPipeW"] = fn((void*)shim_WaitNamedPipeW);
    lib.functions["CallNamedPipeW"] = fn((void*)shim_CallNamedPipeW);
    lib.functions["CreatePipe"] = fn((void*)shim_CreatePipe);
    lib.functions["TransactNamedPipe"] = fn((void*)shim_TransactNamedPipe);
}

} // namespace win32
} // namespace metalsharp
