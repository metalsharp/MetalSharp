/* Exercise routing helpers without launching Wine or touching a real prefix. */
#include "../runtime/steam_actions.c"
#include <assert.h>

static void fixture(const char* home, const char* relative, const char* bytes) {
    char* path = join(home, relative);
    char* parent = strdup(path);
    *strrchr(parent, '/') = '\0';
    assert(ensure_directory(parent));
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(fputs(bytes, f) >= 0);
    assert(fclose(f) == 0);
    free(parent);
    free(path);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const char* home = argv[1];
    fixture(home, "cs2/game/bin/win64/vconsole2.exe", "console helper");
    fixture(home, "cs2/game/bin/win64/cs2.exe", "game executable");
    char* cs2_dir = join(home, "cs2");
    char* cs2_exe = preferred_steam_game_executable(cs2_dir, 730, "m11");
    assert(cs2_exe && strstr(cs2_exe, "/game/bin/win64/cs2.exe"));
    assert(executable_helper_name("vconsole2.exe"));
    free(cs2_exe);
    free(cs2_dir);

    fixture(home, "configs/config.json", "{\"msync\":false}");
    set_wine_msync(home);
    assert(!strcmp(getenv("WINEMSYNC"), "0"));
    fixture(home, "configs/config.json", "{\"msync\":true}");
    set_wine_msync(home);
    assert(!strcmp(getenv("WINEMSYNC"), "1"));
    set_route_paths(home, "vkd3d");
    assert(strstr(getenv("VK_DRIVER_FILES"), "lib/moltenvk-vkmt/MoltenVK_icd.json"));
    assert(!strcmp(pipeline_backend("m12"), "dxmt"));
    set_route_paths(home, "d3dmetal");
    assert(getenv("D3DMETAL_RUNTIME_DIR"));
    assert(strstr(getenv("D3DMETAL_FRAMEWORK_PATH"), "D3DMetal.framework/D3DMetal"));
    assert(!getenv("VK_DRIVER_FILES"));
    fixture(home, "runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows/nvngx-on-metalfx.dll", "known");
    fixture(home, "game/nvngx-on-metalfx.dll", "known");
    fixture(home, "game/d3d11.dll", "user DLL");
    char* game = join(home, "game");
    char* exe = join(game, "game.exe");
    char* stale = join(game, "nvngx-on-metalfx.dll");
    char* custom = join(game, "d3d11.dll");
    remove_stale_route_dlls(home, "vkd3d", game, exe);
    assert(access(stale, F_OK) != 0);
    assert(access(custom, F_OK) == 0);
    fixture(home, "game/nvngx-on-metalfx.dll", "user modification");
    remove_stale_route_dlls(home, "vkd3d", game, exe);
    assert(access(stale, F_OK) == 0);
    free(game);
    free(exe);
    free(stale);
    free(custom);
    puts("runtime routing regressions passed");
    return 0;
}
