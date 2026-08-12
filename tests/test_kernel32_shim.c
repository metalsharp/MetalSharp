#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void* LoadLibraryA(const char* lpLibFileName);
void* LoadLibraryW(const uint16_t* lpLibFileName);
uint32_t GetModuleFileNameA(void* hModule, char* lpFilename, uint32_t nSize);

static int failures = 0;

#define CHECK(condition, message)                                                                                      \
    do {                                                                                                               \
        if (condition) {                                                                                               \
            printf("  [OK] %s\n", message);                                                                            \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", message);                                                                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static size_t copy_ascii_to_utf16(const char* input, uint16_t* output, size_t capacity) {
    size_t length = strlen(input);
    if (length + 5 > capacity)
        return 0;

    for (size_t index = 0; index < length; index++) {
        if ((unsigned char)input[index] > 0x7F)
            return 0;
        output[index] = (uint16_t)(unsigned char)input[index];
    }

    output[length++] = '-';
    output[length++] = 0x4E2D;
    output[length++] = 0xD83D;
    output[length++] = 0xDE00;
    output[length] = 0;
    return length;
}

static void test_get_module_file_name(const char* executable) {
    CHECK(chdir("/") == 0, "Changing directory away from the executable succeeds");

    char away[PATH_MAX];
    CHECK(getcwd(away, sizeof(away)) != NULL, "Working directory is readable after chdir");

    char buffer[PATH_MAX * 2];
    uint32_t length = GetModuleFileNameA(NULL, buffer, sizeof(buffer));
    CHECK(length > 0 && length < sizeof(buffer), "GetModuleFileNameA returns a valid path length");
    if (length == 0 || length >= sizeof(buffer))
        return;

    CHECK(buffer[0] == '/', "GetModuleFileNameA returns an absolute path");
    CHECK(strcmp(buffer, away) != 0, "GetModuleFileNameA does not return the working directory");
    CHECK(access(buffer, F_OK) == 0, "GetModuleFileNameA returns an existing path");

    char expected[PATH_MAX];
    if (realpath(executable, expected) != NULL) {
        char actual[PATH_MAX];
        CHECK(realpath(buffer, actual) != NULL && strcmp(actual, expected) == 0,
              "GetModuleFileNameA returns the test executable path");
    }

    char small[8];
    CHECK(GetModuleFileNameA(NULL, small, sizeof(small)) != 0,
          "GetModuleFileNameA reports truncation for a small buffer");
}

int main(int argc, char** argv) {
    printf("=== kernel32 shim tests ===\n\n");

    CHECK(LoadLibraryA("metalsharp-kernel32-shim-missing-440.dylib") == NULL,
          "LoadLibraryA returns NULL when dlopen cannot load a library");
    CHECK(LoadLibraryW(NULL) == NULL, "LoadLibraryW returns NULL for a NULL name");

    if (argc != 2) {
        printf("  [FAIL] test plugin path argument is required\n");
        return 1;
    }

    char unicode_path[PATH_MAX];
    int written = snprintf(unicode_path, sizeof(unicode_path), "%s-\xE4\xB8\xAD\xF0\x9F\x98\x80", argv[1]);
    CHECK(written > 0 && (size_t)written < sizeof(unicode_path), "Unicode test path fits in PATH_MAX");
    if (written <= 0 || (size_t)written >= sizeof(unicode_path))
        return 1;

    unlink(unicode_path);
    CHECK(symlink(argv[1], unicode_path) == 0, "Unicode test library symlink is created");
    if (failures != 0) {
        unlink(unicode_path);
        return 1;
    }

    void* ansi_handle = LoadLibraryA(unicode_path);
    CHECK(ansi_handle != NULL, "LoadLibraryA opens the Unicode-named test library");

    uint16_t wide_path[PATH_MAX];
    size_t wide_length = copy_ascii_to_utf16(argv[1], wide_path, sizeof(wide_path) / sizeof(wide_path[0]));
    CHECK(wide_length != 0, "Test path is converted to UTF-16 with a surrogate pair");
    if (wide_length == 0) {
        unlink(unicode_path);
        return 1;
    }

    void* wide_handle = LoadLibraryW(wide_path);
    CHECK(wide_handle != NULL, "LoadLibraryW opens a UTF-16 Unicode path");
    CHECK(wide_handle == ansi_handle, "LoadLibraryW returns the real dlopen handle, not a sentinel");

    if (wide_handle != NULL && wide_handle == ansi_handle) {
        int (*test_symbol)(void) = (int (*)(void))dlsym(wide_handle, "metalsharp_kernel32_shim_test_symbol");
        CHECK(test_symbol != NULL && test_symbol() == 440, "UTF-16 path resolves the loaded library");
    }

    test_get_module_file_name(argv[0]);
    unlink(unicode_path);
    printf("\n=== Results: %d failure(s) ===\n", failures);
    return failures == 0 ? 0 : 1;
}
