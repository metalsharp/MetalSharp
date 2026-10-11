/* Exercise routing helpers without launching Wine or touching a real prefix. */
// clang-format off
#include "../runtime/steam_actions.c"
#include "../runtime/epic.c"
#include "../runtime/ubisoft.c"
#include "metalsharp_backend/sharp.h"
#include "metalsharp_backend/gamejolt.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/ubisoft.h"
// clang-format on
#define main metalsharp_backend_main_for_test
#include "../runtime/main.c"
#undef main
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static void wait_for_test_child(pid_t child) {
    pid_t result;
    do {
        result = waitpid(child, NULL, 0);
    } while (result < 0 && errno == EINTR);
    assert(result == child || (result < 0 && errno == ECHILD));
}

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

static void fixture_bytes(const char* home, const char* relative, const unsigned char* bytes, size_t length) {
    char* path = join(home, relative);
    char* parent = strdup(path);
    *strrchr(parent, '/') = '\0';
    assert(ensure_directory(parent));
    FILE* file = fopen(path, "wb");
    assert(file);
    assert(fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
    free(parent);
    free(path);
}

static void pe_fixture(const char* home, const char* relative, unsigned short machine) {
    unsigned char pe[0x5a] = {0};
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[0x3c] = 0x40;
    pe[0x40] = 'P';
    pe[0x41] = 'E';
    pe[0x44] = (unsigned char)(machine & 0xff);
    pe[0x45] = (unsigned char)(machine >> 8);
    fixture_bytes(home, relative, pe, sizeof(pe));
}

static void executable_fixture(const char* home, const char* relative) {
    char* path;
    fixture(home, relative, "x87sidecar");
    path = join(home, relative);
    assert(chmod(path, 0700) == 0);
    free(path);
}

static void test_ubisoft_library(const char* home) {
    const char* registry =
        "[Software\\\\Wow6432Node\\\\Ubisoft\\\\Launcher\\\\Installs\\\\12345] 1\n"
        "\"InstallDir\"=\"C:\\\\Program Files (x86)\\\\Ubisoft\\\\Ubisoft Game Launcher\\\\games\\\\TestGame\"\n"
        "[Software\\\\Wow6432Node\\\\Ubisoft\\\\Launcher\\\\Installs\\\\23456] 1\n"
        "\"InstallDir\"=\"C:\\\\Program Files (x86)\\\\Ubisoft\\\\Ubisoft Game Launcher\\\\games\\\\NotInstalled\"\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Uplay Install 12345] 1\n"
        "\"DisplayName\"=\"Test Ubisoft Game\"\n"
        "\"InstallLocation\"=\"C:\\\\Program Files (x86)\\\\Ubisoft\\\\Ubisoft Game Launcher\\\\games\\\\TestGame\"\n";
    const char* game_exe =
        "prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/TestGame/TestGame.exe";
    char error[128];
    int status = 0;
    char *external = join(home, "external/AverySSD"), *other_external = join(home, "external/Other"),
         *prefix = join(home, "prefix-ubisoft"), *y_drive = NULL;
    assert(external && other_external && prefix && mkdir_p(external) && mkdir_p(other_external));
    {
        ubisoft_game far_cry = {.id = "5266", .name = "Far Cry 6"};
        const char* artwork = ubisoft_official_artwork_url(&far_cry);
        assert(artwork && strstr(artwork, "staticctf.ubisoft.com") && strstr(artwork, "fc6-page_meta-thumbnail.jpg"));
    }
    assert(ensure_ubisoft_drive_mapping(prefix, external));
    y_drive = join(prefix, "dosdevices/y:");
    assert(y_drive && ensure_ubisoft_drive_mapping(prefix, other_external));
    char resolved[PATH_MAX], expected_volume[PATH_MAX];
    assert(realpath(external, expected_volume));
    assert(realpath(y_drive, resolved) && !strcmp(resolved, expected_volume));
    free(external);
    free(other_external);
    free(prefix);
    free(y_drive);
    fixture(home, "prefix-ubisoft/system.reg", registry);
    fixture(home, game_exe, "local game executable");
    fixture(home, "cache/ubisoft-connect/artwork/777.png", "cached icon");
    char* cached_icon = ms_steam_extract_executable_icon(home, "/missing/game.exe", 777);
    assert(cached_icon && strstr(cached_icon, "777.png"));
    free(cached_icon);
    {
        char *wrestool = join(home, "tools/fake-wrestool"), *icotool = join(home, "tools/fake-icotool");
        char* small_icon = join(home, "cache/ubisoft-connect/artwork/778.png");
        char* old_wrestool = getenv("METALSHARP_WRESTOOL_PATH") ? strdup(getenv("METALSHARP_WRESTOOL_PATH")) : NULL;
        char* old_icotool = getenv("METALSHARP_ICOTOOL_PATH") ? strdup(getenv("METALSHARP_ICOTOOL_PATH")) : NULL;
        FILE* file;
        char contents[32] = {0};
        fixture(home, "tools/fake-wrestool", "#!/bin/sh\nprintf 'resource'\n");
        fixture(home, "tools/fake-icotool",
                "#!/bin/sh\nmkdir -p \"$3\"\nprintf 'small' > \"$3/icon_16x16_8.png\"\nprintf 'large' > "
                "\"$3/icon_256x256_8.png\"\n");
        assert(wrestool && icotool && small_icon);
        assert(chmod(wrestool, 0755) == 0 && chmod(icotool, 0755) == 0);
        assert(setenv("METALSHARP_WRESTOOL_PATH", wrestool, 1) == 0);
        assert(setenv("METALSHARP_ICOTOOL_PATH", icotool, 1) == 0);
        char* extracted = ms_steam_extract_executable_icon(home, "/missing/game.exe", 778);
        assert(extracted && !strcmp(extracted, small_icon));
        file = fopen(extracted, "rb");
        assert(file && fgets(contents, sizeof(contents), file));
        fclose(file);
        assert(!strcmp(contents, "large"));
        free(extracted);
        if (old_wrestool)
            assert(setenv("METALSHARP_WRESTOOL_PATH", old_wrestool, 1) == 0);
        else
            unsetenv("METALSHARP_WRESTOOL_PATH");
        if (old_icotool)
            assert(setenv("METALSHARP_ICOTOOL_PATH", old_icotool, 1) == 0);
        else
            unsetenv("METALSHARP_ICOTOOL_PATH");
        free(old_wrestool);
        free(old_icotool);
        free(wrestool);
        free(icotool);
        free(small_icon);
    }
    {
        /* Ubisoft Connect keeps its CEF launch switches and starts with the GPU off. */
        static const char* const required[] = {"-no-cef-sandbox", "-cef-single-process",
                                               "--disable-gpu",   "--in-process-gpu",
                                               "--use-gl=angle",  "--use-angle=swiftshader-webgl"};
        for (size_t r = 0; r < sizeof(required) / sizeof(required[0]); r++) {
            bool found = false;
            for (size_t i = 0; i < sizeof(UBISOFT_CONNECT_ARGS) / sizeof(UBISOFT_CONNECT_ARGS[0]); i++)
                found = found || !strcmp(UBISOFT_CONNECT_ARGS[i], required[r]);
            assert(found);
        }
    }
    char* raw = ms_ubisoft_library_json(home);
    ms_json* root = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    const ms_json* games = root ? ms_json_object_get(root, "games") : NULL;
    assert(root && games && ms_json_array_length(games) == 1);
    const ms_json* game = ms_json_array_get(games, 0);
    bool icon_pending = false;
    assert(ms_json_as_bool(ms_json_object_get(game, "icon_pending"), &icon_pending) && icon_pending);
    char* source = NULL;
    char* name = NULL;
    assert(ms_json_as_string(ms_json_object_get(game, "source"), &source) && !strcmp(source, "ubisoft"));
    assert(ms_json_as_string(ms_json_object_get(game, "name"), &name) && !strcmp(name, "Test Ubisoft Game"));
    assert(ms_json_array_length(ms_json_object_get(game, "available_pipelines")) == 5);
    assert(!ms_json_object_get(game, "steam_emulator"));
    free(source);
    free(name);
    ms_json_free(root);
    free(raw);

    {
        static const char* const files[] = {"d3d11.dll", "d3d10core.dll", "dxgi.dll", "winemetal.dll"};
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            char relative[PATH_MAX];
            snprintf(relative, sizeof(relative), "runtime/wine/lib/dxmt/i386-windows/%s", files[i]);
            fixture(home, relative, files[i]);
        }
    }
    raw = ms_ubisoft_save_pipeline_json(home, "{\"ubisoft_id\":\"12345\",\"pipeline\":\"dxmt_32\"}",
                                        strlen("{\"ubisoft_id\":\"12345\",\"pipeline\":\"dxmt_32\"}"), &status);
    assert(raw && status == 200 && strstr(raw, "\"ok\":true"));
    {
        char* staged = join(
            home, "prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/TestGame/d3d11.dll");
        assert(staged && access(staged, R_OK) == 0);
        free(staged);
    }
    free(raw);
    raw = ms_ubisoft_library_json(home);
    assert(raw && strstr(raw, "\"preferred_pipeline\":\"dxmt_32\""));
    free(raw);
    raw = ms_ubisoft_save_pipeline_json(home, "{\"ubisoft_id\":\"12345\",\"pipeline\":\"fna_arm64\"}",
                                        strlen("{\"ubisoft_id\":\"12345\",\"pipeline\":\"fna_arm64\"}"), &status);
    assert(raw && status == 400);
    free(raw);
    {
        static const char* const files[] = {"d3d10.dll", "d3d11.dll",   "d3d12.dll",
                                            "dxgi.dll",  "nvapi64.dll", "nvngx-on-metalfx.dll"};
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            char relative[PATH_MAX];
            snprintf(relative, sizeof(relative), "runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows/%s", files[i]);
            fixture(home, relative, files[i]);
        }
    }
    raw = ms_ubisoft_save_pipeline_json(home, "{\"ubisoft_id\":\"12345\",\"pipeline\":\"d3dmetal\"}",
                                        strlen("{\"ubisoft_id\":\"12345\",\"pipeline\":\"d3dmetal\"}"), &status);
    assert(raw && status == 200 && strstr(raw, "\"ok\":true"));
    free(raw);
    {
        char* stale_dll = join(
            home,
            "prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/TestGame/winemetal.dll");
        char* staged_dll =
            join(home,
                 "prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/TestGame/nvapi64.dll");
        assert(stale_dll && access(stale_dll, F_OK) != 0);
        assert(staged_dll && access(staged_dll, R_OK) == 0);
        free(stale_dll);
        free(staged_dll);
    }
    {
        static const char* const route_files[] = {"d3d10.dll", "d3d11.dll",   "d3d12.dll",
                                                  "dxgi.dll",  "nvapi64.dll", "nvngx-on-metalfx.dll"};
        const char* game_root = "external/AverySSD/Game";
        const char* executable = "external/AverySSD/Game/bin/FarCry6.exe";
        char* game_dir = join(home, game_root);
        char* executable_path = join(home, executable);
        char* selected_executable = NULL;
        off_t selected_size = 0;
        assert(game_dir && executable_path);
        pe_fixture(home, executable, 0x8664);
        pe_fixture(home, "external/AverySSD/Game/Support/Software/VCRedist/vc_redist.x64.exe", 0x014c);
        ubisoft_find_game_executable(game_dir, 0, &selected_executable, &selected_size);
        assert(selected_executable && !strcmp(selected_executable, executable_path));
        assert(game_process_command_matches("Y:/Ubisoft/Far Cry 6/bin/FarCry6.exe ", selected_executable));
        assert(!game_process_command_matches("C:/Program Files/Ubisoft/upc.exe", selected_executable));
        assert(!game_process_command_matches("C:/Games/NotFarCry6.exe", selected_executable));
        for (size_t i = 0; i < sizeof(route_files) / sizeof(route_files[0]); i++) {
            char relative[PATH_MAX];
            snprintf(relative, sizeof(relative), "runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows/%s", route_files[i]);
            fixture(home, relative, route_files[i]);
        }
        assert(ms_steam_stage_route_for_executable(home, "d3dmetal", game_dir, selected_executable));
        for (size_t i = 0; i < sizeof(route_files) / sizeof(route_files[0]); i++) {
            char relative[PATH_MAX];
            snprintf(relative, sizeof(relative), "%s/bin/%s", game_root, route_files[i]);
            char* staged = join(home, relative);
            assert(staged && access(staged, R_OK) == 0);
            free(staged);
        }
        free(game_dir);
        free(executable_path);
        free(selected_executable);
    }
    raw = ms_ubisoft_launch_game_json(home, "{\"ubisoft_id\":\"12345\",\"pipeline\":\"d3dmetal\"}",
                                      strlen("{\"ubisoft_id\":\"12345\",\"pipeline\":\"d3dmetal\"}"), &status);
    assert(raw && status == 409 && strstr(raw, "Ubisoft Connect is not installed"));
    free(raw);
}

static void test_ubisoft_idle_prefix_stop(const char* home) {
    char* test_home = join(home, "ubisoft-idle-stop");
    char* server_path;
    char* response;
    int status = 500;
    assert(test_home);
    fixture(test_home, "prefix-ubisoft/marker", "prefix");
    fixture(test_home, "runtime/wine/bin/wineserver",
            "#!/bin/sh\ncase \"$1\" in\n  -k) exit 1 ;;\n  -w) exit 0 ;;\n  *) exit 2 ;;\nesac\n");
    server_path = join(test_home, "runtime/wine/bin/wineserver");
    assert(server_path && chmod(server_path, 0755) == 0);
    response = ms_ubisoft_stop_json(test_home, &status);
    assert(response && status == 200 && strstr(response, "\"running\":false"));
    free(response);
    free(server_path);
    free(test_home);
}

#ifdef __APPLE__
static pid_t spawn_sharp_detached_fixture(const char* game_dir, const char* helper, pid_t* detached_pid) {
    int pid_pipe[2];
    pid_t launcher;
    assert(pipe(pid_pipe) == 0);
    launcher = fork();
    assert(launcher >= 0);
    if (launcher == 0) {
        close(pid_pipe[0]);
        (void)setpgid(0, 0);
        pid_t child = fork();
        if (child == 0) {
            (void)setsid();
            (void)chdir(game_dir);
            char* const args[] = {(char*)helper, "--gamejolt-detached-child", NULL};
            execv(helper, args);
            _exit(127);
        }
        if (child < 0 || write(pid_pipe[1], &child, sizeof(child)) != (ssize_t)sizeof(child))
            _exit(1);
        close(pid_pipe[1]);
        for (;;)
            pause();
    }
    close(pid_pipe[1]);
    assert(setpgid(launcher, launcher) == 0 || errno == EACCES);
    assert(read(pid_pipe[0], detached_pid, sizeof(*detached_pid)) == (ssize_t)sizeof(*detached_pid));
    close(pid_pipe[0]);
    return launcher;
}
#endif

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--wine-exe-probe")) {
        for (;;)
            pause();
    }
    if (argc == 2 && !strcmp(argv[1], "--gamejolt-detached-child")) {
        for (;;)
            pause();
    }
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
    assert(valid_pipeline("d3dmetal"));
    assert(valid_pipeline("dxmt"));
    assert(valid_pipeline("dxmt_32"));
    assert(valid_pipeline("d3d9"));
    assert(valid_pipeline("vkd3d"));
    assert(valid_pipeline("fna_arm64"));
    assert(!valid_pipeline("unknown"));
    test_ubisoft_library(home);
    test_ubisoft_idle_prefix_stop(home);
    ms_steam_apply_graphics_route(home, "d3dmetal");
    assert(strstr(getenv("WINEDLLPATH"), "runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows"));
    assert(getenv("D3DMETAL_FRAMEWORK_PATH"));
    ms_steam_apply_graphics_route(home, "vkd3d");
    assert(getenv("VK_DRIVER_FILES"));
    assert(!getenv("D3DMETAL_FRAMEWORK_PATH"));
    assert(!getenv("D3DMETAL_RUNTIME_DIR"));
    ms_steam_apply_graphics_route(home, "dxmt");
    assert(strstr(getenv("WINEDLLPATH"), "runtime/wine/lib/dxmt/x86_64-windows"));
    assert(strstr(getenv("WINEDLLOVERRIDES"), "winemetal,dxgi,d3d11"));
    assert(!getenv("VK_DRIVER_FILES"));
    ms_steam_apply_graphics_route(home, "dxmt_32");
    assert(strstr(getenv("WINEDLLPATH"), "runtime/wine/lib/dxmt/i386-windows"));
    ms_steam_apply_graphics_route(home, "d3d9");
    assert(strstr(getenv("WINEDLLOVERRIDES"), "d3d9,dxgi"));
    assert(!getenv("D3DMETAL_FRAMEWORK_PATH"));
    assert(!getenv("VK_DRIVER_FILES"));
    ms_steam_apply_graphics_route(home, "fna_arm64");
    assert(!strcmp(getenv("MS_GRAPHICS_BACKEND"), "fna_arm64"));
    assert(!getenv("DXMT_CONFIG_FILE"));
    {
        const char* expected[] = {"d3dmetal", "vkd3d", "dxmt", "dxmt_32", "d3d9"};
        char* gj_home = join(home, "gamejolt-options");
        char* response;
        char parse_error[96];
        char* game_id = NULL;
        ms_json* parsed;
        fixture(home, "gamejolt-options/GameJolt/Example/Game.exe", "game");
        response = ms_gamejolt_json(gj_home);
        parsed = response ? ms_json_parse(response, strlen(response), parse_error, sizeof(parse_error)) : NULL;
        assert(parsed);
        const ms_json* games = ms_json_object_get(parsed, "games");
        assert(ms_json_array_length(games) == 1);
        const ms_json* game = ms_json_array_get(games, 0);
        const ms_json* pipelines = ms_json_object_get(game, "available_pipelines");
        assert(ms_json_array_length(pipelines) == sizeof(expected) / sizeof(expected[0]));
        for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
            char* id = NULL;
            assert(ms_json_as_string(ms_json_object_get(ms_json_array_get(pipelines, i), "id"), &id));
            assert(!strcmp(id, expected[i]));
            free(id);
        }
        assert(ms_json_as_string(ms_json_object_get(game, "id"), &game_id));
        ms_json_free(parsed);
        free(response);
        char engines_path[256];
        snprintf(engines_path, sizeof(engines_path), "gamejolt-options/gamejolt/engines.json");
        char legacy_config[256];
        snprintf(legacy_config, sizeof(legacy_config), "{\"%s\":\"m10_32\"}", game_id);
        fixture(home, engines_path, legacy_config);
        response = ms_gamejolt_json(gj_home);
        assert(response && strstr(response, "\"engine\":\"dxmt_32\""));
        free(response);
        char set_engine_body[256];
        snprintf(set_engine_body, sizeof(set_engine_body), "{\"id\":\"%s\",\"engine\":\"dxmt\"}", game_id);
        response = ms_gamejolt_set_engine_json(gj_home, (const unsigned char*)set_engine_body, strlen(set_engine_body));
        assert(response && strstr(response, "\"ok\":true"));
        free(response);
        snprintf(set_engine_body, sizeof(set_engine_body), "{\"id\":\"%s\",\"engine\":\"m11\"}", game_id);
        response = ms_gamejolt_set_engine_json(gj_home, (const unsigned char*)set_engine_body, strlen(set_engine_body));
        assert(response && strstr(response, "invalid GameJolt engine"));
        free(response);
        {
            pid_t child = fork();
            char status_body[64];
            int process_status = 0;
            assert(child >= 0);
            if (child == 0) {
                (void)setpgid(0, 0);
                for (;;)
                    pause();
            }
            assert(setpgid(child, child) == 0 || errno == EACCES);
            ms_process_register_game(987654321U, child);
            snprintf(status_body, sizeof(status_body), "{\"pid\":%ld}", (long)child);
            response = ms_gamejolt_pid_status_json((const unsigned char*)status_body, strlen(status_body));
            assert(response && strstr(response, "\"running\":true"));
            free(response);
            response = ms_process_kill_json(home, status_body, strlen(status_body), &process_status);
            assert(response && process_status == 200 && strstr(response, "\"ok\":true"));
            free(response);
            assert(waitpid(child, NULL, 0) == child);
            response = ms_gamejolt_pid_status_json((const unsigned char*)status_body, strlen(status_body));
            assert(response && strstr(response, "\"running\":false"));
            free(response);
        }
        {
            pid_t child = fork();
            const char* gamejolt_body = "{\"id\":\"gamejolt-smoke\"}";
            int stop_status = 0;
            assert(child >= 0);
            if (child == 0) {
                (void)setpgid(0, 0);
                for (;;)
                    pause();
            }
            assert(setpgid(child, child) == 0 || errno == EACCES);
            ms_gamejolt_register_game_process("/tmp", "gamejolt-smoke", child, "/tmp/game.exe");
            response = ms_gamejolt_running_json();
            assert(response && strstr(response, "gamejolt-smoke"));
            free(response);
            response = ms_gamejolt_stop_json((const unsigned char*)gamejolt_body, strlen(gamejolt_body), &stop_status);
            assert(response && stop_status == 200 && strstr(response, "\"ok\":true"));
            free(response);
            response = ms_gamejolt_running_json();
            assert(response && !strstr(response, "gamejolt-smoke"));
            free(response);
        }
        {
            char source[PATH_MAX];
            char* runtime_dir = join(home, "runtime/wine");
            char* detached_dir = join(home, "gamejolt-detached");
            char* helper = runtime_dir ? join(runtime_dir, "wine-helper") : NULL;
            char* game_exe = detached_dir ? join(detached_dir, "Game.exe") : NULL;
            const char* gamejolt_body = "{\"id\":\"gamejolt-detached\"}";
            int stop_status = 0;
            assert(runtime_dir && detached_dir && helper && game_exe);
            assert(realpath(argv[0], source) != NULL);
            assert(ensure_directory(runtime_dir) && ensure_directory(detached_dir));
            int input = open(source, O_RDONLY);
            int output = open(helper, O_WRONLY | O_CREAT | O_EXCL, 0700);
            assert(input >= 0 && output >= 0);
            char buffer[16384];
            ssize_t bytes;
            while ((bytes = read(input, buffer, sizeof(buffer))) > 0) {
                ssize_t written = 0;
                while (written < bytes) {
                    ssize_t count = write(output, buffer + written, (size_t)(bytes - written));
                    assert(count > 0);
                    written += count;
                }
            }
            assert(bytes == 0 && close(input) == 0 && close(output) == 0);
            pid_t leader = fork();
            assert(leader >= 0);
            if (leader == 0) {
                (void)setpgid(0, 0);
                pid_t detached = fork();
                if (detached == 0) {
                    (void)setpgid(0, 0);
                    (void)chdir(detached_dir);
                    execl(helper, helper, "--gamejolt-detached-child", (char*)NULL);
                    _exit(127);
                }
                _exit(detached < 0 ? 1 : 0);
            }
            assert(setpgid(leader, leader) == 0 || errno == EACCES);
            assert(waitpid(leader, NULL, 0) == leader);
            struct timespec detached_startup_delay = {0, 100000000L};
            (void)nanosleep(&detached_startup_delay, NULL);
            ms_gamejolt_register_game_process(home, "gamejolt-detached", leader, game_exe);
            response = ms_gamejolt_running_json();
            assert(response && strstr(response, "gamejolt-detached"));
            free(response);
            response = ms_gamejolt_stop_json((const unsigned char*)gamejolt_body, strlen(gamejolt_body), &stop_status);
            assert(response && stop_status == 200 && strstr(response, "\"ok\":true"));
            free(response);
            free(runtime_dir);
            free(detached_dir);
            free(helper);
            free(game_exe);
        }
        {
            pid_t leader = fork();
            char status_body[64];
            int process_status = 0;
            assert(leader >= 0);
            if (leader == 0) {
                (void)setpgid(0, 0);
                pid_t member = fork();
                if (member == 0) {
                    for (;;)
                        pause();
                }
                _exit(member < 0 ? 1 : 0);
            }
            assert(setpgid(leader, leader) == 0 || errno == EACCES);
            ms_process_register_game(987654322U, leader);
            assert(waitpid(leader, NULL, 0) == leader);
            response = ms_process_running_json(home);
            assert(response && strstr(response, "987654322"));
            free(response);
            snprintf(status_body, sizeof(status_body), "{\"pid\":%ld}", (long)leader);
            response = ms_process_kill_json(home, status_body, strlen(status_body), &process_status);
            assert(response && process_status == 200 && strstr(response, "\"ok\":true"));
            free(response);
        }
        {
            char source[PATH_MAX];
            char* wine_helper = join(home, "runtime/wine/probe/metalsharp-probe");
            char* wine_bin = join(home, "runtime/wine/probe");
            pid_t leader, wine_game;
            int status = 0;
            assert(wine_helper && wine_bin && realpath(argv[0], source));
            assert(ensure_directory(wine_bin));
            int input = open(source, O_RDONLY);
            int output = open(wine_helper, O_WRONLY | O_CREAT | O_TRUNC, 0700);
            assert(input >= 0 && output >= 0);
            char buffer[16384];
            ssize_t bytes;
            while ((bytes = read(input, buffer, sizeof(buffer))) > 0) {
                ssize_t written = 0;
                while (written < bytes) {
                    ssize_t amount = write(output, buffer + written, (size_t)(bytes - written));
                    assert(amount > 0);
                    written += amount;
                }
            }
            assert(bytes == 0 && close(input) == 0 && close(output) == 0);
            wine_game = fork();
            assert(wine_game >= 0);
            if (wine_game == 0) {
                (void)setpgid(0, 0);
                execl(wine_helper, wine_helper, "--wine-exe-probe", "Game.exe", (char*)NULL);
                _exit(127);
            }
            assert(setpgid(wine_game, wine_game) == 0 || errno == EACCES);
            leader = fork();
            assert(leader >= 0);
            if (leader == 0)
                _exit(0);
            assert(waitpid(leader, NULL, 0) == leader);
            ms_process_register_game(987654323U, leader);
            response = ms_process_running_json(home);
            assert(response && strstr(response, "987654323") && strstr(response, "\"pid\""));
            free(response);
            const char* fallback_stop_body = "{\"appid\":987654323}";
            response = ms_process_kill_json(home, fallback_stop_body, strlen(fallback_stop_body), &status);
            assert(response && status == 200 && strstr(response, "\"ok\":true"));
            free(response);
            wait_for_test_child(wine_game);
            free(wine_helper);
            free(wine_bin);
        }
        {
            char* wine_helper = join(home, "runtime/wine/probe/metalsharp-probe");
            pid_t wine_game = fork();
            int status = 0;
            assert(wine_helper && wine_game >= 0);
            if (wine_game == 0) {
                (void)setpgid(0, 0);
                execl(wine_helper, wine_helper, "--wine-exe-probe", "UntrackedGame.exe", (char*)NULL);
                _exit(127);
            }
            assert(setpgid(wine_game, wine_game) == 0 || errno == EACCES);
            const char* untracked_stop_body = "{\"appid\":987654324}";
            response = ms_process_kill_json(home, untracked_stop_body, strlen(untracked_stop_body), &status);
            assert(response && status == 200 && strstr(response, "\"ok\":true"));
            free(response);
            wait_for_test_child(wine_game);
            free(wine_helper);
        }
        {
            char* wine_helper = join(home, "runtime/wine/probe/metalsharp-probe");
            pid_t wine_game = fork();
            int status = 0;
            assert(wine_helper && wine_game >= 0);
            if (wine_game == 0) {
                (void)setpgid(0, 0);
                execl(wine_helper, wine_helper, "--wine-exe-probe", "Game.exe", (char*)NULL);
                _exit(127);
            }
            assert(setpgid(wine_game, wine_game) == 0 || errno == EACCES);
            response = ms_process_force_quit_json(home, &status);
            assert(response && status == 200 && strstr(response, "\"pid\"") && strstr(response, "\"appid\":0"));
            free(response);
            wait_for_test_child(wine_game);
            free(wine_helper);
        }
        {
            char* wine_helper = join(home, "runtime/wine/probe/metalsharp-probe");
            pid_t steam = fork();
            int status = 0;
            assert(wine_helper && steam >= 0);
            if (steam == 0) {
                (void)setpgid(0, 0);
                execl(wine_helper, wine_helper, "--wine-exe-probe", "Steam.exe", (char*)NULL);
                _exit(127);
            }
            assert(setpgid(steam, steam) == 0 || errno == EACCES);
            response = ms_process_force_kill_json(home, &status);
            assert(response && status == 200 && strstr(response, "\"terminated_count\":0"));
            free(response);
            assert(kill(steam, 0) == 0);
            assert(kill(steam, SIGKILL) == 0);
            assert(waitpid(steam, NULL, 0) == steam);
            free(wine_helper);
        }
        free(game_id);
        free(gj_home);
    }
    {
        char* sharp_home = join(home, "sharp-running");
        char* wine_path = join(sharp_home, "runtime/wine/bin/metalsharp-wine");
        char* exe_path = join(sharp_home, "games/Example.exe");
        char library[PATH_MAX];
        char launch_body[PATH_MAX];
        const char* stop_body = "{\"id\":\"sharp-smoke\"}";
        char* response;
        int status = 0;
        fixture(home, "sharp-running/runtime/wine/bin/metalsharp-wine", "#!/bin/sh\nexec /bin/sleep 30\n");
        assert(chmod(wine_path, 0700) == 0);
        fixture(home, "sharp-running/games/Example.exe", "game");
        snprintf(library, sizeof(library),
                 "[{\"id\":\"sharp-smoke\",\"name\":\"Smoke App\",\"exe_path\":\"%s\","
                 "\"install_dir\":\"%s\",\"engine\":\"auto\",\"bottle_id\":null}]",
                 exe_path, sharp_home);
        fixture(home, "sharp-running/sharp-library/library.json", library);
        snprintf(launch_body, sizeof(launch_body), "{\"id\":\"sharp-smoke\"}");
        response = ms_sharp_action_json(sharp_home, (const unsigned char*)launch_body, strlen(launch_body), "launch");
        assert(response && strstr(response, "\"ok\":true"));
        free(response);
        response = ms_sharp_running_json();
        assert(response && strstr(response, "sharp-smoke"));
        free(response);
        response = ms_sharp_stop_json((const unsigned char*)stop_body, strlen(stop_body), &status);
        assert(response && status == 200 && strstr(response, "\"ok\":true"));
        free(response);
        response = ms_sharp_running_json();
        assert(response && !strstr(response, "sharp-smoke"));
        free(response);
        response = ms_sharp_action_json(sharp_home, (const unsigned char*)launch_body, strlen(launch_body), "launch");
        assert(response && strstr(response, "\"ok\":true"));
        free(response);
        response = ms_sharp_stop_all_json(&status);
        assert(response && status == 200 && strstr(response, "\"stopped\":1"));
        free(response);
        free(sharp_home);
        free(wine_path);
        free(exe_path);
    }
#ifdef __APPLE__
    {
        char *sharp_home = join(home, "sharp-detached"), *game_dir = join(home, "sharp-detached-game"),
             *runtime_bin = join(home, "sharp-detached/runtime/wine/bin"),
             *helper = join(home, "sharp-detached/runtime/wine/bin/sharp-child");
        char source[PATH_MAX], library[PATH_MAX], body[256];
        const char* stop_body = "{\"id\":\"sharp-detached-test\"}";
        int status = 0;
        char* response;
        assert(sharp_home && game_dir && runtime_bin && helper);
        assert(realpath(argv[0], source) && ensure_directory(game_dir) && ensure_directory(runtime_bin));
        assert(copy_file_path(source, helper) && chmod(helper, 0700) == 0);
        snprintf(library, sizeof(library), "[{\"id\":\"sharp-detached-test\",\"install_dir\":\"%s\"}]", game_dir);
        fixture(home, "sharp-detached/sharp-library/library.json", library);
        for (int iteration = 0; iteration < 2; ++iteration) {
            pid_t detached_pid = 0;
            pid_t launcher = spawn_sharp_detached_fixture(game_dir, helper, &detached_pid);
            snprintf(body, sizeof(body), "{\"id\":\"sharp-detached-test\",\"pid\":%ld}", (long)launcher);
            response = ms_sharp_track_running_json(sharp_home, (const unsigned char*)body, strlen(body));
            assert(response && strstr(response, "\"ok\":true"));
            free(response);
            struct timespec startup_delay = {0, 100000000L};
            (void)nanosleep(&startup_delay, NULL);
            response = ms_sharp_running_json();
            assert(response && strstr(response, "sharp-detached-test"));
            free(response);
            if (iteration == 0) {
                response = ms_sharp_stop_json((const unsigned char*)stop_body, strlen(stop_body), &status);
                assert(response && status == 200 && strstr(response, "\"ok\":true"));
                free(response);
            } else {
                response = ms_sharp_stop_all_json(&status);
                assert(response && status == 200 && strstr(response, "\"stopped\":1"));
                free(response);
            }
            for (int retry = 0; retry < 20 && (kill(detached_pid, 0) == 0 || errno == EPERM); ++retry)
                (void)nanosleep(&startup_delay, NULL);
            assert(kill(detached_pid, 0) != 0 && errno == ESRCH);
            response = ms_sharp_running_json();
            assert(response && !strstr(response, "sharp-detached-test"));
            free(response);
        }
        free(sharp_home);
        free(game_dir);
        free(runtime_bin);
        free(helper);
    }
#endif
    {
        const char* stop_body = "{\"appName\":\"SmokeEpic\"}";
        char* epic_home = join(home, "epic-stop");
        char* wineserver_path = join(home, "epic-stop/runtime/wine/bin/wineserver");
        char* launch_pid_path = join(home, "epic-stop/epic/processes/SmokeEpic.launch.pid");
        char* response;
        char pid_text[32];
        int fd;
        fixture(home, "epic-stop/runtime/wine/bin/wineserver", "#!/bin/sh\nexit 1\n");
        assert(chmod(wineserver_path, 0700) == 0);
        snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)getpid());
        fixture(home, "epic-stop/epic/processes/SmokeEpic.launch.pid", pid_text);
        response = ms_epic_running_json(epic_home);
        assert(response && strstr(response, "SmokeEpic"));
        free(response);
        fixture(home, "epic-stop/epic/processes/SmokeEpic.launch.pid", "999999\n");
        response = ms_epic_running_json(epic_home);
        assert(response && !strstr(response, "SmokeEpic"));
        free(response);
        response = ms_epic_stop_json(epic_home, (const unsigned char*)stop_body, strlen(stop_body));
        assert(response && strstr(response, "could not stop Epic game bottle"));
        assert(access(launch_pid_path, F_OK) == 0);
        free(response);
        fd = open(wineserver_path, O_WRONLY | O_TRUNC);
        assert(fd >= 0);
        assert(write(fd, "#!/bin/sh\nexit 0\n", 17) == 17);
        assert(close(fd) == 0);
        assert(chmod(wineserver_path, 0700) == 0);
        response = ms_epic_stop_json(epic_home, (const unsigned char*)stop_body, strlen(stop_body));
        assert(response && strstr(response, "\"ok\":true"));
        assert(access(launch_pid_path, F_OK) != 0);
        free(response);
        free(epic_home);
        free(wineserver_path);
        free(launch_pid_path);
    }
    {
        const char* key_body = "{\"key\":\"test-api-key\"}";
        const char* gamesdb_response =
            "{\"code\":200,\"data\":{\"games\":[{\"id\":42,\"game_title\":\"Example Game\"}]},"
            "\"include\":{\"boxart\":{\"base_url\":{\"medium\":\"https://cdn.thegamesdb.net/images/medium/\"},"
            "\"data\":{\"42\":[{\"type\":\"boxart\",\"side\":\"front\","
            "\"filename\":\"boxart/front/42-1.png\"}]}}}}";
        const char* game_without_art = "{\"metadata\":{\"keyImages\":[]}}";
        const char* game_with_art = "{\"metadata\":{\"keyImages\":[{\"type\":\"DieselGameBoxTall\","
                                    "\"url\":\"https://cdn.epicgames.com/front.jpg\"}]}}";
        const char* expected_image_url = "https://cdn.thegamesdb.net/images/medium/boxart/front/42-1.png";
        const unsigned char png_fixture[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0};
        char *artwork_home = join(home, "thegamesdb-test"), *fixture_path = join(home, "thegamesdb-response.json"),
             *image_fixture_path = join(home, "thegamesdb-image.png");
        char *response, *artwork, *image_cache_path;
        ms_json *missing_art = NULL, *epic_art = NULL;
        struct stat key_metadata;
        bool from_thegamesdb = false;
        int status = 0;
        fixture(home, "thegamesdb-response.json", gamesdb_response);
        response = ms_epic_save_thegamesdb_api_key_json(artwork_home, (const unsigned char*)key_body, strlen(key_body),
                                                        &status);
        assert(response && status == 200 && strstr(response, "\"configured\":true"));
        free(response);
        char* key_path = epic_thegamesdb_key_path(artwork_home);
        assert(key_path && stat(key_path, &key_metadata) == 0 && (key_metadata.st_mode & 0077) == 0);
        free(key_path);
        assert(setenv("METALSHARP_THEGAMESDB_FIXTURE", fixture_path, 1) == 0);
        fixture_bytes(home, "thegamesdb-image.png", png_fixture, sizeof(png_fixture));
        assert(setenv("METALSHARP_THEGAMESDB_IMAGE_FIXTURE", image_fixture_path, 1) == 0);
        char parse_error[128];
        missing_art = ms_json_parse(game_without_art, strlen(game_without_art), parse_error, sizeof(parse_error));
        assert(missing_art);
        artwork = epic_artwork_url(artwork_home, "example_game", "Example Game", missing_art, &from_thegamesdb);
        image_cache_path = epic_thegamesdb_image_path(artwork_home, "example_game", expected_image_url);
        assert(artwork && image_cache_path && !strcmp(artwork, image_cache_path));
        assert(epic_artwork_file_valid(image_cache_path));
        assert(from_thegamesdb);
        free(artwork);
        free(image_cache_path);
        assert(unlink(fixture_path) == 0);
        unsetenv("METALSHARP_THEGAMESDB_FIXTURE");
        unsetenv("METALSHARP_THEGAMESDB_IMAGE_FIXTURE");
        from_thegamesdb = false;
        artwork = epic_artwork_url(artwork_home, "example_game", "Example Game", missing_art, &from_thegamesdb);
        image_cache_path = epic_thegamesdb_image_path(artwork_home, "example_game", expected_image_url);
        assert(artwork && image_cache_path && !strcmp(artwork, image_cache_path));
        assert(from_thegamesdb);
        free(artwork);
        free(image_cache_path);
        epic_art = ms_json_parse(game_with_art, strlen(game_with_art), parse_error, sizeof(parse_error));
        assert(epic_art);
        from_thegamesdb = true;
        artwork = epic_artwork_url(artwork_home, "epic_owned_art", "Example Game", epic_art, &from_thegamesdb);
        assert(artwork && !strcmp(artwork, "https://cdn.epicgames.com/front.jpg") && !from_thegamesdb);
        free(artwork);
        ms_json_free(missing_art);
        ms_json_free(epic_art);
        free(artwork_home);
        free(fixture_path);
        free(image_fixture_path);
    }
    {
        const char* sharp_manifest = "[{\"id\":\"sharp_test\",\"name\":\"Original Name\",\"exe_path\":\"/tmp/app.exe\","
                                     "\"install_dir\":\"/tmp\",\"engine\":\"auto\",\"cover_position_x\":50,"
                                     "\"cover_position_y\":50}]";
        const char* rename_body = "{\"id\":\"sharp_test\",\"name\":\"  Renamed App  \"}";
        const char* empty_name_body = "{\"id\":\"sharp_test\",\"name\":\"   \"}";
        const char* set_position_body = "{\"id\":\"sharp_test\",\"x\":17,\"y\":83}";
        char* sharp_home = join(home, "sharp-test");
        char* response;
        fixture(home, "sharp-test/sharp-library/library.json", sharp_manifest);
        response = ms_sharp_action_json(sharp_home, (const unsigned char*)rename_body, strlen(rename_body), "rename");
        assert(response && strstr(response, "\"ok\":true"));
        free(response);
        response = ms_sharp_library_json(sharp_home);
        assert(response && strstr(response, "\"name\":\"Renamed App\""));
        assert(strstr(response, "\"exe_path\":\"/tmp/app.exe\""));
        free(response);
        response =
            ms_sharp_action_json(sharp_home, (const unsigned char*)empty_name_body, strlen(empty_name_body), "rename");
        assert(response && strstr(response, "name is required"));
        free(response);
        response = ms_sharp_library_json(sharp_home);
        assert(response && strstr(response, "\"name\":\"Renamed App\""));
        free(response);
        response = ms_sharp_action_json(sharp_home, (const unsigned char*)set_position_body, strlen(set_position_body),
                                        "set-cover-position");
        assert(response && strstr(response, "\"ok\":true"));
        free(response);
        response = ms_sharp_library_json(sharp_home);
        assert(response && strstr(response, "\"cover_position_x\":17"));
        assert(strstr(response, "\"cover_position_y\":83"));
        free(response);
        free(sharp_home);
    }
    unsetenv("METALSHARP_PORT");
#ifdef __APPLE__
    unsetenv("ROSETTA_ADVERTISE_AVX");
    set_rosetta_avx_env();
    assert(getenv("ROSETTA_ADVERTISE_AVX") && !strcmp(getenv("ROSETTA_ADVERTISE_AVX"), "1"));
    unsetenv("ROSETTA_ADVERTISE_AVX");
#endif
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

    fixture(home, "cyberpunk/REDprelauncher.exe", "launcher executable");
    fixture(home, "cyberpunk/bin/x64/Cyberpunk2077.exe", "game executable");
    char* cyberpunk_dir = join(home, "cyberpunk");
    char* cyberpunk_exe = preferred_steam_game_executable(cyberpunk_dir, 1091500, "d3dmetal");
    assert(cyberpunk_exe && strstr(cyberpunk_exe, "/cyberpunk/bin/x64/Cyberpunk2077.exe"));
    free(cyberpunk_exe);
    free(cyberpunk_dir);

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

    fixture(home, "helldivers/tools/gguninst.exe", "GameGuard uninstaller");
    fixture(home, "helldivers/bin/helldivers2.exe", "game executable");
    char* helldivers_dir = join(home, "helldivers");
    char* helldivers_exe = preferred_steam_game_executable(helldivers_dir, 553850, "dxmt");
    assert(helldivers_exe && strstr(helldivers_exe, "/helldivers/bin/helldivers2.exe"));
    {
        char launch_cwd[PATH_MAX];
        char launch_program[PATH_MAX];
        assert(direct_game_launch_paths(helldivers_exe, 553850, launch_cwd, sizeof(launch_cwd), launch_program,
                                        sizeof(launch_program)));
        assert(!strcmp(launch_cwd, helldivers_dir));
        assert(!strcmp(launch_program, "bin/helldivers2.exe"));
    }
    free(helldivers_exe);
    free(helldivers_dir);

    fixture(home, "eve/Launcher/evelauncher.exe", "EVE Steam launcher");
    char* eve_dir = join(home, "eve");
    char* eve_dosdevices = join(home, "prefix-steam/dosdevices");
    char* e_drive = join(eve_dosdevices, "e:");
    char* c_drive = join(eve_dosdevices, "c:");
    char* local_eve_dir = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/common/Eve Online");
    assert(ensure_directory(eve_dosdevices));
    assert(symlink(home, e_drive) == 0);
    assert(symlink("../drive_c", c_drive) == 0);
    assert(ensure_directory(local_eve_dir));
    pe_fixture(home, "eve/app-1.9.9/eve-online.exe", 0x8664);
    pe_fixture(home, "eve/app-1.16.1/eve-online.exe", 0x8664);
    pe_fixture(home, "eve/app-2.0.0/eve-online.exe", 0x014c);
    char* eve_exe = preferred_steam_game_executable(eve_dir, 8500, "dxmt_32");
    assert(eve_exe && strstr(eve_exe, "/eve/Launcher/evelauncher.exe"));
    {
        char launch_cwd[PATH_MAX];
        char launch_program[PATH_MAX];
        assert(direct_game_launch_paths(eve_exe, 8500, launch_cwd, sizeof(launch_cwd), launch_program,
                                        sizeof(launch_program)));
        assert(!strcmp(launch_cwd, eve_dir));
        assert(!strcmp(launch_program, "Launcher/evelauncher.exe"));
    }
    char* eve_client = latest_eve_online_client_executable(eve_dir);
    assert(eve_client && strstr(eve_client, "/eve/app-1.16.1/eve-online.exe"));
    {
        char (*paths)[PATH_MAX] = calloc(26, sizeof(*paths));
        size_t count = eve_install_dir_wine_paths(home, eve_dir, paths, 26);
        assert(count >= 2);
        assert(eve_wine_process_command("wine E:\\eve\\Launcher\\evelauncher.exe", (const char (*)[PATH_MAX])paths,
                                        count));
        assert(!eve_wine_process_command("wine E:\\other\\eve-online.exe", (const char (*)[PATH_MAX])paths, count));
        free(paths);
        paths = calloc(26, sizeof(*paths));
        count = eve_install_dir_wine_paths(home, local_eve_dir, paths, 26);
        assert(count >= 1);
        assert(eve_wine_process_command(
            "wine C:\\Program Files (x86)\\Steam\\steamapps\\common\\Eve Online\\Launcher\\evelauncher.exe",
            (const char (*)[PATH_MAX])paths, count));
        assert(
            eve_wine_process_command("wine C:\\CCP\\EVE\\tq\\bin64\\exefile.exe", (const char (*)[PATH_MAX])NULL, 0));
        free(paths);
        assert(unlink(c_drive) == 0);
        assert(unlink(e_drive) == 0);
    }
    char eve_steam_url[512];
    assert(format_steam_run_url(eve_steam_url, sizeof(eve_steam_url), 8500, D3DMETAL_LAUNCHER_STEAM_ARGS));
    assert(!strcmp(eve_steam_url,
                   "steam://run/8500//--no-sandbox%20--in-process-gpu%20--disable-gpu%20--disable-d3d11%20"
                   "--enable-unsafe-swiftshader%20--use-gl=angle%20--use-angle=swiftshader-webgl"));
    char marvel_steam_url[128];
    assert(format_steam_run_url(marvel_steam_url, sizeof(marvel_steam_url), 2767030, MARVEL_RIVALS_STEAM_ARGS));
    assert(!strcmp(marvel_steam_url, "steam://run/2767030//-windowed"));
    assert(marvel_rivals_uses_steam_bootstrap(2767030, "d3dmetal"));
    assert(!marvel_rivals_uses_steam_bootstrap(2767030, "vkd3d"));
    assert(!marvel_rivals_uses_steam_bootstrap(8500, "d3dmetal"));
    char baldurs_gate_3_steam_url[64];
    assert(format_steam_run_url(baldurs_gate_3_steam_url, sizeof(baldurs_gate_3_steam_url), 1086940, NULL));
    assert(!strcmp(baldurs_gate_3_steam_url, "steam://run/1086940"));
    assert(baldurs_gate_3_uses_steam_bootstrap(1086940, "d3dmetal"));
    assert(!baldurs_gate_3_uses_steam_bootstrap(1086940, "vkd3d"));
    assert(!baldurs_gate_3_uses_steam_bootstrap(2767030, "d3dmetal"));
    assert(baldurs_gate_3_process_rank("Z:\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3\\bin\\bg3_dx11.exe") == 3);
    assert(baldurs_gate_3_process_rank("..\\bin\\bg3_dx11.exe -externalcrashhandler") == 3);
    assert(baldurs_gate_3_process_rank("Z:\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3\\bin\\bg3.exe") == 2);
    assert(baldurs_gate_3_process_rank("Z:\\Other\\bg3.exe") == 2);
    assert(baldurs_gate_3_process_matches(3, false, true));
    assert(!baldurs_gate_3_process_matches(3, false, false));
    assert(baldurs_gate_3_process_matches(1, true, false));
    assert(!baldurs_gate_3_process_matches(0, true, true));
    assert(baldurs_gate_3_process_rank("Z:\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3\\LariLauncher.exe") == 1);
    assert(baldurs_gate_3_process_rank("Z:\\SteamLibrary\\steamapps\\common\\Baldurs Gate "
                                       "3\\Launcher\\runtimes\\CefSharp.BrowserSubprocess.exe") == 0);
    const char* marvel_command =
        "Z:\\Volumes\\AverySSD\\SteamLibrary\\steamapps\\common\\MarvelRivals\\MarvelGame\\Marvel\\"
        "Binaries\\Win64\\Marvel-Win64-Shipping.exe";
    assert(marvel_rivals_process_rank(marvel_command) == 4);
    assert(command_contains_wine_path(marvel_command,
                                      "Z:\\Volumes\\AverySSD\\SteamLibrary\\steamapps\\common\\MarvelRivals"));
    assert(!command_contains_wine_path(marvel_command, "Z:\\Volumes\\OtherDrive\\MarvelRivals"));
    const char* baldurs_gate_3_command =
        "Z:\\Volumes\\AverySSD\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3\\bin\\bg3_dx11.exe";
    assert(command_contains_wine_path(baldurs_gate_3_command,
                                      "Z:\\Volumes\\AverySSD\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3"));
    assert(!command_contains_wine_path(baldurs_gate_3_command,
                                       "Z:\\Volumes\\OtherDrive\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3"));
    {
        char overrides[1024];
        assert(format_steam_pipeline_overrides(overrides, sizeof(overrides), "d3dmetal"));
        assert(strstr(overrides, "d3d10core=n,b"));
        assert(strstr(overrides, "bcrypt=b"));
        assert(strstr(overrides, "ncrypt=b"));
    }
    {
        char* args[16];
        size_t arg_count = 0;
        build_launch_args(8500, "dxmt", args, &arg_count, sizeof(args) / sizeof(args[0]));
        assert(arg_count == 7);
        assert(!strcmp(args[0], "--no-sandbox"));
        assert(!strcmp(args[6], "--use-angle=swiftshader-webgl"));
    }
    {
        int handoff_pipe[2];
        pid_t handoff_child;
        int child_status;
        assert(pipe(handoff_pipe) == 0);
        handoff_child = fork();
        assert(handoff_child >= 0);
        if (handoff_child == 0) {
            char byte;
            close(handoff_pipe[1]);
            (void)read(handoff_pipe[0], &byte, 1);
            _exit(0);
        }
        close(handoff_pipe[0]);
        ms_process_register_pending_game(8499, handoff_child, 15);
        ms_process_register_game(8499, getpid());
        close(handoff_pipe[1]);
        usleep(20000);
        char* reap_probe = ms_process_running_json(home);
        assert(reap_probe && strstr(reap_probe, "\"appid\":8499"));
        free(reap_probe);
        errno = 0;
        assert(waitpid(handoff_child, &child_status, WNOHANG) == -1 && errno == ECHILD);
        ms_process_register_pending_game(8499, handoff_child, 0);
        reap_probe = ms_process_running_json(home);
        assert(reap_probe && !strstr(reap_probe, "\"appid\":8499"));
        free(reap_probe);
    }
    {
        pid_t grace_child = fork();
        assert(grace_child >= 0);
        if (grace_child == 0)
            _exit(0);
        ms_process_register_pending_game(8498, grace_child, 1);
        char* grace_probe = ms_process_running_json(home);
        assert(grace_probe && strstr(grace_probe, "\"appid\":8498"));
        free(grace_probe);
        usleep(1100000);
        grace_probe = ms_process_running_json(home);
        assert(grace_probe && !strstr(grace_probe, "\"appid\":8498"));
        free(grace_probe);
    }
    ms_process_register_pending_game(8500, getpid(), 15);
    char* running_during_eve_handoff = ms_process_running_json(home);
    assert(running_during_eve_handoff && strstr(running_during_eve_handoff, "\"appid\":8500"));
    free(running_during_eve_handoff);
    {
        int kill_status = 0;
        const char* eve_stop_body = "{\"appid\":8500}";
        char* stopped = ms_process_kill_json(home, eve_stop_body, strlen(eve_stop_body), &kill_status);
        assert(stopped && kill_status == 200 && strstr(stopped, "\"ok\":true"));
        free(stopped);
    }
    free(eve_client);
    free(eve_exe);
    free(eve_dir);
    free(eve_dosdevices);
    free(e_drive);
    free(c_drive);
    free(local_eve_dir);
    {
        char* runtime_dir = join(home, "runtime");
        assert(ensure_directory(runtime_dir));
        free(runtime_dir);
        assert(write_wine_steam_route_pending(home, "d3dmetal"));
        assert(wine_steam_route_marker_is_pending(home, "d3dmetal"));
        assert(!wine_steam_route_marker_is_pending(home, "dxmt"));
        clear_wine_steam_route_marker(home);
    }

    fixture(home, "aoe4/EssenceEditor.exe", "editor executable that must not launch");
    fixture(home, "aoe4/RelicCardinal.exe", "Age of Empires IV game executable");
    char* aoe4_dir = join(home, "aoe4");
    char* aoe4_exe = preferred_steam_game_executable(aoe4_dir, 1466860, "d3dmetal");
    assert(aoe4_exe && strstr(aoe4_exe, "/aoe4/RelicCardinal.exe"));
    free(aoe4_exe);
    free(aoe4_dir);
    {
        const char* game_root = "beamng";
        static const char* const route_files[] = {"d3d10.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll"};
        char* game_dir = join(home, game_root);
        char* executable;
        assert(game_dir);
        pe_fixture(home, "beamng/BeamNG.drive.exe", 0x014c);
        pe_fixture(home, "beamng/Bin64/BeamNG.drive.x64.exe", 0x8664);
        executable = preferred_steam_game_executable(game_dir, 284160, "d3dmetal");
        assert(executable && strstr(executable, "/beamng/Bin64/BeamNG.drive.x64.exe"));
        for (size_t i = 0; i < sizeof(route_files) / sizeof(route_files[0]); i++) {
            char relative[PATH_MAX];
            snprintf(relative, sizeof(relative), "runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows/%s", route_files[i]);
            fixture(home, relative, route_files[i]);
        }
        assert(ms_steam_stage_route_for_executable(home, "d3dmetal", game_dir, executable));
        for (size_t i = 0; i < sizeof(route_files) / sizeof(route_files[0]); i++) {
            char relative[PATH_MAX];
            snprintf(relative, sizeof(relative), "beamng/Bin64/%s", route_files[i]);
            char* staged = join(home, relative);
            assert(staged && access(staged, R_OK) == 0);
            free(staged);
        }
        free(executable);
        free(game_dir);
    }
    {
        char* game_dir = join(home, "witcher3");
        char* resolved;
        char* steam_selected;
        assert(game_dir);
        fixture(home, "witcher3/REDlauncher.exe", "storefront launcher");
        fixture(home, "witcher3/bin/x64/witcher3.exe", "actual game executable");
        resolved = ms_witcher3_game_executable(game_dir);
        assert(resolved && strstr(resolved, "/witcher3/bin/x64/witcher3.exe"));
        steam_selected = preferred_steam_game_executable(game_dir, 292030, "d3dmetal");
        assert(steam_selected && strstr(steam_selected, "/witcher3/bin/x64/witcher3.exe"));
        free(resolved);
        free(steam_selected);
        free(game_dir);
    }
    {
        char parse_error[128];
        char* defaults_json = ms_mtsp_default_rules_json();
        ms_json* defaults = defaults_json
                                ? ms_json_parse(defaults_json, strlen(defaults_json), parse_error, sizeof(parse_error))
                                : NULL;
        const ms_json* rules = defaults ? ms_json_object_get(defaults, "rules") : NULL;
        bool found = false;
        assert(defaults && rules);
        for (size_t i = 0; i < ms_json_array_length(rules); i++) {
            const ms_json* rule = ms_json_array_get(rules, i);
            long long appid = 0;
            char* pipeline = NULL;
            char* executable = NULL;
            if (!ms_json_as_i64(ms_json_object_get(rule, "appid"), &appid) || appid != 2767030)
                continue;
            assert(ms_json_as_string(ms_json_object_get(rule, "default_pipeline"), &pipeline));
            assert(!strcmp(pipeline, "d3dmetal"));
            const ms_json* exe_names = ms_json_object_get(rule, "exe_names");
            assert(exe_names && ms_json_array_length(exe_names) == 1);
            assert(ms_json_as_string(ms_json_array_get(exe_names, 0), &executable));
            assert(!strcmp(executable, "MarvelGame/Marvel/Binaries/Win64/Marvel-Win64-Shipping.exe"));
            free(pipeline);
            free(executable);
            found = true;
            break;
        }
        assert(found);
        ms_json_free(defaults);
        free(defaults_json);
    }
    {
        char parse_error[128];
        char* defaults_json = ms_mtsp_default_rules_json();
        ms_json* defaults = defaults_json
                                ? ms_json_parse(defaults_json, strlen(defaults_json), parse_error, sizeof(parse_error))
                                : NULL;
        const ms_json* rules = defaults ? ms_json_object_get(defaults, "rules") : NULL;
        bool found = false;
        assert(defaults && rules);
        for (size_t i = 0; i < ms_json_array_length(rules); i++) {
            const ms_json* rule = ms_json_array_get(rules, i);
            long long appid = 0;
            char* pipeline = NULL;
            if (!ms_json_as_i64(ms_json_object_get(rule, "appid"), &appid) || appid != 1086940)
                continue;
            assert(ms_json_as_string(ms_json_object_get(rule, "default_pipeline"), &pipeline));
            assert(!strcmp(pipeline, "d3dmetal"));
            free(pipeline);
            found = true;
            break;
        }
        assert(found);
        ms_json_free(defaults);
        free(defaults_json);
    }
    fixture(home, "bg3-game/bin/bg3_dx11.exe", "game executable");
    fixture(home, "bg3-game/Launcher/runtimes/win-x86/native/CefSharp.BrowserSubprocess.exe", "helper executable");
    char* bg3_game_dir = join(home, "bg3-game");
    char* bg3_executable = preferred_steam_game_executable(bg3_game_dir, 1086940, "d3dmetal");
    assert(bg3_executable && strstr(bg3_executable, "/bg3-game/bin/bg3_dx11.exe"));
    free(bg3_executable);
    free(bg3_game_dir);
    fixture(home, "bottles/steam_1086940/bottle.json",
            "{\"steam_app_id\":1086940,\"preferred_pipeline\":\"dxmt\",\"runtime_profile\":\"dxmt\","
            "\"custom_name\":\"My BG3\",\"updated_at\":\"old\"}");
    assert(ms_steam_migrate_baldurs_gate_3_route_default(home));
    char* bg3_manifest_path = join(home, "bottles/steam_1086940/bottle.json");
    char* migrated_bg3 = read_bounded_file(bg3_manifest_path);
    assert(migrated_bg3 && strstr(migrated_bg3, "\"preferred_pipeline\":\"d3dmetal\""));
    assert(migrated_bg3 && strstr(migrated_bg3, "\"runtime_profile\":\"d3dmetal\""));
    assert(migrated_bg3 && strstr(migrated_bg3, "\"custom_name\":\"My BG3\""));
    free(migrated_bg3);
    free(bg3_manifest_path);
    fixture(home, "bottles/steam_1086940/bottle.json",
            "{\"steam_app_id\":1086940,\"preferred_pipeline\":\"dxmt\",\"runtime_profile\":\"dxmt\"}");
    assert(ms_steam_migrate_baldurs_gate_3_route_default(home));
    bg3_manifest_path = join(home, "bottles/steam_1086940/bottle.json");
    migrated_bg3 = read_bounded_file(bg3_manifest_path);
    assert(migrated_bg3 && strstr(migrated_bg3, "\"preferred_pipeline\":\"dxmt\""));
    free(migrated_bg3);
    free(bg3_manifest_path);
    {
        char* args[8] = {0};
        size_t count = 0;
        build_launch_args(2767030, "d3dmetal", args, &count, sizeof(args) / sizeof(args[0]));
        assert(count == 1 && !strcmp(args[0], "-windowed"));
    }
    {
        char* args[8] = {0};
        size_t count = 0;
        build_launch_args(553850, "d3dmetal", args, &count, sizeof(args) / sizeof(args[0]));
        assert(count == 3);
        assert(!strcmp(args[0], "--bundle-dir"));
        assert(!strcmp(args[1], "data"));
        assert(!strcmp(args[2], "--release"));
    }
    {
        char* args[8] = {0};
        size_t count = 0;
        build_launch_args(1174180, "d3dmetal", args, &count, sizeof(args) / sizeof(args[0]));
        assert(count == 0);
        build_launch_args(1174180, "vkd3d", args, &count, sizeof(args) / sizeof(args[0]));
        assert(count == 2 && !strcmp(args[0], "-api") && !strcmp(args[1], "Vulkan"));
    }
    {
        char* launcher = join(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe");
        char wine_path[PATH_MAX + 3];
        char resolved[PATH_MAX];
        assert(!rockstar_launcher_executable(home, 1174180, "d3dmetal"));
        fixture(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe", "launcher");
        char* found = rockstar_launcher_executable(home, 1174180, "d3dmetal");
        assert(found && !strcmp(found, launcher));
        free(found);
        found = rockstar_launcher_executable(home, 1174180, "vkd3d");
        assert(found && !strcmp(found, launcher));
        free(found);
        assert(!rockstar_launcher_executable(home, 1174180, "dxmt"));
        assert(!rockstar_launcher_executable(home, 271590, "d3dmetal"));
        found = rockstar_launcher_executable(home, 3240220, "vkd3d");
        assert(found && !strcmp(found, launcher));
        free(found);
        assert(rockstar_launcher_game_for(1174180, "vkd3d")->launcher_on_d3dmetal);
        assert(rockstar_launcher_game_for(1174180, "d3dmetal")->dx12_settings);
        assert(!rockstar_launcher_game_for(3240220, "vkd3d")->launcher_on_d3dmetal);
        assert(!strcmp(rockstar_launcher_game_for(3240220, "d3dmetal")->client, "GTA5_Enhanced.exe"));
        assert(rockstar_launcher_game_for(3240220, "d3dmetal")->agility_frontend);
        assert(!strcmp(default_pipeline_for_appid(home, 1174180), "d3dmetal"));
        assert(!strcmp(default_pipeline_for_appid(home, 3240220), "d3dmetal"));
        assert(!rockstar_launcher_game_for(1174180, "d3dmetal")->agility_frontend);
        assert(is_rockstar_launcher_executable(launcher));
        assert(!is_rockstar_launcher_executable("/games/Red Dead Redemption 2/RDR2.exe"));
        assert(format_wine_host_path(wine_path, sizeof(wine_path), home));
        assert(realpath(home, resolved));
        assert(!strncmp(wine_path, "Z:\\", 3) && strlen(wine_path) == strlen(resolved) + 2 && !strchr(wine_path, '/'));
        assert(!format_wine_host_path(wine_path, sizeof(wine_path), "/nonexistent/metalsharp/path"));
        assert(unlink(launcher) == 0);
        free(launcher);
    }
    {
        /* Anti-cheat stub swap: back up the stub once, copy the real exe over
         * it, and refresh the copy after a game update or a restored stub. */
        const protected_exe_swap* gta = NULL;
        char* dir = join(home, "swap-test");
        char *real = join(dir, "GTA5_Enhanced.exe"), *stub = join(dir, "GTA5_Enhanced_BE.exe"),
             *backup = join(dir, "GTA5_Enhanced_BE.old");
        char* text;
        for (size_t i = 0; i < sizeof(PROTECTED_EXE_SWAPS) / sizeof(PROTECTED_EXE_SWAPS[0]); i++)
            if (PROTECTED_EXE_SWAPS[i].appid == 3240220)
                gta = &PROTECTED_EXE_SWAPS[i];
        assert(gta && gta->vkd3d && !strcmp(gta->stub, "GTA5_Enhanced_BE.exe"));
        fixture(home, "swap-test/GTA5_Enhanced.exe", "game v1");
        fixture(home, "swap-test/GTA5_Enhanced_BE.exe", "battleye");
        assert(apply_protected_exe_swap_in(dir, gta));
        text = read_bounded_file(backup);
        assert(text && !strcmp(text, "battleye"));
        free(text);
        assert(files_match(stub, real));
        assert(apply_protected_exe_swap_in(dir, gta));
        fixture(home, "swap-test/GTA5_Enhanced.exe", "game v2");
        assert(apply_protected_exe_swap_in(dir, gta) && files_match(stub, real));
        fixture(home, "swap-test/GTA5_Enhanced_BE.exe", "battleye restored by verify");
        assert(apply_protected_exe_swap_in(dir, gta) && files_match(stub, real));
        text = read_bounded_file(backup);
        assert(text && !strcmp(text, "battleye"));
        free(text);
        assert(unlink(real) == 0);
        assert(!apply_protected_exe_swap_in(dir, gta));
        assert(apply_protected_exe_swap(home, 1245620, "vkd3d"));
        assert(remove_tree(dir));
        free(dir);
        free(real);
        free(stub);
        free(backup);
    }
    {
        const char* vulkan_xml = "<x>\n  <advancedGraphics>\n    <API>kSettingAPI_Vulkan</API>\n    <locked "
                                 "value=\"true\" />\n  </advancedGraphics>\n</x>\n";
        char* settings = join(home, "prefix-steam/drive_c/users/alice/Documents/Rockstar Games/Red Dead Redemption "
                                    "2/Settings/system.xml");
        char* fresh_dir = join(home, "prefix-steam/drive_c/users/bob/Documents");
        char* fresh = join(home, "prefix-steam/drive_c/users/bob/Documents/Rockstar Games/Red Dead Redemption "
                                 "2/Settings/system.xml");
        char* public_settings = join(home, "prefix-steam/drive_c/users/Public/Documents/Rockstar Games");
        char* text;
        fixture(home,
                "prefix-steam/drive_c/users/alice/Documents/Rockstar Games/Red Dead Redemption 2/Settings/system.xml",
                vulkan_xml);
        fixture(home, "prefix-steam/drive_c/users/Public/Documents/.keep", "");
        assert(ensure_directory(fresh_dir));
        assert(ensure_rdr2_dx12_settings(home));
        text = read_bounded_file(settings);
        assert(text && strstr(text, "<API>kSettingAPI_DX12</API>") && !strstr(text, "kSettingAPI_Vulkan") &&
               strstr(text, "<locked value=\"true\" />"));
        free(text);
        assert(ensure_rdr2_dx12_settings(home));
        text = read_bounded_file(fresh);
        assert(text && strstr(text, "<API>kSettingAPI_DX12</API>") && strstr(text, "<version value=\"37\" />"));
        free(text);
        assert(access(public_settings, F_OK) != 0);
        {
            /* A NUL in videoCardDescription must survive the API swap. */
            static const char binary_xml[] = "<API>kSettingAPI_Vulkan</API>\n<v>\xc0\x00\x10</v>\n";
            static const char binary_expected[] = "<API>kSettingAPI_DX12</API>\n<v>\xc0\x00\x10</v>\n";
            unsigned char buffer[128];
            size_t got;
            FILE* file = fopen(settings, "wb");
            assert(file && fwrite(binary_xml, 1, sizeof(binary_xml) - 1, file) == sizeof(binary_xml) - 1);
            fclose(file);
            assert(ensure_rdr2_dx12_settings(home));
            file = fopen(settings, "rb");
            assert(file);
            got = fread(buffer, 1, sizeof(buffer), file);
            fclose(file);
            assert(got == sizeof(binary_expected) - 1 && !memcmp(buffer, binary_expected, got));
        }
        free(settings);
        free(fresh_dir);
        free(fresh);
        free(public_settings);
    }
    {
        char* runtime = join(home, "runtime/wfdxcompat");
        set_wfdxcompat_runtime_env(home, "d3dmetal");
        assert(!getenv("WFDXCOMPAT_RUNTIME_DIR"));
        fixture(home, "runtime/wfdxcompat/x86_64-windows/wfdx-launchers-v1.dll", "launcher companion");
        set_wfdxcompat_runtime_env(home, "d3dmetal");
        assert(getenv("WFDXCOMPAT_RUNTIME_DIR") && !strcmp(getenv("WFDXCOMPAT_RUNTIME_DIR"), runtime));
        set_wfdxcompat_runtime_env(home, "dxmt");
        assert(!getenv("WFDXCOMPAT_RUNTIME_DIR"));
        /* The Agility lane is opt-in per game and only when its frontend is staged. */
        set_wfdxcompat_agility_env(home);
        assert(!getenv("WFDXCOMPAT_RUNTIME_DIR"));
        fixture(home, "runtime/wfdxcompat-agility/x86_64-windows/d3d12.dll", "agility frontend");
        set_wfdxcompat_agility_env(home);
        {
            char* agility = join(home, "runtime/wfdxcompat-agility");
            assert(getenv("WFDXCOMPAT_RUNTIME_DIR") && !strcmp(getenv("WFDXCOMPAT_RUNTIME_DIR"), agility));
            assert(remove_tree(agility));
            free(agility);
        }
        unsetenv("WFDXCOMPAT_RUNTIME_DIR");
        assert(remove_tree(runtime));
        free(runtime);
    }

    fixture(home, "ubisoft/odyssey/uplay_r1_loader64.dll", "Ubisoft Connect marker");
    char* ubisoft_game_dir = join(home, "ubisoft/odyssey");
    assert(steam_game_uses_ubisoft_connect(812140, NULL));
    assert(steam_game_uses_ubisoft_connect(999999, ubisoft_game_dir));
    assert(!steam_game_uses_ubisoft_connect(999999, home));
    {
        const d3dmetal_steam_launcher_game* eve = d3dmetal_steam_launcher_game_for(8500, "d3dmetal");
        const d3dmetal_steam_launcher_game* odyssey = d3dmetal_steam_launcher_game_for(812140, "d3dmetal");
        char odyssey_steam_url[512];
        assert(eve && !strcmp(eve->launcher, "Launcher/evelauncher.exe") && !eve->client);
        assert(odyssey && !strcmp(odyssey->launcher, "ACOdyssey.exe") && !strcmp(odyssey->client, "ACOdyssey.exe"));
        assert(!d3dmetal_steam_launcher_game_for(1174180, "d3dmetal"));
        assert(!d3dmetal_steam_launcher_game_for(812140, "dxmt"));
        assert(!d3dmetal_steam_launcher_game_for(999999, "d3dmetal"));
        assert(!d3dmetal_steam_launcher_game_for(8500, NULL));
        assert(
            format_steam_run_url(odyssey_steam_url, sizeof(odyssey_steam_url), 812140, D3DMETAL_LAUNCHER_STEAM_ARGS));
        assert(!strncmp(odyssey_steam_url, "steam://run/812140//--no-sandbox%20", 35));
        {
            int status = 0;
            char* missing = launch_d3dmetal_launcher_via_steam_json(home, odyssey, &status);
            assert(missing && status == 404 &&
                   strstr(missing, "Assassin's Creed Odyssey Steam launcher was not found"));
            free(missing);
        }
    }
    assert(ubisoft_connect_command("C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe"));
    assert(!odyssey_process_command("C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe",
                                    "C:\\Games\\ACOdyssey.exe"));
    assert(odyssey_process_command("C:\\Games\\ACOdyssey.exe", "C:\\Games\\ACOdyssey.exe"));
    assert(!odyssey_process_command("C:\\Games\\UplayCrashReporter.exe", "C:\\Games\\ACOdyssey.exe"));
    {
        const char* stop_body = "{\"appid\":812140}";
        int status = 500;
        char* stopped = ms_process_kill_json(home, stop_body, strlen(stop_body), &status);
        char parse_error[96];
        bool ok = false;
        assert(stopped);
        ms_json* parsed = ms_json_parse(stopped, strlen(stopped), parse_error, sizeof(parse_error));
        assert(status == 200 && parsed && ms_json_as_bool(ms_json_object_get(parsed, "ok"), &ok) && ok);
        ms_json_free(parsed);
        free(stopped);
    }
    {
        char* json = launch_mode_pid_result(42, 673130, "steam_handoff");
        char* launch_mode = NULL;
        long long number = 0;
        bool ok = false;
        char parse_error[96];
        ms_json* parsed = ms_json_parse(json, strlen(json), parse_error, sizeof(parse_error));
        assert(parsed && ms_json_as_bool(ms_json_object_get(parsed, "ok"), &ok) && ok);
        assert(ms_json_as_i64(ms_json_object_get(parsed, "pid"), &number) && number == 42);
        assert(ms_json_as_i64(ms_json_object_get(parsed, "appid"), &number) && number == 673130);
        assert(ms_json_as_string(ms_json_object_get(parsed, "launch_mode"), &launch_mode));
        assert(!strcmp(launch_mode, "steam_handoff"));
        free(launch_mode);
        ms_json_free(parsed);
        free(json);
    }
    free(ubisoft_game_dir);

    fixture(home, "configs/config.json", "{\"msync\":false}");
    set_wine_msync(home);
    assert(!strcmp(getenv("WINEMSYNC"), "0"));
    fixture(home, "configs/config.json", "{\"msync\":true}");
    set_wine_msync(home);
    assert(!strcmp(getenv("WINEMSYNC"), "1"));
    fixture(home, "configs/config.json",
            "{\"msync\":false,\"windowMode\":\"fullscreen\",\"gameResolution\":\"3840x2160\"}");
    ms_steam_apply_launch_preferences(home);
    assert(!strcmp(getenv("WINEMSYNC"), "0"));
    assert(!strcmp(getenv("METALSHARP_GAME_WINDOW_MODE"), "fullscreen"));
    assert(!strcmp(getenv("METALSHARP_GAME_RESOLUTION"), "3840x2160"));
    fixture(home, "configs/config.json", "{}");
    ms_steam_apply_launch_preferences(home);
    assert(!getenv("METALSHARP_GAME_WINDOW_MODE"));
    assert(!getenv("METALSHARP_GAME_RESOLUTION"));
    {
        char* wrapper = ms_steam_wine_launch_wrapper_path(home);
        char* fake_wine = join(home, "fake-wine");
        char* wine_log = join(home, "fake-wine-args.log");
        char* game_exe = join(home, "Windowed Game.exe");
        fixture(home, "fake-wine", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$METALSHARP_TEST_WINE_LOG\"\n");
        assert(chmod(fake_wine, 0700) == 0 && wrapper && access(wrapper, X_OK) == 0);
        assert(setenv("METALSHARP_WINE_BINARY", fake_wine, 1) == 0);
        assert(setenv("METALSHARP_TEST_WINE_LOG", wine_log, 1) == 0);
        assert(setenv("METALSHARP_GAME_WINDOW_MODE", "windowed", 1) == 0);
        assert(setenv("METALSHARP_GAME_RESOLUTION", "1920x1080", 1) == 0);
        pid_t wrapper_child = fork();
        assert(wrapper_child >= 0);
        if (wrapper_child == 0) {
            execl(wrapper, wrapper, game_exe, (char*)NULL);
            _exit(127);
        }
        int wrapper_status = 0;
        assert(waitpid(wrapper_child, &wrapper_status, 0) == wrapper_child);
        assert(WIFEXITED(wrapper_status) && WEXITSTATUS(wrapper_status) == 0);
        char* args = read_bounded_file(wine_log);
        assert(args && strstr(args, "explorer\n") && strstr(args, "/desktop=MetalSharp,1920x1080\n") &&
               strstr(args, "Windowed Game.exe\n"));
        free(args);
        assert(setenv("METALSHARP_GAME_WINDOW_MODE", "fullscreen", 1) == 0);
        wrapper_child = fork();
        assert(wrapper_child >= 0);
        if (wrapper_child == 0) {
            execl(wrapper, wrapper, game_exe, (char*)NULL);
            _exit(127);
        }
        assert(waitpid(wrapper_child, &wrapper_status, 0) == wrapper_child);
        assert(WIFEXITED(wrapper_status) && WEXITSTATUS(wrapper_status) == 0);
        args = read_bounded_file(wine_log);
        assert(args && !strstr(args, "explorer\n") && strstr(args, "Windowed Game.exe\n"));
        free(args);
        unsetenv("METALSHARP_GAME_WINDOW_MODE");
        unsetenv("METALSHARP_GAME_RESOLUTION");
        unsetenv("METALSHARP_WINE_BINARY");
        unsetenv("METALSHARP_TEST_WINE_LOG");
        free(wrapper);
        free(fake_wine);
        free(wine_log);
        free(game_exe);
    }
    {
        char *vendor_dll, *managed_xinput, *managed_dinput, *shim_manifest, *game_dir;
        fixture(home, "runtime/wine/lib/metalsharp/x86_64-windows/xinput1_1.dll", "managed-xinput");
        fixture(home, "runtime/wine/lib/metalsharp/x86_64-windows/dinput8.dll", "managed-dinput");
        fixture(home, "controller-game/xinput1_3.dll", "game-owned-xinput");
        vendor_dll = join(home, "controller-game/xinput1_3.dll");
        managed_xinput = join(home, "controller-game/xinput1_1.dll");
        managed_dinput = join(home, "controller-game/dinput8.dll");
        shim_manifest = join(home, "controller-game/.metalsharp/input-shims.json");
        game_dir = join(home, "controller-game");
        fixture(home, "configs/config.json", "{\"controllerInput\":\"x\"}");
        ms_steam_deploy_controller_input_shims(home, game_dir);
        char* contents = read_bounded_file(managed_xinput);
        assert(contents && !strcmp(contents, "managed-xinput"));
        free(contents);
        contents = read_bounded_file(vendor_dll);
        assert(contents && !strcmp(contents, "game-owned-xinput"));
        free(contents);
        assert(access(shim_manifest, F_OK) == 0 && access(managed_dinput, F_OK) != 0);
        fixture(home, "configs/config.json", "{\"controllerInput\":\"d\"}");
        ms_steam_deploy_controller_input_shims(home, game_dir);
        assert(access(managed_xinput, F_OK) != 0 && access(managed_dinput, F_OK) == 0);
        fixture(home, "configs/config.json", "{\"controllerInput\":\"off\"}");
        ms_steam_deploy_controller_input_shims(home, game_dir);
        assert(access(managed_dinput, F_OK) != 0 && access(vendor_dll, F_OK) == 0);
        free(vendor_dll);
        free(managed_xinput);
        free(managed_dinput);
        free(shim_manifest);
        free(game_dir);
    }
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
    /* WineMetalGL is the OpenGL of every launch: WINEMETALGL is never forced to 0. */
    setenv("WINEMETALGL", "0", 1);
    set_game_opengl_env(250900, "dxmt_32");
    assert(!getenv("WINEMETALGL"));
    setenv("WINEMETALGL", "0", 1);
    set_game_opengl_env(588650, "wine_bare");
    assert(!getenv("WINEMETALGL"));
    setenv("WINEMETALGL", "0", 1);
    set_game_opengl_env(0, "dxmt");
    assert(!getenv("WINEMETALGL"));
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
        ms_json* library =
            library_json ? ms_json_parse(library_json, strlen(library_json), error, sizeof(error)) : NULL;
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
    fixture(home, "Library/Application Support/Steam/steamapps/appmanifest_3900000002.acf",
            "\"AppState\"\n{\n\t\"appid\"\t\"3900000002\"\n\t\"name\"\t\"Native Test Game\"\n"
            "\t\"installdir\"\t\"Native Test Game\"\n}\n");
    fixture(
        home,
        "Library/Application Support/Steam/steamapps/common/Native Test Game/Native Test Game.app/Contents/Info.plist",
        "native app bundle");
    {
        char* resolved_native_app = ms_steam_native_app_path(home, 3900000002U);
        assert(resolved_native_app && strstr(resolved_native_app, "Native Test Game.app"));
        free(resolved_native_app);
        char* library_json = ms_steam_library_json(home);
        char error[96];
        ms_json* library =
            library_json ? ms_json_parse(library_json, strlen(library_json), error, sizeof(error)) : NULL;
        const ms_json* games = library ? ms_json_object_get(library, "games") : NULL;
        bool found_native = false;
        for (size_t i = 0; games && i < ms_json_array_length(games); i++) {
            const ms_json* game = ms_json_array_get(games, i);
            long long appid;
            bool native_build = false;
            char *launch_method = NULL, *native_app_path = NULL;
            if (!ms_json_as_i64(ms_json_object_get(game, "appid"), &appid) || appid != 3900000002LL)
                continue;
            native_build = ms_json_as_bool(ms_json_object_get(game, "has_native_build"), &native_build) && native_build;
            assert(native_build);
            assert(ms_json_as_string(ms_json_object_get(game, "launch_method"), &launch_method));
            assert(!strcmp(launch_method, "mac_steam"));
            assert(ms_json_as_string(ms_json_object_get(game, "native_app_path"), &native_app_path));
            assert(strstr(native_app_path, "Native Test Game.app"));
            assert(ms_json_array_length(ms_json_object_get(game, "available_pipelines")) == 0);
            found_native = true;
            free(launch_method);
            free(native_app_path);
        }
        assert(found_native);
        ms_json_free(library);
        free(library_json);
    }
    {
        const char* request = "{\"excludeNativeMacSteamGames\":true}";
        int status = 0;
        char* config_json = ms_config_set_json(home, (const unsigned char*)request, strlen(request), &status);
        assert(status == 200 && config_json);
        free(config_json);
        char* library_json = ms_steam_library_json(home);
        char error[96];
        ms_json* library =
            library_json ? ms_json_parse(library_json, strlen(library_json), error, sizeof(error)) : NULL;
        const ms_json* games = library ? ms_json_object_get(library, "games") : NULL;
        for (size_t i = 0; games && i < ms_json_array_length(games); i++) {
            const ms_json* game = ms_json_array_get(games, i);
            long long appid;
            assert(!ms_json_as_i64(ms_json_object_get(game, "appid"), &appid) || appid != 3900000002LL);
        }
        ms_json_free(library);
        free(library_json);
    }
    {
        const char* request = "{\"windowMode\":\"windowed\",\"gameResolution\":\"2560x1440\",\"msync\":false}";
        int status = 0;
        char error[96];
        char* config_json = ms_config_set_json(home, (const unsigned char*)request, strlen(request), &status);
        ms_json* config = config_json ? ms_json_parse(config_json, strlen(config_json), error, sizeof(error)) : NULL;
        char *mode = NULL, *resolution = NULL;
        assert(status == 200 && config_json && config);
        assert(ms_json_as_string(ms_json_object_get(config, "windowMode"), &mode) && !strcmp(mode, "windowed"));
        assert(ms_json_as_string(ms_json_object_get(config, "gameResolution"), &resolution) &&
               !strcmp(resolution, "2560x1440"));
        free(mode);
        free(resolution);
        ms_json_free(config);
        free(config_json);
    }
    free(drive_e_link);
    free(drive_c_link);
    free(dosdevices);
    puts("runtime routing regressions passed");
    return 0;
}
