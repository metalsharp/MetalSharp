#define _DARWIN_C_SOURCE
#include "metalsharp_backend/game_executable.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_file(const char* path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(write(fd, "MZ", 2) == 2);
    assert(close(fd) == 0);
}

int main(void) {
    char temporary[] = "/tmp/metalsharp-game-exe-test-XXXXXX";
    char* home = mkdtemp(temporary);
    char game[PATH_MAX], binaries[PATH_MAX], nested[PATH_MAX], exe[PATH_MAX], invalid[PATH_MAX], outside[PATH_MAX],
        symlink_path[PATH_MAX];
    char error[256], *resolved = NULL, *real_exe = NULL;
    assert(home);
    snprintf(game, sizeof(game), "%s/Game", home);
    snprintf(nested, sizeof(nested), "%s/Game/Binaries/Win64", home);
    assert(mkdir(game, 0700) == 0);
    snprintf(binaries, sizeof(binaries), "%s/Game/Binaries", home);
    assert(mkdir(binaries, 0700) == 0);
    assert(mkdir(nested, 0700) == 0);
    snprintf(exe, sizeof(exe), "%s/Chosen.exe", nested);
    snprintf(invalid, sizeof(invalid), "%s/Installer.msi", nested);
    snprintf(outside, sizeof(outside), "%s/Outside.exe", home);
    snprintf(symlink_path, sizeof(symlink_path), "%s/Escape.exe", game);
    make_file(exe);
    make_file(invalid);
    make_file(outside);
    assert(symlink(outside, symlink_path) == 0);

    assert(!ms_game_executable_override_save(home, "steam", "123", game, outside, error, sizeof(error)));
    assert(!ms_game_executable_override_save(home, "steam", "123", game, invalid, error, sizeof(error)));
    assert(!ms_game_executable_override_save(home, "steam", "123", game, symlink_path, error, sizeof(error)));
    assert(ms_game_executable_override_save(home, "steam", "123", game, exe, error, sizeof(error)));
    real_exe = realpath(exe, NULL);
    assert(real_exe);
    assert(ms_game_executable_override_load(home, "steam", "123", game, &resolved) == 1);
    assert(resolved && strcmp(resolved, real_exe) == 0);
    free(resolved);
    resolved = NULL;
    assert(ms_game_executable_override_load(home, "steam", "123", game, &resolved) == 1);
    assert(resolved);
    assert(unlink(exe) == 0);
    free(resolved);
    resolved = NULL;
    assert(ms_game_executable_override_load(home, "steam", "123", game, &resolved) == -1);
    assert(ms_game_executable_override_save(home, "steam", "123", game, NULL, error, sizeof(error)));
    assert(ms_game_executable_override_load(home, "steam", "123", game, &resolved) == 0);
    assert(!resolved);
    assert(ms_game_executable_override_save(home, "../steam", "123", game, outside, error, sizeof(error)) == false);

    unlink(symlink_path);
    unlink(invalid);
    unlink(outside);
    rmdir(nested);
    rmdir(binaries);
    snprintf(game, sizeof(game), "%s/Game", home);
    rmdir(game);
    snprintf(game, sizeof(game), "%s/config/game-executables/steam", home);
    rmdir(game);
    snprintf(game, sizeof(game), "%s/config/game-executables", home);
    rmdir(game);
    snprintf(game, sizeof(game), "%s/config", home);
    rmdir(game);
    rmdir(home);
    free(real_exe);
    puts("game executable override tests passed");
    return 0;
}
