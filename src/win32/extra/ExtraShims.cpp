/// @file ExtraShims.cpp
/// @brief Windows API shim factories — user32, gdi32, advapi32, shell32, and more.
///
/// This file provides shim factories for the "long tail" of Windows DLLs that games
/// load but only use a handful of functions from. Each createXxxShim() function returns
/// a ShimLibrary with the most commonly-called exports.
///
/// DLL Coverage
/// ============
///
///   user32.dll    — MessageBox, SetCursor, ClipCursor, GetCursorPos, SetWindowText
///   gdi32.dll     — CreateCompatibleDC, SelectObject, GetObjectA, StretchBlt (stubs)
///   advapi32.dll  — Registry functions (delegates to Registry.cpp), event log stubs
///   shell32.dll   — CommandLineToArgvW, SHGetFolderPath (maps to macOS dirs)
///   ole32/oleaut32 — CoInitialize/UnmarshalInterface/VariantChangeType (minimal COM)
///   crypt32.dll   — Certificate open/close stubs
///   bcrypt.dll    — BCryptGenRandom (arc4random_buf), BCryptOpenAlgorithmProvider
///   ws2_32.dll    — WSAStartup/socket/connect/send/recv (delegates to NetworkContext)
///   psapi.dll     — GetProcessMemoryInfo (stub returning zeros)
///   version.dll   — GetFileVersionInfoSize/QueryValue (stub)
///   comctl32.dll  — InitCommonControlsEx (no-op)
///   winhttp.dll   — WinHttpOpen/Connect/SendRequest (stub)
///   dinput8.dll   — DirectInput8Create (returns E_NOTIMPL, games use XInput instead)
///   dwmapi.dll    — DwmIsCompositionEnabled (returns TRUE)
///   shlwapi.dll   — PathAppend/PathRemoveFileSpec (string manipulation)
///   secur32.dll   — GetUserNameExA/W (getpwuid)
///   netapi32.dll  — NetUserGetInfo (stub)
///
/// Design Pattern
/// ==============
///   Each shim factory creates a ShimLibrary and registers export functions.
///   Most implementations are lightweight stubs that return success/zero/NULL
///   to prevent game crashes from unimplemented APIs. Real functionality is
///   only implemented where games actually depend on it.
///
///   Priority: prevent crashes > correct behavior > full API coverage.
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <metalsharp/Logger.h>
#include <metalsharp/NetworkContext.h>
#include <metalsharp/PELoader.h>
#include <metalsharp/Platform.h>
#include <metalsharp/Registry.h>
#include <metalsharp/VirtualFileSystem.h>
#include <metalsharp/Win32Types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#if __has_include(<openssl/rand.h>)
#include <openssl/rand.h>
#endif

namespace metalsharp {
namespace win32 {

static void* MSABI gdi32_CreateCompatibleDC(void*) {
    return reinterpret_cast<void*>(0x9000);
}

static void* MSABI gdi32_CreateDIBSection(void*, const void*, UINT, void** ppv, void*, DWORD) {
    if (ppv)
        *ppv = calloc(1, 1920 * 1080 * 4);
    return reinterpret_cast<void*>(0x9001);
}

static void* MSABI gdi32_CreateFontW(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                     const wchar_t*) {
    return reinterpret_cast<void*>(0x9002);
}

static void* MSABI gdi32_CreateICW(const wchar_t*, const wchar_t*, const wchar_t*, const void*) {
    return reinterpret_cast<void*>(0x9003);
}

static BOOL MSABI gdi32_DeleteDC(void*) {
    return 1;
}

static BOOL MSABI gdi32_DeleteObject(void*) {
    return 1;
}

static void* MSABI gdi32_SelectObject(void*, void* old) {
    return old;
}

static int MSABI gdi32_GetDeviceCaps(void*, int index) {
    switch (index) {
    case 0:
        return 1920;
    case 1:
        return 1080;
    case 12:
        return 96;
    case 88:
        return 32;
    default:
        return 0;
    }
}

static void* MSABI gdi32_GetStockObject(int) {
    return reinterpret_cast<void*>(0x9004);
}

static BOOL MSABI gdi32_GetTextExtentPoint32W(void*, const wchar_t*, int, void* sz) {
    auto* s = reinterpret_cast<DWORD*>(sz);
    s[0] = 8;
    s[1] = 16;
    return 1;
}

static DWORD MSABI gdi32_SetBkColor(void*, DWORD c) {
    return c;
}

static int MSABI gdi32_SetBkMode(void*, int m) {
    return m;
}

static DWORD MSABI gdi32_SetTextColor(void*, DWORD c) {
    return c;
}

static BOOL MSABI gdi32_TextOutW(void*, int, int, const wchar_t*, int) {
    return 1;
}

static BOOL MSABI gdi32_SwapBuffers(void*) {
    return 1;
}

static int MSABI gdi32_ChoosePixelFormat(void*, const void*) {
    return 1;
}

static BOOL MSABI gdi32_SetPixelFormat(void*, int, const void*) {
    return 1;
}

static HANDLE MSABI gdi32_AddFontMemResourceEx(const void*, DWORD, void*, DWORD* cnt) {
    if (cnt)
        *cnt = 1;
    return reinterpret_cast<void*>(0x9005);
}

static BOOL MSABI gdi32_RemoveFontMemResourceEx(void*) {
    return 1;
}

static BOOL MSABI gdi32_BitBlt(void*, int, int, int, int, void*, int, int, DWORD) {
    return 1;
}

static BOOL MSABI gdi32_StretchBlt(void*, int, int, int, int, void*, int, int, int, int, DWORD) {
    return 1;
}

static int MSABI gdi32_DrawTextW(void*, const wchar_t* str, int len, void* rect, UINT) {
    if (rect) {
        auto* r = reinterpret_cast<LONG*>(rect);
        r[0] = 0;
        r[1] = 0;
        r[2] = 200;
        r[3] = 100;
    }
    if (!str)
        return 0;
    return len >= 0 ? len : (int)wcslen(str);
}

static void* MSABI gdi32_CreateFontIndirectW(const void*) {
    return reinterpret_cast<void*>(0x9006);
}

static int MSABI gdi32_GetObjectW(void*, int sz, void* buf) {
    if (buf && sz >= 4) {
        memset(buf, 0, sz);
        *reinterpret_cast<LONG*>(buf) = -13;
    }
    return sz;
}

static void* MSABI gdi32_CreateCompatibleBitmap(void*, int, int) {
    return reinterpret_cast<void*>(0x9007);
}

static BOOL MSABI gdi32_PatBlt(void*, int, int, int, int, DWORD) {
    return 1;
}

static BOOL MSABI gdi32_Rectangle(void*, int, int, int, int) {
    return 1;
}

static int MSABI gdi32_FillRect(void*, const void*, void*) {
    return 1;
}

static void* MSABI gdi32_CreateSolidBrush(DWORD) {
    return reinterpret_cast<void*>(0x9008);
}

static void* MSABI gdi32_CreateBitmap(int, int, UINT, UINT, const void*) {
    return reinterpret_cast<void*>(0x9009);
}

static int MSABI gdi32_SetDIBitsToDevice(void*, int, int, DWORD, DWORD, int, int, UINT, UINT, const void*, const void*,
                                         UINT) {
    return 1;
}

static BOOL MSABI gdi32_GdiFlush() {
    return 1;
}

ShimLibrary createGdi32Shim() {
    ShimLibrary lib;
    lib.name = "GDI32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["CreateCompatibleDC"] = fn((void*)gdi32_CreateCompatibleDC);
    lib.functions["CreateDIBSection"] = fn((void*)gdi32_CreateDIBSection);
    lib.functions["CreateFontW"] = fn((void*)gdi32_CreateFontW);
    lib.functions["CreateICW"] = fn((void*)gdi32_CreateICW);
    lib.functions["DeleteDC"] = fn((void*)gdi32_DeleteDC);
    lib.functions["DeleteObject"] = fn((void*)gdi32_DeleteObject);
    lib.functions["SelectObject"] = fn((void*)gdi32_SelectObject);
    lib.functions["GetDeviceCaps"] = fn((void*)gdi32_GetDeviceCaps);
    lib.functions["GetStockObject"] = fn((void*)gdi32_GetStockObject);
    lib.functions["GetTextExtentPoint32W"] = fn((void*)gdi32_GetTextExtentPoint32W);
    lib.functions["SetBkColor"] = fn((void*)gdi32_SetBkColor);
    lib.functions["SetBkMode"] = fn((void*)gdi32_SetBkMode);
    lib.functions["SetTextColor"] = fn((void*)gdi32_SetTextColor);
    lib.functions["TextOutW"] = fn((void*)gdi32_TextOutW);
    lib.functions["SwapBuffers"] = fn((void*)gdi32_SwapBuffers);
    lib.functions["ChoosePixelFormat"] = fn((void*)gdi32_ChoosePixelFormat);
    lib.functions["SetPixelFormat"] = fn((void*)gdi32_SetPixelFormat);
    lib.functions["AddFontMemResourceEx"] = fn((void*)gdi32_AddFontMemResourceEx);
    lib.functions["RemoveFontMemResourceEx"] = fn((void*)gdi32_RemoveFontMemResourceEx);
    lib.functions["BitBlt"] = fn((void*)gdi32_BitBlt);
    lib.functions["StretchBlt"] = fn((void*)gdi32_StretchBlt);
    lib.functions["DrawTextW"] = fn((void*)gdi32_DrawTextW);
    lib.functions["DrawTextA"] = fn((void*)gdi32_DrawTextW);
    lib.functions["CreateFontIndirectW"] = fn((void*)gdi32_CreateFontIndirectW);
    lib.functions["GetObjectW"] = fn((void*)gdi32_GetObjectW);
    lib.functions["CreateCompatibleBitmap"] = fn((void*)gdi32_CreateCompatibleBitmap);
    lib.functions["PatBlt"] = fn((void*)gdi32_PatBlt);
    lib.functions["Rectangle"] = fn((void*)gdi32_Rectangle);
    lib.functions["FillRect"] = fn((void*)gdi32_FillRect);
    lib.functions["CreateSolidBrush"] = fn((void*)gdi32_CreateSolidBrush);
    lib.functions["CreateBitmap"] = fn((void*)gdi32_CreateBitmap);
    lib.functions["SetDIBitsToDevice"] = fn((void*)gdi32_SetDIBitsToDevice);
    lib.functions["GdiFlush"] = fn((void*)gdi32_GdiFlush);

    return lib;
}

static LONG MSABI advapi32_RegOpenKeyA(void* hKey, const char* subKey, void** key) {
    return Registry::instance().openKey(hKey, subKey ? subKey : "", key);
}

static LONG MSABI advapi32_RegOpenKeyExA(void* hKey, const char* subKey, DWORD ulOptions, DWORD samDesired,
                                         void** key) {
    return Registry::instance().openKeyEx(hKey, subKey ? subKey : "", ulOptions, samDesired, key);
}

static LONG MSABI advapi32_RegOpenKeyExW(void* hKey, const wchar_t* subKey, DWORD ulOptions, DWORD samDesired,
                                         void** key) {
    char narrow[1024];
    if (subKey) {
        int j = 0;
        for (int i = 0; subKey[i] && j < 1023; i++)
            narrow[j++] = (char)(subKey[i] & 0xFF);
        narrow[j] = 0;
    } else {
        narrow[0] = 0;
    }
    return Registry::instance().openKeyEx(hKey, narrow, ulOptions, samDesired, key);
}

static LONG MSABI advapi32_RegCreateKeyExA(void* hKey, const char* subKey, DWORD reserved, char* lpClass,
                                           DWORD dwOptions, DWORD samDesired, void* lpSecurityAttributes, void** key,
                                           void* lpdwDisposition) {
    return Registry::instance().createKeyEx(hKey, subKey ? subKey : "", reserved, lpClass, dwOptions, samDesired,
                                            lpSecurityAttributes, key, lpdwDisposition);
}

static LONG MSABI advapi32_RegCreateKeyExW(void* hKey, const wchar_t* subKey, DWORD reserved, wchar_t* lpClass,
                                           DWORD dwOptions, DWORD samDesired, void* lpSecurityAttributes, void** key,
                                           void* lpdwDisposition) {
    char narrow[1024];
    if (subKey) {
        int j = 0;
        for (int i = 0; subKey[i] && j < 1023; i++)
            narrow[j++] = (char)(subKey[i] & 0xFF);
        narrow[j] = 0;
    } else {
        narrow[0] = 0;
    }
    return Registry::instance().createKeyEx(hKey, narrow, reserved, nullptr, dwOptions, samDesired,
                                            lpSecurityAttributes, key, lpdwDisposition);
}

static LONG MSABI advapi32_RegCloseKey(void* hKey) {
    return Registry::instance().closeKey(hKey);
}

static LONG MSABI advapi32_RegQueryValueExA(void* hKey, const char* valueName, DWORD* lpReserved, DWORD* lpType,
                                            BYTE* lpData, DWORD* lpcbData) {
    (void)lpReserved;
    return Registry::instance().queryValue(hKey, valueName ? valueName : "", lpType, lpData, lpcbData);
}

static LONG MSABI advapi32_RegQueryValueExW(void* hKey, const wchar_t* valueName, DWORD* lpReserved, DWORD* lpType,
                                            BYTE* lpData, DWORD* lpcbData) {
    (void)lpReserved;
    char narrow[256];
    if (valueName) {
        int j = 0;
        for (int i = 0; valueName[i] && j < 255; i++)
            narrow[j++] = (char)(valueName[i] & 0xFF);
        narrow[j] = 0;
    } else {
        narrow[0] = 0;
    }
    return Registry::instance().queryValue(hKey, narrow, lpType, lpData, lpcbData);
}

static LONG MSABI advapi32_RegSetValueExA(void* hKey, const char* valueName, DWORD reserved, DWORD dwType,
                                          const BYTE* lpData, DWORD cbData) {
    (void)reserved;
    return Registry::instance().setValue(hKey, valueName ? valueName : "", dwType, lpData, cbData);
}

static LONG MSABI advapi32_RegSetValueExW(void* hKey, const wchar_t* valueName, DWORD reserved, DWORD dwType,
                                          const BYTE* lpData, DWORD cbData) {
    (void)reserved;
    char narrow[256];
    if (valueName) {
        int j = 0;
        for (int i = 0; valueName[i] && j < 255; i++)
            narrow[j++] = (char)(valueName[i] & 0xFF);
        narrow[j] = 0;
    } else {
        narrow[0] = 0;
    }
    return Registry::instance().setValue(hKey, narrow, dwType, lpData, cbData);
}

static LONG MSABI advapi32_RegDeleteValueA(void* hKey, const char* valueName) {
    return Registry::instance().deleteValue(hKey, valueName ? valueName : "");
}

static LONG MSABI advapi32_RegDeleteKeyA(void* hKey, const char* subKey) {
    return Registry::instance().deleteKey(hKey, subKey ? subKey : "");
}

static LONG MSABI advapi32_RegDeleteKeyW(void* hKey, const wchar_t* subKey) {
    char narrow[1024];
    if (subKey) {
        int j = 0;
        for (int i = 0; subKey[i] && j < 1023; i++)
            narrow[j++] = (char)(subKey[i] & 0xFF);
        narrow[j] = 0;
    } else {
        narrow[0] = 0;
    }
    return Registry::instance().deleteKey(hKey, narrow);
}

static LONG MSABI advapi32_RegEnumKeyExW(void* hKey, DWORD dwIndex, wchar_t* lpName, DWORD* lpcchName,
                                         DWORD* lpReserved, wchar_t* lpClass, DWORD* lpcchClass,
                                         void* lpftLastWriteTime) {
    (void)hKey;
    (void)dwIndex;
    (void)lpName;
    (void)lpcchName;
    (void)lpReserved;
    (void)lpClass;
    (void)lpcchClass;
    (void)lpftLastWriteTime;
    return 2;
}

static LONG MSABI advapi32_RegEnumValueW(void* hKey, DWORD dwIndex, wchar_t* lpValueName, DWORD* lpcchValueName,
                                         DWORD* lpReserved, DWORD* lpType, BYTE* lpData, DWORD* lpcbData) {
    (void)hKey;
    (void)dwIndex;
    (void)lpValueName;
    (void)lpcchValueName;
    (void)lpReserved;
    (void)lpType;
    (void)lpData;
    (void)lpcbData;
    return 2;
}

static BOOL MSABI advapi32_InitializeSecurityDescriptor(void* sd, DWORD) {
    memset(sd, 0, 32);
    return 1;
}

static BOOL MSABI advapi32_SetSecurityDescriptorDacl(void*, BOOL, void*, BOOL) {
    return 1;
}

static void* MSABI advapi32_RegisterEventSourceW(const wchar_t*, const wchar_t*) {
    return reinterpret_cast<void*>(0xA100);
}

static BOOL MSABI advapi32_DeregisterEventSource(void*) {
    return 1;
}

static BOOL MSABI advapi32_ReportEventW(void*, WORD, WORD, DWORD, void*, WORD, DWORD, const wchar_t**, void*) {
    return 1;
}

ShimLibrary createAdvapi32Shim() {
    ShimLibrary lib;
    lib.name = "ADVAPI32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["RegOpenKeyA"] = fn((void*)advapi32_RegOpenKeyA);
    lib.functions["RegOpenKeyExA"] = fn((void*)advapi32_RegOpenKeyExA);
    lib.functions["RegOpenKeyExW"] = fn((void*)advapi32_RegOpenKeyExW);
    lib.functions["RegCreateKeyExA"] = fn((void*)advapi32_RegCreateKeyExA);
    lib.functions["RegCreateKeyExW"] = fn((void*)advapi32_RegCreateKeyExW);
    lib.functions["RegCloseKey"] = fn((void*)advapi32_RegCloseKey);
    lib.functions["RegQueryValueExA"] = fn((void*)advapi32_RegQueryValueExA);
    lib.functions["RegQueryValueExW"] = fn((void*)advapi32_RegQueryValueExW);
    lib.functions["RegSetValueExA"] = fn((void*)advapi32_RegSetValueExA);
    lib.functions["RegSetValueExW"] = fn((void*)advapi32_RegSetValueExW);
    lib.functions["RegDeleteValueA"] = fn((void*)advapi32_RegDeleteValueA);
    lib.functions["RegDeleteKeyA"] = fn((void*)advapi32_RegDeleteKeyA);
    lib.functions["RegDeleteKeyW"] = fn((void*)advapi32_RegDeleteKeyW);
    lib.functions["RegEnumKeyExW"] = fn((void*)advapi32_RegEnumKeyExW);
    lib.functions["RegEnumValueW"] = fn((void*)advapi32_RegEnumValueW);
    lib.functions["InitializeSecurityDescriptor"] = fn((void*)advapi32_InitializeSecurityDescriptor);
    lib.functions["SetSecurityDescriptorDacl"] = fn((void*)advapi32_SetSecurityDescriptorDacl);
    lib.functions["RegisterEventSourceW"] = fn((void*)advapi32_RegisterEventSourceW);
    lib.functions["DeregisterEventSource"] = fn((void*)advapi32_DeregisterEventSource);
    lib.functions["ReportEventW"] = fn((void*)advapi32_ReportEventW);

    return lib;
}

static int MSABI ws2_32_WSAStartup(WORD, void* data) {
    if (data) {
        memset(data, 0, 400);
        reinterpret_cast<WORD*>(data)[0] = 0x0202;
    }
    return 0;
}

static int MSABI ws2_32_WSACleanup() {
    return 0;
}

static int MSABI ws2_32_WSAGetLastError() {
    return (int)NetworkContext::instance().getWsaError();
}

static void* MSABI ws2_32_WSASocketA(int af, int type, int proto, void*, DWORD, DWORD) {
    int sock = socket(af, type, proto);
    if (sock < 0) {
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }
    int id = NetworkContext::instance().allocSocket(sock);
    return reinterpret_cast<void*>(static_cast<intptr_t>(id));
}

static int MSABI ws2_32_closesocket(void* s) {
    int fd = NetworkContext::instance().releaseSocket((int)(intptr_t)s);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    NetworkContext::instance().setWsaError(WSAENOTSOCK);
    return -1;
}

static int MSABI ws2_32_connect(void* s, const void* addr, int len) {
    auto& ctx = NetworkContext::instance();
    int handle = (int)(intptr_t)s;
    int fd = ctx.getFd(handle);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = ::connect(fd, (const sockaddr*)addr, (socklen_t)len);
    if (ret < 0) {
        if (errno == EINPROGRESS) {
            ctx.setWsaError(WSAEWOULDBLOCK);
            ctx.markSocketConnecting(handle, true);
            return -1;
        }
        ctx.setWsaError(ctx.mapErrnoToWsa(errno));
    }
    return ret;
}

static int MSABI ws2_32_send(void* s, const char* buf, int len, int flags) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = (int)::send(fd, buf, (size_t)len, flags);
    if (ret < 0)
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
    return ret;
}

static int MSABI ws2_32_recv(void* s, char* buf, int len, int flags) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = (int)::recv(fd, buf, (size_t)len, flags);
    if (ret < 0)
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
    return ret;
}

static int MSABI ws2_32_bind(void* s, const void* addr, int len) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = ::bind(fd, (const sockaddr*)addr, (socklen_t)len);
    if (ret < 0)
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
    return ret;
}

static int MSABI ws2_32_listen(void* s, int backlog) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = ::listen(fd, backlog);
    if (ret < 0)
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
    return ret;
}

static void* MSABI ws2_32_accept(void* s, void* addr, int* len) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }
    int nfd = ::accept(fd, (sockaddr*)addr, (socklen_t*)len);
    if (nfd < 0) {
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }
    int id = NetworkContext::instance().allocSocket(nfd);
    return reinterpret_cast<void*>(static_cast<intptr_t>(id));
}

static int MSABI ws2_32_shutdown(void* s, int how) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = ::shutdown(fd, how);
    if (ret < 0)
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
    return ret;
}

static int MSABI ws2_32_ioctlsocket(void* s, long cmd, void* argp) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    if (cmd == 0x8004667E) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (*(reinterpret_cast<uint32_t*>(argp)))
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        else
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return 0;
}

static int MSABI ws2_32_getsockname(void* s, void* addr, int* len) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0)
        return -1;
    return ::getsockname(fd, (sockaddr*)addr, (socklen_t*)len);
}

static int MSABI ws2_32_getpeername(void* s, void* addr, int* len) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0)
        return -1;
    return ::getpeername(fd, (sockaddr*)addr, (socklen_t*)len);
}

static int MSABI ws2_32_getsockopt(void* s, int level, int opt, char* val, int* len) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0)
        return -1;
    return ::getsockopt(fd, level, opt, val, (socklen_t*)len);
}

static int MSABI ws2_32_setsockopt(void* s, int level, int opt, const char* val, int len) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0)
        return -1;
    return ::setsockopt(fd, level, opt, val, (socklen_t)len);
}

struct WinFDSet {
    uint32_t fd_count;
    void* fd_array[64];
};

static int MSABI ws2_32_select(int nfds, void* readfds, void* writefds, void* exceptfds, const void* timeout) {
    auto& ctx = NetworkContext::instance();

    fd_set prd, pwr, pex;
    FD_ZERO(&prd);
    FD_ZERO(&pwr);
    FD_ZERO(&pex);

    int maxfd = 0;

    auto translateSet = [&](void* winSet, fd_set& posixSet) {
        if (!winSet)
            return;
        auto* wfd = reinterpret_cast<WinFDSet*>(winSet);
        for (uint32_t i = 0; i < wfd->fd_count; i++) {
            int fd = ctx.getFd((int)(intptr_t)wfd->fd_array[i]);
            if (fd >= 0) {
                FD_SET(fd, &posixSet);
                if (fd > maxfd)
                    maxfd = fd;
            }
        }
    };

    translateSet(readfds, prd);
    translateSet(writefds, pwr);
    translateSet(exceptfds, pex);

    struct timeval tv;
    struct timeval* ptv = nullptr;
    if (timeout) {
        /* Winsock TIMEVAL is {LONG tv_sec; LONG tv_usec} — two 32-bit
         * fields, not a single microsecond count.  Read both fields and
         * normalize an out-of-range usec into tv_sec so sub-second
         * timeouts such as {0, 500000} actually wait 500 ms instead of
         * returning immediately. */
        auto* wt = static_cast<const LONG*>(timeout);
        tv.tv_sec = wt[0];
        tv.tv_usec = wt[1];
        if (tv.tv_usec >= 1000000) {
            tv.tv_sec += tv.tv_usec / 1000000;
            tv.tv_usec %= 1000000;
        }
        ptv = &tv;
    }

    int ret = ::select(maxfd + 1, readfds ? &prd : nullptr, writefds ? &pwr : nullptr, exceptfds ? &pex : nullptr, ptv);
    if (ret < 0) {
        ctx.setWsaError(ctx.mapErrnoToWsa(errno));
        return -1;
    }

    auto translateBack = [&](void* winSet, fd_set& posixSet) {
        if (!winSet)
            return;
        auto* wfd = reinterpret_cast<WinFDSet*>(winSet);
        uint32_t outCount = 0;
        for (uint32_t i = 0; i < wfd->fd_count; i++) {
            int fd = ctx.getFd((int)(intptr_t)wfd->fd_array[i]);
            if (fd >= 0 && FD_ISSET(fd, &posixSet)) {
                wfd->fd_array[outCount++] = wfd->fd_array[i];
            }
        }
        wfd->fd_count = outCount;
    };

    translateBack(readfds, prd);
    translateBack(writefds, pwr);
    translateBack(exceptfds, pex);

    return ret;
}

static int MSABI ws2_32_getaddrinfo(const char* node, const char* service, const void* hints, void** res) {
    int ret = getaddrinfo(node, service, (const addrinfo*)hints, (addrinfo**)res);
    if (ret != 0) {
        NetworkContext::instance().setWsaError(WSAEHOSTUNREACH);
    }
    return ret;
}

static void MSABI ws2_32_freeaddrinfo(void* p) {
    freeaddrinfo((addrinfo*)p);
}

static uint16_t MSABI ws2_32_htons(uint16_t v) {
    return __builtin_bswap16(v);
}
static uint16_t MSABI ws2_32_ntohs(uint16_t v) {
    return __builtin_bswap16(v);
}
static uint32_t MSABI ws2_32_htonl(uint32_t v) {
    return __builtin_bswap32(v);
}
static uint32_t MSABI ws2_32_ntohl(uint32_t v) {
    return __builtin_bswap32(v);
}
static uint32_t MSABI ws2_32_inet_addr(const char* cp) {
    return inet_addr(cp);
}
static char* MSABI ws2_32_inet_ntoa(struct in_addr in) {
    return inet_ntoa(in);
}

static struct hostent g_hostent;
static char* g_hostentAliases[2] = {nullptr, nullptr};
static char* g_hostentAddrList[2] = {nullptr};
static uint32_t g_hostentAddr;
static pthread_mutex_t g_hostentMutex = PTHREAD_MUTEX_INITIALIZER;

static void* MSABI ws2_32_gethostbyname(const char* name) {
    if (!name)
        return nullptr;
    pthread_mutex_lock(&g_hostentMutex);
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    int rc = getaddrinfo(name, nullptr, &hints, &res);
    if (rc != 0 || !res) {
        pthread_mutex_unlock(&g_hostentMutex);
        NetworkContext::instance().setWsaError(WSAEHOSTUNREACH);
        return nullptr;
    }
    auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    g_hostentAddr = sa->sin_addr.s_addr;
    g_hostent.h_name = const_cast<char*>(name);
    g_hostent.h_aliases = g_hostentAliases;
    g_hostent.h_addrtype = AF_INET;
    g_hostent.h_length = 4;
    g_hostentAddrList[0] = reinterpret_cast<char*>(&g_hostentAddr);
    g_hostentAddrList[1] = nullptr;
    g_hostent.h_addr_list = g_hostentAddrList;
    freeaddrinfo(res);
    pthread_mutex_unlock(&g_hostentMutex);
    return &g_hostent;
}

static int MSABI ws2_32_WSAIoctl(void* s, DWORD dwIoControlCode, void* lpvInBuffer, DWORD cbInBuffer,
                                 void* lpvOutBuffer, DWORD cbOutBuffer, DWORD* lpcbBytesReturned, void*, void*) {
    auto& ctx = NetworkContext::instance();
    if (dwIoControlCode == SIO_GET_EXTENSION_FUNCTION_POINTER) {
        if (lpvInBuffer && cbInBuffer >= sizeof(uint32_t)) {
            uint32_t guid0 = *reinterpret_cast<uint32_t*>(lpvInBuffer);
            if (lpvOutBuffer && cbOutBuffer >= sizeof(void*)) {
                *reinterpret_cast<void**>(lpvOutBuffer) = reinterpret_cast<void*>(static_cast<intptr_t>(guid0));
                if (lpcbBytesReturned)
                    *lpcbBytesReturned = sizeof(void*);
                return 0;
            }
        }
    }

    int fd = ctx.getFd((int)(intptr_t)s);
    if (fd < 0)
        return -1;

    if (dwIoControlCode == 0x4004667F || dwIoControlCode == 0x8004667D) {
        int arg = 0;
        if (dwIoControlCode == 0x8004667D)
            arg = 1;
        int ret = ::ioctl(fd, FIONBIO, &arg);
        if (ret < 0) {
            ctx.setWsaError(ctx.mapErrnoToWsa(errno));
            return -1;
        }
        return 0;
    }

    return 0;
}

static int MSABI ws2_32_WSARecv(void* s, void* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesRecvd,
                                DWORD* lpFlags, void* lpOverlapped, void* lpCompletionRoutine) {
    auto& ctx = NetworkContext::instance();
    int fd = ctx.getFd((int)(intptr_t)s);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }

    auto* bufs = reinterpret_cast<WSABUF*>(lpBuffers);
    if (lpOverlapped) {
        ctx.setWsaError(WSAEWOULDBLOCK);
        return -1;
    }

    DWORD totalRead = 0;
    for (DWORD i = 0; i < dwBufferCount; i++) {
        ssize_t n = ::recv(fd, bufs[i].buf, bufs[i].len, 0);
        if (n < 0) {
            ctx.setWsaError(ctx.mapErrnoToWsa(errno));
            if (totalRead > 0)
                break;
            return -1;
        }
        totalRead += (DWORD)n;
        if ((DWORD)n < bufs[i].len)
            break;
    }
    if (lpNumberOfBytesRecvd)
        *lpNumberOfBytesRecvd = totalRead;
    return 0;
}

static int MSABI ws2_32_WSARecvFrom(void* s, void* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesRecvd,
                                    DWORD* lpFlags, void* lpFrom, int* lpFromlen, void* lpOverlapped,
                                    void* lpCompletionRoutine) {
    auto& ctx = NetworkContext::instance();
    int fd = ctx.getFd((int)(intptr_t)s);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }

    auto* bufs = reinterpret_cast<WSABUF*>(lpBuffers);
    if (lpOverlapped) {
        ctx.setWsaError(WSAEWOULDBLOCK);
        return -1;
    }

    DWORD totalRead = 0;
    for (DWORD i = 0; i < dwBufferCount; i++) {
        ssize_t n = ::recvfrom(fd, bufs[i].buf, bufs[i].len, 0, (sockaddr*)lpFrom, (socklen_t*)(lpFromlen));
        if (n < 0) {
            ctx.setWsaError(ctx.mapErrnoToWsa(errno));
            if (totalRead > 0)
                break;
            return -1;
        }
        totalRead += (DWORD)n;
        if ((DWORD)n < bufs[i].len)
            break;
    }
    if (lpNumberOfBytesRecvd)
        *lpNumberOfBytesRecvd = totalRead;
    return 0;
}

static int MSABI ws2_32_WSASend(void* s, void* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesSent,
                                DWORD dwFlags, void* lpOverlapped, void* lpCompletionRoutine) {
    auto& ctx = NetworkContext::instance();
    int fd = ctx.getFd((int)(intptr_t)s);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }

    auto* bufs = reinterpret_cast<WSABUF*>(lpBuffers);
    if (lpOverlapped) {
        ctx.setWsaError(WSAEWOULDBLOCK);
        return -1;
    }

    DWORD totalSent = 0;
    for (DWORD i = 0; i < dwBufferCount; i++) {
        ssize_t n = ::send(fd, bufs[i].buf, bufs[i].len, dwFlags);
        if (n < 0) {
            ctx.setWsaError(ctx.mapErrnoToWsa(errno));
            if (totalSent > 0)
                break;
            return -1;
        }
        totalSent += (DWORD)n;
    }
    if (lpNumberOfBytesSent)
        *lpNumberOfBytesSent = totalSent;
    return 0;
}

static int MSABI ws2_32_WSASendTo(void* s, void* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesSent,
                                  DWORD dwFlags, const void* lpTo, int iTolen, void* lpOverlapped,
                                  void* lpCompletionRoutine) {
    auto& ctx = NetworkContext::instance();
    int fd = ctx.getFd((int)(intptr_t)s);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }

    auto* bufs = reinterpret_cast<WSABUF*>(lpBuffers);
    if (lpOverlapped) {
        ctx.setWsaError(WSAEWOULDBLOCK);
        return -1;
    }

    DWORD totalSent = 0;
    for (DWORD i = 0; i < dwBufferCount; i++) {
        ssize_t n = ::sendto(fd, bufs[i].buf, bufs[i].len, dwFlags, (const sockaddr*)lpTo, (socklen_t)iTolen);
        if (n < 0) {
            ctx.setWsaError(ctx.mapErrnoToWsa(errno));
            if (totalSent > 0)
                break;
            return -1;
        }
        totalSent += (DWORD)n;
    }
    if (lpNumberOfBytesSent)
        *lpNumberOfBytesSent = totalSent;
    return 0;
}

// Winsock network-event constants (winsock2.h). FD_MAX_EVENTS is the size of
// the WSANETWORKEVENTS::iErrorCode array.
constexpr int WSA_FD_MAX_EVENTS = 10;
constexpr int WSA_INVALID_EVENT = 0; // WSACreateEvent failure value (NULL)

// Maps a WSAEventSelect FD_* mask to the poll(2) events that can satisfy it.
static short wsaEventMaskToPoll(uint32_t mask) {
    short events = 0;
    if (mask & (FD_READ | FD_ACCEPT))
        events |= POLLIN;
    if (mask & (FD_WRITE | FD_CONNECT))
        events |= POLLOUT;
    return events;
}

// True when the socket is a listening socket (SO_ACCEPTCONN).
static bool socketIsListening(int fd) {
    int acceptConn = 0;
    socklen_t acceptLen = sizeof(acceptConn);
    return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acceptConn, &acceptLen) == 0 && acceptConn;
}

static int MSABI ws2_32_WSAEventSelect(void* s, void* hEventObject, uint32_t lNetworkEvents) {
    auto& ctx = NetworkContext::instance();
    int handle = (int)(intptr_t)s;
    int fd = ctx.getFd(handle);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }

    int eventId = (int)(intptr_t)hEventObject;
    if (lNetworkEvents != 0) {
        ctx.setSocketEventMask(handle, lNetworkEvents, hEventObject);
        // Associate the socket with its event object so waits can watch it,
        // and start a fresh recording period for the edge-triggered events.
        if (ctx.isValidEvent(eventId)) {
            ctx.associateSocketWithEvent(handle, eventId);
            ctx.resetSocketEventTracking(handle);
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } else {
        // A zero mask ends network-event recording and disassociates the
        // socket from its event object.
        ctx.setSocketEventMask(handle, 0, nullptr);
        ctx.unassociateSocketFromEvent(handle);
    }
    return 0;
}

static int MSABI ws2_32_WSAAsyncSelect(void* s, void* hWnd, uint32_t wMsg, uint32_t lEvent) {
    auto& ctx = NetworkContext::instance();
    int handle = (int)(intptr_t)s;
    int fd = ctx.getFd(handle);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }

    ctx.setSocketEventMask(handle, lEvent, hWnd);

    if (lEvent != 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return 0;
}

static int MSABI ws2_32_WSAGetOverlappedResult(void* s, void* lpOverlapped, DWORD* lpcbTransfer, BOOL fWait,
                                               DWORD* lpdwFlags) {
    (void)s;
    (void)lpOverlapped;
    (void)fWait;
    if (lpcbTransfer)
        *lpcbTransfer = 0;
    if (lpdwFlags)
        *lpdwFlags = 0;
    return 1;
}

static void* MSABI ws2_32_WSACreateEvent() {
    int eventId = NetworkContext::instance().allocEvent();
    if (eventId < 0)
        return reinterpret_cast<void*>(static_cast<intptr_t>(WSA_INVALID_EVENT));
    return reinterpret_cast<void*>(static_cast<intptr_t>(eventId));
}

static int MSABI ws2_32_WSACloseEvent(void* hEvent) {
    auto& ctx = NetworkContext::instance();
    int eventId = (int)(intptr_t)hEvent;
    if (!ctx.isValidEvent(eventId)) {
        ctx.setWsaError(WSA_INVALID_HANDLE);
        return 0;
    }
    ctx.closeEvent(eventId);
    return 1;
}

static int MSABI ws2_32_WSASetEvent(void* hEvent) {
    auto& ctx = NetworkContext::instance();
    int eventId = (int)(intptr_t)hEvent;
    if (!ctx.isValidEvent(eventId)) {
        ctx.setWsaError(WSA_INVALID_HANDLE);
        return 0;
    }
    ctx.setEventSignaled(eventId, true);
    return 1;
}

static int MSABI ws2_32_WSAResetEvent(void* hEvent) {
    auto& ctx = NetworkContext::instance();
    int eventId = (int)(intptr_t)hEvent;
    if (!ctx.isValidEvent(eventId)) {
        ctx.setWsaError(WSA_INVALID_HANDLE);
        return 0;
    }
    ctx.setEventSignaled(eventId, false);
    return 1;
}

static int MSABI ws2_32_WSAWaitForMultipleEvents(DWORD cEvents, void** lphEvents, BOOL fWaitAll, DWORD dwTimeout,
                                                 BOOL fAlertable) {
    (void)fAlertable;
    auto& ctx = NetworkContext::instance();
    if (!lphEvents || cEvents == 0 || cEvents > WSA_MAXIMUM_WAIT_EVENTS) {
        ctx.setWsaError(WSA_INVALID_PARAMETER);
        return (int)WSA_WAIT_FAILED;
    }
    for (DWORD i = 0; i < cEvents; i++) {
        if (!ctx.isValidEvent((int)(intptr_t)lphEvents[i])) {
            ctx.setWsaError(WSA_INVALID_HANDLE);
            return (int)WSA_WAIT_FAILED;
        }
    }

    const bool infinite = dwTimeout == WSA_INFINITE;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(dwTimeout);

    std::vector<pollfd> pfds;
    std::vector<DWORD> pfdEventIndex;         // pfds[i] -> index into lphEvents
    std::vector<int> pfdSocketHandle;         // pfds[i] -> shim socket handle
    const int wakeFd = ctx.eventWakeReadFd(); // cross-thread WSASetEvent wake
    size_t wakePfdIndex = SIZE_MAX;
    bool everPolled = false;

    for (;;) {
        // 1. Already-signaled event objects satisfy the wait immediately.
        bool anySignaled = false;
        bool allSignaled = true;
        DWORD firstSignaled = 0;
        for (DWORD i = 0; i < cEvents; i++) {
            if (ctx.isEventSignaled((int)(intptr_t)lphEvents[i])) {
                if (!anySignaled)
                    firstSignaled = i;
                anySignaled = true;
            } else {
                allSignaled = false;
            }
        }
        if (fWaitAll) {
            if (allSignaled)
                return (int)WSA_WAIT_OBJECT_0;
        } else if (anySignaled) {
            return (int)(WSA_WAIT_OBJECT_0 + firstSignaled);
        }

        // 2. Nothing satisfied yet: poll the sockets behind the remaining
        //    events, honoring the remaining timeout. A zero timeout still
        //    polls once (dwTimeout == 0 tests the current state, like
        //    Windows); afterwards the deadline check takes over.
        int timeoutMs = -1;
        if (!infinite) {
            const auto now = std::chrono::steady_clock::now();
            const int64_t remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (remainMs <= 0) {
                if (everPolled)
                    return (int)WSA_WAIT_TIMEOUT;
                timeoutMs = 0;
            } else {
                timeoutMs = (int)std::min<int64_t>(remainMs, INT32_MAX);
            }
        }

        pfds.clear();
        pfdEventIndex.clear();
        pfdSocketHandle.clear();
        wakePfdIndex = SIZE_MAX;
        for (DWORD i = 0; i < cEvents; i++) {
            if (fWaitAll && ctx.isEventSignaled((int)(intptr_t)lphEvents[i]))
                continue; // already satisfied; only the rest are outstanding
            for (int socketHandle : ctx.socketsForEvent((int)(intptr_t)lphEvents[i])) {
                int fd = ctx.getFd(socketHandle);
                if (fd < 0 || ctx.socketHasReportedClose(socketHandle))
                    continue; // closed sockets cannot fire again (FD_CLOSE is edge-triggered)
                short pollEvents = wsaEventMaskToPoll(ctx.getSocketEventMask(socketHandle));
                if (pollEvents == 0)
                    continue;
                pfds.push_back(pollfd{fd, pollEvents, 0});
                pfdEventIndex.push_back(i);
                pfdSocketHandle.push_back(socketHandle);
            }
        }
        if (wakeFd >= 0) {
            wakePfdIndex = pfds.size();
            pfds.push_back(pollfd{wakeFd, POLLIN, 0});
            pfdEventIndex.push_back(0);
            pfdSocketHandle.push_back(-1);
        }

        if (pfds.empty()) {
            // No watchable sockets and no wake pipe: only WSASetEvent (possibly
            // from another thread) can satisfy the wait. Sleep and re-check.
            if (!infinite && std::chrono::steady_clock::now() >= deadline)
                return (int)WSA_WAIT_TIMEOUT;
            usleep(1000);
            continue;
        }

        int rc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeoutMs);
        everPolled = true;
        if (rc == 0)
            return (int)WSA_WAIT_TIMEOUT;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            ctx.setWsaError(ctx.mapErrnoToWsa(errno));
            return (int)WSA_WAIT_FAILED;
        }

        // 3. Classify the poll results against each socket's event mask and
        //    signal the owning event objects.
        bool firedAny = false;
        bool wakeFired = false;
        for (size_t k = 0; k < pfds.size(); k++) {
            if (pfds[k].revents == 0)
                continue;
            if (k == wakePfdIndex) {
                // Cross-thread WSASetEvent wrote a byte; drain it and let the
                // signaled-state check at the top of the loop decide.
                char byte;
                while (::read(wakeFd, &byte, 1) > 0) {
                }
                wakeFired = true;
                continue;
            }
            bool listening = socketIsListening(pfds[k].fd);
            uint32_t firedEvents = ctx.applySocketPollResult(pfdSocketHandle[k], pfds[k].revents, listening);
            if (firedEvents != 0) {
                ctx.recordSocketEvents(pfdSocketHandle[k], firedEvents);
                ctx.setEventSignaled((int)(intptr_t)lphEvents[pfdEventIndex[k]], true);
                firedAny = true;
            }
        }
        if (!firedAny && !wakeFired) {
            // Ready fds that produced no fire (undeliverable POLLERR when no
            // FD_CLOSE/FD_CONNECT is nominated, or data consumed between poll
            // and classification) must not busy-loop the wait.
            usleep(1000);
        }
        // Loop back: re-evaluate the signaled state (cheap when nothing fired).
    }
}

static int MSABI ws2_32_WSAEnumNetworkEvents(void* s, void* hEvent, void* lpNetworkEvents) {
    auto& ctx = NetworkContext::instance();
    int handle = (int)(intptr_t)s;
    int fd = ctx.getFd(handle);
    if (fd < 0) {
        ctx.setWsaError(WSAENOTSOCK);
        return -1;
    }
    if (!lpNetworkEvents) {
        ctx.setWsaError(WSAEFAULT);
        return -1;
    }

    struct WsaNetworkEvents {
        int32_t lNetworkEvents;
        int32_t iErrorCode[WSA_FD_MAX_EVENTS];
    };
    auto* ne = reinterpret_cast<WsaNetworkEvents*>(lpNetworkEvents);
    ne->lNetworkEvents = 0;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = wsaEventMaskToPoll(ctx.getSocketEventMask(handle));
    ::poll(&pfd, 1, 0);

    uint32_t events = ctx.takeSocketEvents(handle);
    events |= ctx.applySocketPollResult(handle, pfd.revents, socketIsListening(fd));
    ne->lNetworkEvents = static_cast<int32_t>(events);

    if (events & FD_CONNECT) {
        int err = 0;
        socklen_t errLen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) == 0)
            ne->iErrorCode[FD_CONNECT_BIT] = err == 0 ? 0 : (int)ctx.mapErrnoToWsa(err);
    }
    if (events & FD_CLOSE) {
        int err = 0;
        socklen_t errLen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) == 0)
            ne->iErrorCode[FD_CLOSE_BIT] = err == 0 ? 0 : (int)ctx.mapErrnoToWsa(err);
    }
    // Remaining iErrorCode entries are left untouched, matching the documented
    // behavior (only entries for set bits are modified).

    // WSAEnumNetworkEvents resets the associated event object after copying
    // and clearing the network event record.
    if (hEvent && ctx.isValidEvent((int)(intptr_t)hEvent))
        ctx.setEventSignaled((int)(intptr_t)hEvent, false);
    return 0;
}

static int MSABI ws2_32_ord3(void* s) {
    return ws2_32_closesocket(s);
}

static int MSABI ws2_32_ord4(void* s, int level, int opt) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0)
        return -1;
    int val = 0;
    return ::getsockopt(fd, level, opt, reinterpret_cast<char*>(&val), nullptr) == 0 ? val : 0;
}

static void* MSABI ws2_32_ord6(void* s, void* addr, int* len) {
    return ws2_32_accept(s, addr, len);
}

static int MSABI ws2_32_ord8(void* s, int level, int opt, char* val, int* len) {
    return ws2_32_getsockopt(s, level, opt, val, len);
}

static int MSABI ws2_32_ord9(void* s, int level, int opt, const char* val, int len) {
    return ws2_32_setsockopt(s, level, opt, val, len);
}

static int MSABI ws2_32_ord10(void* s, void* addr, int len) {
    return ws2_32_connect(s, addr, len);
}

static int MSABI ws2_32_ord15(void* s, int how) {
    return ws2_32_shutdown(s, how);
}

static int MSABI ws2_32_ord16(void* s, int backlog) {
    return ws2_32_listen(s, backlog);
}

static int MSABI ws2_32_ord18(void* s, const void* addr, int len) {
    return ws2_32_bind(s, addr, len);
}

static int MSABI ws2_32_ord19(void* s, int cmd, long arg) {
    uint32_t val = (uint32_t)arg;
    return ws2_32_ioctlsocket(s, cmd, &val);
}

static int MSABI ws2_32_ord23(void* s, void* addr, int* len) {
    return ws2_32_getsockname(s, addr, len);
}

static int MSABI ws2_32_ord111(void* s, int level, int opt, const char* val, int len) {
    return ws2_32_setsockopt(s, level, opt, val, len);
}

static int MSABI ws2_32_ord112(void* s, int level, int opt, char* val, int* len) {
    return ws2_32_getsockopt(s, level, opt, val, len);
}

static int MSABI ws2_32_ord115(void* s, void* addr, int* len) {
    return ws2_32_getpeername(s, addr, len);
}

static int MSABI ws2_32_ord116(void* s, int what, int backlog, char* addr, int* addrLen) {
    (void)what;
    return ws2_32_listen(s, backlog);
}

static int MSABI ws2_32_ord151(void* s, const char* host, const char* service, const void* hints, void** res) {
    (void)s;
    return getaddrinfo(host, service, (const addrinfo*)hints, (addrinfo**)res);
}

static int MSABI ws2_32_recvfrom(void* s, char* buf, int len, int flags, void* from, int* fromlen) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = (int)::recvfrom(fd, buf, (size_t)len, flags, (sockaddr*)from, (socklen_t*)fromlen);
    if (ret < 0)
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
    return ret;
}

static int MSABI ws2_32_sendto(void* s, const char* buf, int len, int flags, const void* to, int tolen) {
    int fd = NetworkContext::instance().getFd((int)(intptr_t)s);
    if (fd < 0) {
        NetworkContext::instance().setWsaError(WSAENOTSOCK);
        return -1;
    }
    int ret = (int)::sendto(fd, buf, (size_t)len, flags, (const sockaddr*)to, (socklen_t)tolen);
    if (ret < 0)
        NetworkContext::instance().setWsaError(NetworkContext::instance().mapErrnoToWsa(errno));
    return ret;
}

ShimLibrary createWs2_32Shim() {
    ShimLibrary lib;
    lib.name = "WS2_32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["WSAStartup"] = fn((void*)ws2_32_WSAStartup);
    lib.functions["WSACleanup"] = fn((void*)ws2_32_WSACleanup);
    lib.functions["WSAGetLastError"] = fn((void*)ws2_32_WSAGetLastError);
    lib.functions["WSASocketA"] = fn((void*)ws2_32_WSASocketA);
    lib.functions["closesocket"] = fn((void*)ws2_32_closesocket);
    lib.functions["connect"] = fn((void*)ws2_32_connect);
    lib.functions["send"] = fn((void*)ws2_32_send);
    lib.functions["recv"] = fn((void*)ws2_32_recv);
    lib.functions["bind"] = fn((void*)ws2_32_bind);
    lib.functions["listen"] = fn((void*)ws2_32_listen);
    lib.functions["accept"] = fn((void*)ws2_32_accept);
    lib.functions["shutdown"] = fn((void*)ws2_32_shutdown);
    lib.functions["ioctlsocket"] = fn((void*)ws2_32_ioctlsocket);
    lib.functions["getsockname"] = fn((void*)ws2_32_getsockname);
    lib.functions["getpeername"] = fn((void*)ws2_32_getpeername);
    lib.functions["getsockopt"] = fn((void*)ws2_32_getsockopt);
    lib.functions["setsockopt"] = fn((void*)ws2_32_setsockopt);
    lib.functions["select"] = fn((void*)ws2_32_select);
    lib.functions["getaddrinfo"] = fn((void*)ws2_32_getaddrinfo);
    lib.functions["freeaddrinfo"] = fn((void*)ws2_32_freeaddrinfo);
    lib.functions["htons"] = fn((void*)ws2_32_htons);
    lib.functions["ntohs"] = fn((void*)ws2_32_ntohs);
    lib.functions["htonl"] = fn((void*)ws2_32_htonl);
    lib.functions["ntohl"] = fn((void*)ws2_32_ntohl);
    lib.functions["inet_addr"] = fn((void*)ws2_32_inet_addr);
    lib.functions["inet_ntoa"] = fn((void*)ws2_32_inet_ntoa);
    lib.functions["gethostbyname"] = fn((void*)ws2_32_gethostbyname);
    lib.functions["WSAIoctl"] = fn((void*)ws2_32_WSAIoctl);
    lib.functions["WSARecv"] = fn((void*)ws2_32_WSARecv);
    lib.functions["WSARecvFrom"] = fn((void*)ws2_32_WSARecvFrom);
    lib.functions["WSASend"] = fn((void*)ws2_32_WSASend);
    lib.functions["WSASendTo"] = fn((void*)ws2_32_WSASendTo);
    lib.functions["WSAEventSelect"] = fn((void*)ws2_32_WSAEventSelect);
    lib.functions["WSAAsyncSelect"] = fn((void*)ws2_32_WSAAsyncSelect);
    lib.functions["WSAGetOverlappedResult"] = fn((void*)ws2_32_WSAGetOverlappedResult);
    lib.functions["WSACreateEvent"] = fn((void*)ws2_32_WSACreateEvent);
    lib.functions["WSACloseEvent"] = fn((void*)ws2_32_WSACloseEvent);
    lib.functions["WSASetEvent"] = fn((void*)ws2_32_WSASetEvent);
    lib.functions["WSAResetEvent"] = fn((void*)ws2_32_WSAResetEvent);
    lib.functions["WSAWaitForMultipleEvents"] = fn((void*)ws2_32_WSAWaitForMultipleEvents);
    lib.functions["WSAEnumNetworkEvents"] = fn((void*)ws2_32_WSAEnumNetworkEvents);
    lib.functions["recvfrom"] = fn((void*)ws2_32_recvfrom);
    lib.functions["sendto"] = fn((void*)ws2_32_sendto);

    lib.ordinals[2] = fn((void*)ws2_32_select);
    lib.ordinals[3] = fn((void*)ws2_32_ord3);
    lib.ordinals[4] = fn((void*)ws2_32_ord4);
    lib.ordinals[6] = fn((void*)ws2_32_ord6);
    lib.ordinals[8] = fn((void*)ws2_32_ord8);
    lib.ordinals[9] = fn((void*)ws2_32_ord9);
    lib.ordinals[10] = fn((void*)ws2_32_ord10);
    lib.ordinals[14] = fn((void*)ws2_32_ord3);
    lib.ordinals[15] = fn((void*)ws2_32_ord15);
    lib.ordinals[16] = fn((void*)ws2_32_ord16);
    lib.ordinals[18] = fn((void*)ws2_32_ord18);
    lib.ordinals[19] = fn((void*)ws2_32_ord19);
    lib.ordinals[21] = fn((void*)ws2_32_ord3);
    lib.ordinals[22] = fn((void*)ws2_32_ord15);
    lib.ordinals[23] = fn((void*)ws2_32_ord23);
    lib.ordinals[111] = fn((void*)ws2_32_ord111);
    lib.ordinals[112] = fn((void*)ws2_32_ord112);
    lib.ordinals[115] = fn((void*)ws2_32_ord115);
    lib.ordinals[116] = fn((void*)ws2_32_ord116);
    lib.ordinals[151] = fn((void*)ws2_32_ord151);

    return lib;
}

static wchar_t** MSABI shell32_CommandLineToArgvW(const wchar_t*, int* argc) {
    if (argc)
        *argc = 1;
    auto** argv = (wchar_t**)malloc(sizeof(wchar_t*) * 2);
    argv[0] = (wchar_t*)L"steam.exe";
    argv[1] = nullptr;
    return argv;
}

static DWORD MSABI shell32_SHGetFileInfoW(const wchar_t*, DWORD, void*, UINT, UINT) {
    return 0;
}

static LONG MSABI shell32_SHGetKnownFolderPath(const void*, DWORD, void*, wchar_t** ppszPath) {
    if (ppszPath) {
        *ppszPath = (wchar_t*)malloc(64);
        wcscpy(*ppszPath, L"C:\\Users\\user");
    }
    return 0;
}

static LONG MSABI shell32_SHGetFolderPathW(void*, int, void*, DWORD, wchar_t* path) {
    if (path)
        wcscpy(path, L"C:\\Users\\user");
    return 0;
}

ShimLibrary createShell32Shim() {
    ShimLibrary lib;
    lib.name = "SHELL32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["CommandLineToArgvW"] = fn((void*)shell32_CommandLineToArgvW);
    lib.functions["SHGetFileInfoW"] = fn((void*)shell32_SHGetFileInfoW);
    lib.functions["SHGetKnownFolderPath"] = fn((void*)shell32_SHGetKnownFolderPath);
    lib.functions["SHGetFolderPathW"] = fn((void*)shell32_SHGetFolderPathW);

    lib.ordinals[680] = fn((void*)shell32_SHGetFolderPathW);

    return lib;
}

static void MSABI ole32_CoTaskMemFree(void* p) {
    free(p);
}

static HRESULT MSABI ole32_CoInitialize(void*) {
    return 0;
}
static HRESULT MSABI ole32_CoInitializeEx(void*, DWORD) {
    return 0;
}
static void MSABI ole32_CoUninitialize() {}
static HRESULT MSABI ole32_CoCreateInstance(const GUID* rclsid, void* punkOuter, DWORD dwClsCtx, const GUID* riid,
                                            void** ppv) {
    if (ppv)
        *ppv = nullptr;
    return (HRESULT)0x80040154L;
}
static void* MSABI ole32_CoTaskMemAlloc(SIZE_T cb) {
    return malloc(cb);
}
static void* MSABI ole32_CoTaskMemRealloc(void* pv, SIZE_T cb) {
    return realloc(pv, cb);
}
static int MSABI ole32_StringFromGUID2(const GUID* guid, wchar_t* lpsz, int cchMax) {
    if (!guid || !lpsz || cchMax < 39)
        return 0;
    swprintf(lpsz, cchMax, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid->Data1, guid->Data2, guid->Data3,
             guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5],
             guid->Data4[6], guid->Data4[7]);
    return 39;
}
static HRESULT MSABI ole32_IIDFromString(const wchar_t*, GUID* guid) {
    if (guid)
        memset(guid, 0, sizeof(GUID));
    return 0;
}
static HRESULT MSABI ole32_CoGetClassObject(const GUID*, DWORD, void*, const GUID*, void**) {
    return (HRESULT)0x80040154L;
}
static HRESULT MSABI ole32_OleInitialize(void*) {
    return 0;
}
static void MSABI ole32_OleUninitialize() {}

ShimLibrary createOle32Shim() {
    ShimLibrary lib;
    lib.name = "ole32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };
    lib.functions["CoInitialize"] = fn((void*)ole32_CoInitialize);
    lib.functions["CoInitializeEx"] = fn((void*)ole32_CoInitializeEx);
    lib.functions["CoUninitialize"] = fn((void*)ole32_CoUninitialize);
    lib.functions["CoCreateInstance"] = fn((void*)ole32_CoCreateInstance);
    lib.functions["CoTaskMemAlloc"] = fn((void*)ole32_CoTaskMemAlloc);
    lib.functions["CoTaskMemRealloc"] = fn((void*)ole32_CoTaskMemRealloc);
    lib.functions["StringFromGUID2"] = fn((void*)ole32_StringFromGUID2);
    lib.functions["IIDFromString"] = fn((void*)ole32_IIDFromString);
    lib.functions["CoGetClassObject"] = fn((void*)ole32_CoGetClassObject);
    lib.functions["OleInitialize"] = fn((void*)ole32_OleInitialize);
    lib.functions["OleUninitialize"] = fn((void*)ole32_OleUninitialize);
    lib.functions["CoTaskMemFree"] = fn((void*)ole32_CoTaskMemFree);
    return lib;
}

static void* MSABI oleaut32_SysAllocString(const wchar_t* str) {
    if (!str)
        return nullptr;
    size_t len = wcslen(str);
    wchar_t* buf = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
    wcscpy(buf, str);
    return buf;
}

static void MSABI oleaut32_SysFreeString(void* bstr) {
    free(bstr);
}
static void* MSABI oleaut32_SysAllocStringLen(const wchar_t*, UINT len) {
    return malloc((len + 1) * sizeof(wchar_t));
}
static UINT MSABI oleaut32_SysStringLen(void* bstr) {
    if (!bstr)
        return 0;
    return (UINT)wcslen((const wchar_t*)bstr);
}
static void MSABI oleaut32_VariantInit(void* pv) {
    if (pv)
        memset(pv, 0, 16);
}
static HRESULT MSABI oleaut32_VariantClear(void* pv) {
    if (pv)
        memset(pv, 0, 16);
    return 0;
}
static HRESULT MSABI oleaut32_VariantChangeType(void*, void*, UINT, UINT) {
    return 0;
}
static HRESULT MSABI oleaut32_VariantChangeTypeEx(void*, void*, DWORD, UINT, UINT) {
    return 0;
}
static void* MSABI oleaut32_SafeArrayCreate(UINT, UINT, void*) {
    return calloc(1, 32);
}
static HRESULT MSABI oleaut32_SafeArrayDestroy(void*) {
    return 0;
}
static HRESULT MSABI oleaut32_SafeArrayAccessData(void*, void** ppv) {
    if (ppv)
        *ppv = nullptr;
    return 0;
}
static HRESULT MSABI oleaut32_SafeArrayUnaccessData(void*) {
    return 0;
}
static HRESULT MSABI oleaut32_SafeArrayGetLBound(void*, UINT, LONG*) {
    return 0;
}
static HRESULT MSABI oleaut32_SafeArrayGetUBound(void*, UINT, LONG*) {
    return 0;
}

ShimLibrary createOleAut32Shim() {
    ShimLibrary lib;
    lib.name = "OLEAUT32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };
    lib.functions["SysAllocString"] = fn((void*)oleaut32_SysAllocString);
    lib.functions["SysFreeString"] = fn((void*)oleaut32_SysFreeString);
    lib.functions["SysAllocStringLen"] = fn((void*)oleaut32_SysAllocStringLen);
    lib.functions["SysStringLen"] = fn((void*)oleaut32_SysStringLen);
    lib.functions["VariantInit"] = fn((void*)oleaut32_VariantInit);
    lib.functions["VariantClear"] = fn((void*)oleaut32_VariantClear);
    lib.functions["VariantChangeType"] = fn((void*)oleaut32_VariantChangeType);
    lib.functions["VariantChangeTypeEx"] = fn((void*)oleaut32_VariantChangeTypeEx);
    lib.functions["SafeArrayCreate"] = fn((void*)oleaut32_SafeArrayCreate);
    lib.functions["SafeArrayDestroy"] = fn((void*)oleaut32_SafeArrayDestroy);
    lib.functions["SafeArrayAccessData"] = fn((void*)oleaut32_SafeArrayAccessData);
    lib.functions["SafeArrayUnaccessData"] = fn((void*)oleaut32_SafeArrayUnaccessData);
    lib.functions["SafeArrayGetLBound"] = fn((void*)oleaut32_SafeArrayGetLBound);
    lib.functions["SafeArrayGetUBound"] = fn((void*)oleaut32_SafeArrayGetUBound);
    lib.ordinals[9] = fn((void*)oleaut32_SysAllocString);
    return lib;
}

static void* MSABI crypt32_CertOpenStore(const char*, DWORD, void*, DWORD, const void*) {
    return reinterpret_cast<void*>(0xC000);
}

static BOOL MSABI crypt32_CertCloseStore(void*, DWORD) {
    return 1;
}

static void* MSABI crypt32_CertCreateCertificateContext(DWORD, const BYTE*, DWORD) {
    return reinterpret_cast<void*>(0xC001);
}

static BOOL MSABI crypt32_CertFreeCertificateContext(void*) {
    return 1;
}

static BOOL MSABI crypt32_CertGetCertificateChain(void*, void*, void*, void*, const void*, DWORD, void*, void** chain) {
    if (chain)
        *chain = reinterpret_cast<void*>(0xC002);
    return 1;
}

static void MSABI crypt32_CertFreeCertificateChain(void*) {}

static BOOL MSABI crypt32_CertAddCertificateContextToStore(void*, void*, DWORD, void**) {
    return 1;
}

ShimLibrary createCrypt32Shim() {
    ShimLibrary lib;
    lib.name = "CRYPT32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["CertOpenStore"] = fn((void*)crypt32_CertOpenStore);
    lib.functions["CertCloseStore"] = fn((void*)crypt32_CertCloseStore);
    lib.functions["CertCreateCertificateContext"] = fn((void*)crypt32_CertCreateCertificateContext);
    lib.functions["CertFreeCertificateContext"] = fn((void*)crypt32_CertFreeCertificateContext);
    lib.functions["CertGetCertificateChain"] = fn((void*)crypt32_CertGetCertificateChain);
    lib.functions["CertFreeCertificateChain"] = fn((void*)crypt32_CertFreeCertificateChain);
    lib.functions["CertAddCertificateContextToStore"] = fn((void*)crypt32_CertAddCertificateContextToStore);

    return lib;
}

static DWORD MSABI psapi_GetModuleFileNameExW(void*, void*, wchar_t* buf, DWORD sz) {
    if (buf && sz > 0) {
        buf[0] = 0;
    }
    return 0;
}

static BOOL MSABI psapi_GetModuleInformation(void*, void*, void*, DWORD) {
    return 1;
}

static BOOL MSABI psapi_GetProcessMemoryInfo(void*, void* pmc, DWORD sz) {
    memset(pmc, 0, sz);
    return 1;
}

ShimLibrary createPsapiShim() {
    ShimLibrary lib;
    lib.name = "PSAPI.DLL";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    lib.functions["GetModuleFileNameExW"] = fn((void*)psapi_GetModuleFileNameExW);
    lib.functions["GetModuleInformation"] = fn((void*)psapi_GetModuleInformation);
    lib.functions["GetProcessMemoryInfo"] = fn((void*)psapi_GetProcessMemoryInfo);

    return lib;
}

struct FakeVersionResource {
    DWORD dwSignature;
    DWORD dwStrucVersion;
    DWORD dwFileVersionMS;
    DWORD dwFileVersionLS;
    DWORD dwProductVersionMS;
    DWORD dwProductVersionLS;
    DWORD dwFileFlagsMask;
    DWORD dwFileFlags;
    DWORD dwFileOS;
    DWORD dwFileType;
    DWORD dwFileSubtype;
    DWORD dwFileDateMS;
    DWORD dwFileDateLS;
};

static DWORD MSABI version_GetFileVersionInfoSizeW(const wchar_t* lptstrFilename, DWORD* lpdwHandle) {
    if (lpdwHandle)
        *lpdwHandle = 0;
    return sizeof(FakeVersionResource) + 512;
}

static BOOL MSABI version_GetFileVersionInfoW(const wchar_t* lptstrFilename, DWORD dwHandle, DWORD dwLen,
                                              void* lpData) {
    if (!lpData || dwLen < sizeof(FakeVersionResource))
        return 0;
    auto* info = reinterpret_cast<FakeVersionResource*>(lpData);
    memset(info, 0, sizeof(FakeVersionResource));
    info->dwSignature = 0xFEEF04BD;
    info->dwStrucVersion = 0x00010000;
    info->dwFileVersionMS = (7 << 16) | 0;
    info->dwFileVersionLS = (96 << 16) | 0;
    info->dwProductVersionMS = (7 << 16) | 0;
    info->dwProductVersionLS = (96 << 16) | 0;
    info->dwFileFlagsMask = 0x3F;
    info->dwFileType = 1;
    info->dwFileOS = 0x00040004;
    char* strArea = reinterpret_cast<char*>(info + 1);
    memset(strArea, 0, 512);
    return 1;
}

static BOOL MSABI version_VerQueryValueW(const void* pBlock, const wchar_t* lpSubBlock, void** lplpBuffer,
                                         UINT* puLen) {
    if (!pBlock || !lplpBuffer)
        return 0;
    if (wcsncmp(lpSubBlock, L"\\", 2) == 0) {
        *lplpBuffer = const_cast<void*>(pBlock);
        if (puLen)
            *puLen = sizeof(FakeVersionResource);
        return 1;
    }
    if (wcsncmp(lpSubBlock, L"\\VarFileInfo\\Translation", 24) == 0) {
        static DWORD translation = 0x040904B0;
        *lplpBuffer = &translation;
        if (puLen)
            *puLen = 4;
        return 1;
    }
    if (puLen)
        *puLen = 0;
    *lplpBuffer = nullptr;
    return 0;
}

ShimLibrary createVersionShim() {
    ShimLibrary lib;
    lib.name = "VERSION.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };
    lib.functions["GetFileVersionInfoSizeW"] = fn((void*)version_GetFileVersionInfoSizeW);
    lib.functions["GetFileVersionInfoW"] = fn((void*)version_GetFileVersionInfoW);
    lib.functions["GetFileVersionInfoSizeA"] = fn((void*)version_GetFileVersionInfoSizeW);
    lib.functions["GetFileVersionInfoA"] = fn((void*)version_GetFileVersionInfoW);
    lib.functions["VerQueryValueW"] = fn((void*)version_VerQueryValueW);
    return lib;
}

static LONG MSABI bcrypt_BCryptGenRandom(void*, BYTE* buf, ULONG len, ULONG) {
    for (ULONG i = 0; i < len; i++)
        buf[i] = (BYTE)(rand() & 0xFF);
    return 0;
}

ShimLibrary createBcryptShim() {
    ShimLibrary lib;
    lib.name = "bcrypt.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };
    lib.functions["BCryptGenRandom"] = fn((void*)bcrypt_BCryptGenRandom);
    return lib;
}

static BOOL MSABI comctl32_InitCommonControlsEx(const void*) {
    return 1;
}

ShimLibrary createComCtl32Shim() {
    ShimLibrary lib;
    lib.name = "COMCTL32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };
    lib.functions["InitCommonControlsEx"] = fn((void*)comctl32_InitCommonControlsEx);
    return lib;
}

static int MSABI wsock32_ord1142(WORD, void* data) {
    if (data) {
        memset(data, 0, 400);
        reinterpret_cast<WORD*>(data)[0] = 0x0202;
    }
    return 0;
}

ShimLibrary createWsock32Shim() {
    ShimLibrary lib;
    lib.name = "WSOCK32.dll";
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };
    lib.ordinals[1142] = fn((void*)wsock32_ord1142);
    return lib;
}

} // namespace win32
} // namespace metalsharp
