/* FEX runtime and shell-injection regressions use a scratch home. An optional real
 * MetalSharp Wine app (argv[2]) also checks its facade. Built with -DMS_BACKEND_VARIANT="fex". */
#include "../runtime/wine_runtime.c"
#include "metalsharp_backend/cache.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define EXPECT(condition)                                                                                              \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #condition);                                   \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void write_text(const char* path, const char* text) {
    FILE* file = fopen(path, "wb");
    if (file) {
        fputs(text, file);
        fclose(file);
    }
}

static void test_app_identity(const char* home) {
    static const char identity[] = "CDHash=0123456789abcdef0123456789abcdef01234567";
    static const char* const cases[] = {
        "Executable=/example\nCDHash=0123456789abcdef0123456789abcdef01234567\nSignature=adhoc\n",
        "",
        "CandidateCDHash sha256=0123456789abcdef0123456789abcdef01234567\n",
        "CDHash=short\n",
        "CDHash=0123456789abcdef0123456789abcdef0123456g\n",
        "CDHash=0123456789abcdef0123456789abcdef01234567extra\n",
        "CDHash=0123456789abcdef0123456789abcdef01234567\nCDHash=0123456789abcdef0123456789abcdef01234567\n"};
    char parsed[256], expected[256], path[PATH_MAX], previous[PATH_MAX];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char* output = strdup(cases[i]);
        EXPECT(output && parse_app_identity(output, parsed, sizeof(parsed)) == (i == 0));
        if (i == 0)
            EXPECT(!strcmp(parsed, identity));
        free(output);
    }
    EXPECT(app_identity("/usr/bin/true", expected, sizeof(expected)));
    EXPECT(!strncmp(expected, "CDHash=", 7) && strlen(expected) == 47);
    if (!getcwd(previous, sizeof(previous)) || chdir(home) != 0) {
        EXPECT(false);
        return;
    }
    static const char* const names[] = {"signed-wine", "O'Connor-wine", "wine'; touch identity-injected; #"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", home, names[i]);
        const char* const copy[] = {"/bin/cp", "/usr/bin/true", path, NULL};
        EXPECT(run_wait(copy, NULL, NULL, 5));
        EXPECT(app_identity(path, parsed, sizeof(parsed)));
        EXPECT(!strcmp(parsed, expected));
    }
    EXPECT(access("identity-injected", F_OK) != 0);
    snprintf(path, sizeof(path), "%s/unsigned-wine", home);
    write_text(path, "unsigned\n");
    EXPECT(!app_identity(path, parsed, sizeof(parsed)));
    EXPECT(!app_identity("/nonexistent/metalsharp-wine", parsed, sizeof(parsed)));
    EXPECT(!app_identity("/usr/bin/true", parsed, 47));
    EXPECT(chdir(previous) == 0);
}

static void test_facade_paths(const char* home) {
    static const char* const names[] = {"plain-home", "O'Connor-home", "home$(touch facade-dollar)",
                                        "home`touch facade-backtick`", "home\"; touch facade-quote; #"};
    char previous[PATH_MAX], fixture[PATH_MAX], path[PATH_MAX], result[PATH_MAX], error[256];
    if (!getcwd(previous, sizeof(previous)) || chdir(home) != 0) {
        EXPECT(false);
        return;
    }
    snprintf(result, sizeof(result), "%s/facade-result", home);
    EXPECT(setenv("METALSHARP_TEST_OUTPUT", result, 1) == 0);
    EXPECT(unsetenv("WINEPREFIX") == 0);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(fixture, sizeof(fixture), "%s/%s", home, names[i]);
        EXPECT(mkdir(fixture, 0755) == 0);
        static const char* const dirs[] = {"runtime", "runtime/MetalSharp-Wine.app",
                                           "runtime/MetalSharp-Wine.app/Contents",
                                           "runtime/MetalSharp-Wine.app/Contents/MacOS"};
        for (size_t j = 0; j < sizeof(dirs) / sizeof(dirs[0]); j++) {
            EXPECT(format_path(path, sizeof(path), "%s/%s", fixture, dirs[j]));
            EXPECT(mkdir(path, 0755) == 0);
        }
        EXPECT(format_path(path, sizeof(path), "%s/%s", fixture, "runtime/MetalSharp-Wine.app/Contents/MacOS/wine"));
        write_text(path,
                   "#!/bin/sh\nprintf '%s\\n' \"$WINEPREFIX\" \"$WINESERVER\" \"$1\" > \"$METALSHARP_TEST_OUTPUT\"\n");
        EXPECT(chmod(path, 0755) == 0);
        EXPECT(ms_wine_fex_write_facade(fixture, error, sizeof(error)));
        if (i == 0) {
            EXPECT(format_path(path, sizeof(path), "%s/%s", fixture, "runtime/wine/" FACADE_MARKER));
            write_text(path, "2\n");
            EXPECT(!ms_wine_fex_facade_ready(fixture));
            EXPECT(ms_wine_fex_write_facade(fixture, error, sizeof(error)));
            EXPECT(ms_wine_fex_facade_ready(fixture));
        }
        EXPECT(format_path(path, sizeof(path), "%s/%s", fixture, "runtime/wine/bin/wine"));
        const char* const launch[] = {path, "literal game argument", NULL};
        EXPECT(run_wait(launch, NULL, NULL, 5));
        FILE* file = fopen(result, "r");
        char output[PATH_MAX * 2 + 64] = "", expected[PATH_MAX * 2 + 64];
        if (file) {
            size_t got = fread(output, 1, sizeof(output) - 1, file);
            output[got] = '\0';
            fclose(file);
        }
        snprintf(expected, sizeof(expected),
                 "%s/prefix-steam\n%s/runtime/MetalSharp-Wine.app/Contents/MacOS/wineserver\nliteral game argument\n",
                 fixture, fixture);
        EXPECT(file && !strcmp(output, expected));
        unlink(result);
    }
    EXPECT(access("facade-dollar", F_OK) != 0);
    EXPECT(access("facade-backtick", F_OK) != 0);
    EXPECT(access("facade-quote", F_OK) != 0);
    EXPECT(unsetenv("METALSHARP_TEST_OUTPUT") == 0);
    EXPECT(chdir(previous) == 0);
}

int main(int argc, char** argv) {
    char path[PATH_MAX], other[PATH_MAX], command[PATH_MAX * 3], output[256] = "";
    const char* home = argc > 1 ? argv[1] : NULL;
    const char* app = argc > 2 ? argv[2] : NULL;
    FILE* pipe;

    EXPECT(ms_wine_is_fex() && !strcmp(ms_wine_variant(), "fex"));
    EXPECT(!strcmp(ms_wine_route_pipeline("d3dmetal"), "vkd3d"));
    EXPECT(!strcmp(ms_wine_route_pipeline("vkd3d"), "vkd3d"));
    EXPECT(!strcmp(ms_wine_fex_graphics_backend("vkd3d"), "vkd3d"));
    EXPECT(!strcmp(ms_wine_route_pipeline("m13"), "vkd3d"));
    EXPECT(!strcmp(ms_wine_route_pipeline("d3d9"), "dxvk"));
    EXPECT(!strcmp(ms_wine_route_pipeline("fna_arm64"), "dxmt"));
    EXPECT(!strcmp(ms_wine_fex_graphics_backend("wine_bare"), "wine"));
    EXPECT(!strcmp(ms_wine_fex_graphics_backend("dxvk"), "dxvk"));
    EXPECT(!strcmp(ms_wine_fex_graphics_backend("dxmt_32"), "dxmt"));
    EXPECT(ms_wine_is_runtime_wine_path("/h", "/h/runtime/MetalSharp-Wine.app/Contents/MacOS/wine"));
    EXPECT(ms_wine_is_runtime_wine_path("/h", "/h/runtime/MetalSharp-Wine.app/Contents/lib/wine/aarch64-unix/wine"));
    EXPECT(ms_wine_is_runtime_wine_path("/h", "/h/runtime/wine/lib/wine/x86_64-unix/wine"));
    EXPECT(!ms_wine_is_runtime_wine_path("/h", "/h/runtime/MetalSharp-Wine.app/Contents/MacOS/wineserver"));
    EXPECT(!ms_wine_is_runtime_wine_path("/h", "/other/runtime/wine/bin/wine"));
    EXPECT(ms_wine_server_path("/h", path, sizeof(path)) &&
           !strcmp(path, "/h/runtime/MetalSharp-Wine.app/Contents/MacOS/wineserver"));
    if (!home) {
        fprintf(stderr, "usage: %s <scratch home> [MetalSharp-Wine.app]\n", argv[0]);
        return 2;
    }
    test_app_identity(home);
    test_facade_paths(home);

    /* Settings' cache rows on the fex build: each lane's size from its fex-<lane> folders, and a
     * lane's Clear empties only that lane. (Mullion's lane also measures the user's cache folder, so
     * it is not cleared here.) */
    {
        static const char clear_vkd3d[] = "{\"type\":\"lane\",\"lane\":\"vkd3d\"}";
        static const char clear_unknown[] = "{\"type\":\"lane\",\"lane\":\"x\"}";
        static const char clear_dxmt[] = "{\"type\":\"lane\",\"lane\":\"dxmt\"}";
        char* json;
        snprintf(command, sizeof(command), "mkdir -p '%s/shader-cache/fex-dxvk/77' '%s/pipeline-cache/fex-vkd3d/123'",
                 home, home);
        EXPECT(system(command) == 0);
        snprintf(path, sizeof(path), "%s/pipeline-cache/fex-vkd3d/123/vkd3d-proton.cache", home);
        write_text(path, "vkd3d pipelines\n");
        snprintf(path, sizeof(path), "%s/shader-cache/fex-dxvk/77/game.dxvk-cache", home);
        write_text(path, "dxvk state\n");
        json = ms_cache_size_json(home);
        EXPECT(json && strstr(json, "\"lanes\":{\"dxmt\":") && strstr(json, "\"vkd3d\":{\"bytes\":16,"));
        EXPECT(json && strstr(json, "\"dxvk\":{\"bytes\":11,\"files\":1,\"apps\":1,"));
        free(json);
        json = ms_cache_clear_json(home, (const unsigned char*)clear_vkd3d, strlen(clear_vkd3d));
        EXPECT(json && strstr(json, "\"bytes_freed\":16"));
        free(json);
        snprintf(path, sizeof(path), "%s/pipeline-cache/fex-vkd3d/123", home);
        EXPECT(access(path, F_OK) != 0);
        snprintf(path, sizeof(path), "%s/shader-cache/fex-dxvk/77/game.dxvk-cache", home);
        EXPECT(access(path, F_OK) == 0);
        json = ms_cache_clear_json(home, (const unsigned char*)clear_unknown, strlen(clear_unknown));
        EXPECT(json && strstr(json, "\"ok\":false"));
        free(json);
        /* Mullion's Clear empties MetalSharp's per-game folders and, without "shared", leaves Metal's
         * cache that every Mullion game shares (reported apart) alone. */
        snprintf(command, sizeof(command), "mkdir -p '%s/shader-cache/fex-dxmt/9' '%s/shader-cache/fex-dxmt_32/8'", home,
                 home);
        EXPECT(system(command) == 0);
        snprintf(path, sizeof(path), "%s/shader-cache/fex-dxmt/9/shaders_320.db", home);
        write_text(path, "mullion shaders\n");
        json = ms_cache_size_json(home);
        EXPECT(json && strstr(json, "\"shared_bytes\":") && strstr(json, "\"apps\":2,"));
        free(json);
        json = ms_cache_clear_json(home, (const unsigned char*)clear_dxmt, strlen(clear_dxmt));
        EXPECT(json && strstr(json, "\"bytes_freed\":16"));
        free(json);
        EXPECT(access(path, F_OK) != 0);
        snprintf(path, sizeof(path), "%s/shader-cache/fex-dxmt", home);
        EXPECT(access(path, F_OK) == 0);
    }

    /* The startup refresh and a migration's install never replace MetalSharp Wine at once: the
     * install lock is exclusive, and a pending or running migration keeps the refresh away. */
    {
        int held = ms_wine_fex_lock(home, false);
        EXPECT(held >= 0);
        EXPECT(ms_wine_fex_lock(home, false) == -1);
        ms_wine_fex_unlock(held);
        held = ms_wine_fex_lock(home, false);
        EXPECT(held >= 0);
        ms_wine_fex_unlock(held);
        EXPECT(!ms_wine_fex_refresh_blocked(home));
        snprintf(path, sizeof(path), "%s/.post-update-migration", home);
        write_text(path, "{\"needed\":true}\n");
        EXPECT(ms_wine_fex_refresh_blocked(home));
        unlink(path);
        snprintf(path, sizeof(path), "%s/migration.lock", home);
        snprintf(other, sizeof(other), "%ld\n", (long)getpid());
        write_text(path, other);
        EXPECT(ms_wine_fex_refresh_blocked(home));
        write_text(path, "999999\n");
        EXPECT(!ms_wine_fex_refresh_blocked(home));
        unlink(path);
    }

    /* Caches earlier FEX builds left next to a game go, once: exactly vkd3d-proton.cache(.write)
     * and <exe>.dxvk-cache; everything else in the folder stays. */
    {
        static const char* const removed[] = {"vkd3d-proton.cache", "vkd3d-proton.cache.write",
                                              "Game.dxvk-cache"};
        static const char* const kept[] = {"Other.dxvk-cache", "vkd3d-proton.cache.bak", "save.dat",
                                           "Game.exe"};
        snprintf(command, sizeof(command), "mkdir -p '%s/old-caches/sub/vkd3d-proton.cache'", home);
        EXPECT(system(command) == 0);
        for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
            snprintf(path, sizeof(path), "%s/old-caches/%s", home, removed[i]);
            write_text(path, "cache\n");
        }
        for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
            snprintf(path, sizeof(path), "%s/old-caches/%s", home, kept[i]);
            write_text(path, "keep\n");
        }
        snprintf(path, sizeof(path), "%s/old-caches", home);
        snprintf(other, sizeof(other), "%s/old-caches/Game.exe", home);
        EXPECT(ms_wine_fex_remove_old_game_caches(path, other) == 3);
        for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
            snprintf(other, sizeof(other), "%s/old-caches/%s", home, removed[i]);
            EXPECT(access(other, F_OK) != 0);
        }
        for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
            snprintf(other, sizeof(other), "%s/old-caches/%s", home, kept[i]);
            EXPECT(access(other, F_OK) == 0);
        }
        snprintf(other, sizeof(other), "%s/old-caches/sub/vkd3d-proton.cache", home);
        EXPECT(access(other, F_OK) == 0);
        /* Once: a cache written after the cleanup is the game's own. */
        snprintf(other, sizeof(other), "%s/old-caches/vkd3d-proton.cache", home);
        write_text(other, "new cache\n");
        EXPECT(ms_wine_fex_remove_old_game_caches(path, "Game.exe") == 0);
        EXPECT(access(other, F_OK) == 0);
    }

    /* A prefix the fex build converted gets its x86_64 backup back; one without a backup is
     * counted apart (not a failure to retry) and left alone; an x86_64 prefix is not touched. */
    {
        static const char* const dirs[] = {"prefix-steam/drive_c/windows",
                                           "prefix-steam/.metalsharp-x86_64-backup/drive_c/windows",
                                           "prefix-ubisoft/drive_c/windows", "sharp-prefix/drive_c/windows"};
        char command[PATH_MAX * 2];
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            snprintf(command, sizeof(command), "mkdir -p '%s/%s'", home, dirs[i]);
            EXPECT(system(command) == 0);
        }
        snprintf(path, sizeof(path), "%s/prefix-steam/system.reg", home);
        write_text(path, "arm64 registry\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/windows/arm64.txt", home);
        write_text(path, "arm64\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-wine-arch", home);
        write_text(path, "arm64\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup/system.reg", home);
        write_text(path, "x86_64 registry\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup/drive_c/windows/x86_64.txt", home);
        write_text(path, "x86_64\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup/.complete", home);
        write_text(path, "x86_64\n");
        snprintf(path, sizeof(path), "%s/prefix-ubisoft/system.reg", home);
        write_text(path, "arm64 registry\n");
        snprintf(path, sizeof(path), "%s/prefix-ubisoft/.metalsharp-wine-arch", home);
        write_text(path, "arm64\n");
        snprintf(path, sizeof(path), "%s/sharp-prefix/system.reg", home);
        write_text(path, "x86_64 registry\n");
        unsigned without_backup = 0;
        EXPECT(ms_wine_restore_x86_64_prefixes(home, &without_backup) == 0);
        EXPECT(without_backup == 1);
        snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/windows/x86_64.txt", home);
        EXPECT(access(path, R_OK) == 0);
        snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/windows/arm64.txt", home);
        EXPECT(access(path, F_OK) != 0);
        snprintf(path, sizeof(path), "%s/prefix-steam", home);
        EXPECT(!ms_wine_prefix_is_arm64(path));
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup", home);
        EXPECT(access(path, F_OK) != 0);
        snprintf(path, sizeof(path), "%s/prefix-ubisoft", home);
        EXPECT(ms_wine_prefix_is_arm64(path));
        snprintf(command, sizeof(command), "rm -rf '%s/prefix-steam' '%s/prefix-ubisoft' '%s/sharp-prefix'", home, home,
                 home);
        EXPECT(system(command) == 0);
    }

    /* A conversion whose prepare fails after its backup completed (here: no MetalSharp Wine app) leaves no
     * arch marker, though MetalSharp Wine's wineboot may have changed the prefix. Going back to the regular
     * build restores it anyway and consumes the backup, so a later move backs the prefix up afresh; a
     * restore that fails (an unreadable backup, like a full disk) leaves the prefix whole and the backup
     * for the next try. */
    {
        char prefix[PATH_MAX], command[PATH_MAX * 2], error[256] = "";
        unsigned without_backup = 0;
        FILE* file;
        char text[64];
        snprintf(prefix, sizeof(prefix), "%s/bottles/cycle/prefix", home);
        snprintf(command, sizeof(command), "mkdir -p '%s/drive_c/windows'", prefix);
        EXPECT(system(command) == 0);
        snprintf(path, sizeof(path), "%s/system.reg", prefix);
        write_text(path, "x86_64 v1\n");
        snprintf(path, sizeof(path), "%s/drive_c/windows/x86_64.txt", prefix);
        write_text(path, "x86_64\n");
        EXPECT(!ms_wine_fex_convert_prefix(home, prefix, error, sizeof(error)));
        snprintf(path, sizeof(path), "%s/.metalsharp-x86_64-backup/.complete", prefix);
        EXPECT(access(path, F_OK) == 0);
        EXPECT(!ms_wine_prefix_is_arm64(prefix));
        /* What MetalSharp Wine's wineboot did before the prepare failed. */
        snprintf(path, sizeof(path), "%s/system.reg", prefix);
        write_text(path, "arm64 wineboot\n");
        snprintf(path, sizeof(path), "%s/drive_c/windows/arm64.txt", prefix);
        write_text(path, "arm64\n");
        EXPECT(ms_wine_restore_x86_64_prefixes(home, &without_backup) == 0);
        snprintf(path, sizeof(path), "%s/system.reg", prefix);
        file = fopen(path, "r");
        EXPECT(file && fgets(text, sizeof(text), file) && !strcmp(text, "x86_64 v1\n"));
        if (file)
            fclose(file);
        snprintf(path, sizeof(path), "%s/drive_c/windows/arm64.txt", prefix);
        EXPECT(access(path, F_OK) != 0);
        snprintf(path, sizeof(path), "%s/.metalsharp-x86_64-backup", prefix);
        EXPECT(access(path, F_OK) != 0);

        /* Games installed on the regular build since: the next move backs this state up, not the old one. */
        snprintf(path, sizeof(path), "%s/system.reg", prefix);
        write_text(path, "x86_64 v2\n");
        EXPECT(!ms_wine_fex_convert_prefix(home, prefix, error, sizeof(error)));
        snprintf(path, sizeof(path), "%s/.metalsharp-x86_64-backup/system.reg", prefix);
        file = fopen(path, "r");
        EXPECT(file && fgets(text, sizeof(text), file) && !strcmp(text, "x86_64 v2\n"));
        if (file)
            fclose(file);

        /* A restore that can't copy the backup leaves the prefix's own drive_c/windows and the backup. */
        snprintf(command, sizeof(command), "chmod 000 '%s/.metalsharp-x86_64-backup/drive_c/windows'", prefix);
        EXPECT(system(command) == 0);
        EXPECT(ms_wine_restore_x86_64_prefixes(home, &without_backup) == 1);
        snprintf(path, sizeof(path), "%s/drive_c/windows/x86_64.txt", prefix);
        EXPECT(access(path, R_OK) == 0);
        snprintf(path, sizeof(path), "%s/.metalsharp-x86_64-backup/.complete", prefix);
        EXPECT(access(path, F_OK) == 0);
        snprintf(command, sizeof(command), "chmod 755 '%s/.metalsharp-x86_64-backup/drive_c/windows'", prefix);
        EXPECT(system(command) == 0);
        EXPECT(ms_wine_restore_x86_64_prefixes(home, &without_backup) == 0);
        snprintf(command, sizeof(command), "rm -rf '%s/bottles'", home);
        EXPECT(system(command) == 0);
    }

    /* No app: refused, and nothing is created. */
    snprintf(path, sizeof(path), "%s/runtime", home);
    mkdir(path, 0755);
    EXPECT(!ms_wine_fex_write_facade(home, other, sizeof(other)));
    EXPECT(!ms_wine_fex_facade_ready(home));

    if (!app) {
        printf("wine runtime tests passed (no real MetalSharp Wine app)\n");
        return failures ? 1 : 0;
    }
    snprintf(path, sizeof(path), "%s/runtime/MetalSharp-Wine.app", home);
    EXPECT(symlink(app, path) == 0);

    /* A 0.80.0 x86_64 runtime is kept aside as runtime/wine-x86_64. */
    snprintf(path, sizeof(path), "%s/runtime/wine", home);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/runtime/wine/x86_64-runtime", home);
    write_text(path, "x86_64\n");
    EXPECT(ms_wine_fex_write_facade(home, other, sizeof(other)));
    snprintf(path, sizeof(path), "%s/runtime/wine-x86_64/x86_64-runtime", home);
    EXPECT(access(path, R_OK) == 0);
    EXPECT(ms_wine_fex_facade_ready(home));
    /* Writing it again (an update) keeps the runtime that was set aside. */
    EXPECT(ms_wine_fex_write_facade(home, other, sizeof(other)));
    EXPECT(access(path, R_OK) == 0);

    /* The x86_64 runtime's paths do not reach MetalSharp Wine. */
    snprintf(command, sizeof(command),
             "DYLD_FALLBACK_LIBRARY_PATH=/nonexistent WINEDLLPATH=/nonexistent VK_ICD_FILENAMES=/nonexistent "
             "'%s/runtime/wine/bin/wine' --version 2>&1",
             home);
    pipe = popen(command, "r");
    if (pipe) {
        if (!fgets(output, sizeof(output), pipe))
            output[0] = '\0';
        pclose(pipe);
    }
    EXPECT(strstr(output, "MetalSharp Wine") != NULL);
    snprintf(path, sizeof(path), "%s/runtime/wine/bin/wineserver", home);
    EXPECT(access(path, X_OK) == 0);
    snprintf(path, sizeof(path), "%s/runtime/wine/lib/wine/aarch64-unix/wine", home);
    EXPECT(access(path, X_OK) == 0);
    if (failures)
        fprintf(stderr, "wine --version said: %s\n", output);
    else
        printf("wine runtime tests passed (%s)", output);
    return failures ? 1 : 0;
}
