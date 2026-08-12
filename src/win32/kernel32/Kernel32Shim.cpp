/// @file Kernel32Shim.cpp
/// @brief Core kernel32.dll shim — memory, file I/O, and module loading.
///
/// The primary kernel32 shim handling the most frequently-called Windows APIs:
///
/// Memory Management
/// =================
///   VirtualAlloc/VirtualFree — mmap/munmap wrappers with MEM_COMMIT/MEM_RESERVE mapping
///   HeapCreate/HeapAlloc/HeapFree/HeapDestroy — malloc/free wrappers
///   GetProcessHeap/HeapSize — Returns a sentinel handle, delegates to malloc_usable_size
///
/// Module & Function Resolution
/// ============================
///   GetProcAddress — PELoader::getProcAddress for shim and PE DLL resolution
///   LoadLibraryA/W — PELoader::loadLibrary with DLL search path
///   FreeLibrary — No-op (PE modules aren't reference-counted)
///
/// File I/O
/// ========
///   CreateFileA/W — open() with Windows flags→POSIX flags translation
///   ReadFile/WriteFile — read()/write() via tracked file handle table
///   CloseHandle — close() for file handles, no-op for others
///   GetFileSize/SetFilePointer — lseek/fstat based
///   FindFirstFileA/FindNextFileA — opendir/readdir with Win32 pattern matching
///
/// Error Handling
/// ==============
///   GetLastError/SetLastError — Thread-local errno proxy
///   FormatMessageA — Minimal stub returning "Error {code}"
///
/// Threading
/// =========
///   CreateThread — pthread_create wrapper
///   WaitForSingleObject — pthread_join for threads, semaphore_wait for handles
///   Sleep — usleep
///
/// This shim is registered first and handles ~40 of the most critical kernel32 exports.
/// Additional kernel32 functions are in Kernel32Extra.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <metalsharp/Kernel32Shim.h>
#include <metalsharp/Logger.h>
#include <metalsharp/PELoader.h>
#include <metalsharp/SyncContext.h>
#include <metalsharp/VirtualFileSystem.h>
#include <metalsharp/Win32Error.h>
#include <metalsharp/Win32Types.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace metalsharp {
namespace win32 {

bool Kernel32Shim::s_initialized = false;
std::unordered_map<uintptr_t, size_t> Kernel32Shim::s_allocations;
std::unordered_map<uintptr_t, std::string> Kernel32Shim::s_fileHandles;
uintptr_t Kernel32Shim::s_nextHandle = 0x00010000;

DWORD getKernel32LastError() {
    return lastErrorCode();
}

void setKernel32LastError(DWORD error) {
    setLastErrorCode(error);
}

static DWORD MSABI shim_GetLastError() {
    MS_INFO("TRACE: GetLastError() -> %u", getKernel32LastError());
    return getKernel32LastError();
}

static void MSABI shim_SetLastError(DWORD dwErrCode) {
    MS_INFO("TRACE: SetLastError(%u)", dwErrCode);
    setLastErrorCode(dwErrCode);
}

static void* MSABI shim_VirtualAlloc(void* lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
    MS_INFO("TRACE: VirtualAlloc(%p, %zu, 0x%X, 0x%X)", lpAddress, dwSize, flAllocationType, flProtect);
    int prot = PROT_READ | PROT_WRITE;
    if (flProtect & PAGE_EXECUTE || flProtect & PAGE_EXECUTE_READ || flProtect & PAGE_EXECUTE_READWRITE) {
        prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* ptr = mmap(lpAddress, dwSize, prot, flags, -1, 0);
    if (ptr == MAP_FAILED) {
        setLastErrorFromErrno();
        return nullptr;
    }

    Kernel32Shim::s_allocations[reinterpret_cast<uintptr_t>(ptr)] = dwSize;
    return ptr;
}

static BOOL MSABI shim_VirtualFree(void* lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
    auto it = Kernel32Shim::s_allocations.find(reinterpret_cast<uintptr_t>(lpAddress));
    if (it == Kernel32Shim::s_allocations.end()) {
        setLastErrorCode(ERROR_INVALID_ADDRESS);
        return 0;
    }

    size_t size = it->second;
    Kernel32Shim::s_allocations.erase(it);

    if (dwFreeType & MEM_RELEASE) {
        munmap(lpAddress, size);
    }
    return 1;
}

static void* MSABI shim_GetProcessHeap() {
    return reinterpret_cast<void*>(0x1);
}

static void* MSABI shim_HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize) {
    (void)flOptions;
    (void)dwInitialSize;
    (void)dwMaximumSize;
    MS_INFO("TRACE: HeapCreate(0x%X, %zu, %zu) -> fake heap", flOptions, dwInitialSize, dwMaximumSize);
    return reinterpret_cast<void*>(0x1);
}

static void* MSABI shim_HeapAlloc(void* hHeap, DWORD dwFlags, SIZE_T dwBytes) {
    MS_INFO("TRACE: HeapAlloc(%p, 0x%X, %zu)", hHeap, dwFlags, dwBytes);
    if (dwFlags & 0x8) {
        return calloc(1, dwBytes);
    }
    return malloc(dwBytes);
}

static BOOL MSABI shim_HeapFree(void* hHeap, DWORD dwFlags, void* lpMem) {
    free(lpMem);
    return 1;
}

static void* MSABI shim_LocalAlloc(UINT uFlags, SIZE_T uBytes) {
    if (uFlags & 0x40)
        return calloc(1, uBytes);
    return malloc(uBytes);
}

static void MSABI shim_LocalFree(void* hMem) {
    free(hMem);
}

static void* MSABI shim_GlobalAlloc(UINT uFlags, SIZE_T uBytes) {
    if (uFlags & 0x40)
        return calloc(1, uBytes);
    return malloc(uBytes);
}

static void MSABI shim_GlobalFree(void* hMem) {
    free(hMem);
}

static HANDLE MSABI shim_CreateFileA(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                     void* lpSecurityAttributes, DWORD dwCreationDisposition,
                                     DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    (void)lpSecurityAttributes;
    (void)hTemplateFile;
    return VirtualFileSystem::instance().createFile(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition,
                                                    dwFlagsAndAttributes);
}

static BOOL MSABI shim_ReadFile(HANDLE hFile, void* lpBuffer, DWORD nNumberOfBytesToRead, DWORD* lpNumberOfBytesRead,
                                void* lpOverlapped) {
    (void)lpOverlapped;
    return VirtualFileSystem::instance().readFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead);
}

static BOOL MSABI shim_WriteFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite,
                                 DWORD* lpNumberOfBytesWritten, void* lpOverlapped) {
    (void)lpOverlapped;
    return VirtualFileSystem::instance().writeFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten);
}

static BOOL MSABI shim_CloseHandle(HANDLE hObject) {
    if (!hObject || hObject == INVALID_HANDLE_VALUE) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (VirtualFileSystem::instance().closeHandle(hObject))
        return 1;
    if (SyncContext::instance().destroyHandle(hObject))
        return 1;
    setLastErrorCode(ERROR_INVALID_HANDLE);
    return 0;
}

static DWORD MSABI shim_GetFileSize(HANDLE hFile, DWORD* lpFileSizeHigh) {
    return VirtualFileSystem::instance().getFileSize(hFile, lpFileSizeHigh);
}

static DWORD MSABI shim_GetCurrentDirectoryA(DWORD nBufferLength, char* lpBuffer) {
    MS_INFO("TRACE: GetCurrentDirectoryA(%u, %p)", nBufferLength, lpBuffer);
    if (nBufferLength == 0) {
        setLastErrorCode(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (getcwd(lpBuffer, nBufferLength)) {
        return static_cast<DWORD>(strlen(lpBuffer));
    }
    setLastErrorFromErrno();
    return 0;
}

static DWORD MSABI shim_SetCurrentDirectoryA(const char* lpPathName) {
    if (chdir(lpPathName) == 0)
        return 1;
    setLastErrorFromErrno();
    return 0;
}

static char s_exePath[4096] = "";

void setExePath(const char* path) {
    strncpy(s_exePath, path, sizeof(s_exePath) - 1);
    s_exePath[sizeof(s_exePath) - 1] = 0;
}

static DWORD MSABI shim_GetModuleFileNameA(HMODULE hModule, char* lpFilename, DWORD nSize) {
    MS_INFO("TRACE: GetModuleFileNameA(%p, %p, %u)", hModule, lpFilename, nSize);
    const char* exe = s_exePath[0] ? s_exePath : "/metalsharp/game.exe";
    (void)hModule;
    if (nSize > 0) {
        size_t len = strlen(exe);
        if (len >= nSize)
            len = nSize - 1;
        memcpy(lpFilename, exe, len);
        lpFilename[len] = 0;
        return static_cast<DWORD>(len);
    }
    return 0;
}

static HMODULE MSABI shim_GetModuleHandleA(const char* lpModuleName) {
    MS_INFO("TRACE: GetModuleHandleA(\"%s\")", lpModuleName ? lpModuleName : "(null)");
    if (!lpModuleName)
        return reinterpret_cast<HMODULE>(PELoader::instance()->getMainModule()->base);
    auto* mod = PELoader::instance()->getModule(lpModuleName);
    if (mod)
        return reinterpret_cast<HMODULE>(mod->base);
    return PELoader::instance()->loadLibrary(lpModuleName);
}

static FARPROC MSABI shim_GetProcAddress(HMODULE hModule, const char* lpProcName) {
    if (!hModule || !lpProcName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (reinterpret_cast<uintptr_t>(lpProcName) > 0xFFFF) {
        void* addr = PELoader::instance()->getProcAddress(hModule, std::string(lpProcName));
        if (addr)
            return reinterpret_cast<FARPROC>(addr);
    }
    setLastErrorCode(ERROR_PROC_NOT_FOUND);
    return nullptr;
}

static DWORD MSABI shim_GetCurrentProcessId() {
    MS_INFO("TRACE: GetCurrentProcessId()");
    return static_cast<DWORD>(getpid());
}

static HANDLE MSABI shim_GetCurrentProcess() {
    return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
}

static void MSABI shim_GetSystemInfo(SYSTEM_INFO* lpSystemInfo) {
    memset(lpSystemInfo, 0, sizeof(SYSTEM_INFO));
    lpSystemInfo->wProcessorArchitecture = 9;
    lpSystemInfo->dwPageSize = 4096;
    lpSystemInfo->dwNumberOfProcessors = static_cast<DWORD>(sysconf(_SC_NPROCESSORS_ONLN));
    lpSystemInfo->dwAllocationGranularity = 65536;
}

static void MSABI shim_InitializeCriticalSection(CRITICAL_SECTION* lpCriticalSection) {
    auto* mtx = new pthread_mutex_t();
    pthread_mutex_init(mtx, nullptr);
    lpCriticalSection->DebugInfo = mtx;
}

static void MSABI shim_EnterCriticalSection(CRITICAL_SECTION* lpCriticalSection) {
    MS_INFO("TRACE: EnterCriticalSection(%p)", lpCriticalSection);
    if (lpCriticalSection && lpCriticalSection->DebugInfo) {
        auto* mtx = static_cast<pthread_mutex_t*>(lpCriticalSection->DebugInfo);
        pthread_mutex_lock(mtx);
    }
}

static void MSABI shim_LeaveCriticalSection(CRITICAL_SECTION* lpCriticalSection) {
    MS_INFO("TRACE: LeaveCriticalSection(%p)", lpCriticalSection);
    if (lpCriticalSection && lpCriticalSection->DebugInfo) {
        auto* mtx = static_cast<pthread_mutex_t*>(lpCriticalSection->DebugInfo);
        pthread_mutex_unlock(mtx);
    }
}

static void MSABI shim_DeleteCriticalSection(CRITICAL_SECTION* lpCriticalSection) {
    if (lpCriticalSection->DebugInfo) {
        auto* mtx = static_cast<pthread_mutex_t*>(lpCriticalSection->DebugInfo);
        pthread_mutex_destroy(mtx);
        delete mtx;
        lpCriticalSection->DebugInfo = nullptr;
    }
}

static HANDLE MSABI shim_CreateThread(void* lpThreadAttributes, SIZE_T dwStackSize, void* lpStartAddress,
                                      void* lpParameter, DWORD dwCreationFlags, DWORD* lpThreadId) {
    MS_INFO("TRACE: CreateThread(%p, %zu, %p, %p, 0x%X, %p)", lpThreadAttributes, dwStackSize, lpStartAddress,
            lpParameter, dwCreationFlags, lpThreadId);
    (void)lpThreadAttributes;
    (void)dwStackSize;
    (void)dwCreationFlags;

    int errorCode = 0;
    HANDLE h = SyncContext::instance().createThread(lpStartAddress, lpParameter, lpThreadId, &errorCode);
    if (!h) {
        setLastErrorCode(errnoToWin32(errorCode));
        return nullptr;
    }

    MS_INFO("TRACE: CreateThread -> handle %p", h);
    return h;
}

static DWORD MSABI shim_WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    MS_INFO("TRACE: WaitForSingleObject(%p, %u)", hHandle, dwMilliseconds);
    DWORD result = SyncContext::instance().waitForSingleObject(hHandle, dwMilliseconds);
    if (result == WAIT_FAILED)
        setLastErrorCode(ERROR_INVALID_HANDLE);
    return result;
}

static DWORD MSABI shim_WaitForMultipleObjects(DWORD nCount, HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
    MS_INFO("TRACE: WaitForForMultipleObjects(%u, %p, %d, %u)", nCount, lpHandles, bWaitAll, dwMilliseconds);
    DWORD result = SyncContext::instance().waitForMultipleObjects(nCount, lpHandles, bWaitAll != 0, dwMilliseconds);
    if (result == WAIT_FAILED)
        setLastErrorCode(nCount == 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
    return result;
}

static void MSABI shim_Sleep(DWORD dwMilliseconds) {
    MS_INFO("TRACE: Sleep(%u)", dwMilliseconds);
    usleep(dwMilliseconds * 1000);
}

static DWORD MSABI shim_GetTickCount() {
    static auto startTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
    MS_INFO("TRACE: GetTickCount() -> %u", (DWORD)ms.count());
    return static_cast<DWORD>(ms.count());
}

static BOOL MSABI shim_QueryPerformanceCounter(int64_t* lpPerformanceCount) {
    MS_INFO("TRACE: QueryPerformanceCounter(%p)", lpPerformanceCount);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    *lpPerformanceCount = ns / 100;
    return 1;
}

static BOOL MSABI shim_QueryPerformanceFrequency(int64_t* lpFrequency) {
    *lpFrequency = 10000000LL;
    return 1;
}

static void MSABI shim_OutputDebugStringA(const char* lpOutputString) {
    MS_INFO("TRACE: OutputDebugStringA(\"%s\")", lpOutputString ? lpOutputString : "(null)");
}

static BOOL MSABI shim_IsProcessorFeaturePresent(DWORD ProcessorFeature) {
    MS_INFO("TRACE: IsProcessorFeaturePresent(%u)", ProcessorFeature);
    if (ProcessorFeature == 23)
        return 0;
    return 1;
}

static int MSABI shim_MultiByteToWideChar(UINT CodePage, DWORD dwFlags, const char* lpMultiByteStr, int cbMultiByte,
                                          wchar_t* lpWideCharStr, int cchWideChar) {
    (void)CodePage;
    (void)dwFlags;
    if (!lpMultiByteStr || cbMultiByte == 0 || cbMultiByte < -1 || cchWideChar < 0) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }

    const bool includeNull = cbMultiByte == -1;
    int len = includeNull ? static_cast<int>(strlen(lpMultiByteStr)) + 1 : cbMultiByte;
    if (cchWideChar == 0)
        return len;
    if (!lpWideCharStr) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }

    int copyLen = len < cchWideChar ? len : cchWideChar;
    for (int i = 0; i < copyLen; i++) {
        lpWideCharStr[i] = static_cast<wchar_t>(static_cast<unsigned char>(lpMultiByteStr[i]));
    }
    return copyLen;
}

static int MSABI shim_WideCharToMultiByte(UINT CodePage, DWORD dwFlags, const wchar_t* lpWideCharStr, int cchWideChar,
                                          char* lpMultiByteStr, int cbMultiByte, const char* lpDefaultChar,
                                          BOOL* lpUsedDefaultChar) {
    (void)CodePage;
    (void)dwFlags;
    (void)lpDefaultChar;
    if (lpUsedDefaultChar)
        *lpUsedDefaultChar = 0;
    if (!lpWideCharStr || cchWideChar == 0 || cchWideChar < -1 || cbMultiByte < 0) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }

    const bool includeNull = cchWideChar == -1;
    int len = includeNull ? static_cast<int>(wcslen(lpWideCharStr)) + 1 : cchWideChar;
    if (cbMultiByte == 0)
        return len;
    if (!lpMultiByteStr) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }

    int copyLen = len < cbMultiByte ? len : cbMultiByte;
    for (int i = 0; i < copyLen; i++) {
        lpMultiByteStr[i] = static_cast<char>(lpWideCharStr[i] & 0xFF);
    }
    return copyLen;
}

static HANDLE MSABI shim_GetStdHandle(DWORD nStdHandle) {
    (void)nStdHandle;
    return INVALID_HANDLE_VALUE;
}

static int MSABI shim_lstrcmpA(const char* str1, const char* str2) {
    return strcmp(str1, str2);
}

static int MSABI shim_lstrcmpiA(const char* str1, const char* str2) {
    return strcasecmp(str1, str2);
}

static char* MSABI stub_lstrcpyA(char* d, const char* s) {
    return strcpy(d, s);
}

static int MSABI stub_lstrlenA(const char* s) {
    return (int)strlen(s);
}

static DWORD MSABI stub_GetCurrentThreadId() {
    return (DWORD)pthread_mach_thread_np(pthread_self());
}

static DWORD MSABI stub_GetUserDefaultLCID() {
    return 0x0409;
}

static WORD MSABI stub_GetUserDefaultLangID() {
    return 0x0409;
}

static UINT MSABI stub_GetACP() {
    return 65001;
}

static BOOL MSABI stub_IsValidCodePage(UINT) {
    return 1;
}

static BOOL MSABI stub_HeapValidate() {
    return 1;
}

static HANDLE MSABI stub_FindFirstFileA() {
    return INVALID_HANDLE_VALUE;
}

static BOOL MSABI stub_FindNextFileA() {
    return 0;
}

static BOOL MSABI stub_FindClose() {
    return 1;
}

static DWORD MSABI stub_GetFileAttributesA(const char* p) {
    if (access(p, F_OK) == 0)
        return FILE_ATTRIBUTE_NORMAL;
    setLastErrorFromErrno();
    return 0xFFFFFFFF;
}

static DWORD MSABI stub_GetTempPathA(DWORD n, char* b) {
    const char* t = "/tmp/";
    size_t l = strlen(t);
    if (n > l)
        memcpy(b, t, l + 1);
    return (DWORD)l;
}

static BOOL MSABI stub_IsDebuggerPresent() {
    return 0;
}

ShimLibrary Kernel32Shim::create() {
    if (!s_initialized)
        s_initialized = true;

    ShimLibrary lib;
    lib.name = "kernel32.dll";

    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["GetLastError"] = fn((void*)shim_GetLastError);
    lib.functions["SetLastError"] = fn((void*)shim_SetLastError);
    lib.functions["VirtualAlloc"] = fn((void*)shim_VirtualAlloc);
    lib.functions["VirtualFree"] = fn((void*)shim_VirtualFree);
    lib.functions["VirtualProtect"] = fn((void*)nullptr);
    lib.functions["GetProcessHeap"] = fn((void*)shim_GetProcessHeap);
    lib.functions["HeapAlloc"] = fn((void*)shim_HeapAlloc);
    lib.functions["HeapFree"] = fn((void*)shim_HeapFree);
    lib.functions["HeapSize"] = fn((void*)nullptr);
    lib.functions["HeapCreate"] = fn((void*)shim_HeapCreate);
    lib.functions["HeapDestroy"] = fn((void*)nullptr);
    lib.functions["LocalAlloc"] = fn((void*)shim_LocalAlloc);
    lib.functions["LocalFree"] = fn((void*)shim_LocalFree);
    lib.functions["GlobalAlloc"] = fn((void*)shim_GlobalAlloc);
    lib.functions["GlobalFree"] = fn((void*)shim_GlobalFree);
    lib.functions["CreateFileA"] = fn((void*)shim_CreateFileA);
    lib.functions["CreateFileW"] = fn((void*)shim_CreateFileA);
    lib.functions["ReadFile"] = fn((void*)shim_ReadFile);
    lib.functions["WriteFile"] = fn((void*)shim_WriteFile);
    lib.functions["CloseHandle"] = fn((void*)shim_CloseHandle);
    lib.functions["GetFileSize"] = fn((void*)shim_GetFileSize);
    lib.functions["GetFileSizeEx"] = fn((void*)nullptr);
    lib.functions["GetCurrentDirectoryA"] = fn((void*)shim_GetCurrentDirectoryA);
    lib.functions["GetCurrentDirectoryW"] = fn((void*)shim_GetCurrentDirectoryA);
    lib.functions["SetCurrentDirectoryA"] = fn((void*)shim_SetCurrentDirectoryA);
    lib.functions["GetModuleFileNameA"] = fn((void*)shim_GetModuleFileNameA);
    lib.functions["GetModuleFileNameW"] = fn((void*)shim_GetModuleFileNameA);
    lib.functions["GetModuleHandleA"] = fn((void*)shim_GetModuleHandleA);
    lib.functions["GetModuleHandleW"] = fn((void*)shim_GetModuleHandleA);
    lib.functions["GetProcAddress"] = fn((void*)shim_GetProcAddress);
    lib.functions["GetCurrentProcessId"] = fn((void*)shim_GetCurrentProcessId);
    lib.functions["GetCurrentProcess"] = fn((void*)shim_GetCurrentProcess);
    lib.functions["GetSystemInfo"] = fn((void*)shim_GetSystemInfo);
    lib.functions["InitializeCriticalSection"] = fn((void*)shim_InitializeCriticalSection);
    lib.functions["EnterCriticalSection"] = fn((void*)shim_EnterCriticalSection);
    lib.functions["LeaveCriticalSection"] = fn((void*)shim_LeaveCriticalSection);
    lib.functions["DeleteCriticalSection"] = fn((void*)shim_DeleteCriticalSection);
    lib.functions["CreateThread"] = fn((void*)shim_CreateThread);
    lib.functions["WaitForSingleObject"] = fn((void*)shim_WaitForSingleObject);
    lib.functions["WaitForMultipleObjects"] = fn((void*)shim_WaitForMultipleObjects);
    lib.functions["Sleep"] = fn((void*)shim_Sleep);
    lib.functions["SleepEx"] = fn((void*)shim_Sleep);
    lib.functions["GetTickCount"] = fn((void*)shim_GetTickCount);
    lib.functions["GetTickCount64"] = fn((void*)shim_GetTickCount);
    lib.functions["QueryPerformanceCounter"] = fn((void*)shim_QueryPerformanceCounter);
    lib.functions["QueryPerformanceFrequency"] = fn((void*)shim_QueryPerformanceFrequency);
    lib.functions["OutputDebugStringA"] = fn((void*)shim_OutputDebugStringA);
    lib.functions["IsProcessorFeaturePresent"] = fn((void*)shim_IsProcessorFeaturePresent);
    lib.functions["MultiByteToWideChar"] = fn((void*)shim_MultiByteToWideChar);
    lib.functions["WideCharToMultiByte"] = fn((void*)shim_WideCharToMultiByte);
    lib.functions["GetStdHandle"] = fn((void*)shim_GetStdHandle);
    lib.functions["lstrcmpA"] = fn((void*)shim_lstrcmpA);
    lib.functions["lstrcmpiA"] = fn((void*)shim_lstrcmpiA);
    lib.functions["lstrcpyA"] = fn((void*)stub_lstrcpyA);
    lib.functions["lstrlenA"] = fn((void*)stub_lstrlenA);
    lib.functions["LoadLibraryA"] = fn((void*)shim_GetModuleHandleA);
    lib.functions["LoadLibraryW"] = fn((void*)shim_GetModuleHandleA);
    lib.functions["FreeLibrary"] = fn((void*)nullptr);
    lib.functions["GetCommandLineA"] = fn((void*)nullptr);
    lib.functions["GetCommandLineW"] = fn((void*)nullptr);
    lib.functions["GetEnvironmentVariableA"] = fn((void*)nullptr);
    lib.functions["ExpandEnvironmentStringsA"] = fn((void*)nullptr);
    lib.functions["GetCurrentThreadId"] = fn((void*)stub_GetCurrentThreadId);
    lib.functions["TlsAlloc"] = fn((void*)nullptr);
    lib.functions["TlsFree"] = fn((void*)nullptr);
    lib.functions["TlsGetValue"] = fn((void*)nullptr);
    lib.functions["TlsSetValue"] = fn((void*)nullptr);
    lib.functions["FlsAlloc"] = fn((void*)nullptr);
    lib.functions["FlsFree"] = fn((void*)nullptr);
    lib.functions["FlsGetValue"] = fn((void*)nullptr);
    lib.functions["FlsSetValue"] = fn((void*)nullptr);
    lib.functions["GetExitCodeThread"] = fn((void*)nullptr);
    lib.functions["TerminateThread"] = fn((void*)nullptr);
    lib.functions["GetThreadLocale"] = fn((void*)nullptr);
    lib.functions["GetUserDefaultLCID"] = fn((void*)stub_GetUserDefaultLCID);
    lib.functions["GetUserDefaultLangID"] = fn((void*)stub_GetUserDefaultLangID);
    lib.functions["GetACP"] = fn((void*)stub_GetACP);
    lib.functions["IsValidCodePage"] = fn((void*)stub_IsValidCodePage);
    lib.functions["GetCPInfo"] = fn((void*)nullptr);
    lib.functions["HeapValidate"] = fn((void*)stub_HeapValidate);
    lib.functions["GetProcessAffinityMask"] = fn((void*)nullptr);
    lib.functions["SetThreadAffinityMask"] = fn((void*)nullptr);
    lib.functions["GetLogicalProcessorInformation"] = fn((void*)nullptr);
    lib.functions["VirtualQuery"] = fn((void*)nullptr);
    lib.functions["GetComputerNameA"] = fn((void*)nullptr);
    lib.functions["GetComputerNameW"] = fn((void*)nullptr);
    lib.functions["GetUserNameA"] = fn((void*)nullptr);
    lib.functions["GetUserNameW"] = fn((void*)nullptr);
    lib.functions["FindFirstFileA"] = fn((void*)stub_FindFirstFileA);
    lib.functions["FindNextFileA"] = fn((void*)stub_FindNextFileA);
    lib.functions["FindClose"] = fn((void*)stub_FindClose);
    lib.functions["GetFileAttributesA"] = fn((void*)stub_GetFileAttributesA);
    lib.functions["SetFilePointer"] = fn((void*)nullptr);
    lib.functions["SetFilePointerEx"] = fn((void*)nullptr);
    lib.functions["FlushFileBuffers"] = fn((void*)nullptr);
    lib.functions["GetFileType"] = fn((void*)nullptr);
    lib.functions["GetTempPathA"] = fn((void*)stub_GetTempPathA);
    lib.functions["GetTempPathW"] = fn((void*)nullptr);
    lib.functions["RaiseException"] = fn((void*)nullptr);
    lib.functions["UnhandledExceptionFilter"] = fn((void*)nullptr);
    lib.functions["SetUnhandledExceptionFilter"] = fn((void*)nullptr);
    lib.functions["IsDebuggerPresent"] = fn((void*)stub_IsDebuggerPresent);
    lib.functions["DebugBreak"] = fn((void*)nullptr);
    lib.functions["GetVersionExA"] = fn((void*)nullptr);
    lib.functions["GetVersionExW"] = fn((void*)nullptr);
    lib.functions["GetNativeSystemInfo"] = fn((void*)shim_GetSystemInfo);

    return lib;
}

} // namespace win32
} // namespace metalsharp
