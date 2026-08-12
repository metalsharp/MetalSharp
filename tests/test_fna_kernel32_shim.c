#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef uint32_t DWORD;
typedef int32_t BOOL;
typedef void* HANDLE;

int MultiByteToWideChar(uint32_t CodePage, uint32_t dwFlags, const char* lpMultiByteStr, int cbMultiByte,
                        void* lpWideCharStr, int cchWideChar);
extern HANDLE GetStdHandle(DWORD nStdHandle);
extern BOOL CloseHandle(HANDLE hObject);

static int passed;
static int failed;

#define CHECK(condition, message)                                                                                      \
    do {                                                                                                               \
        if (condition) {                                                                                               \
            printf("  [OK] %s\n", message);                                                                            \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", message);                                                                          \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int check_standard_handle(DWORD selector, int expected_fd) {
    int backup = dup(expected_fd);
    if (backup < 0)
        return 0;

    HANDLE handle = GetStdHandle(selector);
    int handle_matches = (int)(intptr_t)handle == expected_fd;
    (void)CloseHandle(handle);
    int remains_open = fcntl(expected_fd, F_GETFD) != -1;

    int restored = dup2(backup, expected_fd) == expected_fd;
    close(backup);

    return handle_matches && remains_open && restored;
}

int main(void) {
    const char input[] = "FNA";
    uint16_t output[sizeof(input) + 1];
    for (size_t i = 0; i < sizeof(output) / sizeof(output[0]); i++)
        output[i] = 0xA55A;

    int converted = MultiByteToWideChar(0, 0, input, 0, output, (int)sizeof(input));

    CHECK(converted == 3, "auto-length conversion reports payload length");
    CHECK(output[0] == 'F' && output[1] == 'N' && output[2] == 'A', "auto-length conversion copies input");
    CHECK(output[3] == 0, "auto-length conversion writes a UTF-16 terminator");
    CHECK(output[4] == 0xA55A, "terminator write stays within the requested buffer");

    CHECK(check_standard_handle((DWORD)-10, STDIN_FILENO), "CloseHandle preserves borrowed stdin");
    CHECK(check_standard_handle((DWORD)-11, STDOUT_FILENO), "CloseHandle preserves borrowed stdout");
    CHECK(check_standard_handle((DWORD)-12, STDERR_FILENO), "CloseHandle preserves borrowed stderr");

    int unowned_fd = open("/dev/null", O_RDONLY);
    CHECK(unowned_fd >= 0, "open an unowned descriptor");
    if (unowned_fd >= 0) {
        CHECK(CloseHandle((HANDLE)(intptr_t)unowned_fd), "CloseHandle accepts an unowned descriptor");
        CHECK(CloseHandle((HANDLE)(intptr_t)unowned_fd), "repeated CloseHandle remains harmless");
        CHECK(fcntl(unowned_fd, F_GETFD) != -1, "CloseHandle does not close an unowned descriptor");
        close(unowned_fd);
    }

    printf("=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed != 0;
}
