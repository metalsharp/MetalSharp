/// Regression tests for #418: kernel32 wide-char entry points were bound to
/// ANSI implementations, so UTF-16 paths/module names were read as char*
/// (truncated to the first character). These tests exercise the real W
/// variants through the shim's exported function table.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include <metalsharp/Kernel32Shim.h>
#include <metalsharp/PELoader.h>
#include <metalsharp/Win32Types.h>

static int passed = 0;
static int failed = 0;

using metalsharp::win32::BOOL;
using metalsharp::win32::DWORD;
using metalsharp::win32::FARPROC;
using metalsharp::win32::HANDLE;
using metalsharp::win32::HMODULE;
using metalsharp::win32::INVALID_HANDLE_VALUE;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  [OK] %s\n", msg);                                                                                \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

using CreateFileW_t = HANDLE(MSABI*)(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
using CloseHandle_t = BOOL(MSABI*)(HANDLE);
using ReadFile_t = BOOL(MSABI*)(HANDLE, void*, DWORD, DWORD*, void*);
using WriteFile_t = BOOL(MSABI*)(HANDLE, const void*, DWORD, DWORD*, void*);
using GetModuleHandleW_t = HMODULE(MSABI*)(const wchar_t*);
using LoadLibraryW_t = HMODULE(MSABI*)(const wchar_t*);
using GetProcAddress_t = FARPROC(MSABI*)(HMODULE, const char*);
using GetModuleFileNameW_t = DWORD(MSABI*)(HMODULE, wchar_t*, DWORD);
using GetCurrentDirectoryW_t = DWORD(MSABI*)(DWORD, wchar_t*);

// Build a guest UTF-16 string (uint16_t units, as the Windows ABI passes).
static std::vector<uint16_t> utf16(const char* ascii) {
    std::vector<uint16_t> out;
    for (const char* p = ascii; *p; p++)
        out.push_back(static_cast<uint16_t>(static_cast<unsigned char>(*p)));
    out.push_back(0);
    return out;
}

// Present the guest UTF-16 buffer as the shim's wchar_t* parameter. Host
// wchar_t is 32-bit, so the cast is explicit, exactly as the shim receives
// it from PE code.
static const wchar_t* widePtr(const std::vector<uint16_t>& u16) {
    return reinterpret_cast<const wchar_t*>(u16.data());
}

// Decode a wide buffer written by the shim (host wchar_t units) back to a
// narrow string for assertions.
static std::string fromWide(const uint16_t* wide) {
    std::string out;
    for (size_t i = 0; wide[i]; i++)
        out.push_back(static_cast<char>(wide[i] & 0xFF));
    return out;
}

template <typename T> static T getFn(const metalsharp::ShimLibrary& lib, const char* name) {
    auto it = lib.functions.find(name);
    if (it == lib.functions.end())
        return nullptr;
    return reinterpret_cast<T>(it->second());
}

int main() {
    printf("=== Kernel32 Wide-Char Shim Tests ===\n\n");

    metalsharp::PELoader loader; // establishes PELoader::instance()
    auto lib = metalsharp::win32::Kernel32Shim::create();

    auto createFileW = getFn<CreateFileW_t>(lib, "CreateFileW");
    auto closeHandle = getFn<CloseHandle_t>(lib, "CloseHandle");
    auto readFile = getFn<ReadFile_t>(lib, "ReadFile");
    auto writeFile = getFn<WriteFile_t>(lib, "WriteFile");
    auto getModuleHandleW = getFn<GetModuleHandleW_t>(lib, "GetModuleHandleW");
    auto loadLibraryW = getFn<LoadLibraryW_t>(lib, "LoadLibraryW");
    auto getProcAddress = getFn<GetProcAddress_t>(lib, "GetProcAddress");
    auto getModuleFileNameW = getFn<GetModuleFileNameW_t>(lib, "GetModuleFileNameW");
    auto getCurrentDirectoryW = getFn<GetCurrentDirectoryW_t>(lib, "GetCurrentDirectoryW");

    CHECK(createFileW && closeHandle && readFile && writeFile && getModuleHandleW && loadLibraryW && getProcAddress &&
              getModuleFileNameW && getCurrentDirectoryW,
          "wide-char shims are present in the kernel32 function table");

    {
        printf("\n--- CreateFileW ---\n");
        char tmpl[] = "/tmp/ms418_wide_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        CHECK(dir != nullptr, "create temp test directory");
        if (!dir)
            return 1;
        CHECK(chdir(dir) == 0, "enter temp test directory");

        auto name = utf16("wide-create-file.bin");
        HANDLE h =
            createFileW(widePtr(name), 0x40000000 /*GENERIC_WRITE*/, 0, nullptr, 2 /*CREATE_ALWAYS*/, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE, "CreateFileW opens a file via its full UTF-16 path");
        const char* payload = "wide-data";
        DWORD written = 0;
        CHECK(writeFile(h, payload, static_cast<DWORD>(strlen(payload)), &written, nullptr) &&
                  written == strlen(payload),
              "WriteFile through the wide-created handle");
        CHECK(closeHandle(h) == 1, "CloseHandle after write");

        // Regression: the old binding read the wide path as char*, truncating
        // to "w" and creating a junk file named "w".
        CHECK(access("wide-create-file.bin", F_OK) == 0, "file exists under the full wide name");
        CHECK(access("w", F_OK) != 0, "no truncated first-character file created");

        h = createFileW(widePtr(name), 0x80000000 /*GENERIC_READ*/, 0, nullptr, 3 /*OPEN_EXISTING*/, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE, "CreateFileW reopens the file for read");
        char buf[32] = {0};
        DWORD got = 0;
        CHECK(readFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got == strlen(payload),
              "ReadFile reads back through the wide path");
        CHECK(strcmp(buf, payload) == 0, "read payload matches written payload");
        CHECK(closeHandle(h) == 1, "CloseHandle after read");

        remove("wide-create-file.bin");
        chdir("/");
        rmdir(dir);
    }

    {
        printf("\n--- GetModuleHandleW / LoadLibraryW ---\n");
        auto mkLib = [](const char* name, void* func) {
            metalsharp::ShimLibrary s;
            s.name = name;
            s.functions["ProbeFunc"] = [func]() -> void* { return func; };
            return s;
        };
        loader.registerShim("wmod-a.dll", mkLib("wmod-a.dll", reinterpret_cast<void*>(0x1111)));
        loader.registerShim("wmod-b.dll", mkLib("wmod-b.dll", reinterpret_cast<void*>(0x2222)));

        auto nameA = utf16("wmod-a.dll");
        HMODULE ha = getModuleHandleW(widePtr(nameA));
        CHECK(ha != nullptr, "GetModuleHandleW returns a handle for a UTF-16 module name");
        // Regression: the old binding resolved "w" instead of "wmod-a.dll", so
        // the module-handle→name map missed the registered shim.
        CHECK(getProcAddress(ha, "ProbeFunc") == reinterpret_cast<void*>(0x1111),
              "GetModuleHandleW resolves the full wide module name (not the first char)");

        auto nameB = utf16("wmod-b.dll");
        HMODULE hb = loadLibraryW(widePtr(nameB));
        CHECK(hb != nullptr, "LoadLibraryW returns a handle for a UTF-16 module name");
        CHECK(getProcAddress(hb, "ProbeFunc") == reinterpret_cast<void*>(0x2222),
              "LoadLibraryW resolves the full wide module name (not the first char)");
    }

    {
        printf("\n--- GetModuleFileNameW ---\n");
        metalsharp::win32::setExePath("/fake/game/wide.exe");
        std::vector<uint16_t> buf(512);
        DWORD n = getModuleFileNameW(nullptr, reinterpret_cast<wchar_t*>(buf.data()), static_cast<DWORD>(buf.size()));
        CHECK(n == strlen("/fake/game/wide.exe"), "GetModuleFileNameW returns the correct length");
        CHECK(fromWide(buf.data()) == "/fake/game/wide.exe", "GetModuleFileNameW writes the executable path as UTF-16");
    }

    {
        printf("\n--- GetCurrentDirectoryW ---\n");
        char cwd[512];
        CHECK(getcwd(cwd, sizeof(cwd)) != nullptr, "host getcwd succeeds");
        std::vector<uint16_t> buf(512);
        DWORD n = getCurrentDirectoryW(static_cast<DWORD>(buf.size()), reinterpret_cast<wchar_t*>(buf.data()));
        CHECK(n > 0 && n < 512, "GetCurrentDirectoryW returns a valid length");
        CHECK(fromWide(buf.data()) == cwd, "GetCurrentDirectoryW writes the host cwd as UTF-16");
    }

    printf("\n=== Kernel32 Wide-Char Shim Tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
