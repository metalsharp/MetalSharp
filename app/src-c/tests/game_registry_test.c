/* game_registry.c: a launch of Assetto Corsa writes its AppDefaults values into the prefix once, a game with none
 * changes nothing, and a missing prefix is left alone. A fake runtime Wine records each `reg` call. */
#include "metalsharp_backend/game_registry.h"
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
    const ms_game_registry_value* values;
    size_t n;
    FILE* file;
    char* calls;
    assert(argc == 2 && realpath(argv[1], base));
    snprintf(home, sizeof(home), "%s", at("home"));
    snprintf(calls_path, sizeof(calls_path), "%s", at("calls.log"));
    snprintf(prefix, sizeof(prefix), "%s", at("home/prefix-steam"));
    snprintf(marker, sizeof(marker), "%s/%s/244210", prefix, MS_GAME_REGISTRY_MARKER_DIR);
    make_dir("home/runtime/wine/bin");
    make_dir("home/prefix-steam");
    file = fopen(at("home/runtime/wine/bin/wine"), "w");
    assert(file);
    /* printf, not echo: sh's echo would read the backslashes */
    fprintf(file, "#!/bin/sh\nprintf '%%s\\n' \"$WINEPREFIX|$*\" >> '%s'\n", calls_path);
    fclose(file);
    assert(chmod(at("home/runtime/wine/bin/wine"), 0755) == 0);

    /* A game with no values: nothing to do. */
    assert(!ms_game_registry_values(570, &n) && n == 0);
    assert(ms_game_registry_apply(home, prefix, 570));
    assert(access(calls_path, F_OK) != 0);

    /* Assetto Corsa: its launcher's assembly folder and the game's d3dx11_43, once. */
    values = ms_game_registry_values(244210, &n);
    assert(values && n == 2);
    assert(ms_game_registry_apply(home, prefix, 244210));
    calls = read_text(calls_path);
    assert(count(calls,
                 "|reg add HKCU\\Software\\Wine\\AppDefaults\\AssettoCorsa.exe\\Mono /v PrivatePath /t REG_SZ /d "
                 "launcher\\support /f\n") == 1);
    assert(count(calls, "|reg add HKCU\\Software\\Wine\\AppDefaults\\acs.exe\\DllOverrides /v d3dx11_43 /t REG_SZ /d "
                        "native,builtin /f\n") == 1);
    assert(count(calls, prefix) == 2);
    free(calls);
    assert(access(marker, F_OK) == 0);
    assert(ms_game_registry_apply(home, prefix, 244210));
    calls = read_text(calls_path);
    assert(count(calls, "|reg ") == 2);
    free(calls);

    /* A marker from an older revision writes them again. */
    file = fopen(marker, "w");
    assert(file);
    fputs("0\n", file);
    fclose(file);
    assert(ms_game_registry_apply(home, prefix, 244210));
    calls = read_text(calls_path);
    assert(count(calls, "|reg ") == 4);
    free(calls);

    /* A prefix not made yet is left alone. */
    assert(!ms_game_registry_apply(home, at("home/prefix-missing"), 244210));
    calls = read_text(calls_path);
    assert(count(calls, "|reg ") == 4);
    free(calls);

    printf("game registry test passed\n");
    return 0;
}
