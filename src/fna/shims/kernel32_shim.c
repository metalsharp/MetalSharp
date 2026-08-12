#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t DWORD;
typedef uint64_t DWORD64;
typedef int32_t BOOL;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef void* HANDLE;
typedef void* LPVOID;
typedef size_t SIZE_T;
typedef long LONG;
typedef const char* LPCSTR;
typedef const uint16_t* LPCWSTR;
typedef char* LPSTR;
typedef const void* LPCVOID;
typedef uint32_t UINT;
typedef int32_t INT;

static __thread DWORD t_lastError = 0;

DWORD GetLastError(void) {
    return t_lastError;
}
void SetLastError(DWORD code) {
    t_lastError = code;
}

HANDLE GetStdHandle(DWORD nStdHandle) {
    switch (nStdHandle) {
    case 0xFFFFFFF6:
        return (HANDLE)(intptr_t)STDIN_FILENO;
    case 0xFFFFFFF5:
        return (HANDLE)(intptr_t)STDOUT_FILENO;
    case 0xFFFFFFF4:
        return (HANDLE)(intptr_t)STDERR_FILENO;
    default:
        return (HANDLE)(intptr_t)-1;
    }
}

BOOL SetConsoleMode(HANDLE hConsoleHandle, DWORD dwMode) {
    (void)hConsoleHandle;
    (void)dwMode;
    return 1;
}

BOOL GetConsoleMode(HANDLE hConsoleHandle, DWORD* lpMode) {
    (void)hConsoleHandle;
    if (lpMode)
        *lpMode = 0x0007;
    return 1;
}

BOOL AllocConsole(void) {
    return 1;
}
BOOL FreeConsole(void) {
    return 1;
}
UINT GetConsoleCP(void) {
    return 437;
}
UINT GetConsoleOutputCP(void) {
    return 437;
}

UINT GetACP(void) {
    return 65001;
}
UINT GetOEMCP(void) {
    return 437;
}

DWORD GetFileType(HANDLE hFile) {
    int fd = (int)(intptr_t)hFile;
    struct stat st;
    if (fstat(fd, &st) != 0)
        return 0;
    if (S_ISCHR(st.st_mode))
        return 0x0002;
    if (S_ISFIFO(st.st_mode))
        return 0x0003;
    if (S_ISREG(st.st_mode))
        return 0x0001;
    return 0;
}

LPCSTR GetCommandLineA(void) {
    return getenv("METALSHARP_COMMAND_LINE") ? getenv("METALSHARP_COMMAND_LINE") : "";
}

HANDLE GetCurrentProcess(void) {
    return (HANDLE)(intptr_t)-1;
}
HANDLE GetCurrentThread(void) {
    return (HANDLE)(intptr_t)-2;
}
DWORD GetCurrentProcessId(void) {
    return (DWORD)getpid();
}
DWORD GetCurrentThreadId(void) {
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return (DWORD)tid;
}

DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize) {
    const char* val = getenv(lpName);
    if (!val) {
        t_lastError = 203;
        return 0;
    }
    size_t len = strlen(val);
    if (!lpBuffer || nSize == 0)
        return (DWORD)(len + 1);
    if (len >= nSize) {
        t_lastError = 122;
        return (DWORD)(len + 1);
    }
    memcpy(lpBuffer, val, len + 1);
    return (DWORD)len;
}

BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue) {
    if (lpValue) {
        setenv(lpName, lpValue, 1);
    } else {
        unsetenv(lpName);
    }
    return 1;
}

DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize) {
    if (!lpSrc)
        return 0;
    size_t srcLen = strlen(lpSrc);
    if (!lpDst || nSize == 0)
        return (DWORD)(srcLen + 1);
    strncpy(lpDst, lpSrc, nSize - 1);
    lpDst[nSize - 1] = '\0';
    return (DWORD)strlen(lpDst) + 1;
}

DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer) {
    const char* tmp = getenv("TMPDIR");
    if (!tmp)
        tmp = "/tmp";
    size_t len = strlen(tmp);
    if (lpBuffer && nBufferLength > len) {
        strcpy(lpBuffer, tmp);
        if (lpBuffer[len - 1] != '/') {
            lpBuffer[len] = '/';
            lpBuffer[len + 1] = '\0';
            len++;
        }
    }
    return (DWORD)(len + 1);
}

void Sleep(DWORD dwMilliseconds) {
    usleep((useconds_t)dwMilliseconds * 1000);
}

DWORD SleepEx(DWORD dwMilliseconds, BOOL bAlertable) {
    (void)bAlertable;
    usleep((useconds_t)dwMilliseconds * 1000);
    return 0;
}

BOOL QueryPerformanceCounter(int64_t* lpCount) {
    *lpCount = (int64_t)mach_absolute_time();
    return 1;
}

BOOL QueryPerformanceFrequency(int64_t* lpFrequency) {
    static int64_t freq = 0;
    if (!freq) {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        freq = (int64_t)(1e9 / ((double)info.numer / info.denom));
    }
    *lpFrequency = freq;
    return 1;
}

DWORD GetTickCount(void) {
    return (DWORD)(mach_absolute_time() / 1000000);
}

uint64_t GetTickCount64(void) {
    return (uint64_t)(mach_absolute_time() / 1000000);
}

void GetSystemTimeAsFileTime(void* lpFileTime) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = ((uint64_t)ts.tv_sec + 11644473600ULL) * 10000000ULL + (uint64_t)ts.tv_nsec / 100;
    memcpy(lpFileTime, &ft, 8);
}

BOOL GetComputerNameA(LPSTR lpBuffer, DWORD* nSize) {
    const char* name = getenv("HOSTNAME");
    if (!name)
        name = "metalsharp";
    size_t len = strlen(name);
    if (*nSize > len) {
        strcpy(lpBuffer, name);
        *nSize = (DWORD)len;
        return 1;
    }
    *nSize = (DWORD)(len + 1);
    return 0;
}

BOOL GetUserNameA(LPSTR lpBuffer, DWORD* pcbBuffer) {
    const char* user = getenv("USER");
    if (!user)
        user = "player";
    size_t len = strlen(user);
    if (*pcbBuffer > len) {
        strcpy(lpBuffer, user);
        *pcbBuffer = (DWORD)len;
        return 1;
    }
    *pcbBuffer = (DWORD)(len + 1);
    return 0;
}

int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte, void* lpWideCharStr,
                        int cchWideChar) {
    (void)CodePage;
    (void)dwFlags;
    int len = cbMultiByte == 0 ? (int)strlen(lpMultiByteStr) : cbMultiByte;
    if (cchWideChar == 0)
        return len;
    uint16_t* out = (uint16_t*)lpWideCharStr;
    for (int i = 0; i < len && i < cchWideChar; i++)
        out[i] = (uint16_t)(unsigned char)lpMultiByteStr[i];
    if (len >= 0 && len < cchWideChar)
        out[len] = 0;
    return len < cchWideChar ? len : 0;
}

int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, const void* lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr,
                        int cbMultiByte, LPCSTR lpDefaultChar, BOOL* lpUsedDefaultChar) {
    (void)CodePage;
    (void)dwFlags;
    (void)lpDefaultChar;
    if (lpUsedDefaultChar)
        *lpUsedDefaultChar = 0;
    const uint16_t* in = (const uint16_t*)lpWideCharStr;
    int len = cchWideChar == 0 ? 0 : cchWideChar;
    if (cbMultiByte == 0)
        return len;
    for (int i = 0; i < len && i < cbMultiByte - 1; i++) {
        lpMultiByteStr[i] = in[i] < 128 ? (char)in[i] : '?';
    }
    if (len < cbMultiByte)
        lpMultiByteStr[len] = '\0';
    return len;
}

int lstrlenA(LPCSTR lpString) {
    return lpString ? (int)strlen(lpString) : 0;
}

void OutputDebugStringA(LPCSTR lpOutputString) {
    if (lpOutputString)
        fprintf(stderr, "[DEBUG] %s\n", lpOutputString);
}

BOOL IsDebuggerPresent(void) {
    return 0;
}

BOOL CloseHandle(HANDLE hObject) {
    if (!hObject)
        return 0;

    /*
     * GetStdHandle returns borrowed POSIX descriptors in this shim. A file
     * descriptor is process-global, so treating every small HANDLE value as
     * owned would let CloseHandle(GetStdHandle(...)) close the host's stdio
     * (or an unrelated descriptor that has since reused the same number).
     * This shim currently has no API that creates fd-backed HANDLEs; until
     * such an API can register its owned handles, CloseHandle must not infer
     * ownership from the numeric value.
     */
    return 1;
}

void* VirtualAlloc(void* lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
    (void)flAllocationType;
    (void)flProtect;
    int prot = PROT_READ | PROT_WRITE;
    return mmap(lpAddress, dwSize, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

BOOL VirtualFree(void* lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
    (void)dwSize;
    if (dwFreeType & 0x8000) {
        munmap(lpAddress, dwSize);
        return 1;
    }
    return 1;
}

BOOL VirtualProtect(void* lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect) {
    (void)lpAddress;
    (void)dwSize;
    (void)flNewProtect;
    if (lpflOldProtect)
        *lpflOldProtect = 0x40;
    return 1;
}

void* GetProcAddress(void* hModule, LPCSTR lpProcName) {
    (void)hModule;
    void* sym = dlsym(RTLD_DEFAULT, lpProcName);
    if (!sym)
        sym = dlsym(RTLD_DEFAULT, lpProcName);
    return sym;
}

void* LoadLibraryA(LPCSTR lpLibFileName) {
    if (!lpLibFileName)
        return NULL;
    void* h = dlopen(lpLibFileName, RTLD_LAZY);
    if (!h) {
        char buf[512];
        snprintf(buf, sizeof(buf), "@loader_path/%s", lpLibFileName);
        h = dlopen(buf, RTLD_LAZY);
    }
    return h;
}

// Windows passes LoadLibraryW names as null-terminated UTF-16. macOS dlopen
// expects UTF-8, so decode code points instead of passing the wide bytes as a
// narrow string (which would stop at the first embedded NUL).
static uint32_t next_utf16_codepoint(const uint16_t* input, size_t* index) {
    uint32_t codepoint = input[*index];
    (*index)++;

    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
        uint32_t low = input[*index];
        if (low >= 0xDC00 && low <= 0xDFFF) {
            (*index)++;
            return 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
        }
        return 0xFFFD;
    }

    if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
        return 0xFFFD;
    return codepoint;
}

static size_t utf8_codepoint_length(uint32_t codepoint) {
    if (codepoint <= 0x7F)
        return 1;
    if (codepoint <= 0x7FF)
        return 2;
    if (codepoint <= 0xFFFF)
        return 3;
    return 4;
}

static char* utf16_to_utf8(const uint16_t* input) {
    if (!input)
        return NULL;

    size_t output_length = 1;
    for (size_t index = 0; input[index] != 0;) {
        size_t length = utf8_codepoint_length(next_utf16_codepoint(input, &index));
        if (output_length > SIZE_MAX - length)
            return NULL;
        output_length += length;
    }

    char* output = (char*)malloc(output_length);
    if (!output)
        return NULL;

    size_t index = 0;
    size_t output_index = 0;
    while (input[index] != 0) {
        uint32_t codepoint = next_utf16_codepoint(input, &index);
        if (codepoint <= 0x7F) {
            output[output_index++] = (char)codepoint;
        } else if (codepoint <= 0x7FF) {
            output[output_index++] = (char)(0xC0 | (codepoint >> 6));
            output[output_index++] = (char)(0x80 | (codepoint & 0x3F));
        } else if (codepoint <= 0xFFFF) {
            output[output_index++] = (char)(0xE0 | (codepoint >> 12));
            output[output_index++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            output[output_index++] = (char)(0x80 | (codepoint & 0x3F));
        } else {
            output[output_index++] = (char)(0xF0 | (codepoint >> 18));
            output[output_index++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
            output[output_index++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            output[output_index++] = (char)(0x80 | (codepoint & 0x3F));
        }
    }
    output[output_index] = '\0';
    return output;
}

void* LoadLibraryW(LPCWSTR lpLibFileName) {
    char* utf8_name = utf16_to_utf8(lpLibFileName);
    if (!utf8_name)
        return NULL;

    void* handle = LoadLibraryA(utf8_name);
    free(utf8_name);
    return handle;
}

BOOL FreeLibrary(void* hLibModule) {
    (void)hLibModule;
    return 1;
}

void* GetModuleHandleA(LPCSTR lpModuleName) {
    (void)lpModuleName;
    return (void*)(intptr_t)1;
}

DWORD GetModuleFileNameA(void* hModule, LPSTR lpFilename, DWORD nSize) {
    (void)hModule;
    if (!lpFilename || nSize == 0)
        return 0;
#if defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", lpFilename, nSize - 1);
    if (len > 0) {
        lpFilename[len] = '\0';
        return (DWORD)len;
    }
    // readlink failed (e.g. /proc unavailable); fall back to the cwd below.
#elif defined(__APPLE__)
    // _NSGetExecutablePath() returns the main executable's launch path but
    // does not resolve symlinks. Canonicalize with realpath() so callers see
    // the actual on-disk binary (e.g. the one inside a .app bundle).
    uint32_t bufSize = nSize - 1;
    if (_NSGetExecutablePath(lpFilename, &bufSize) == 0) {
        char resolved[PATH_MAX];
        if (realpath(lpFilename, resolved) != NULL) {
            size_t rlen = strlen(resolved);
            if (rlen <= nSize - 1) {
                memcpy(lpFilename, resolved, rlen + 1);
                return (DWORD)rlen;
            }
        }
        // realpath() failed or the resolved path does not fit; the launch
        // path returned by _NSGetExecutablePath() is still the executable.
        return (DWORD)strlen(lpFilename);
    }
    // Buffer too small: _NSGetExecutablePath() stored the required size in
    // bufSize. Match the Win32 contract of returning nSize on truncation.
    lpFilename[0] = '\0';
    return nSize;
#endif
    // Fallback (Linux /proc failure or unsupported platform): cwd.
    (void)getcwd(lpFilename, nSize);
    lpFilename[nSize - 1] = '\0';
    return (DWORD)strlen(lpFilename);
}
