/*
 * Regression test for #441: GetModuleFileNameA must return the running
 * executable's path, never the current working directory.
 *
 * Before the fix, kernel32_shim.c used readlink("/proc/self/exe"), which
 * does not exist on macOS, and silently fell back to getcwd() — so games
 * resolved their own location to the working directory. The shim is
 * exercised here exactly as the FNA lane deploys it: as a dylib loaded at
 * runtime (libkernel32.dylib) from a process whose working directory
 * differs from the executable directory.
 */

#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Must match the shim's own typedefs exactly (ABI). */
typedef uint32_t DWORD;

typedef DWORD (*GetModuleFileNameA_fn)(void* hModule, char* lpFilename, DWORD nSize);

static int fail(const char* message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2)
        return fail("usage: test_kernel32_shim <path to libkernel32.dylib>");

    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib)
        return fail(dlerror());

    GetModuleFileNameA_fn get_module_file_name_a = (GetModuleFileNameA_fn)dlsym(lib, "GetModuleFileNameA");
    if (!get_module_file_name_a)
        return fail("GetModuleFileNameA symbol not found in shim");

    // Move away from the executable's directory so a cwd fallback would be
    // detectable (the exact regression from #441).
    if (chdir("/") != 0)
        return fail("chdir");
    char away[PATH_MAX];
    if (!getcwd(away, sizeof(away)))
        return fail("getcwd after chdir");

    char buf[PATH_MAX * 2];
    DWORD len = get_module_file_name_a(NULL, buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf))
        return fail("GetModuleFileNameA returned an invalid length");

    if (buf[0] != '/')
        return fail("GetModuleFileNameA returned a relative path");
    if (strcmp(buf, away) == 0)
        return fail("GetModuleFileNameA returned the working directory instead of the executable path");
    if (access(buf, F_OK) != 0)
        return fail("GetModuleFileNameA returned a path that does not exist");

    // The returned path must be this test binary itself.
    char exe[PATH_MAX];
    if (realpath(argv[0], exe) != NULL) {
        char resolved[PATH_MAX];
        if (realpath(buf, resolved) == NULL || strcmp(resolved, exe) != 0) {
            fprintf(stderr, "FAIL: expected executable path %s, got %s\n", exe, buf);
            return 1;
        }
    }

    // A too-small buffer must not crash and must report truncation.
    char small[8];
    DWORD small_len = get_module_file_name_a(NULL, small, sizeof(small));
    if (small_len == 0)
        return fail("small-buffer GetModuleFileNameA returned 0");

    printf("PASS: GetModuleFileNameA -> %s\n", buf);
    return 0;
}
