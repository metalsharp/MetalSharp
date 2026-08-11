#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mscoree_unix.h"

typedef long NTSTATUS;
typedef NTSTATUS (*unixlib_call_t)(unsigned int, void*);

extern unixlib_call_t __wine_unix_call_funcs[2];

static int fail(const char* message) {
    fprintf(stderr, "test_mscoree_unix: %s\n", message);
    return 1;
}

int main(void) {
    unsetenv("_");

    struct mscoree_cor_exe_main_params params;
    memset(&params, 0, sizeof(params));
    strcpy(params.exe_path, "test-fixtures/managed-exit-37.exe");
    strcpy(params.exe_dir, "test-fixtures");
    params.exit_code = 99;

    if (__wine_unix_call_funcs[0](MSCOREE_FUNC_INIT, NULL) != 0)
        return fail("Mono initialization failed");

    NTSTATUS status = __wine_unix_call_funcs[1](MSCOREE_FUNC_COR_EXE_MAIN, &params);
    if (status != 37)
        return fail("the Unix bridge did not return Mono's exit code");
    if (params.exit_code != 37)
        return fail("the Unix bridge did not write the exit code to the launch parameters");

    return 0;
}
