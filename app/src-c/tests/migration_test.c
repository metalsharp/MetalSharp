#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned steam_stop_calls = 0;
static unsigned ubisoft_stop_calls = 0;

char* ms_setup_install_all_json(const char* home, int* status) {
    (void)home;
    (void)status;
    return NULL;
}

char* ms_steam_stop_json(const char* home, int* status) {
    assert(home != NULL);
    steam_stop_calls++;
    if (status)
        *status = 200;
    return strdup("{\"ok\":true,\"running\":false}");
}

char* ms_ubisoft_stop_json(const char* home, int* status) {
    assert(home != NULL);
    ubisoft_stop_calls++;
    if (status)
        *status = 200;
    return strdup("{\"ok\":true,\"running\":false}");
}

#include <stdbool.h>

/* migration.c now clears quarantine on staged lanes and guarantees the
 * Steam wrappers; the test tree links neither setup.c nor steam_actions.c,
 * so provide no-ops. */
void ms_clear_quarantine_tree(const char* path) {
    (void)path;
}

bool ms_steam_wrappers_ensure(const char* home) {
    (void)home;
    return true;
}

bool ms_steam_usability_ensure(const char* home) {
    (void)home;
    return true;
}

#include "../runtime/migration.c"

static void make_directory(const char* path) {
    char command[4096];
    snprintf(command, sizeof(command), "mkdir -p '%s'", path);
    assert(system(command) == 0);
}

static void write_file(const char* path, const char* contents) {
    FILE* file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}

static bool file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

/* The migration each update takes, per build and per version it comes from. */
static void test_migration_paths(void) {
    /* FEX 1.0.0 -> FEX 1.1.0: the updater's handoff marker, setup.json recorded by the FEX build. */
    assert(ms_migration_path_for("fex", "fex", "1.0.0", true, true, true) == MS_MIGRATION_PATH_FEX_UPDATE);
    /* ... also when the runtime needs repair, and never the 0.80.0 conversion or the welcome again. */
    assert(ms_migration_path_for("fex", "fex", "1.0.0", true, false, false) == MS_MIGRATION_PATH_FEX_UPDATE);
    assert(!ms_migration_fex_welcome_due(ms_migration_path_for("fex", "fex", "1.0.0", true, true, true)));
    /* FEX with nothing to do. */
    assert(ms_migration_path_for("fex", "fex", "1.1.0", true, false, true) == MS_MIGRATION_PATH_NONE);

    /* Regular 0.80.x -> regular 1.1.0: setup.json from 0.80.x has no runtime_variant. The regular
     * build's own migration, with no prefix restore. */
    assert(ms_migration_path_for("regular", NULL, "0.80.0", true, true, true) == MS_MIGRATION_PATH_FULL);
    assert(ms_migration_path_for("regular", NULL, "0.80.3", true, false, false) == MS_MIGRATION_PATH_FULL);
    assert(ms_migration_path_for("regular", "regular", "1.0.0", true, true, true) == MS_MIGRATION_PATH_FULL);
    assert(ms_migration_path_for("regular", NULL, "0.80.0", true, false, true) == MS_MIGRATION_PATH_NONE);
    assert(!ms_migration_fex_welcome_due(MS_MIGRATION_PATH_FULL));

    /* Regular -> FEX through the updater's build switch, or 0.80.0 -> FEX 1.0.0: the full migration,
     * the prefix conversion and, once, the welcome. Without the handoff marker too (a DMG installed by
     * hand), and with a ready runtime. */
    assert(ms_migration_path_for("fex", NULL, "0.80.0", true, true, false) == MS_MIGRATION_PATH_REGULAR_TO_FEX);
    assert(ms_migration_path_for("fex", "regular", "1.0.0", true, false, true) ==
           MS_MIGRATION_PATH_REGULAR_TO_FEX);
    assert(ms_migration_fex_welcome_due(MS_MIGRATION_PATH_REGULAR_TO_FEX));
    /* A FEX record from before 1.0.0 (a pre-release) is converted again. */
    assert(ms_migration_path_for("fex", "fex", "0.80.0", true, true, true) == MS_MIGRATION_PATH_REGULAR_TO_FEX);

    /* FEX -> regular: the regular migration, with the x86_64 backups restored. */
    assert(ms_migration_path_for("regular", "fex", "1.0.0", true, true, true) == MS_MIGRATION_PATH_FEX_TO_REGULAR);
    assert(ms_migration_path_for("regular", "fex", "1.0.0", true, false, true) ==
           MS_MIGRATION_PATH_FEX_TO_REGULAR);

    /* A fresh install of either build migrates nothing and shows no welcome. */
    assert(ms_migration_path_for("fex", NULL, NULL, false, false, false) == MS_MIGRATION_PATH_NONE);
    assert(ms_migration_path_for("regular", NULL, NULL, false, false, false) == MS_MIGRATION_PATH_NONE);
    assert(!ms_migration_fex_welcome_due(MS_MIGRATION_PATH_NONE));
}

int main(void) {
    char home[256];
    char path[512];
    preserved_data preserved;

    test_migration_paths();
    /* Back on the regular build, the old runtime goes completely, MetalSharp Wine's sealed
     * (read-only) app included. */
    {
        char runtime_home[256], app[512], command[1200];
        snprintf(runtime_home, sizeof(runtime_home), "/tmp/metalsharp-readonly-test-%ld", (long)getpid());
        remove_tree_local(runtime_home);
        snprintf(app, sizeof(app), "%s/runtime/MetalSharp-Wine.app/Contents/MacOS", runtime_home);
        make_directory(app);
        snprintf(app, sizeof(app), "%s/runtime/MetalSharp-Wine.app/Contents/MacOS/wine", runtime_home);
        write_file(app, "wine");
        snprintf(app, sizeof(app), "%s/runtime/MetalSharp-Wine.app", runtime_home);
        snprintf(command, sizeof(command), "chmod -R a-w '%s'", app);
        assert(system(command) == 0);
        if (ms_wine_is_fex()) {
            /* The FEX build keeps it, still sealed. */
            struct stat info;
            remove_old_runtime(runtime_home);
            assert(stat(app, &info) == 0 && (info.st_mode & S_IWUSR) == 0);
            snprintf(command, sizeof(command), "chmod -R u+w '%s'", app);
            assert(system(command) == 0);
        } else {
            remove_old_runtime(runtime_home);
            assert(!file_exists(app));
        }
        remove_tree_local(runtime_home);
        assert(!file_exists(runtime_home));
    }
    /* The welcome is the FEX build's: the regular build never shows it, whatever is recorded. */
    {
        char welcome_home[256], marker[512];
        char* json;
        snprintf(welcome_home, sizeof(welcome_home), "/tmp/metalsharp-welcome-test-%ld", (long)getpid());
        remove_tree_local(welcome_home);
        make_directory(welcome_home);
        snprintf(marker, sizeof(marker), "%s/%s", welcome_home, FEX_WELCOME_PENDING);
        write_file(marker, "1\n");
        json = ms_migration_fex_welcome_json(welcome_home);
        assert(json && strstr(json, "\"show\":false"));
        free(json);
        json = ms_migration_fex_welcome_dismiss_json(welcome_home);
        assert(json && strstr(json, "\"ok\":true"));
        free(json);
        assert(!file_exists(marker));
        snprintf(marker, sizeof(marker), "%s/%s", welcome_home, FEX_WELCOME_SHOWN);
        assert(file_exists(marker));
        remove_tree_local(welcome_home);
    }
    snprintf(home, sizeof(home), "/tmp/metalsharp-migration-test-%ld", (long)getpid());
    remove_tree_local(home);
    make_directory(home);
    assert(stop_managed_wine_processes(home));
    assert(steam_stop_calls == 1);
    assert(ubisoft_stop_calls == 1);

    /* Content verification was removed from migration entirely: hash drift,
     * signature format changes, and version strings must never fail a
     * finished install. Only the presence of the staged manifest matters
     * now, and that is covered by the runtime-ready simulation. */
    unlink(path);

    snprintf(path, sizeof(path), "%s/setup.json", home);
    write_file(path, "{\"completed\":true,\"deviceName\":\"test\"}");
    snprintf(path, sizeof(path), "%s/cache/downloads", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/cache/downloads/payload", home);
    write_file(path, "download payload");
    snprintf(path, sizeof(path), "%s/cache/steam_config.json", home);
    write_file(path, "{\"api_key\":\"secret\"}");

    snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/Program Files (x86)/Steam/steamapps", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/appmanifest_440.acf",
             home);
    write_file(path, "manifest");
    snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/Program Files/Game", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/Program Files/Game/game.exe", home);
    write_file(path, "game payload");

    snprintf(path, sizeof(path), "%s/prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher", home);
    make_directory(path);
    snprintf(path, sizeof(path),
             "%s/prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/UbisoftConnect.exe", home);
    write_file(path, "Ubisoft Connect install payload");
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices", home);
    make_directory(path);
    {
        char c_drive[512], y_drive[512], z_drive[512];
        snprintf(c_drive, sizeof(c_drive), "%s/prefix-ubisoft/dosdevices/c:", home);
        snprintf(y_drive, sizeof(y_drive), "%s/prefix-ubisoft/dosdevices/y:", home);
        snprintf(z_drive, sizeof(z_drive), "%s/prefix-ubisoft/dosdevices/z:", home);
        assert(symlink("../drive_c", c_drive) == 0);
        assert(symlink("/Volumes/AverySSD", y_drive) == 0);
        assert(symlink("/", z_drive) == 0);
    }
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/drive_c/windows", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/drive_c/windows/user.reg", home);
    write_file(path, "gog settings");
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/dosdevices", home);
    make_directory(path);
    {
        char c_drive[512], z_drive[512];
        snprintf(c_drive, sizeof(c_drive), "%s/bottles/gog-prefix/prefix/dosdevices/c:", home);
        snprintf(z_drive, sizeof(z_drive), "%s/bottles/gog-prefix/prefix/dosdevices/z:", home);
        assert(symlink("../drive_c", c_drive) == 0);
        assert(symlink("/", z_drive) == 0);
    }
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/drive_c/windows", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/bottle.json", home);
    write_file(path, "{\"id\":\"epic_TestGame\",\"mouse_mode\":\"no-recenter\"}");
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/user.reg", home);
    write_file(path, "MouseWarpOverride=disable");
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/system.reg", home);
    write_file(path, "Wine registry");
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/drive_c/windows/runtime.dll", home);
    write_file(path, "runtime payload");
    snprintf(path, sizeof(path), "%s/epic/legendary", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/epic/library.json", home);
    write_file(path, "[]");
    snprintf(path, sizeof(path), "%s/epic/legendary/user.json", home);
    write_file(path, "{\"displayName\":\"Player\"}");
    snprintf(path, sizeof(path), "%s/launcher-games/epic", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/launcher-games/epic/location.txt", home);
    write_file(path, "/Volumes/Games/Epic\n");
    snprintf(path, sizeof(path), "%s/compatdata/old", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/compatdata/old/state", home);
    write_file(path, "deprecated");

    assert(preserve_user_data(home, &preserved));
    snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/appmanifest_440.acf",
             preserved.temp);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/Program Files/Game/game.exe", preserved.temp);
    assert(!file_exists(path));
    snprintf(path, sizeof(path),
             "%s/prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/UbisoftConnect.exe",
             preserved.temp);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/c:", preserved.temp);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/y:", preserved.temp);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/z:", preserved.temp);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/drive_c/windows/user.reg", preserved.temp);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/dosdevices/c:", preserved.temp);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/dosdevices/z:", preserved.temp);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/user.reg", preserved.temp);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/drive_c/windows/runtime.dll", preserved.temp);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/epic/library.json", preserved.temp);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/launcher-games/epic/location.txt", preserved.temp);
    assert(file_exists(path));

    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame", home);
    remove_tree_local(path);
    snprintf(path, sizeof(path), "%s/epic", home);
    remove_tree_local(path);
    snprintf(path, sizeof(path), "%s/launcher-games", home);
    remove_tree_local(path);
    remove_old_runtime(home);
    restore_preserved_data(home, &preserved);
    snprintf(path, sizeof(path), "%s/setup.json", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/cache/steam_config.json", home);
    assert(file_exists(path));
    {
        FILE* config = fopen(path, "rb");
        char contents[256] = {0};
        assert(config != NULL);
        assert(fread(contents, 1, sizeof(contents) - 1, config) > 0);
        fclose(config);
        assert(strstr(contents, "steam_api_key") != NULL);
    }
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/drive_c/windows/user.reg", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path),
             "%s/prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/UbisoftConnect.exe", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/c:", home);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/z:", home);
    assert(!file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/y:", home);
    {
        char target[512] = {0};
        ssize_t length = readlink(path, target, sizeof(target) - 1);
        assert(length > 0);
        target[length] = '\0';
        assert(!strcmp(target, "/Volumes/AverySSD"));
    }
    assert(!rebuild_gog_prefix_after_migration(home));
    assert(!rebuild_ubisoft_prefix_after_migration(home));
    snprintf(path, sizeof(path), "%s/runtime/wine/bin", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/runtime/wine/bin/metalsharp-wine", home);
    write_file(path, "#!/bin/sh\n"
                     "[ \"$1\" = wineboot ] && [ \"$2\" = -u ] || exit 4\n"
                     "[ \"$WINEMSYNC\" = 0 ] && [ \"$MS_FWD_COMPAT_GL_CTX\" = 1 ] || exit 5\n"
                     "[ \"$(readlink \"$WINEPREFIX/dosdevices/c:\")\" = ../drive_c ] || exit 7\n"
                     "[ \"$(readlink \"$WINEPREFIX/dosdevices/z:\")\" = / ] || exit 8\n"
                     "printf '%s %s\\n' \"$1\" \"$2\" > \"$WINEPREFIX/migration-wineboot-args\"\n"
                     "case \"$WINEPREFIX\" in *gog-prefix*) [ \"${FAIL_GOG_WINEBOOT:-0}\" != 1 ] || exit 6;; esac\n");
    assert(chmod(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/dosdevices/c:", home);
    unlink(path);
    assert(symlink("/", path) == 0);
    setenv("FAIL_GOG_WINEBOOT", "1", 1);
    {
        bool gog_ok = true, ubisoft_ok = false;
        assert(!rebuild_preserved_wine_prefixes(home, &gog_ok, &ubisoft_ok));
        assert(!gog_ok && ubisoft_ok);
    }
    unsetenv("FAIL_GOG_WINEBOOT");
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/migration-wineboot-args", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/migration-wineboot-args", home);
    assert(file_exists(path));
    assert(rebuild_preserved_wine_prefixes(home, &(bool){false}, &(bool){false}));
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/migration-wineboot-args", home);
    {
        FILE* args = fopen(path, "rb");
        char contents[64] = {0};
        assert(args != NULL);
        assert(fread(contents, 1, sizeof(contents) - 1, args) > 0);
        fclose(args);
        assert(strcmp(contents, "wineboot -u\n") == 0);
    }
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/c:", home);
    {
        struct stat st;
        assert(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
    }
    snprintf(path, sizeof(path), "%s/prefix-ubisoft/dosdevices/z:", home);
    {
        struct stat st;
        assert(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
    }
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/migration-wineboot-args", home);
    {
        FILE* args = fopen(path, "rb");
        char contents[64] = {0};
        assert(args != NULL);
        assert(fread(contents, 1, sizeof(contents) - 1, args) > 0);
        fclose(args);
        assert(strcmp(contents, "wineboot -u\n") == 0);
    }
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/dosdevices/c:", home);
    {
        struct stat st;
        assert(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
    }
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/dosdevices/z:", home);
    {
        struct stat st;
        assert(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
    }
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/user.reg", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/bottles/epic_TestGame/prefix/system.reg", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/epic/legendary/user.json", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/epic/library.json", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/launcher-games/epic/location.txt", home);
    assert(file_exists(path));
    snprintf(path, sizeof(path), "%s/compatdata", home);
    assert(!file_exists(path));

    free_preserved_data(&preserved);
    remove_tree_local(home);
    puts("migration tests passed");
    return 0;
}
