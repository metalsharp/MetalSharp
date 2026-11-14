/* ngx_core.c: a D3DMetal launch points NGX at Apple's core, the next launch on another lane takes it back out, and
 * a prefix is changed only when its lane changes. A fake runtime Wine records each `reg` call. */
#include "metalsharp_backend/ngx_core.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char base[PATH_MAX];

static char* at(const char* relative) {
    static char paths[4][PATH_MAX];
    static int next;
    char* out = paths[next++ % 4];
    snprintf(out, PATH_MAX, "%s/%s", base, relative);
    return out;
}

static void make_dir(const char* relative) {
    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "mkdir -p '%s'", at(relative));
    assert(system(command) == 0);
}

static char* read_text(const char* path) {
    FILE* file = fopen(path, "r");
    char* text = calloc(1, 8192);
    assert(text);
    if (file) {
        size_t n = fread(text, 1, 8191, file);
        text[n] = '\0';
        fclose(file);
    }
    return text;
}

static int count(const char* text, const char* needle) {
    int n = 0;
    for (const char* p = text; (p = strstr(p, needle)); p += strlen(needle))
        n++;
    return n;
}

int main(int argc, char** argv) {
    char home[PATH_MAX], calls_path[PATH_MAX], prefix[PATH_MAX], marker[PATH_MAX];
    FILE* wine;
    char* calls;
    assert(argc == 2 && realpath(argv[1], base));
    snprintf(home, sizeof(home), "%s", at("home"));
    snprintf(calls_path, sizeof(calls_path), "%s", at("calls.log"));
    snprintf(prefix, sizeof(prefix), "%s", at("home/prefix-ubisoft"));
    snprintf(marker, sizeof(marker), "%s/%s", prefix, MS_NGX_CORE_MARKER);
    make_dir("home/runtime/wine/bin");
    make_dir("home/prefix-ubisoft");
    wine = fopen(at("home/runtime/wine/bin/wine"), "w");
    assert(wine);
    /* printf, not echo: sh's echo would read the \\n of \\nvlddmkm as a newline */
    fprintf(wine, "#!/bin/sh\nprintf '%%s\\n' \"$WINEPREFIX|$*\" >> '%s'\n", calls_path);
    fclose(wine);
    assert(chmod(at("home/runtime/wine/bin/wine"), 0755) == 0);
    assert(setenv("WINEPREFIX", prefix, 1) == 0);

    /* Nothing set yet: a Mullion launch changes nothing. */
    assert(ms_ngx_core_action_for(prefix, false) == MS_NGX_CORE_KEEP);
    assert(ms_ngx_core_route(home, "dxmt"));
    assert(access(calls_path, F_OK) != 0);

    /* A D3DMetal launch points both at System32, once. */
    assert(ms_ngx_core_action_for(prefix, true) == MS_NGX_CORE_SET);
    assert(ms_ngx_core_route(home, "d3dmetal"));
    calls = read_text(calls_path);
    assert(count(calls, "|reg add HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore /v FullPath /t REG_SZ /d "
                        "C:\\windows\\system32 /f\n") == 1);
    assert(count(calls, "|reg add HKLM\\SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore /v NGXPath /t REG_SZ "
                        "/d C:\\windows\\system32 /f\n") == 1);
    assert(count(calls, prefix) == 2);
    free(calls);
    assert(access(marker, F_OK) == 0);
    assert(ms_ngx_core_route(home, "d3dmetal"));
    calls = read_text(calls_path);
    assert(count(calls, "|reg ") == 2);
    free(calls);

    /* The next launch on another lane takes them back out, once, so Mullion writes its own. */
    assert(ms_ngx_core_action_for(prefix, false) == MS_NGX_CORE_CLEAR);
    assert(ms_ngx_core_route(home, "vkd3d"));
    calls = read_text(calls_path);
    assert(count(calls, "|reg delete HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore /v FullPath /f\n") == 1);
    assert(count(calls, "|reg delete HKLM\\SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore /v NGXPath /f\n") ==
           1);
    free(calls);
    assert(access(marker, F_OK) != 0);
    assert(ms_ngx_core_route(home, "dxmt"));
    calls = read_text(calls_path);
    assert(count(calls, "|reg ") == 4);
    free(calls);

    /* A prefix that is not there yet is left alone; without WINEPREFIX the Steam prefix is meant. */
    assert(setenv("WINEPREFIX", at("home/prefix-missing"), 1) == 0);
    assert(!ms_ngx_core_route(home, "d3dmetal"));
    assert(ms_ngx_core_route(home, "dxmt"));
    assert(unsetenv("WINEPREFIX") == 0);
    make_dir("home/prefix-steam");
    assert(ms_ngx_core_route(home, "d3dmetal"));
    assert(access(at("home/prefix-steam/" MS_NGX_CORE_MARKER), F_OK) == 0);
    calls = read_text(calls_path);
    assert(count(calls, "|reg ") == 6);
    free(calls);

    printf("ngx core test passed\n");
    return 0;
}
