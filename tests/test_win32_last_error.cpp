/// @file test_win32_last_error.cpp
/// @brief Regression coverage for issue #422: shim/VFS failure paths must set
/// the single shared thread-local last error (errno-mapped Win32 codes), and
/// CloseHandle-style operations must report failure for invalid handles.
///
/// The kernel32 shim exports (GetLastError/SetLastError/CloseHandle) are
/// static functions registered into a ShimLibrary, so this test exercises the
/// shared machinery they delegate to: the shared TLS last-error state, the
/// VirtualFileSystem handle/error layer, and the SyncContext handle lifecycle.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include <metalsharp/SyncContext.h>
#include <metalsharp/VirtualFileSystem.h>
#include <metalsharp/Win32Error.h>

using namespace metalsharp::win32;

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

int main() {
    // Isolate the VFS prefix from the real ~/.metalsharp/prefix.
    char tmpl[] = "/tmp/ms-win32-last-error-XXXXXX";
    char* dir = mkdtemp(tmpl);
    VirtualFileSystem::instance().setPrefix(dir);

    // --- errno -> Win32 mapping ---
    CHECK(errnoToWin32(ENOENT) == ERROR_FILE_NOT_FOUND);
    CHECK(errnoToWin32(EACCES) == ERROR_ACCESS_DENIED);
    CHECK(errnoToWin32(EPERM) == ERROR_ACCESS_DENIED);
    CHECK(errnoToWin32(EBADF) == ERROR_INVALID_HANDLE);
    CHECK(errnoToWin32(EEXIST) == ERROR_ALREADY_EXISTS);
    CHECK(errnoToWin32(ENOTDIR) == ERROR_PATH_NOT_FOUND);
    CHECK(errnoToWin32(EISDIR) == ERROR_ACCESS_DENIED);
    CHECK(errnoToWin32(EINVAL) == ERROR_INVALID_PARAMETER);
    CHECK(errnoToWin32(ENOMEM) == ERROR_NOT_ENOUGH_MEMORY);
    CHECK(errnoToWin32(ENOSPC) == ERROR_DISK_FULL);
    CHECK(errnoToWin32(ENOTEMPTY) == ERROR_DIR_NOT_EMPTY);
    CHECK(errnoToWin32(0xDEADBEEF) == ERROR_GEN_FAILURE);

    // --- single shared thread-local last error ---
    setLastErrorCode(ERROR_SUCCESS);
    CHECK(lastErrorCode() == ERROR_SUCCESS);
    CHECK(t_lastError == ERROR_SUCCESS);
    setLastErrorCode(ERROR_ACCESS_DENIED);
    CHECK(lastErrorCode() == ERROR_ACCESS_DENIED);

    auto& vfs = VirtualFileSystem::instance();

    // --- CreateFile on a missing file: invalid handle + errno-mapped error ---
    setLastErrorCode(ERROR_SUCCESS);
    HANDLE h = vfs.createFile("C:\\nope\\missing.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK(lastErrorCode() == ERROR_FILE_NOT_FOUND);

    // --- CreateFile with a null name: invalid handle + invalid parameter ---
    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.createFile(nullptr, GENERIC_READ, 0, OPEN_EXISTING, 0) == INVALID_HANDLE_VALUE);
    CHECK(lastErrorCode() == ERROR_INVALID_PARAMETER);

    // --- CreateFile success path still works ---
    setLastErrorCode(ERROR_SUCCESS);
    h = vfs.createFile("C:\\Users\\user\\last-error-test.txt", GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, 0);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(lastErrorCode() == ERROR_SUCCESS);

    // --- ReadFile/WriteFile/GetFileSize on the valid handle ---
    DWORD written = 0;
    CHECK(vfs.writeFile(h, "hello", 5, &written) == 1);
    CHECK(written == 5);
    DWORD sizeHigh = 0;
    CHECK(vfs.getFileSize(h, &sizeHigh) == 5);

    char buf[16] = {0};
    DWORD read = 0;
    CHECK(vfs.readFile(h, buf, sizeof(buf), &read) == 1);
    CHECK(read == 0); // at EOF: success with zero bytes

    // --- Invalid-handle operations report ERROR_INVALID_HANDLE ---
    HANDLE bogus = reinterpret_cast<HANDLE>(0x12345678);
    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.readFile(bogus, buf, sizeof(buf), &read) == 0);
    CHECK(lastErrorCode() == ERROR_INVALID_HANDLE);

    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.writeFile(bogus, "x", 1, &written) == 0);
    CHECK(lastErrorCode() == ERROR_INVALID_HANDLE);

    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.getFileSize(bogus, nullptr) == 0xFFFFFFFF);
    CHECK(lastErrorCode() == ERROR_INVALID_HANDLE);

    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.getFileSizeEx(bogus, nullptr) == 0);
    CHECK(lastErrorCode() == ERROR_INVALID_HANDLE);

    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.setFilePointer(bogus, 0, nullptr, SEEK_SET) == 0xFFFFFFFF);
    CHECK(lastErrorCode() == ERROR_INVALID_HANDLE);

    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.setFilePointerEx(bogus, 0, nullptr, SEEK_SET) == 0);
    CHECK(lastErrorCode() == ERROR_INVALID_HANDLE);

    // --- Invalid move method: ERROR_INVALID_PARAMETER ---
    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.setFilePointer(h, 0, nullptr, 99) == 0xFFFFFFFF);
    CHECK(lastErrorCode() == ERROR_INVALID_PARAMETER);

    // --- CloseHandle-style lifecycle: valid closes, invalid does not ---
    CHECK(vfs.closeHandle(h) == true);

    setLastErrorCode(ERROR_SUCCESS);
    CHECK(vfs.closeHandle(bogus) == false);
    CHECK(lastErrorCode() == ERROR_INVALID_HANDLE);

    // --- FindFirstFile on a missing directory: errno-mapped error ---
    uint8_t findData[512];
    setLastErrorCode(ERROR_SUCCESS);
    HANDLE hFind = vfs.findFirstFileW("C:\\no_such_dir\\*.txt", findData);
    CHECK(hFind == INVALID_HANDLE_VALUE);
    CHECK(lastErrorCode() == ERROR_FILE_NOT_FOUND);

    // --- SyncContext handle lifecycle (CloseHandle delegation target) ---
    auto& sync = SyncContext::instance();
    void* evt = sync.createEvent(false, false, "");
    CHECK(evt != nullptr);
    CHECK(sync.destroyHandle(evt) == true);
    CHECK(sync.destroyHandle(evt) == false); // unknown handle now

    if (g_failures == 0) {
        printf("win32_last_error: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "win32_last_error: %d check(s) failed\n", g_failures);
    return 1;
}
