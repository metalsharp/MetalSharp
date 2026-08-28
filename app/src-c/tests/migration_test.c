#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char* ms_setup_install_all_json(const char* home, int* status) {
    (void)home;
    (void)status;
    return NULL;
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

int main(void) {
    char home[256];
    char path[512];
    preserved_data preserved;

    snprintf(home, sizeof(home), "/tmp/metalsharp-migration-test-%ld", (long)getpid());
    remove_tree_local(home);
    make_directory(home);

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

    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/drive_c/windows", home);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/drive_c/windows/user.reg", home);
    write_file(path, "gog settings");
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
    snprintf(path, sizeof(path), "%s/bottles/gog-prefix/prefix/drive_c/windows/user.reg", preserved.temp);
    assert(file_exists(path));
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
