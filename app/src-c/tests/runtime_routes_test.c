/* Exercise routing helpers without launching Wine or touching a real prefix. */
#include "../runtime/steam_actions.c"
#define main metalsharp_backend_main_for_test
#include "../runtime/main.c"
#undef main
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

static void executable_fixture(const char* home, const char* relative) {
    char* path;
    fixture(home, relative, "x87sidecar");
    path = join(home, relative);
    assert(chmod(path, 0700) == 0);
    free(path);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const char* home = argv[1];
    assert(setenv("WINEPREFIX", "/tmp/foreign-wine-prefix", 1) == 0);
    assert(setenv("WINEARCH", "win32", 1) == 0);
    assert(setenv("PROTON_LOG", "1", 1) == 0);
    assert(setenv("STEAM_COMPAT_DATA_PATH", "/tmp/foreign-proton-prefix", 1) == 0);
    assert(setenv("DXVK_HUD", "fps", 1) == 0);
    assert(setenv("VK_ICD_FILENAMES", "/tmp/foreign-vulkan/icd.json", 1) == 0);
    assert(setenv("DYLD_FALLBACK_LIBRARY_PATH", "/tmp/foreign-wine/lib", 1) == 0);
    assert(setenv("SteamAppId", "999", 1) == 0);
    assert(setenv("GRAPHICS_BACKEND", "foreign", 1) == 0);
    assert(setenv("METALSHARP_PORT", "9123", 1) == 0);
    sanitize_inherited_runtime_environment();
    assert(getenv("WINEPREFIX") == NULL && getenv("WINEARCH") == NULL);
    assert(getenv("PROTON_LOG") == NULL && getenv("STEAM_COMPAT_DATA_PATH") == NULL);
    assert(getenv("DXVK_HUD") == NULL && getenv("VK_ICD_FILENAMES") == NULL);
    assert(getenv("DYLD_FALLBACK_LIBRARY_PATH") == NULL && getenv("SteamAppId") == NULL);
    assert(getenv("GRAPHICS_BACKEND") == NULL);
    assert(getenv("METALSHARP_PORT") && !strcmp(getenv("METALSHARP_PORT"), "9123"));
    unsetenv("METALSHARP_PORT");
    fixture(home, "graphics-scan/d3d12/Binaries/Win64/D3D12Core.DLL", "d3d12");
    fixture(home, "graphics-scan/dxmt/d3d11.dll", "d3d11");
    fixture(home, "graphics-scan/dxmt10/d3d10core.dll", "d3d10");
    fixture(home, "graphics-scan/dxmt32/i386-windows/D3D10.DLL", "i386 d3d10");
    fixture(home, "graphics-scan/dxmt32_11/i386-windows/D3D11.DLL", "i386 d3d11");
    fixture(home, "graphics-scan/d3d9/D3D9.DLL", "d3d9");
    fixture(home, "graphics-scan/priority/i386/d3d11.dll", "i386 d3d11");
    fixture(home, "graphics-scan/priority/d3d12.dll", "d3d12");
    char* graphics_path = join(home, "graphics-scan/d3d12");
    assert(!strcmp(ms_steam_detect_graphics_pipeline(graphics_path), "d3dmetal"));
    free(graphics_path);
    graphics_path = join(home, "graphics-scan/dxmt");
    assert(!strcmp(ms_steam_detect_graphics_pipeline(graphics_path), "dxmt"));
    free(graphics_path);
    graphics_path = join(home, "graphics-scan/dxmt10");
    assert(!strcmp(ms_steam_detect_graphics_pipeline(graphics_path), "dxmt"));
    free(graphics_path);
    graphics_path = join(home, "graphics-scan/dxmt32");
    assert(!strcmp(ms_steam_detect_graphics_pipeline(graphics_path), "dxmt_32"));
    free(graphics_path);
    graphics_path = join(home, "graphics-scan/dxmt32_11");
    assert(!strcmp(ms_steam_detect_graphics_pipeline(graphics_path), "dxmt_32"));
    free(graphics_path);
    graphics_path = join(home, "graphics-scan/d3d9");
    assert(!strcmp(ms_steam_detect_graphics_pipeline(graphics_path), "d3d9"));
    free(graphics_path);
    graphics_path = join(home, "graphics-scan/priority");
    assert(!strcmp(ms_steam_detect_graphics_pipeline(graphics_path), "d3dmetal"));
    free(graphics_path);
    graphics_path = join(home, "graphics-scan/none");
    assert(ms_steam_detect_graphics_pipeline(graphics_path) == NULL);
    free(graphics_path);

    fixture(home, "cs2/game/bin/win64/vconsole2.exe", "console helper");
    fixture(home, "cs2/game/bin/win64/cs2.exe", "game executable");
    char* cs2_dir = join(home, "cs2");
    char* cs2_exe = preferred_steam_game_executable(cs2_dir, 730, "dxmt");
    assert(cs2_exe && strstr(cs2_exe, "/game/bin/win64/cs2.exe"));
    assert(executable_helper_name("vconsole2.exe"));
    free(cs2_exe);
    free(cs2_dir);

    fixture(home, "aniimo/repair.exe", "repair helper");
    fixture(home, "aniimo/KernelDumpAnalyzer.exe", "dump helper");
    fixture(home, "aniimo/Aniimo.exe", "game executable");
    char* aniimo_dir = join(home, "aniimo");
    char* aniimo_exe = preferred_steam_game_executable(aniimo_dir, 4126040, "d3dmetal");
    assert(aniimo_exe && strstr(aniimo_exe, "/aniimo/Aniimo.exe"));
    free(aniimo_exe);
    free(aniimo_dir);

    fixture(home, "ubisoft/odyssey/uplay_r1_loader64.dll", "Ubisoft Connect marker");
    char* ubisoft_game_dir = join(home, "ubisoft/odyssey");
    assert(steam_game_uses_ubisoft_connect(812140, NULL));
    assert(steam_game_uses_ubisoft_connect(999999, ubisoft_game_dir));
    assert(!steam_game_uses_ubisoft_connect(999999, home));
    free(ubisoft_game_dir);

    fixture(home, "configs/config.json", "{\"msync\":false}");
    set_wine_msync(home);
    assert(!strcmp(getenv("WINEMSYNC"), "0"));
    fixture(home, "configs/config.json", "{\"msync\":true}");
    set_wine_msync(home);
    assert(!strcmp(getenv("WINEMSYNC"), "1"));
    /* Retina now defaults to DISABLED when the key is absent. */
    seed_steam_registry(home);
    char* steam_reg_path = join(home, "prefix-steam/drive_c/metalsharp-steam.reg");
    char* steam_reg = read_bounded_file(steam_reg_path);
    assert(steam_reg && strstr(steam_reg, "\"RetinaMode\"=\"N\"") && strstr(steam_reg, "\"LogPixels\"=dword:00000060"));
    free(steam_reg);
    fixture(home, "configs/config.json", "{\"retinaMode\":true}");
    seed_steam_registry(home);
    steam_reg = read_bounded_file(steam_reg_path);
    assert(steam_reg && strstr(steam_reg, "\"RetinaMode\"=\"Y\"") && strstr(steam_reg, "\"LogPixels\"=dword:000000c0"));
    free(steam_reg);
    fixture(home, "configs/config.json", "{\"retinaMode\":false}");
    seed_steam_registry(home);
    steam_reg = read_bounded_file(steam_reg_path);
    assert(steam_reg && strstr(steam_reg, "\"RetinaMode\"=\"N\"") && strstr(steam_reg, "\"LogPixels\"=dword:00000060"));
    free(steam_reg);
    free(steam_reg_path);
    unsetenv("ROSETTA_X87_PATH");
    set_route_paths(home, "vkd3d");
    assert(!getenv("ROSETTA_X87_PATH"));
    {
        char* sidecar;
        executable_fixture(home, "runtime/wine/bin/x87sidecar");
        sidecar = join(home, "runtime/wine/bin/x87sidecar");
        assert(unlink(sidecar) == 0);
        assert(symlink("/bin/sh", sidecar) == 0);
        set_route_paths(home, "d3d9");
        assert(!getenv("ROSETTA_X87_PATH"));
        assert(unlink(sidecar) == 0);
        free(sidecar);
    }
    executable_fixture(home, "runtime/wine/bin/x87sidecar");
    executable_fixture(home, "runtime/wine/lib/wine/x86_64-unix/wine");
    set_route_paths(home, "d3d9");
    assert(!getenv("VK_DRIVER_FILES"));
    assert(getenv("ROSETTA_X87_PATH") && strstr(getenv("ROSETTA_X87_PATH"), "/runtime/wine/bin/x87sidecar"));
    {
        char* wow64_loader = join(home, "runtime/wine/lib/wine/i386-unix/wine");
        assert(wow64_loader && access(wow64_loader, X_OK) == 0);
        free(wow64_loader);
    }
    assert(!strcmp(pipeline_backend("vkd3d"), "vulkan"));
    assert(!strcmp(pipeline_backend("d3d9"), "dxmt"));
    assert(!strcmp(canonical_pipeline("dxmt"), "dxmt"));
    assert(!strcmp(canonical_pipeline("dxvk"), "d3d9"));
    assert(!strcmp(canonical_pipeline("dxmt_32"), "dxmt_32"));
    assert(!strcmp(canonical_pipeline("dxvk_32"), "d3d9"));
    assert(!strcmp(pipeline_backend("vkd3d"), "vulkan"));
    assert(strstr(pipeline_overrides("dxmt"), "d3d10core"));
    assert(strstr(pipeline_overrides("vkd3d"), "d3d9"));
    assert(strstr(pipeline_overrides("d3d9"), "d3d9,dxgi=n,b"));
    /* Isaac is OpenGL-based: WineMetalGL breaks GL_VERSION on the 32-bit route. */
    setenv("WINEMETALGL", "1", 1);
    set_game_opengl_env(250900, "dxmt_32");
    assert(!strcmp(getenv("WINEMETALGL"), "0"));
    setenv("WINEMETALGL", "1", 1);
    set_game_opengl_env(391540, "d3d9");
    assert(!strcmp(getenv("WINEMETALGL"), "0"));
    setenv("WINEMETALGL", "1", 1);
    set_game_opengl_env(250900, "dxmt");
    assert(!strcmp(getenv("WINEMETALGL"), "0"));
    setenv("WINEMETALGL", "1", 1);
    set_game_opengl_env(42, "vkd3d");
    assert(!strcmp(getenv("WINEMETALGL"), "0"));
    set_game_opengl_env(42, "dxmt_32");
    assert(!strcmp(getenv("WINEMETALGL"), "0"));
    set_route_paths(home, "d3dmetal");
    assert(getenv("D3DMETAL_RUNTIME_DIR"));
    assert(strstr(getenv("D3DMETAL_FRAMEWORK_PATH"), "D3DMetal.framework/D3DMetal"));
    assert(!getenv("VK_DRIVER_FILES"));
    assert(!getenv("ROSETTA_X87_PATH"));
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

    /* Steam libraries on external drives are stored as Wine drive paths in libraryfolders.vdf. */
    char* host_path = ms_steam_library_host_path(NULL, "Z:\\\\Volumes\\\\SSD\\\\SteamLibrary", 30);
    assert(host_path && !strcmp(host_path, "/Volumes/SSD/SteamLibrary"));
    free(host_path);
    host_path = ms_steam_library_host_path(NULL, "/Volumes/SSD/SteamLibrary", 25);
    assert(host_path && !strcmp(host_path, "/Volumes/SSD/SteamLibrary"));
    free(host_path);
    setenv("HOME", home, 1);
    fixture(home, "external/SteamLibrary/steamapps/appmanifest_812140.acf",
            "\"AppState\"\n{\n\t\"appid\"\t\t\"812140\"\n\t\"name\"\t\t\"External Game\"\n"
            "\t\"installdir\"\t\t\"External Game\"\n}\n");
    fixture(home, "external/SteamLibrary/steamapps/common/External Game/game.exe", "game executable");
    fixture(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/libraryfolders.vdf",
            "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t}\n"
            "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\SteamLibrary\"\n\t}\n}\n");
    char* dosdevices = join(home, "prefix-steam/dosdevices");
    char* drive_c_link = join(dosdevices, "c:");
    char* drive_e_link = join(dosdevices, "e:");
    char* external = join(home, "external");
    assert(ensure_directory(dosdevices));
    assert(symlink("../drive_c", drive_c_link) == 0);
    assert(symlink(external, drive_e_link) == 0);
    char* external_dir = ms_steam_game_dir(home, 812140);
    assert(external_dir && strstr(external_dir, "/dosdevices/e:/SteamLibrary/steamapps/common/External Game"));
    char* external_exe = find_steam_game_executable(home, 812140, "dxmt");
    assert(external_exe && strstr(external_exe, "/External Game/game.exe"));
    free(external_exe);
    free(external_dir);
    free(external);
    fixture(home, "Library/Application Support/Steam/steamapps/appmanifest_3900000001.acf",
            "\"AppState\"\n{\n\t\"appid\"\t\"3900000001\"\n\t\"name\"\t\"Detected D3D12 Game\"\n"
            "\t\"installdir\"\t\"Detected D3D12 Game\"\n}\n");
    fixture(home, "Library/Application Support/Steam/steamapps/common/Detected D3D12 Game/d3d12.dll", "d3d12");
    {
        char* library_json = ms_steam_library_json(home);
        char error[96];
        ms_json* library = library_json ? ms_json_parse(library_json, strlen(library_json), error, sizeof(error)) : NULL;
        const ms_json* games = library ? ms_json_object_get(library, "games") : NULL;
        bool detected_route = false;
        for (size_t i = 0; games && i < ms_json_array_length(games); i++) {
            const ms_json* game = ms_json_array_get(games, i);
            long long appid;
            char* pipeline = NULL;
            if (!ms_json_as_i64(ms_json_object_get(game, "appid"), &appid) || appid != 3900000001LL)
                continue;
            if (ms_json_as_string(ms_json_object_get(game, "launch_method"), &pipeline) && pipeline &&
                !strcmp(pipeline, "d3dmetal"))
                detected_route = true;
            free(pipeline);
        }
        assert(detected_route);
        ms_json_free(library);
        free(library_json);
    }
    free(drive_e_link);
    free(drive_c_link);
    free(dosdevices);
    puts("runtime routing regressions passed");
    return 0;
}
