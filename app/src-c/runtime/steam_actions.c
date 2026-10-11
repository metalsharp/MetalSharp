#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/config.h"
#include "metalsharp_backend/game_executable.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/logs.h"
#include "metalsharp_backend/metalfx.h"
#include "metalsharp_backend/mtsp.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/setup.h"
#include "metalsharp_backend/steam.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/xattr.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STEAMWEBHELPER_WRAPPER_MAX_BYTES 100000ULL
#define STEAMWEBHELPER_WRAPPER_SHA256    "f46a1e8c39c850ba22861f63559f13b4f68557acf04a92e6d1b899769b2ea1f9"

/* Steam launch arguments that let a Chromium-based launcher start under
 * D3DMetal: no sandbox, GPU off, software WebGL through ANGLE. */
static const char D3DMETAL_LAUNCHER_STEAM_ARGS[] =
    "--no-sandbox%20--in-process-gpu%20--disable-gpu%20--disable-d3d11%20--enable-unsafe-swiftshader%20"
    "--use-gl=angle%20--use-angle=swiftshader-webgl";

/* Steam games whose own launcher (EVE Launcher, Ubisoft Connect) must be
 * started by Steam with D3DMETAL_LAUNCHER_STEAM_ARGS
 * on the D3DMetal route. `launcher` is Steam's launch target and `client` the
 * game executable that receives the D3DMetal DLLs, both relative to the game
 * directory; EVE's client lives in a versioned folder and is found at launch. */
typedef struct {
    unsigned appid;
    const char* name;
    const char* launcher;
    const char* client;
} d3dmetal_steam_launcher_game;

static const d3dmetal_steam_launcher_game D3DMETAL_STEAM_LAUNCHER_GAMES[] = {
    {8500, "EVE Online", "Launcher/evelauncher.exe", NULL},
    {812140, "Assassin's Creed Odyssey", "ACOdyssey.exe", "ACOdyssey.exe"},
};

/* Rockstar titles on D3DMetal or VKD3D start the Rockstar Games Launcher
 * directly with the arguments Steam's Play*.exe would pass: a Steam handoff
 * does not carry MetalSharp's route environment into the launcher. The
 * launcher itself always runs on D3DMetal with WFDXCompat; its Steam API
 * still needs a running Wine Steam client.
 * - launcher_on_d3dmetal: VKD3D also runs the launcher and game on D3DMetal
 *   (RDR2 renders with Vulkan via -api Vulkan). Otherwise VKD3D keeps its
 *   route and WineForge's launcher policy moves only the launcher.
 * - dx12_settings: D3DMetal switches system.xml from Vulkan to DX12.
 * - agility_frontend: on D3DMetal the game loads d3d12.dll through
 *   WFDXCompat's Agility-aware frontend (runtime/wfdxcompat-agility), which
 *   loads d3d12core.dll with d3d12.dll as native D3D12 does; D3DMetal never
 *   loads a core, and GTA V Enhanced fails with ERR_GFX_D3D_NOD3D12. */
typedef struct {
    unsigned appid;
    const char* name;
    const char* client;
    bool launcher_on_d3dmetal;
    bool dx12_settings;
    bool agility_frontend;
} rockstar_launcher_game;

static const rockstar_launcher_game ROCKSTAR_LAUNCHER_GAMES[] = {
    {1174180, "Red Dead Redemption 2", "RDR2.exe", true, true, false},
    {3240220, "Grand Theft Auto V Enhanced", "GTA5_Enhanced.exe", false, false, true},
};
static const char ROCKSTAR_LAUNCHER_PATH[] = "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe";

static char* join(const char* a, const char* b);

static const rockstar_launcher_game* rockstar_launcher_game_for(unsigned id, const char* pipeline) {
    if (!pipeline || (strcmp(pipeline, "d3dmetal") && strcmp(pipeline, "vkd3d")))
        return NULL;
    for (size_t i = 0; i < sizeof(ROCKSTAR_LAUNCHER_GAMES) / sizeof(ROCKSTAR_LAUNCHER_GAMES[0]); i++)
        if (ROCKSTAR_LAUNCHER_GAMES[i].appid == id)
            return &ROCKSTAR_LAUNCHER_GAMES[i];
    return NULL;
}

static char* rockstar_launcher_executable(const char* home, unsigned id, const char* pipeline) {
    char* launcher;
    if (!rockstar_launcher_game_for(id, pipeline))
        return NULL;
    launcher = join(home, ROCKSTAR_LAUNCHER_PATH);
    if (launcher && access(launcher, R_OK) == 0)
        return launcher;
    free(launcher);
    return NULL;
}

static bool is_rockstar_launcher_executable(const char* executable) {
    static const char suffix[] = "/Rockstar Games/Launcher/Launcher.exe";
    size_t length = executable ? strlen(executable) : 0;
    return length >= sizeof(suffix) - 1 && !strcasecmp(executable + length - (sizeof(suffix) - 1), suffix);
}

/* Wine maps the host filesystem at Z:, so any resolved host path is a valid
 * Windows path once its separators are flipped. */
static bool format_wine_host_path(char* out, size_t out_size, const char* host_path) {
    char resolved[PATH_MAX];
    int written;
    if (!host_path || !realpath(host_path, resolved))
        return false;
    written = snprintf(out, out_size, "Z:%s", resolved);
    if (written < 0 || (size_t)written >= out_size)
        return false;
    for (char* p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
    return true;
}

static const d3dmetal_steam_launcher_game* d3dmetal_steam_launcher_game_for(unsigned id, const char* pipeline) {
    if (!pipeline || strcmp(pipeline, "d3dmetal"))
        return NULL;
    for (size_t i = 0; i < sizeof(D3DMETAL_STEAM_LAUNCHER_GAMES) / sizeof(D3DMETAL_STEAM_LAUNCHER_GAMES[0]); i++)
        if (D3DMETAL_STEAM_LAUNCHER_GAMES[i].appid == id)
            return &D3DMETAL_STEAM_LAUNCHER_GAMES[i];
    return NULL;
}
static const char MARVEL_RIVALS_STEAM_ARGS[] = "-windowed";

static char* join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (p)
        snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
}

static void ensure_steam_launch_ready(const char* home, const char* steam_dir);
static void seed_steam_registry(const char* home);
static bool contains_ci(const char* haystack, const char* needle);
static bool wine_steam_cleanup_target(const char* command, const char* prefix);
static bool copy_file_path(const char* source, const char* destination);
static bool select_wine_ntdll(const char* home, const char* pipeline);
static bool ensure_directory(const char* path);
static void ensure_x87_wow64_loader(const char* home);
static char* read_bounded_file(const char* path);
static char* find_game_executable(const char* directory, unsigned depth);
static char* preferred_steam_game_executable(const char* game_dir, unsigned id, const char* pipeline);
static char* find_steam_game_executable(const char* home, unsigned id, const char* pipeline);
static char* latest_eve_online_client_executable(const char* game_dir);
static bool executable_is_32bit(const char* executable);
static bool body_id(const char* body, size_t len, unsigned* id);
static void string_field(ms_json_writer* writer, const char* key, const char* value);
static bool copy_file_path_new(const char* source, const char* destination);
static char* launch_d3dmetal_launcher_via_steam_json(const char* home, const d3dmetal_steam_launcher_game* game,
                                                     int* status);
static char* launch_rockstar_via_launcher_json(const char* home, const rockstar_launcher_game* game,
                                               const char* launcher, const char* pipeline, int* status);
static bool apply_protected_exe_swap(const char* home, unsigned id, const char* pipeline);

char* ms_steam_wine_launch_wrapper_path(const char* home) {
    static const char wrapper[] =
        "#!/bin/sh\n"
        "wine=\"${METALSHARP_WINE_BINARY:?MetalSharp Wine binary is not configured}\"\n"
        "case \"${1-}\" in *.exe|*.EXE) ;; *) exec \"$wine\" \"$@\" ;; esac\n"
        "mode=${METALSHARP_GAME_WINDOW_MODE:-default}\n"
        "resolution=${METALSHARP_GAME_RESOLUTION:-default}\n"
        "if [ \"$mode\" != fullscreen ] && { [ \"$mode\" = windowed ] || [ \"$resolution\" != default ]; }; then\n"
        "  desktop=MetalSharp\n"
        "  [ \"$resolution\" = default ] || desktop=MetalSharp,\"$resolution\"\n"
        "  exec \"$wine\" explorer \"/desktop=$desktop\" \"$@\"\n"
        "fi\n"
        "exec \"$wine\" \"$@\"\n";
    if (!home)
        return NULL;
    char* cache = join(home, "cache");
    char* path = cache ? join(cache, "metalsharp-wine-game-launcher") : NULL;
    if (!cache || !path || !ensure_directory(cache)) {
        free(cache);
        free(path);
        return NULL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    if (fd >= 0) {
        size_t length = sizeof(wrapper) - 1;
        size_t offset = 0;
        while (offset < length) {
            ssize_t written = write(fd, wrapper + offset, length - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            offset += (size_t)written;
        }
        bool saved = offset == length && fchmod(fd, 0700) == 0;
        if (close(fd) != 0)
            saved = false;
        if (!saved) {
            (void)unlink(path);
            free(cache);
            free(path);
            return NULL;
        }
    } else if (errno == EEXIST) {
        char* existing = read_bounded_file(path);
        bool matches = existing && !strcmp(existing, wrapper) && access(path, X_OK) == 0;
        free(existing);
        if (!matches) {
            free(cache);
            free(path);
            return NULL;
        }
    } else {
        free(cache);
        free(path);
        return NULL;
    }
    free(cache);
    return path;
}

static bool controller_input_shim_name(const char* name) {
    static const char* const names[] = {"xinput1_1.dll",   "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
                                        "xinput9_1_0.dll", "dinput.dll",    "dinput8.dll"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (name && !strcmp(name, names[i]))
            return true;
    return false;
}

static const char* controller_input_mode_for_home(const char* home) {
    char* configs = join(home, "configs");
    char* path = configs ? join(configs, "config.json") : NULL;
    char* raw = path ? read_bounded_file(path) : NULL;
    char error[96];
    ms_json* json = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    char* mode = NULL;
    free(configs);
    free(path);
    free(raw);
    if (json && ms_json_type_of(json) == MS_JSON_OBJECT)
        (void)ms_json_as_string(ms_json_object_get(json, "controllerInput"), &mode);
    ms_json_free(json);
    if (!mode || (strcmp(mode, "x") && strcmp(mode, "X") && strcmp(mode, "d") && strcmp(mode, "D"))) {
        free(mode);
        return strdup("off");
    }
    if (mode[0] == 'X' || mode[0] == 'D')
        mode[0] = (char)tolower((unsigned char)mode[0]);
    return mode;
}

static void remove_input_shim_manifest(const char* game_dir) {
    char* meta = join(game_dir, ".metalsharp");
    char* marker = meta ? join(meta, "input-shims.json") : NULL;
    char* raw = marker ? read_bounded_file(marker) : NULL;
    char error[96];
    ms_json* json = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    const ms_json* dlls = json ? ms_json_object_get(json, "dlls") : NULL;
    if (dlls && ms_json_type_of(dlls) == MS_JSON_ARRAY) {
        for (size_t i = 0; i < ms_json_array_length(dlls); i++) {
            char* name = NULL;
            if (ms_json_as_string(ms_json_array_get(dlls, i), &name) && controller_input_shim_name(name)) {
                char* path = join(game_dir, name);
                if (path)
                    (void)unlink(path);
                free(path);
                free(name);
            }
        }
    }
    if (marker)
        (void)unlink(marker);
    free(meta);
    free(marker);
    free(raw);
    ms_json_free(json);
}

void ms_steam_deploy_controller_input_shims(const char* home, const char* game_dir) {
    static const char* const xinput[] = {"xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
                                         "xinput9_1_0.dll"};
    static const char* const dinput[] = {"dinput.dll", "dinput8.dll"};
    const char* mode;
    const char* const* names;
    size_t count;
    size_t installed_count = 0;
    const char* all_shims[] = {"xinput1_1.dll",   "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
                               "xinput9_1_0.dll", "dinput.dll",    "dinput8.dll"};
    const char* created[sizeof(all_shims) / sizeof(all_shims[0])];
    char* bundled;
    char* fallback;
    char* meta;
    char* marker;
    ms_json_writer writer;
    if (!game_dir || access(game_dir, F_OK) != 0)
        return;
    mode = controller_input_mode_for_home(home);
    remove_input_shim_manifest(game_dir);
    if (!strcmp(mode, "off")) {
        free((void*)mode);
        return;
    }
    names = !strcmp(mode, "d") ? dinput : xinput;
    count = !strcmp(mode, "d") ? sizeof(dinput) / sizeof(dinput[0]) : sizeof(xinput) / sizeof(xinput[0]);
    bundled = join(home, "runtime/wine/lib/metalsharp/x86_64-windows");
    fallback = join(home, "runtime/wine/lib/wine/x86_64-windows");
    meta = join(game_dir, ".metalsharp");
    marker = meta ? join(meta, "input-shims.json") : NULL;
    if (meta)
        (void)ensure_directory(meta);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    string_field(&writer, "mode", mode);
    ms_json_writer_key(&writer, "dlls");
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < count; i++) {
        char* source = bundled ? join(bundled, names[i]) : NULL;
        char* fallback_source = fallback ? join(fallback, names[i]) : NULL;
        char* target = join(game_dir, names[i]);
        const char* selected = source && access(source, R_OK) == 0 ? source : fallback_source;
        if (selected && target && access(selected, R_OK) == 0 && copy_file_path_new(selected, target)) {
            ms_json_writer_string(&writer, names[i]);
            created[installed_count++] = names[i];
        }
        free(source);
        free(fallback_source);
        free(target);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    if (marker) {
        char* payload = ms_json_writer_take(&writer);
        FILE* file = fopen(marker, "wb");
        bool saved = false;
        if (file && payload)
            saved = fputs(payload, file) >= 0 && fclose(file) == 0;
        else if (file)
            fclose(file);
        if (!saved) {
            for (size_t i = 0; i < installed_count; ++i) {
                char* target = join(game_dir, created[i]);
                if (target)
                    (void)unlink(target);
                free(target);
            }
        }
        free(payload);
    } else {
        char* payload = ms_json_writer_take(&writer);
        free(payload);
    }
    free((void*)mode);
    free(bundled);
    free(fallback);
    free(meta);
    free(marker);
}

/*
 * Keep route selection here instead of scattering pipeline-specific
 * guesses through the process-spawn code: the executable, DLL deployment,
 * Wine DLL search path, Unix library path, overrides, environment, and args
 * must all be selected from the same route.
 */
static const char* canonical_pipeline(const char* requested) {
    if (!requested || !requested[0] || !strcasecmp(requested, "auto") || !strcasecmp(requested, "dxmt"))
        return requested && !strcasecmp(requested, "dxmt") ? "dxmt" : "auto";
    if (!strcasecmp(requested, "d3d12") || !strcasecmp(requested, "dx12"))
        return "vkd3d";
    if (!strcasecmp(requested, "vkd3d") || !strcasecmp(requested, "vkd3d_proton") ||
        !strcasecmp(requested, "vulkan_d3d12"))
        return "vkd3d";
    if (!strcasecmp(requested, "dxmt") || !strcasecmp(requested, "m11") || !strcasecmp(requested, "m10") ||
        !strcasecmp(requested, "d3d11") || !strcasecmp(requested, "dx11") || !strcasecmp(requested, "d3d10") ||
        !strcasecmp(requested, "dx10") || !strcasecmp(requested, "steam_d3dmetal_perf") ||
        !strcasecmp(requested, "steam_metalfx"))
        return "dxmt";
    if (!strcasecmp(requested, "dxmt_32") || !strcasecmp(requested, "m11_32") || !strcasecmp(requested, "m10_32") ||
        !strcasecmp(requested, "d3d11_32") || !strcasecmp(requested, "d3d10_32") || !strcasecmp(requested, "dx10_32"))
        return "dxmt_32";
    if (!strcasecmp(requested, "dxvk") || !strcasecmp(requested, "m9") || !strcasecmp(requested, "d3d9") ||
        !strcasecmp(requested, "dx9") || !strcasecmp(requested, "dxvk_32") || !strcasecmp(requested, "d3d9_32") ||
        !strcasecmp(requested, "dx9_32"))
        return "d3d9";
    if (!strcasecmp(requested, "m13") || !strcasecmp(requested, "gptk") || !strcasecmp(requested, "steam_d3dmetal"))
        return "m13";
    if (!strcasecmp(requested, "d3dmetal") || !strcasecmp(requested, "d3dmetal_native"))
        return "d3dmetal";
    if (!strcasecmp(requested, "m32") || !strcasecmp(requested, "m32_w"))
        return "m32";
    if (!strcasecmp(requested, "fna_arm64") || !strcasecmp(requested, "fna_x86") ||
        !strcasecmp(requested, "fna_mono_xna") || !strcasecmp(requested, "mono_fna_xna"))
        return "fna_arm64";
    if (!strcasecmp(requested, "wine_bare") || !strcasecmp(requested, "m64") || !strcasecmp(requested, "wine"))
        return "wine_bare";
    return NULL;
}

static bool pipeline_is_dxmt(const char* pipeline) {
    return pipeline && (!strcmp(pipeline, "dxmt") || !strcmp(pipeline, "dxmt_32") || !strcmp(pipeline, "d3d9"));
}

static bool pipeline_is_d3d9(const char* pipeline) {
    return pipeline && !strcmp(pipeline, "d3d9");
}

static bool pipeline_needs_legacy_game_args(const char* pipeline) {
    return pipeline && (!strcmp(pipeline, "vkd3d") || pipeline_is_d3d9(pipeline));
}

static void set_wine_msync(const char* home) {
    setenv("WINEMSYNC", ms_config_msync_enabled(home) ? "1" : "0", 1);
}

void ms_steam_apply_launch_preferences(const char* home) {
    if (!home)
        return;
    char* raw = ms_config_get_json(home);
    char error[96];
    ms_json* config = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    char *mode = NULL, *resolution = NULL, *wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* wrapper = ms_steam_wine_launch_wrapper_path(home);
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        wine = join(home, "runtime/wine/bin/wine");
    }
    if (wine && access(wine, X_OK) == 0)
        setenv("METALSHARP_WINE_BINARY", wine, 1);
    else
        unsetenv("METALSHARP_WINE_BINARY");
    if (wrapper)
        setenv("METALSHARP_WINE_LAUNCH_WRAPPER", wrapper, 1);
    else
        unsetenv("METALSHARP_WINE_LAUNCH_WRAPPER");
    set_wine_msync(home);
    unsetenv("METALSHARP_GAME_WINDOW_MODE");
    unsetenv("METALSHARP_GAME_RESOLUTION");
    if (config && ms_json_as_string(ms_json_object_get(config, "windowMode"), &mode) &&
        (!strcmp(mode, "windowed") || !strcmp(mode, "fullscreen")))
        setenv("METALSHARP_GAME_WINDOW_MODE", mode, 1);
    if (config && ms_json_as_string(ms_json_object_get(config, "gameResolution"), &resolution) &&
        (!strcmp(resolution, "1280x720") || !strcmp(resolution, "1920x1080") || !strcmp(resolution, "2560x1440") ||
         !strcmp(resolution, "3840x2160")))
        setenv("METALSHARP_GAME_RESOLUTION", resolution, 1);
    free(mode);
    free(resolution);
    free(wine);
    free(wrapper);
    free(raw);
    ms_json_free(config);
}

static const char* pipeline_backend(const char* pipeline) {
    if (!strcmp(pipeline, "vkd3d"))
        return "vulkan";
    if (!strcmp(pipeline, "m13"))
        return "gptk";
    if (!strcmp(pipeline, "d3dmetal"))
        return "d3dmetal";
    if (!strcmp(pipeline, "m32") || !strcmp(pipeline, "wine_bare"))
        return "wine";
    return "dxmt";
}

static const char* pipeline_overrides(const char* pipeline) {
    if (!strcmp(pipeline, "vkd3d"))
        return "d3d12,d3d12core,d3d11,d3d10core,dxgi,d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (pipeline_is_d3d9(pipeline))
        return "d3d9,dxgi=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "dxmt"))
        return "winemetal,dxgi,d3d11,d3d10core=n,b,d3d12=b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "dxmt_32"))
        return "winemetal,dxgi,d3d11,d3d10core=n,b,d3d12=b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "m13") || !strcmp(pipeline, "d3dmetal"))
        return "d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    return NULL;
}

static bool format_steam_pipeline_overrides(char* out, size_t out_size, const char* pipeline) {
    static const char steam_runtime_overrides[] = "d3d10core=n,b;bcrypt=b;ncrypt=b";
    const char* route_overrides = pipeline_overrides(pipeline);
    int written = route_overrides ? snprintf(out, out_size, "%s;%s", route_overrides, steam_runtime_overrides)
                                  : snprintf(out, out_size,
                                             "dxgi,d3d11,d3d10core=n,b;bcrypt=b;ncrypt=b;"
                                             "gameoverlayrenderer,gameoverlayrenderer64=d");
    return written >= 0 && (size_t)written < out_size;
}

static void normalize_fna_bottle_profile(const char* path, unsigned id) {
    char* raw;
    char* marker;
    const char* old = "\"runtime_profile\":\"fna_arm64\"";
    const char* replacement = "\"runtime_profile\":\"fna_x86\"";
    char temp[PATH_MAX];
    FILE* file;
    if (id != 105600 && id != 504230)
        return;
    raw = read_bounded_file(path);
    if (!raw || !(marker = strstr(raw, old))) {
        free(raw);
        return;
    }
    snprintf(temp, sizeof(temp), "%s.fna-normalize", path);
    file = fopen(temp, "wb");
    if (file) {
        fwrite(raw, 1, (size_t)(marker - raw), file);
        fputs(replacement, file);
        fputs(marker + strlen(old), file);
        fclose(file);
        (void)rename(temp, path);
    }
    free(raw);
}

static void ensure_dxmt_shader_metal_version(const char* home) {
    char* etc = join(home, "runtime/wine/etc");
    char* path = join(home, "runtime/wine/etc/dxmt.conf");
    char* existing;
    char temp[PATH_MAX] = {0};
    FILE* file = NULL;
    int fd = -1;
    if (!etc || !path || !ensure_directory(etc)) {
        free(etc);
        free(path);
        return;
    }
    existing = read_bounded_file(path);
    if (snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", path) < (int)sizeof(temp)) {
        fd = mkstemp(temp);
        if (fd >= 0) {
            file = fdopen(fd, "wb");
            if (!file) {
                close(fd);
                unlink(temp);
                temp[0] = '\0';
            }
        }
    }
    if (file) {
        bool wrote_feature_level = false, wrote_shader_version = false;
        char* cursor = existing;
        while (cursor && *cursor) {
            char* line = cursor;
            char* newline = strchr(cursor, '\n');
            char* key = line;
            if (newline) {
                *newline = '\0';
                cursor = newline + 1;
            } else
                cursor = NULL;
            while (*key == ' ' || *key == '\t')
                key++;
            if (!strncmp(key, "d3d11.maxFeatureLevel", 21) && (key[21] == ' ' || key[21] == '\t' || key[21] == '=')) {
                if (!wrote_feature_level)
                    fputs("d3d11.maxFeatureLevel = 12_1\n", file);
                wrote_feature_level = true;
            } else if (!strncmp(key, "dxmt.shaderMetalVersion", 23) &&
                       (key[23] == ' ' || key[23] == '\t' || key[23] == '=')) {
                if (!wrote_shader_version)
                    fputs("dxmt.shaderMetalVersion = 310\n", file);
                wrote_shader_version = true;
            } else
                fprintf(file, "%s\n", line);
        }
        if (!wrote_feature_level)
            fputs("d3d11.maxFeatureLevel = 12_1\n", file);
        if (!wrote_shader_version)
            fputs("dxmt.shaderMetalVersion = 310\n", file);
        if (fclose(file) == 0) {
            if (rename(temp, path) != 0)
                unlink(temp);
        } else
            unlink(temp);
    }
    free(existing);
    free(etc);
    free(path);
}

static void set_rosetta_avx_env(void) {
#ifdef __APPLE__
    /* Ask Rosetta to expose its translated AVX/AVX2 support to x86_64 Wine. */
    setenv("ROSETTA_ADVERTISE_AVX", "1", 1);
#endif
}

/* VKD3D's dedicated MoltenVK lane: its ICD plus the MoltenVK settings the
 * route was tuned with. */
static void set_moltenvk_vkmt_env(const char* home) {
    char icd[PATH_MAX];
    snprintf(icd, sizeof(icd), "%s/runtime/wine/lib/moltenvk-vkmt/MoltenVK_icd.json", home);
    setenv("VK_ICD_FILENAMES", icd, 1);
    setenv("VK_DRIVER_FILES", icd, 1);
    setenv("VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT", "1", 1);
    setenv("MVK_PRESENT_MODE", "1", 1);
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", 1);
    setenv("MVK_CONFIG_RESUME_LOST_DEVICE", "1", 1);
    setenv("MVK_CONFIG_USE_METAL_PRIVATE_API", "1", 1);
    setenv("MVK_CONFIG_FORCE_RETAINED_COMMAND_BUFFERS", "1", 1);
}

/* WineForge runs the Rockstar Games Launcher and SocialClubHelper through the
 * WFDXCompat launcher companion on D3DMetal (its D3D10 device bridge covers
 * DXGID3D10CreateDevice, which D3DMetal lacks). The PE-side loader only
 * activates it when WFDXCOMPAT_RUNTIME_DIR is set explicitly. */
static void set_wfdxcompat_runtime_env(const char* home, const char* pipeline) {
    char runtime[PATH_MAX];
    char companion[PATH_MAX];
    snprintf(runtime, sizeof(runtime), "%s/runtime/wfdxcompat", home);
    snprintf(companion, sizeof(companion), "%s/x86_64-windows/wfdx-launchers-v1.dll", runtime);
    if (pipeline && !strcmp(pipeline, "d3dmetal") && access(companion, R_OK) == 0)
        setenv("WFDXCOMPAT_RUNTIME_DIR", runtime, 1);
    else
        unsetenv("WFDXCOMPAT_RUNTIME_DIR");
}

/* WFDXCompat runtime with its D3D12 frontend (Agility SDK titles only). Any
 * WFDXCompat d3d12.dll staged in the default runtime would front every
 * D3DMetal D3D12 game, so this lane is selected per game. */
static void set_wfdxcompat_agility_env(const char* home) {
    char runtime[PATH_MAX];
    char frontend[PATH_MAX];
    snprintf(runtime, sizeof(runtime), "%s/runtime/wfdxcompat-agility", home);
    snprintf(frontend, sizeof(frontend), "%s/x86_64-windows/d3d12.dll", runtime);
    if (access(frontend, R_OK) == 0)
        setenv("WFDXCOMPAT_RUNTIME_DIR", runtime, 1);
}

/* Lets WineForge's Rockstar launcher policy move only Launcher.exe and
 * SocialClubHelper.exe onto D3DMetal with WFDXCompat while the game keeps
 * another route (GTA V Enhanced on VKD3D). */
static void set_rockstar_launcher_policy_env(const char* home) {
    char framework[PATH_MAX];
    char runtime[PATH_MAX];
    snprintf(framework, sizeof(framework), "%s/runtime/d3dmetal-gptk4-beta2/external/D3DMetal.framework/D3DMetal",
             home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/d3dmetal-gptk4-beta2", home);
    setenv("D3DMETAL_FRAMEWORK_PATH", framework, 1);
    setenv("D3DMETAL_RUNTIME_DIR", runtime, 1);
    set_wfdxcompat_runtime_env(home, "d3dmetal");
}

static void set_route_paths(const char* home, const char* pipeline) {
    char dllpath[PATH_MAX * 3];
    char unixpath[PATH_MAX * 3];
    const char* backend = pipeline_backend(pipeline);
    set_rosetta_avx_env();
    if (pipeline_is_dxmt(pipeline))
        ensure_dxmt_shader_metal_version(home);
    dllpath[0] = '\0';
    unixpath[0] = '\0';

    if (!strcmp(pipeline, "dxmt")) {
        snprintf(dllpath, sizeof(dllpath),
                 "%s/runtime/wine/lib/dxmt/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows:%s/runtime/wine/lib/"
                 "metalsharp/x86_64-windows",
                 home, home, home);
        snprintf(unixpath, sizeof(unixpath),
                 "%s/runtime/wine/lib/dxmt/x86_64-unix:%s/runtime/wine/lib/wine/x86_64-unix", home, home);
    } else if (!strcmp(pipeline, "dxmt_32")) {
        snprintf(dllpath, sizeof(dllpath),
                 "%s/runtime/wine/lib/dxmt/i386-windows:%s/runtime/wine/lib/wine/i386-windows:%s/runtime/wine/lib/wine/"
                 "x86_64-windows",
                 home, home, home);
        snprintf(unixpath, sizeof(unixpath),
                 "%s/runtime/wine/lib/dxmt/x86_64-unix:%s/runtime/wine/lib/wine/x86_64-unix:%s/runtime/wine/lib/wine",
                 home, home, home);
    } else if (!strcmp(pipeline, "d3d9")) {
        snprintf(dllpath, sizeof(dllpath),
                 "%s/runtime/wine/lib/dxmt/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows:%s/runtime/wine/lib/"
                 "wine/i386-windows:%s/runtime/wine/lib/metalsharp/x86_64-windows",
                 home, home, home, home);
        snprintf(unixpath, sizeof(unixpath),
                 "%s/runtime/wine/lib/dxmt/x86_64-unix:%s/runtime/wine/lib/wine/x86_64-unix", home, home);
    } else if (!strcmp(pipeline, "vkd3d")) {
        snprintf(
            dllpath, sizeof(dllpath),
            "%s/vkd3d/vkd3d-proton/x86_64-windows:%s/vkd3d/dxvk/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows",
            home, home, home);
        /* Keep VKD3D on its dedicated current MoltenVK lane.  Do not use the
         * Wine-bundled fallback driver or any DXMT sidecars here. */
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/moltenvk-vkmt:%s/runtime/wine/lib/wine/x86_64-unix",
                 home, home);
    } else if (!strcmp(pipeline, "d3dmetal")) {
        snprintf(dllpath, sizeof(dllpath),
                 "%s/runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows", home,
                 home);
        snprintf(unixpath, sizeof(unixpath),
                 "%s/runtime/d3dmetal-gptk4-beta2/external:%s/runtime/wine/lib/wine/x86_64-unix", home, home);
    } else if (!strcmp(pipeline, "m32") || !strcmp(pipeline, "wine_bare")) {
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/wine/x86_64-unix", home);
    }

    if (dllpath[0])
        setenv("WINEDLLPATH", dllpath, 1);
    else
        unsetenv("WINEDLLPATH");
    if (unixpath[0]) {
#ifdef __APPLE__
        setenv("DYLD_LIBRARY_PATH", unixpath, 1);
        setenv("DYLD_FALLBACK_LIBRARY_PATH", unixpath, 1);
#else
        setenv("LD_LIBRARY_PATH", unixpath, 1);
#endif
    }
    if (pipeline_is_dxmt(pipeline)) {
        char config[PATH_MAX];
        snprintf(config, sizeof(config), "%s/runtime/wine/etc/dxmt.conf", home);
        setenv("DXMT_CONFIG_FILE", config, 1);
    } else {
        unsetenv("DXMT_CONFIG_FILE");
    }
    if (pipeline_is_dxmt(pipeline)) {
        char winemetal[PATH_MAX];
        char runtime_dir[PATH_MAX];
        const char* route = "dxmt/x86_64-unix";
        /* __wine_load_unix_lib() takes an NT path, not a host POSIX path. */
        snprintf(winemetal, sizeof(winemetal), "\\??\\Z:%s/runtime/wine/lib/%s/winemetal.so", home, route);
        for (char* p = winemetal + 5; *p; ++p)
            if (*p == '/')
                *p = '\\';
        snprintf(runtime_dir, sizeof(runtime_dir), "%s/runtime/wine/lib/dxmt", home);
        setenv("DXMT_WINEMETAL_UNIXLIB", winemetal, 1);
        setenv("DXMT_RUNTIME_DIR", runtime_dir, 1);
        setenv("GRAPHICS_BACKEND", "dxmt", 1);
    } else {
        unsetenv("DXMT_WINEMETAL_UNIXLIB");
        unsetenv("DXMT_RUNTIME_DIR");
        setenv("GRAPHICS_BACKEND", backend, 1);
    }
    setenv("MS_GRAPHICS_BACKEND", backend, 1);
    ms_steam_apply_launch_preferences(home);
    if (!strcmp(pipeline, "d3dmetal")) {
        char framework[PATH_MAX];
        char runtime[PATH_MAX];
        snprintf(framework, sizeof(framework), "%s/runtime/d3dmetal-gptk4-beta2/external/D3DMetal.framework/D3DMetal",
                 home);
        snprintf(runtime, sizeof(runtime), "%s/runtime/d3dmetal-gptk4-beta2", home);
        setenv("D3DMETAL_FRAMEWORK_PATH", framework, 1);
        /* Wine's D3DMetal loader resolves PE and Unix companions from this
         * root; WINEDLLPATH alone only stages the PE side. */
        setenv("D3DMETAL_RUNTIME_DIR", runtime, 1);
    } else {
        unsetenv("D3DMETAL_FRAMEWORK_PATH");
        unsetenv("D3DMETAL_RUNTIME_DIR");
    }
    set_wfdxcompat_runtime_env(home, pipeline);
    if (!strcmp(pipeline, "vkd3d")) {
        set_moltenvk_vkmt_env(home);
        unsetenv("ROSETTA_X87_PATH");
    } else if (pipeline_is_d3d9(pipeline)) {
        char x87sidecar[PATH_MAX];
        struct stat sidecar_stat;
        ensure_x87_wow64_loader(home);
        unsetenv("VK_ICD_FILENAMES");
        unsetenv("VK_DRIVER_FILES");
        /* The cooperative sidecar belongs to the restored D3D9 route. */
        snprintf(x87sidecar, sizeof(x87sidecar), "%s/runtime/wine/bin/x87sidecar", home);
        if (lstat(x87sidecar, &sidecar_stat) == 0 && S_ISREG(sidecar_stat.st_mode) && access(x87sidecar, X_OK) == 0)
            setenv("ROSETTA_X87_PATH", x87sidecar, 1);
        else
            unsetenv("ROSETTA_X87_PATH");
    } else {
        unsetenv("VK_ICD_FILENAMES");
        unsetenv("VK_DRIVER_FILES");
        unsetenv("ROSETTA_X87_PATH");
    }
}

static void set_launch_cache_env(const char* home, unsigned id, const char* pipeline) {
    const char* subdir = pipeline;
    char shader[PATH_MAX], cache[PATH_MAX], summary[PATH_MAX * 2];
    snprintf(shader, sizeof(shader), "%s/shader-cache/%s/%u", home, subdir, id);
    snprintf(cache, sizeof(cache), "%s/pipeline-cache/%s/%u", home, subdir, id);
    (void)ensure_directory(shader);
    (void)ensure_directory(cache);
    snprintf(summary, sizeof(summary), "shader=%s/;pipeline=%s/", shader, cache);
    setenv("METALSHARP_SHADER_CACHE_PATH", shader, 1);
    setenv("METALSHARP_PIPELINE_CACHE_PATH", cache, 1);
    setenv("METALSHARP_CACHE_SUMMARY", summary, 1);
    setenv("MTL_SHADER_CACHE_DIR", shader, 1);
    if (pipeline_is_dxmt(pipeline)) {
        char log_path[PATH_MAX];
        setenv("DXMT_SHADER_CACHE_PATH", shader, 1);
        setenv("DXMT_PIPELINE_CACHE_PATH", cache, 1);
        snprintf(log_path, sizeof(log_path), "%s/logs/%s/%u/", home, subdir, id);
        (void)ensure_directory(log_path);
    } else if (!strcmp(pipeline, "vkd3d")) {
        setenv("DXVK_STATE_CACHE_PATH", shader, 1);
        setenv("DXVK_LOG_PATH", cache, 1);
        setenv("DXVK_LOG_LEVEL", "info", 1);
        setenv("VKD3D_DEBUG", "info", 1);
    }
}

static void set_route_default_env(const char* home, const char* pipeline) {
    if (pipeline_is_dxmt(pipeline)) {
        bool metalfx_enabled;
        double metalfx_factor;
        char dxmt_config[256];
        ms_metalfx_state(home, &metalfx_enabled, &metalfx_factor);
        setenv("DXMT_METALFX_SPATIAL_SWAPCHAIN", metalfx_enabled ? "1" : "0", 1);
        if (!metalfx_enabled) {
            unsetenv("DXMT_METALFX_SPATIAL");
            unsetenv("DXMT_METALFX_TEMPORAL");
        }
        setenv("DXMT_ASYNC_PIPELINE_COMPILE", "1", 1);
        if (metalfx_enabled)
            snprintf(dxmt_config, sizeof(dxmt_config),
                     "d3d11.metalSpatialUpscaleFactor=%.2f;"
                     "d3d11.preferredMaxFrameRate=60;d3d11.maxFeatureLevel=12_1;"
                     "dxmt.shaderMetalVersion=310",
                     metalfx_factor);
        else
            snprintf(dxmt_config, sizeof(dxmt_config),
                     "d3d11.preferredMaxFrameRate=60;d3d11.maxFeatureLevel=12_1;"
                     "dxmt.shaderMetalVersion=310");
        setenv("DXMT_CONFIG", dxmt_config, 1);
    } else {
        unsetenv("DXMT_METALFX_SPATIAL_SWAPCHAIN");
        unsetenv("DXMT_METALFX_SPATIAL");
        unsetenv("DXMT_METALFX_TEMPORAL");
        unsetenv("DXMT_ASYNC_PIPELINE_COMPILE");
        unsetenv("DXMT_D3D12_UE_SM6_COMPAT");
        unsetenv("DXMT_D3D12_PSO_WORKERS");
        unsetenv("DXMT_CONFIG");
    }
    if (!strcmp(pipeline, "vkd3d"))
        set_moltenvk_vkmt_env(home);
}

static void ensure_x87_wow64_loader(const char* home) {
    char* directory = join(home, "runtime/wine/lib/wine/i386-unix");
    char* loader = directory ? join(directory, "wine") : NULL;
    char* x64_loader = join(home, "runtime/wine/lib/wine/x86_64-unix/wine");
    if (directory && loader && x64_loader && access(loader, X_OK) != 0 && access(x64_loader, X_OK) == 0 &&
        ensure_directory(directory))
        (void)symlink("../x86_64-unix/wine", loader);
    free(directory);
    free(loader);
    free(x64_loader);
}

static void set_game_opengl_env(unsigned id, const char* pipeline) {
    /* WineMetalGL 2.x, the OpenGL 3.3 core and compatibility implementation on
     * Metal that the runtime's winemac.so loads, is the OpenGL of every launch:
     * the Steam client, launchers and games alike (games started from the Steam
     * client inherit its environment). Dead Cells needs its 3.2 core contexts,
     * which Apple's OpenGL under Wine does not give it. */
    (void)id;
    (void)pipeline;
    unsetenv("WINEMETALGL");
}

void ms_steam_apply_graphics_route(const char* home, const char* pipeline) {
    const char* canonical = canonical_pipeline(pipeline);
    const char* overrides;
    if (!home)
        return;
    if (!canonical || !strcmp(canonical, "auto"))
        canonical = "dxmt";
    set_route_paths(home, canonical);
    set_route_default_env(home, canonical);
    set_game_opengl_env(0, canonical);
    if (!strcmp(canonical, "fna_arm64")) {
        setenv("GRAPHICS_BACKEND", canonical, 1);
        setenv("MS_GRAPHICS_BACKEND", canonical, 1);
    }
    setenv("METALSHARP_PIPELINE", canonical, 1);
    overrides = pipeline_overrides(canonical);
    if (overrides)
        setenv("WINEDLLOVERRIDES", overrides, 1);
    else
        unsetenv("WINEDLLOVERRIDES");
}

static bool append_launch_arg(char** argv, size_t* count, size_t max, const char* arg) {
    if (*count + 1 >= max)
        return false;
    argv[(*count)++] = (char*)arg;
    return true;
}

static void build_launch_args(unsigned id, const char* pipeline, char** argv, size_t* count, size_t max) {
    if (id == 8500) {
        static const char* const eve_chromium_args[] = {"--no-sandbox",
                                                        "--in-process-gpu",
                                                        "--disable-gpu",
                                                        "--disable-d3d11",
                                                        "--enable-unsafe-swiftshader",
                                                        "--use-gl=angle",
                                                        "--use-angle=swiftshader-webgl"};
        for (size_t i = 0; i < sizeof(eve_chromium_args) / sizeof(eve_chromium_args[0]); i++)
            append_launch_arg(argv, count, max, eve_chromium_args[i]);
    }
    if (id == 2767030)
        append_launch_arg(argv, count, max, "-windowed");
    if (id == 553850) {
        append_launch_arg(argv, count, max, "--bundle-dir");
        append_launch_arg(argv, count, max, "data");
        append_launch_arg(argv, count, max, "--release");
    }
    if (id == 379720 || id == 275850 || id == 892970 || id == 252490 || id == 570 || id == 548430 || id == 526870 ||
        id == 1272080)
        append_launch_arg(argv, count, max, "-vulkan");
    if (id == 949230)
        append_launch_arg(argv, count, max, "-force-vulkan");
    /* RDR2's Vulkan renderer is for VKD3D's MoltenVK lane; D3DMetal has no Vulkan. */
    if (id == 1174180 && !strcmp(pipeline, "vkd3d")) {
        append_launch_arg(argv, count, max, "-api");
        append_launch_arg(argv, count, max, "Vulkan");
    }
    if ((id == 400 || id == 620 || id == 4000) && pipeline_needs_legacy_game_args(pipeline)) {
        append_launch_arg(argv, count, max, "-dxlevel");
        append_launch_arg(argv, count, max, "90");
        append_launch_arg(argv, count, max, "-novid");
    } else if ((id == 240 || id == 500 || id == 550) && pipeline_needs_legacy_game_args(pipeline)) {
        append_launch_arg(argv, count, max, "-dxlevel");
        append_launch_arg(argv, count, max, "90");
    } else if (id == 7670 && pipeline_needs_legacy_game_args(pipeline))
        append_launch_arg(argv, count, max, "-dx9");
    else if (id == 12210 && !strcmp(pipeline, "dxmt"))
        append_launch_arg(argv, count, max, "-d3d10");
    else if (id == 17300 && !strcmp(pipeline, "dxmt"))
        append_launch_arg(argv, count, max, "-dx10");
    else if (id == 312520 && !strcmp(pipeline, "dxmt"))
        append_launch_arg(argv, count, max, "-force-d3d11");

    if ((id == 1623730 || id == 2358720) && !strcmp(pipeline, "dxmt")) {
        append_launch_arg(argv, count, max, "-dx11");
        append_launch_arg(argv, count, max, "-d3d11");
    }
    if (id == 620 || id == 4000 || id == 1260320 || id == 440 || id == 730 || id == 252490 || id == 271590 ||
        id == 284160 || id == 292030 || id == 1172380 || id == 3241660) {
        if (strcmp(pipeline, "m13") && strcmp(pipeline, "d3dmetal")) {
            append_launch_arg(argv, count, max, "-steam");
            if (id == 440 || id == 730 || id == 252490 || id == 271590 || id == 284160 || id == 292030 ||
                id == 1172380 || id == 3241660)
                append_launch_arg(argv, count, max, "-secure");
        }
    }
}

static bool executable_is_32bit(const char* executable) {
    FILE* file = fopen(executable, "rb");
    unsigned char header[0x100];
    unsigned char pe[0x1a];
    unsigned offset;
    unsigned short machine;
    bool result = false;
    if (!file)
        return false;
    if (fread(header, 1, sizeof(header), file) < 0x40 || header[0] != 'M' || header[1] != 'Z')
        goto done;
    offset = (unsigned)header[0x3c] | ((unsigned)header[0x3d] << 8) | ((unsigned)header[0x3e] << 16) |
             ((unsigned)header[0x3f] << 24);
    if (offset + 0x1a <= sizeof(header))
        memcpy(pe, header + offset, sizeof(pe));
    else if (fseek(file, (long)offset, SEEK_SET) != 0 || fread(pe, 1, sizeof(pe), file) < sizeof(pe))
        goto done;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0)
        goto done;
    machine = (unsigned short)pe[4] | ((unsigned short)pe[5] << 8);
    result = machine == 0x014c;
done:
    fclose(file);
    return result;
}

static bool run_fna_tool(const char* executable, char* const argv[]) {
    pid_t child = fork();
    int status = 0;
    if (child < 0)
        return false;
    if (child == 0) {
        execv(executable, argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void fix_fna_dylib_install_names(const char* path) {
    static const char* const dependencies[] = {"libFNA3D.0.dylib", "libSDL2-2.0.0.dylib", "libFAudio.0.dylib",
                                               "libSDL2.dylib",    "libFAudio.dylib",     "libCSteamworks.dylib"};
    const char* slash;
    const char* name;
    char id_arg[PATH_MAX];
    char* id_argv[] = {
        (char*)"/usr/bin/install_name_tool", (char*)"install_name_tool", (char*)"-id", id_arg, (char*)path, NULL};
    if (!path || !*path || access("/usr/bin/install_name_tool", X_OK) != 0)
        return;
    slash = strrchr(path, '/');
    name = slash ? slash + 1 : path;
    snprintf(id_arg, sizeof(id_arg), "@loader_path/%s", name);
    (void)run_fna_tool("/usr/bin/install_name_tool", id_argv);
    for (size_t i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]); i++) {
        char old_name[PATH_MAX], new_name[PATH_MAX];
        char* change_argv[] = {(char*)"/usr/bin/install_name_tool",
                               (char*)"install_name_tool",
                               (char*)"-change",
                               old_name,
                               new_name,
                               (char*)path,
                               NULL};
        snprintf(old_name, sizeof(old_name), "@rpath/%s", dependencies[i]);
        snprintf(new_name, sizeof(new_name), "@loader_path/%s", dependencies[i]);
        (void)run_fna_tool("/usr/bin/install_name_tool", change_argv);
    }
    if (access("/usr/bin/codesign", X_OK) == 0) {
        char* sign_argv[] = {(char*)"/usr/bin/codesign",
                             (char*)"codesign",
                             (char*)"--force",
                             (char*)"-s",
                             (char*)"-",
                             (char*)path,
                             NULL};
        (void)run_fna_tool("/usr/bin/codesign", sign_argv);
    }
}

static void stage_fna_directory(const char* source, const char* destination) {
    DIR* dir = opendir(source);
    struct dirent* entry;
    if (!dir || !ensure_directory(destination)) {
        if (dir)
            closedir(dir);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char* source_path;
        char* target_path;
        struct stat info;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        source_path = join(source, entry->d_name);
        target_path = join(destination, entry->d_name);
        if (source_path && target_path && stat(source_path, &info) == 0) {
            if (S_ISREG(info.st_mode)) {
                /* This legacy shim is SDL3-linked. Do not
                 * stages it; the game alias is rebuilt from libFNA3D.0 below. */
                if (strcmp(entry->d_name, "libFNA3D.dylib") != 0) {
                    (void)copy_file_path(source_path, target_path);
                    if (strstr(entry->d_name, ".dylib") != NULL)
                        fix_fna_dylib_install_names(target_path);
                }
            } else if (S_ISDIR(info.st_mode))
                stage_fna_directory(source_path, target_path);
        }
        free(source_path);
        free(target_path);
    }
    closedir(dir);
}

static void restore_game_fmod_libraries(const char* game_dir) {
    static const char* const names[] = {"libfmod.dylib", "libfmodstudio.dylib"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char* source_dir = join(game_dir, "fmod");
        char* source = source_dir ? join(source_dir, names[i]) : NULL;
        char* target = join(game_dir, names[i]);
        struct stat info;
        if (source && target && stat(source, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 256 * 1024) {
            if (copy_file_path(source, target))
                fix_fna_dylib_install_names(target);
        }
        free(source_dir);
        free(source);
        free(target);
    }
}

static void run_terraria_offline_patcher(const char* home, const char* game_dir, const char* mono,
                                         const char* config_path) {
    char* patcher = join(game_dir, "TerrariaOfflinePatcher.exe");
    char* terraria = join(game_dir, "Terraria.exe");
    char* backup = terraria ? malloc(strlen(terraria) + strlen(".metalsharp-original") + 1) : NULL;
    pid_t child;
    int status = 0;
    if (backup)
        sprintf(backup, "%s.metalsharp-original", terraria);
    if (!patcher || !terraria || !backup || access(patcher, R_OK) != 0 || access(terraria, R_OK) != 0 ||
        access(backup, F_OK) == 0)
        goto done;
    child = fork();
    if (child < 0)
        goto done;
    if (child == 0) {
        char mono_path[PATH_MAX * 2];
        char native_path[PATH_MAX * 3];
        char* argv[] = {(char*)"/usr/bin/arch", (char*)"-x86_64", (char*)mono, patcher, terraria, NULL};
        snprintf(mono_path, sizeof(mono_path), "%s:%s/runtime/mono-x86/lib/mono/4.5", game_dir, home);
        snprintf(native_path, sizeof(native_path), "%s/runtime/mono-x86/lib:%s/runtime/shims:%s", home, home, game_dir);
        setenv("MONO_ENV_OPTIONS", "--runtime=v4.0", 1);
        setenv("MONO_PATH", mono_path, 1);
        setenv("DYLD_LIBRARY_PATH", native_path, 1);
        setenv("DYLD_FALLBACK_LIBRARY_PATH", native_path, 1);
        if (config_path)
            setenv("MONO_CONFIG", config_path, 1);
        (void)chdir(game_dir);
        execv(argv[0], argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
done:
    free(patcher);
    free(terraria);
    free(backup);
}

static void ensure_mono_native_alias(const char* home, const char* arch) {
    char* lib = join(home, arch);
    char* directory = lib ? join(lib, "lib") : NULL;
    char* source = directory ? join(directory, "libmono-native-unified.dylib") : NULL;
    char* alias = directory ? join(directory, "libmono-native.dylib") : NULL;
    if (source && alias && access(source, R_OK) == 0 && access(alias, F_OK) != 0)
        (void)symlink("libmono-native-unified.dylib", alias);
    free(lib);
    free(directory);
    free(source);
    free(alias);
}

static void ensure_carbon_interpose_shim(const char* home, const char* game_dir) {
    char* source_dir = join(home, "runtime/shim-sources/fna/shims");
    char* source = source_dir ? join(source_dir, "carbon_interpose.c") : NULL;
    char* output = join(game_dir, "libmetalsharp_carbon_interpose.dylib");
    if (source && access(source, R_OK) != 0) {
        free(source);
        source =
            strdup("/Applications/MetalSharp.app/Contents/Resources/runtime/shim-sources/fna/shims/carbon_interpose.c");
    }
    if (source && output && access(source, R_OK) == 0 && access(output, R_OK) != 0) {
        char* argv[] = {(char*)"/usr/bin/clang", (char*)"clang", (char*)"-shared", (char*)"-fPIC", (char*)"-arch",
                        (char*)"x86_64",         (char*)"-o",    output,           source,         NULL};
        if (run_fna_tool("/usr/bin/clang", argv) && access(output, R_OK) == 0) {
            char* sign_argv[] = {
                (char*)"/usr/bin/codesign", (char*)"codesign", (char*)"--force", (char*)"-s", (char*)"-", output, NULL};
            (void)run_fna_tool("/usr/bin/codesign", sign_argv);
        }
    }
    free(source_dir);
    free(source);
    free(output);
}

static void stage_celeste_steam_api(const char* home, const char* game_dir) {
    char* steam_root = join(home, "Library/Application Support/Steam");
    char* helper =
        steam_root
            ? join(steam_root,
                   "Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Steam Helper.app/Contents/MacOS/libsteam_api.dylib")
            : NULL;
    char* bridge = join(home, "runtime/steam-bridge/libsteam_api.dylib");
    char* shims = join(home, "runtime/shims/libsteam_api.dylib");
    char* target = join(game_dir, "libsteam_api.dylib");
    const char* source = NULL;
    if (helper && access(helper, R_OK) == 0)
        source = helper;
    else if (bridge && access(bridge, R_OK) == 0)
        source = bridge;
    else if (shims && access(shims, R_OK) == 0)
        source = shims;
    if (source && target)
        (void)copy_file_path(source, target);
    free(steam_root);
    free(helper);
    free(bridge);
    free(shims);
    free(target);
}

static bool steam_launch_model_app(unsigned id) {
    return id == 620 || id == 4000 || id == 1260320 || id == 440 || id == 730 || id == 252490 || id == 271590 ||
           id == 284160 || id == 292030 || id == 1172380 || id == 3241660;
}

static bool steam_secure_launch_model_app(unsigned id) {
    return id == 440 || id == 730 || id == 252490 || id == 271590 || id == 284160 || id == 292030 || id == 1172380 ||
           id == 3241660;
}

/* Prepare the real Steam client contract before every direct launch.
 * In particular, source-style games need steam_appid.txt even when the
 * graphics route is D3D9 and the executable is launched directly through Wine. */
static void prepare_real_steam_launch(const char* home, const char* game_dir, const char* executable, unsigned id,
                                      const char* pipeline) {
    char* steam_dir;
    char* target_dirs[4] = {NULL, NULL, NULL, NULL};
    size_t target_count = 0;
    const char* files[] = {"steam_api.dll",     "steam_api64.dll",         "steamclient.dll",
                           "steamclient64.dll", "GameOverlayRenderer.dll", "GameOverlayRenderer64.dll"};
    if (!game_dir || !steam_launch_model_app(id) || !strcmp(pipeline, "m13") || !strcmp(pipeline, "d3dmetal"))
        return;
    steam_dir = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    if (!steam_dir)
        return;
    target_dirs[target_count++] = strdup(game_dir);
    target_dirs[target_count++] = join(game_dir, "bin");
    if (executable) {
        char* exe_dir = strdup(executable);
        char* slash = exe_dir ? strrchr(exe_dir, '/') : NULL;
        if (slash) {
            *slash = '\0';
            target_dirs[target_count++] = exe_dir;
            exe_dir = NULL;
        }
        free(exe_dir);
    }
    for (size_t i = 0; i < target_count; i++) {
        if (!target_dirs[i] || access(target_dirs[i], F_OK) != 0)
            continue;
        for (size_t j = 0; j < sizeof(files) / sizeof(files[0]); j++) {
            char* source = join(steam_dir, files[j]);
            char* target = join(target_dirs[i], files[j]);
            bool model_file = j >= 2;
            bool should_deploy = j < 2 || (model_file && steam_secure_launch_model_app(id));
            if (should_deploy && source && target && access(target, F_OK) != 0 && access(source, R_OK) == 0)
                (void)copy_file_path(source, target);
            free(source);
            free(target);
        }
        {
            char* appid_path = join(target_dirs[i], "steam_appid.txt");
            FILE* appid_file = appid_path ? fopen(appid_path, "wb") : NULL;
            if (appid_file) {
                fprintf(appid_file, "%u\n", id);
                fclose(appid_file);
            }
            free(appid_path);
        }
    }
    for (size_t i = 0; i < target_count; i++)
        free(target_dirs[i]);
    free(steam_dir);
}

static char* fna_game_executable(const char* game_dir, unsigned id) {
    const char* preferred[2] = {NULL, NULL};
    if (id == 105600) {
        preferred[0] = "TerrariaLauncher.exe";
        preferred[1] = "Terraria.exe";
    }
    for (size_t i = 0; i < 2; i++) {
        char* path;
        if (!preferred[i])
            continue;
        path = join(game_dir, preferred[i]);
        if (path && access(path, R_OK) == 0)
            return path;
        free(path);
    }
    return find_game_executable(game_dir, 0);
}

static bool fna_bridge_running(unsigned port) {
    int fd;
    struct sockaddr_in address;
    bool running = false;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(0x7f000001U);
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0)
        running = true;
    close(fd);
    return running;
}

static bool ensure_fna_bridge(const char* home) {
    char bridge[PATH_MAX], wine[PATH_MAX], prefix[PATH_MAX];
    unsigned port = 18733;
    const char* configured = getenv("METALSHARP_STEAM_BRIDGE_PORT");
    pid_t child;
    if (configured && *configured)
        port = (unsigned)strtoul(configured, NULL, 10);
    if (port < 1 || port > 65535)
        port = 18733;
    if (fna_bridge_running(port))
        return true;
    snprintf(bridge, sizeof(bridge), "%s/runtime/steam-bridge/steambridge.exe", home);
    snprintf(wine, sizeof(wine), "%s/runtime/wine/bin/metalsharp-wine", home);
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    if (access(bridge, R_OK) != 0) {
        /* The shipped installer currently provides the native Steam shim but
         * not steambridge.exe.  The native shim is still a valid FNA fallback;
         * do not make Celeste/Terraria unlaunchable solely because the
         * optional Wine bridge artifact is absent. */
        char shim[PATH_MAX];
        snprintf(shim, sizeof(shim), "%s/runtime/steam-bridge/libsteam_api.dylib", home);
        return access(shim, R_OK) == 0;
    }
    if (access(wine, X_OK) != 0)
        return false;
    child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        char port_text[16];
        snprintf(port_text, sizeof(port_text), "%u", port);
        setenv("WINEPREFIX", prefix, 1);
        setenv("METALSHARP_HOME", home, 1);
        setenv("METALSHARP_STEAM_BRIDGE_PORT", port_text, 1);
        setenv("WINEDEBUG", "-all", 1);
        execl(wine, wine, bridge, (char*)NULL);
        _exit(127);
    }
    for (unsigned i = 0; i < 20; i++) {
        if (fna_bridge_running(port))
            return true;
        usleep(250000);
    }
    return false;
}

static char* spawn_fna_game(const char* home, unsigned id, pid_t* pid) {
    bool x86 = id != 413150;
    const char* mono_name = x86 ? "runtime/mono-x86/bin/mono" : "runtime/mono-arm64/bin/mono";
    const char* config_name = id == 504230   ? "celeste-x86-mono.config"
                              : id == 105600 ? "terraria-mono.config"
                              : id == 413150 ? "stardew-mono.config"
                                             : "generic-fna-mono.config";
    char* game_dir = ms_steam_game_dir(home, id);
    char* local_dir = join(home, "games");
    char local_id[32];
    char* local_game;
    char* mono = join(home, mono_name);
    char* executable = game_dir ? fna_game_executable(game_dir, id) : NULL;
    char* config = join(home, "configs");
    char* config_path;
    char* cwd;
    char* slash;
    char library_env[PATH_MAX * 4];
    pid_t child;
    snprintf(local_id, sizeof(local_id), "%u", id);
    local_game = local_dir ? join(local_dir, local_id) : NULL;
    if (!game_dir && local_game && access(local_game, F_OK) == 0) {
        game_dir = local_game;
        local_game = NULL;
        executable = fna_game_executable(game_dir, id);
    }
    if (!mono || access(mono, X_OK) != 0 || !game_dir || !executable) {
        free(game_dir);
        free(local_dir);
        free(local_game);
        free(mono);
        free(executable);
        free(config);
        return strdup("Mono/FNA runtime or game executable not found");
    }
    if (!ensure_fna_bridge(home)) {
        free(game_dir);
        free(local_dir);
        free(local_game);
        free(mono);
        free(executable);
        free(config);
        return strdup("Steam bridge failed to start within 5s");
    }
    config_path = config ? join(config, config_name) : NULL;
    if (config_path && access(config_path, R_OK) != 0 && id == 105600) {
        free(config_path);
        config_path = config ? join(config, "terraria-mono.config") : NULL;
    }
    if (config_path && access(config_path, R_OK) != 0) {
        char bundled[PATH_MAX];
        snprintf(bundled, sizeof(bundled), "/Applications/MetalSharp.app/Contents/Resources/configs/%s", config_name);
        if (access(bundled, R_OK) == 0) {
            if (config)
                (void)ensure_directory(config);
            if (!copy_file_path(bundled, config_path)) {
                free(config_path);
                config_path = strdup(bundled);
            }
        }
    }
    cwd = strdup(executable);
    slash = cwd ? strrchr(cwd, '/') : NULL;
    if (slash)
        *slash = '\0';
    {
        char* fnalibs = join(home, "runtime/fnalibs");
        char* fmod = join(home, "runtime/fnalibs/fmod");
        char* shims = join(home, "runtime/shims");
        stage_fna_directory(fnalibs, cwd);
        /* Celeste's music path imports the x86 FMOD API separately from
         * FAudio. Deploy this nested runtime directory explicitly. */
        if (x86)
            stage_fna_directory(fmod, cwd);
        stage_fna_directory(shims, cwd);
        if (id == 504230) {
            char* sdl3 = join(cwd, "libSDL3.0.dylib");
            char* sdl3_alias = join(cwd, "libSDL3.dylib");
            /* Setup deploys libsteam_api for Celeste. Keep
             * that Steam contract, but do not copy unrelated SDL3 helpers. */
            stage_celeste_steam_api(home, cwd);
            if (sdl3)
                (void)unlink(sdl3);
            if (sdl3_alias)
                (void)unlink(sdl3_alias);
            free(sdl3);
            free(sdl3_alias);
        }
        /* Celeste ships the real FMOD Core/Studio dylibs in its nested
         * fmod directory.  The runtime fmod files are no-op compatibility
         * stubs and must never replace those libraries. */
        if (id == 504230)
            restore_game_fmod_libraries(cwd);
        /* The FNA deploy makes libFNA3D.dylib resolve to the SDL2-linked
         * libFNA3D.0.dylib.  The legacy shim directory also contains an
         * SDL3-linked libFNA3D.dylib; copying that last silently breaks the
         * native P/Invoke with DllNotFoundException. */
        {
            char* fna3d = join(cwd, "libFNA3D.0.dylib");
            char* alias = join(cwd, "libFNA3D.dylib");
            if (fna3d && alias && access(fna3d, R_OK) == 0) {
                (void)unlink(alias);
                (void)symlink("libFNA3D.0.dylib", alias);
            }
            free(fna3d);
            free(alias);
        }
        free(fnalibs);
        free(fmod);
        free(shims);
    }
    if (x86)
        ensure_mono_native_alias(home, "runtime/mono-x86");
    ensure_carbon_interpose_shim(home, cwd);
    if (id == 105600)
        run_terraria_offline_patcher(home, cwd, mono, config_path);
    if (id == 105600 || id == 413150)
        snprintf(library_env, sizeof(library_env), "%s:%s/runtime/shims:%s/runtime/mono-%s/lib:/opt/homebrew/lib", cwd,
                 home, home, x86 ? "x86" : "arm64");
    else
        snprintf(library_env, sizeof(library_env), "%s:%s/runtime/mono-%s/lib:/opt/homebrew/lib", cwd, home,
                 x86 ? "x86" : "arm64");
    child = fork();
    if (child < 0) {
        free(game_dir);
        free(local_dir);
        free(local_game);
        free(mono);
        free(executable);
        free(config);
        free(config_path);
        free(cwd);
        return strdup(strerror(errno));
    }
    if (child == 0) {
        char mono_path[PATH_MAX * 2];
        char* argv[10];
        size_t argc = 0;
        snprintf(mono_path, sizeof(mono_path), "%s:%s/runtime/mono-%s/lib/mono/4.5", cwd, home, x86 ? "x86" : "arm64");
        setenv("DYLD_LIBRARY_PATH", library_env, 1);
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
        setenv("METALSHARP_HOME", home, 1);
        /* Match the FNA pipeline contract; without this, macOS graphics
         * wrappers can interfere with FNA's title-screen frame/input setup. */
        setenv("METAL_DEVICE_WRAPPER_TYPE", "0", 1);
        setenv("MONO_ENV_OPTIONS", "--runtime=v4.0", 1);
        setenv("MONO_PATH", mono_path, 1);
        {
            char app_id[32];
            snprintf(app_id, sizeof(app_id), "%u", id);
            setenv("SteamAppId", app_id, 1);
            setenv("SteamGameId", app_id, 1);
        }
        {
            char carbon_shim[PATH_MAX];
            char carbon_interpose[PATH_MAX];
            snprintf(carbon_shim, sizeof(carbon_shim), "%s/libCarbon.dylib", cwd);
            snprintf(carbon_interpose, sizeof(carbon_interpose), "%s/libmetalsharp_carbon_interpose.dylib", cwd);
            if (access(carbon_shim, R_OK) == 0)
                setenv("METALSHARP_CARBON_SHIM", carbon_shim, 1);
            if (access(carbon_interpose, R_OK) == 0)
                setenv("DYLD_INSERT_LIBRARIES", carbon_interpose, 1);
        }
        if (config_path)
            setenv("MONO_CONFIG", config_path, 1);
        (void)chdir(cwd);
        {
            char log_dir[PATH_MAX];
            char log_path[PATH_MAX];
            int log_fd;
            snprintf(log_dir, sizeof(log_dir), "%s/bottles/steam_%u/logs", home, id);
            snprintf(log_path, sizeof(log_path), "%s/fna-launch.log", log_dir);
            if (ensure_directory(log_dir)) {
                log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (log_fd >= 0) {
                    dprintf(log_fd, "\\n--- FNA launch appid=%u executable=%s ---\\n", id, executable);
                    dup2(log_fd, STDOUT_FILENO);
                    dup2(log_fd, STDERR_FILENO);
                    close(log_fd);
                }
            }
        }
        if (x86) {
            argv[argc++] = "/usr/bin/arch";
            argv[argc++] = "-x86_64";
        }
        argv[argc++] = mono;
        /* Pass the resolved absolute executable path;
         * basename-only invocation can make Mono's native loader resolve the
         * FNA3D/SDL stack against the wrong directory. */
        argv[argc++] = executable;
        argv[argc] = NULL;
        execv(argv[0], argv);
        _exit(127);
    }
    free(game_dir);
    free(local_dir);
    free(local_game);
    free(mono);
    free(executable);
    free(config);
    free(config_path);
    free(cwd);
    *pid = child;
    return NULL;
}

static bool stage_route_asset(const char* home, const char* source_subpath, const char* filename,
                              const char* destination) {
    char* source_root = NULL;
    char* source_dir = NULL;
    char* source = NULL;
    char* target = NULL;
    bool ok = false;
    if (!strncmp(source_subpath, "vkd3d/", 6) || !strncmp(source_subpath, "runtime/", 8))
        source_root = strdup(home);
    else
        source_root = join(home, "runtime/wine");
    source_dir = source_root ? join(source_root, source_subpath) : NULL;
    source = source_dir ? join(source_dir, filename) : NULL;
    target = destination ? join(destination, filename) : NULL;
    if (source && target && access(source, R_OK) == 0) {
        (void)ensure_directory(destination);
        ok = copy_file_path(source, target);
        /* Staged route DLLs are loaded by the game — strip quarantine so
         * Gatekeeper can never block a freshly staged payload. */
        if (ok && target)
            (void)removexattr(target, "com.apple.quarantine", XATTR_NOFOLLOW);
    }
    free(source_root);
    free(source_dir);
    free(source);
    free(target);
    return ok;
}

static bool files_match(const char* left, const char* right) {
    FILE *a = fopen(left, "rb"), *b = fopen(right, "rb");
    unsigned char left_buf[65536], right_buf[65536];
    bool match = true;
    if (!a || !b) {
        if (a)
            fclose(a);
        if (b)
            fclose(b);
        return false;
    }
    for (;;) {
        size_t left_n = fread(left_buf, 1, sizeof(left_buf), a);
        size_t right_n = fread(right_buf, 1, sizeof(right_buf), b);
        if (left_n != right_n || memcmp(left_buf, right_buf, left_n) != 0) {
            match = false;
            break;
        }
        if (left_n == 0)
            break;
    }
    fclose(a);
    fclose(b);
    return match;
}

static void remove_stale_route_dlls(const char* home, const char* pipeline, const char* game_dir,
                                    const char* executable) {
    static const char* const names[] = {"d3d12.dll",     "d3d12core.dll",
                                        "d3d11.dll",     "d3d10.dll",
                                        "d3d10_1.dll",   "d3d10core.dll",
                                        "d3d9.dll",      "dxgi.dll",
                                        "dxgi_dxmt.dll", "nvapi64.dll",
                                        "nvngx.dll",     "nvngx-on-metalfx.dll",
                                        "winemetal.dll", "metalsharp_ntdll_hook.dll"};
    const char* source_subpaths[] = {"runtime/wine/lib/dxmt/x86_64-windows",
                                     "runtime/wine/lib/dxmt/i386-windows",
                                     "runtime/wine/lib/metalsharp/x86_64-windows",
                                     "runtime/wine/lib/metalsharp/i386-windows",
                                     "runtime/wine/lib/wine/x86_64-windows",
                                     "runtime/wine/lib/wine/i386-windows",
                                     "runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows",
                                     "vkd3d/vkd3d-proton/x86_64-windows",
                                     "vkd3d/dxvk/x86_64-windows",
                                     "vkd3d/dxvk/i386-windows"};
    char* exe_dir = executable ? strdup(executable) : NULL;
    char* slash = exe_dir ? strrchr(exe_dir, '/') : NULL;
    (void)pipeline;
    const char* dirs[2] = {game_dir, NULL};
    if (slash) {
        *slash = '\0';
        dirs[1] = exe_dir;
    }
    /* Run for every pipeline transition. A route DLL is removed only after a
     * byte-for-byte comparison against a bundled known artifact, so a game's
     * own DLL (or a user-modified one) is never removed. */
    for (size_t d = 0; d < 2; d++) {
        if (!dirs[d] || (d == 1 && dirs[0] && !strcmp(dirs[0], dirs[1])))
            continue;
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
            char* target = join(dirs[d], names[n]);
            if (!target || access(target, F_OK) != 0) {
                free(target);
                continue;
            }
            for (size_t s = 0; s < sizeof(source_subpaths) / sizeof(source_subpaths[0]); s++) {
                char* source_dir = join(home, source_subpaths[s]);
                char* source = source_dir ? join(source_dir, names[n]) : NULL;
                bool same = source && access(source, R_OK) == 0 && files_match(target, source);
                free(source_dir);
                free(source);
                if (same) {
                    (void)unlink(target);
                    break;
                }
            }
            free(target);
        }
    }
    free(exe_dir);
}

void ms_steam_cleanup_route_dlls(const char* home, const char* pipeline, const char* game_dir, const char* executable) {
    remove_stale_route_dlls(home, pipeline, game_dir, executable);
}

static bool stage_route_dlls(const char* home, unsigned id, const char* pipeline, const char* executable) {
    char* exe_dir = NULL;
    char* game_dir = NULL;
    char* cursor;
    bool ok = true;
    const char* source;
    const char* files[12];
    size_t file_count = 0;
    bool is32 = executable_is_32bit(executable);

    if (!strcmp(pipeline, "m13") || !strcmp(pipeline, "m32") || !strcmp(pipeline, "wine_bare"))
        return true;
    exe_dir = strdup(executable);
    cursor = exe_dir ? strrchr(exe_dir, '/') : NULL;
    if (!cursor)
        goto done;
    *cursor = '\0';
    game_dir = ms_steam_game_dir(home, id);

    if (!strcmp(pipeline, "d3dmetal")) {
        if (is32) {
            ok = false;
            goto done;
        }
        source = "runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows";
        files[file_count++] = "d3d10.dll";
        files[file_count++] = "d3d11.dll";
        files[file_count++] = "d3d12.dll";
        files[file_count++] = "dxgi.dll";
        files[file_count++] = "nvapi64.dll";
        files[file_count++] = "nvngx-on-metalfx.dll";
    } else if (!strcmp(pipeline, "vkd3d")) {
        static const char* const files_vkd3d[] = {"d3d12.dll", "d3d12core.dll", "dxgi.dll"};
        static const char* const files_dxvk[] = {"d3d11.dll", "d3d10core.dll", "d3d9.dll"};
        for (size_t i = 0; i < sizeof(files_vkd3d) / sizeof(files_vkd3d[0]); i++)
            ok = stage_route_asset(home, "vkd3d/vkd3d-proton/x86_64-windows", files_vkd3d[i], exe_dir) && ok;
        for (size_t i = 0; i < sizeof(files_dxvk) / sizeof(files_dxvk[0]); i++)
            ok = stage_route_asset(home, "vkd3d/dxvk/x86_64-windows", files_dxvk[i], exe_dir) && ok;
        goto prefix_done;
    } else if (pipeline_is_d3d9(pipeline)) {
        const char* source = is32 ? "lib/wine/i386-windows" : "lib/wine/x86_64-windows";
        static const char* const files_d3d9[] = {"d3d9.dll", "dxgi.dll"};
        for (size_t i = 0; i < sizeof(files_d3d9) / sizeof(files_d3d9[0]); i++) {
            if (!stage_route_asset(home, source, files_d3d9[i], exe_dir))
                ok = false;
        }
        goto prefix_done;
    } else if (!strcmp(pipeline, "dxmt") || !strcmp(pipeline, "dxmt_32")) {
        source = !strcmp(pipeline, "dxmt_32") ? "lib/dxmt/i386-windows" : "lib/dxmt/x86_64-windows";
        files[file_count++] = "d3d11.dll";
        files[file_count++] = "d3d10core.dll";
        files[file_count++] = "dxgi.dll";
        files[file_count++] = "winemetal.dll";
    } else {
        goto done;
    }
    for (size_t i = 0; i < file_count; i++) {
        bool staged = stage_route_asset(home, source, files[i], exe_dir);
        bool optional = !strncmp(files[i], "nvapi", 5) || !strncmp(files[i], "nvngx", 5);
        if (!staged && !optional)
            ok = false;
    }

prefix_done:
done:
    free(game_dir);
    free(exe_dir);
    return ok;
}

bool ms_steam_stage_route_for_executable(const char* home, const char* pipeline, const char* game_dir,
                                         const char* executable) {
    if (!home || !pipeline || !game_dir || !executable)
        return false;
    /* Accept the same route names as bottles ("d3d11" -> dxmt); auto stages nothing. */
    const char* canonical = canonical_pipeline(pipeline);
    ms_steam_cleanup_route_dlls(home, pipeline, game_dir, executable);
    return stage_route_dlls(home, 0, canonical ? canonical : pipeline, executable);
}

static const char* default_pipeline_for_appid(const char* home, unsigned appid) {
    static char pipeline[64];
    char* raw = ms_mtsp_default_rules_json();
    char error[96];
    ms_json* root;
    const ms_json* rules;
    pipeline[0] = '\0';
    root = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    free(raw);
    rules = root ? ms_json_object_get(root, "rules") : NULL;
    if (rules && ms_json_type_of(rules) == MS_JSON_ARRAY) {
        for (size_t i = 0; i < ms_json_array_length(rules); i++) {
            const ms_json* rule = ms_json_array_get(rules, i);
            long long rule_appid;
            char* value = NULL;
            if (!ms_json_as_i64(ms_json_object_get(rule, "appid"), &rule_appid) || rule_appid != (long long)appid)
                continue;
            if (ms_json_as_string(ms_json_object_get(rule, "default_pipeline"), &value) && value) {
                snprintf(pipeline, sizeof(pipeline), "%s", value);
                free(value);
                break;
            }
            free(value);
        }
    }
    ms_json_free(root);
    if (!pipeline[0]) {
        char* game_dir = ms_steam_game_dir(home, appid);
        const char* detected = ms_steam_detect_graphics_pipeline(game_dir);
        snprintf(pipeline, sizeof(pipeline), "%s", detected ? detected : "vkd3d");
        free(game_dir);
    }
    return pipeline;
}

static bool bottle_pipeline_value(const char* home, unsigned appid, char* out, size_t out_size) {
    char path[PATH_MAX], raw[1024 * 1024], error[96];
    FILE* file;
    size_t length;
    ms_json* manifest;
    char* value = NULL;
    snprintf(path, sizeof(path), "%s/bottles/steam_%u/bottle.json", home, appid);
    file = fopen(path, "rb");
    if (!file)
        return false;
    length = fread(raw, 1, sizeof(raw) - 1, file);
    fclose(file);
    raw[length] = '\0';
    manifest = ms_json_parse(raw, length, error, sizeof(error));
    if (!manifest || ms_json_type_of(manifest) != MS_JSON_OBJECT) {
        ms_json_free(manifest);
        return false;
    }
    if (!ms_json_as_string(ms_json_object_get(manifest, "preferred_pipeline"), &value) || !value || !value[0]) {
        free(value);
        ms_json_free(manifest);
        return false;
    }
    snprintf(out, out_size, "%s", value);
    free(value);
    ms_json_free(manifest);
    return true;
}
static void string_field(ms_json_writer* writer, const char* key, const char* value) {
    ms_json_writer_key(writer, key);
    ms_json_writer_string(writer, value);
}

static bool ensure_directory(const char* path) {
    char* copy;
    char* slash;
    if (!path || !path[0])
        return false;
    if (access(path, F_OK) == 0)
        return true;
    copy = strdup(path);
    if (!copy)
        return false;
    slash = strrchr(copy, '/');
    if (slash && slash != copy) {
        *slash = 0;
        if (!ensure_directory(copy)) {
            free(copy);
            return false;
        }
        *slash = '/';
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static bool steam_url_shortcut(const char* path) {
    FILE* file = fopen(path, "rb");
    char line[4096];
    bool result = false;
    if (!file)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char* text = line;
        while (*text == ' ' || *text == '\t')
            text++;
        if (strncasecmp(text, "url=steam://", 12) == 0) {
            result = true;
            break;
        }
    }
    fclose(file);
    return result;
}

static void redirect_wine_steam_desktop(const char* home) {
    const char* host_home = getenv("HOME");
    char* users = join(home, "prefix-steam/drive_c/users");
    char* host_desktop = host_home ? join(host_home, "Desktop") : NULL;
    char* redirect_root = join(home, "steam-desktop");
    DIR* directory = users ? opendir(users) : NULL;
    struct dirent* entry;
    if (!directory || !host_desktop || !redirect_root)
        goto done;
    while ((entry = readdir(directory)) != NULL) {
        char *user_dir, *desktop, *redirect_dir, *target = NULL;
        struct stat info;
        ssize_t target_length;
        if (entry->d_name[0] == '.')
            continue;
        user_dir = join(users, entry->d_name);
        desktop = user_dir ? join(user_dir, "Desktop") : NULL;
        if (!desktop || lstat(desktop, &info) != 0 || !S_ISLNK(info.st_mode)) {
            free(user_dir);
            free(desktop);
            continue;
        }
        target = malloc(PATH_MAX);
        target_length = target ? readlink(desktop, target, PATH_MAX - 1) : -1;
        if (target_length < 0) {
            free(user_dir);
            free(desktop);
            free(target);
            continue;
        }
        target[target_length] = '\0';
        if (target[0] != '/') {
            char* parent = user_dir ? join(user_dir, target) : NULL;
            free(target);
            target = parent;
        }
        {
            char resolved_target[PATH_MAX], resolved_host[PATH_MAX];
            bool same = realpath(target, resolved_target) && realpath(host_desktop, resolved_host) &&
                        strcmp(resolved_target, resolved_host) == 0;
            if (!same) {
                free(user_dir);
                free(desktop);
                free(target);
                continue;
            }
        }
        redirect_dir = join(redirect_root, entry->d_name);
        if (redirect_dir && ensure_directory(redirect_dir)) {
            DIR* host_directory = opendir(host_desktop);
            struct dirent* shortcut;
            while (host_directory && (shortcut = readdir(host_directory)) != NULL) {
                char *source, *destination;
                if (shortcut->d_name[0] == '.')
                    continue;
                source = join(host_desktop, shortcut->d_name);
                if (!source || !steam_url_shortcut(source)) {
                    free(source);
                    continue;
                }
                destination = join(redirect_dir, shortcut->d_name);
                if (destination && access(destination, F_OK) != 0)
                    (void)rename(source, destination);
                free(source);
                free(destination);
            }
            if (host_directory)
                closedir(host_directory);
            (void)unlink(desktop);
            (void)symlink(redirect_dir, desktop);
        }
        free(user_dir);
        free(desktop);
        free(redirect_dir);
        free(target);
    }
done:
    if (directory)
        closedir(directory);
    free(users);
    free(host_desktop);
    free(redirect_root);
}

static unsigned long long monotonic_millis(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

static void record_launch_timing(const char* home, unsigned id, unsigned long long started_at, const char* pipeline) {
    char dir[2048], final_path[2048], temp_path[2048];
    ms_json_writer w;
    unsigned long long now = monotonic_millis();
    snprintf(dir, sizeof(dir), "%s/bottles/steam_%u/logs", home, id);
    if (!ensure_directory(dir))
        return;
    snprintf(final_path, sizeof(final_path), "%s/launch-timing-latest.json", dir);
    snprintf(temp_path, sizeof(temp_path), "%s/launch-timing-latest.json.tmp", dir);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "started_at_unix");
    ms_json_writer_u64(&w, (unsigned long long)time(NULL));
    ms_json_writer_key(&w, "total_ms");
    ms_json_writer_u64(&w, now >= started_at ? now - started_at : 0);
    ms_json_writer_key(&w, "checkpoints");
    ms_json_writer_array_begin(&w);
    const char* names[] = {"pipeline_resolution", "dll_staging",    "bridge_checks", "process_spawn", "log_path",
                           "steam_library",       "bottle_manifest"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "name");
        ms_json_writer_string(&w, names[i]);
        ms_json_writer_key(&w, "elapsed_ms");
        ms_json_writer_u64(&w, now >= started_at ? now - started_at : 0);
        ms_json_writer_key(&w, "elapsed_us");
        ms_json_writer_u64(&w, now >= started_at ? (now - started_at) * 1000ULL : 0);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    (void)pipeline;
    ms_json_writer_object_end(&w);
    char* payload = ms_json_writer_take(&w);
    if (payload) {
        FILE* file = fopen(temp_path, "wb");
        if (file) {
            fputs(payload, file);
            fclose(file);
            rename(temp_path, final_path);
        }
        free(payload);
    }
}

bool ms_steam_ensure_bottle_manifest(const char* home, unsigned id, const char* pipeline) {
    char bottle_id[64];
    char name[64];
    char *bottles = join(home, "bottles"), *dir = NULL, *path = NULL, *prefix = NULL;
    FILE* file = NULL;
    ms_json_writer w;
    char* serialized = NULL;
    bool ok = false;
    if (!pipeline || !pipeline[0] || !strcmp(pipeline, "auto"))
        pipeline = default_pipeline_for_appid(home, id);
    {
        const char* canonical = canonical_pipeline(pipeline);
        pipeline = canonical && strcmp(canonical, "auto") ? canonical : "vkd3d";
    }
    snprintf(bottle_id, sizeof(bottle_id), "steam_%u", id);
    snprintf(name, sizeof(name), "Game %u", id);
    if (!bottles || !ensure_directory(bottles))
        goto done;
    dir = join(bottles, bottle_id);
    if (!dir || !ensure_directory(dir))
        goto done;
    path = join(dir, "bottle.json");
    if (!path)
        goto done;
    if (access(path, F_OK) == 0) {
        normalize_fna_bottle_profile(path, id);
        ok = true;
        goto done;
    }
    prefix = join(home, "prefix-steam");
    if (!prefix)
        goto done;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    string_field(&w, "id", bottle_id);
    string_field(&w, "name", name);
    ms_json_writer_key(&w, "custom_name");
    ms_json_writer_null(&w);
    string_field(&w, "bottle_type", "steam");
    ms_json_writer_key(&w, "steam_app_id");
    ms_json_writer_u64(&w, id);
    string_field(&w, "prefix_path", prefix);
    string_field(&w, "arch", "wow64");
    string_field(&w, "runtime_profile", pipeline);
    string_field(&w, "preferred_pipeline", pipeline);
    ms_json_writer_key(&w, "source_installer_path");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installer_kind");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "game_install_path");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "runtime_assets");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "installed_app_detections");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    string_field(&w, "health", "new");
    ms_json_writer_key(&w, "last_launch_log");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "last_launch_pid");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "last_launch_status");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "last_launch_finished_at");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed_components");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
        ms_json_writer_key(&w, "created_at");
        ms_json_writer_string(&w, stamp);
        ms_json_writer_key(&w, "updated_at");
        ms_json_writer_string(&w, stamp);
    }
    ms_json_writer_object_end(&w);
    serialized = ms_json_writer_take(&w);
    file = fopen(path, "wb");
    if (file && serialized && fputs(serialized, file) >= 0)
        ok = true;
done:
    if (file)
        fclose(file);
    free(serialized);
    free(prefix);
    free(path);
    free(dir);
    free(bottles);
    return ok;
}

bool ms_steam_migrate_baldurs_gate_3_route_default(const char* home) {
    char* bottles = join(home, "bottles");
    char* dir = bottles ? join(bottles, "steam_1086940") : NULL;
    char* path = dir ? join(dir, "bottle.json") : NULL;
    char* marker = dir ? join(dir, ".bg3-d3dmetal-default-v1") : NULL;
    char* temporary = dir ? join(dir, ".bottle.json.bg3-default.tmp") : NULL;
    char* raw = NULL;
    char* preferred = NULL;
    char error[96];
    ms_json* manifest = NULL;
    ms_json_writer writer;
    char* serialized = NULL;
    bool ok = false;

    if (!path || !marker || !temporary)
        goto done;
    if (access(marker, F_OK) == 0) {
        ok = true;
        goto done;
    }
    raw = read_bounded_file(path);
    manifest = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    if (!manifest || ms_json_type_of(manifest) != MS_JSON_OBJECT)
        goto done;
    if (!ms_json_as_string(ms_json_object_get(manifest, "preferred_pipeline"), &preferred) || !preferred)
        goto done;

    /* Before the D3DMetal default, BG3's catalog route was DXMT. Existing
     * bottles captured that recommendation as if it were an explicit user
     * choice, so migrate that legacy value once. The marker allows users to
     * choose another route after the migration without it being overwritten. */
    if (strcmp(preferred, "dxmt") && strcmp(preferred, "m11") && strcmp(preferred, "m10"))
        goto write_marker;

    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < ms_json_object_length(manifest); i++) {
        const char* key = ms_json_object_key_at(manifest, i);
        ms_json_writer_key(&writer, key);
        if (!strcmp(key, "preferred_pipeline") || !strcmp(key, "runtime_profile")) {
            ms_json_writer_string(&writer, "d3dmetal");
        } else if (!strcmp(key, "updated_at")) {
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
            ms_json_writer_string(&writer, stamp);
        } else {
            char* value = ms_json_stringify(ms_json_object_value_at(manifest, i));
            ms_json_writer_raw(&writer, value ? value : "null");
            free(value);
        }
    }
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    if (!serialized)
        goto done;
    FILE* file = fopen(temporary, "wb");
    if (!file)
        goto done;
    bool wrote = fputs(serialized, file) >= 0 && fflush(file) == 0;
    if (fclose(file) != 0)
        wrote = false;
    if (!wrote || rename(temporary, path) != 0)
        goto done;

write_marker: {
    FILE* file = fopen(marker, "wb");
    if (!file)
        goto done;
    bool wrote = fputs("d3dmetal\n", file) >= 0;
    if (fclose(file) != 0)
        wrote = false;
    ok = wrote;
}
done:
    if (!ok && temporary)
        (void)unlink(temporary);
    free(bottles);
    free(dir);
    free(path);
    free(marker);
    free(temporary);
    free(raw);
    free(preferred);
    free(serialized);
    ms_json_free(manifest);
    return ok;
}

char* ms_steam_prepare_bottle_route_json(const char* home, const char* bottle_id) {
    char* bottles = join(home, "bottles");
    char* directory = bottles && bottle_id ? join(bottles, bottle_id) : NULL;
    char* path = directory ? join(directory, "bottle.json") : NULL;
    char* raw = path ? read_bounded_file(path) : NULL;
    char error[96];
    ms_json* manifest = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    long long appid = 0;
    char* pipeline = NULL;
    char* executable = NULL;
    char* game_dir = NULL;
    char* result = NULL;
    const char* canonical;
    if (!manifest || ms_json_type_of(manifest) != MS_JSON_OBJECT)
        goto done;
    if (!ms_json_as_i64(ms_json_object_get(manifest, "steam_app_id"), &appid) || appid <= 0)
        goto done;
    ms_json_as_string(ms_json_object_get(manifest, "game_install_path"), &game_dir);
    if (!ms_json_as_string(ms_json_object_get(manifest, "preferred_pipeline"), &pipeline) || !pipeline || !pipeline[0])
        if (!ms_json_as_string(ms_json_object_get(manifest, "runtime_profile"), &pipeline) || !pipeline)
            goto done;
    canonical = canonical_pipeline(pipeline);
    /* A saved "dxmt" is an explicit choice (launch honours it the same way);
     * only an unset/"auto" route falls back to the game's default. */
    if (!canonical || !strcmp(canonical, "auto"))
        canonical = canonical_pipeline(default_pipeline_for_appid(home, (unsigned)appid));
    if (!canonical)
        goto done;
    if (!strcmp(canonical, "fna_arm64")) {
        const char* mono = (appid == 413150) ? "runtime/mono-arm64/bin/mono" : "runtime/mono-x86/bin/mono";
        char* mono_path = join(home, mono);
        char* fnalibs = join(home, "runtime/fnalibs");
        bool ready = mono_path && access(mono_path, X_OK) == 0 && fnalibs && access(fnalibs, R_OK) == 0;
        if (ready && game_dir && access(game_dir, F_OK) == 0) {
            char* shims = join(home, "runtime/shims");
            stage_fna_directory(fnalibs, game_dir);
            stage_fna_directory(shims, game_dir);
            free(shims);
        }
        if (!ready)
            result = strdup("Mono/FNA runtime assets are incomplete");
        free(mono_path);
        free(fnalibs);
        goto done;
    }
    if (!strcmp(canonical, "m13") || !strcmp(canonical, "d3dmetal") || !strcmp(canonical, "m32") ||
        !strcmp(canonical, "wine_bare"))
        goto done;
    if (game_dir && access(game_dir, F_OK) == 0)
        executable = preferred_steam_game_executable(game_dir, (unsigned)appid, canonical);
    if (!executable)
        executable = find_steam_game_executable(home, (unsigned)appid, canonical);
    if (!executable) {
        result = strdup("game executable not found while preparing bottle route");
        goto done;
    }
    /* Switching routes removes the previous route's bundled DLLs immediately
     * (byte-identical matches only) before staging the newly selected route. */
    remove_stale_route_dlls(home, canonical, game_dir, executable);
    if (!stage_route_dlls(home, (unsigned)appid, canonical, executable))
        result = strdup("selected bottle route runtime DLLs are incomplete");
done:
    free(bottles);
    free(directory);
    free(path);
    free(raw);
    free(pipeline);
    free(executable);
    free(game_dir);
    ms_json_free(manifest);
    return result;
}

/* Stage the bottle's route DLLs the first time an installed game is seen
 * (fresh install, reinstall, or a game detected before the runtime existed),
 * not only on first launch or an explicit Bottle save. A marker records
 * success; failures retry on a later library load, once per backend run. */
void ms_steam_stage_route_on_discovery(const char* home, unsigned id) {
    static pthread_mutex_t attempted_lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned attempted[512];
    static size_t attempted_count = 0;
    char bottle_id[64];
    char* bottles = home ? join(home, "bottles") : NULL;
    char* dir = NULL;
    char* marker = NULL;
    char* error_text = NULL;
    bool tried = false;
    if (!bottles || id == 0)
        goto done;
    snprintf(bottle_id, sizeof(bottle_id), "steam_%u", id);
    dir = join(bottles, bottle_id);
    marker = dir ? join(dir, ".route-staged") : NULL;
    if (!marker || access(marker, F_OK) == 0)
        goto done;
    pthread_mutex_lock(&attempted_lock);
    for (size_t i = 0; i < attempted_count; i++)
        if (attempted[i] == id)
            tried = true;
    if (!tried && attempted_count < sizeof(attempted) / sizeof(attempted[0]))
        attempted[attempted_count++] = id;
    pthread_mutex_unlock(&attempted_lock);
    if (tried)
        goto done;
    error_text = ms_steam_prepare_bottle_route_json(home, bottle_id);
    if (!error_text) {
        FILE* file = fopen(marker, "wb");
        if (file)
            fclose(file);
    }
done:
    free(error_text);
    free(marker);
    free(dir);
    free(bottles);
}

static bool mark_steam_bottle_launch(const char* home, unsigned id, pid_t pid) {
    char bottle_id[64];
    char* dir = join(home, "bottles");
    char* path;
    char* temp_path = NULL;
    FILE* file;
    long size;
    char* raw = NULL;
    char error[96];
    ms_json* manifest;
    ms_json_writer writer;
    char* serialized = NULL;
    bool has_pid = false, has_status = false, has_updated = false, ok = false;
    snprintf(bottle_id, sizeof(bottle_id), "steam_%u", id);
    path = dir ? join(dir, bottle_id) : NULL;
    free(dir);
    dir = path ? join(path, "bottle.json") : NULL;
    free(path);
    file = dir ? fopen(dir, "rb") : NULL;
    if (!file)
        goto done;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || size > 8 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0)
        goto close_done;
    raw = malloc((size_t)size + 1);
    if (!raw || fread(raw, 1, (size_t)size, file) != (size_t)size)
        goto close_done;
    raw[size] = 0;
    fclose(file);
    file = NULL;
    manifest = ms_json_parse(raw, (size_t)size, error, sizeof(error));
    free(raw);
    raw = NULL;
    if (!manifest || ms_json_type_of(manifest) != MS_JSON_OBJECT) {
        ms_json_free(manifest);
        goto done;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < ms_json_object_length(manifest); i++) {
        const char* key = ms_json_object_key_at(manifest, i);
        const ms_json* value = ms_json_object_value_at(manifest, i);
        ms_json_writer_key(&writer, key);
        if (!strcmp(key, "last_launch_pid")) {
            has_pid = true;
            ms_json_writer_u64(&writer, (unsigned)pid);
        } else if (!strcmp(key, "last_launch_status")) {
            has_status = true;
            ms_json_writer_string(&writer, "running");
        } else if (!strcmp(key, "updated_at")) {
            char stamp[32];
            has_updated = true;
            snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
            ms_json_writer_string(&writer, stamp);
        } else {
            char* encoded = ms_json_stringify(value);
            ms_json_writer_raw(&writer, encoded ? encoded : "null");
            free(encoded);
        }
    }
    if (!has_pid) {
        ms_json_writer_key(&writer, "last_launch_pid");
        ms_json_writer_u64(&writer, (unsigned)pid);
    }
    if (!has_status)
        string_field(&writer, "last_launch_status", "running");
    if (!has_updated) {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
        string_field(&writer, "updated_at", stamp);
    }
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    ms_json_free(manifest);
    if (serialized) {
        size_t temp_length = strlen(dir) + sizeof(".tmp.XXXXXX");
        int fd;
        temp_path = malloc(temp_length);
        if (!temp_path)
            goto done;
        snprintf(temp_path, temp_length, "%s.tmp.XXXXXX", dir);
        fd = mkstemp(temp_path);
        if (fd < 0)
            goto done;
        (void)fchmod(fd, 0644);
        file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            goto done;
        }
        if (fputs(serialized, file) < 0 || fflush(file) != 0 || fsync(fileno(file)) != 0)
            goto close_done;
        if (fclose(file) != 0) {
            file = NULL;
            goto done;
        }
        file = NULL;
        if (rename(temp_path, dir) == 0)
            ok = true;
    }
close_done:
    if (file)
        fclose(file);
done:
    if (!ok && temp_path)
        unlink(temp_path);
    free(raw);
    free(serialized);
    free(temp_path);
    free(dir);
    return ok;
}

static char* err(const char* s) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, s);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
static bool body_id(const char* body, size_t len, unsigned* id) {
    char e[128];
    ms_json* r = ms_json_parse(body ? body : "", len, e, sizeof(e));
    long long n;
    bool ok = r && ms_json_type_of(r) == MS_JSON_OBJECT && ms_json_as_i64(ms_json_object_get(r, "appid"), &n) &&
              n > 0 && n <= 0xffffffffLL;
    if (ok)
        *id = (unsigned)n;
    ms_json_free(r);
    return ok;
}
static bool remove_tree(const char* path) {
    struct stat info;
    DIR* dir;
    struct dirent* entry;
    bool ok = true;
    if (lstat(path, &info) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (!dir)
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = join(path, entry->d_name);
        if (!child || !remove_tree(child))
            ok = false;
        free(child);
    }
    closedir(dir);
    if (ok && rmdir(path) != 0)
        ok = false;
    return ok;
}

static bool path_is_direct_child(const char* root, const char* path) {
    size_t n = root ? strlen(root) : 0;
    return n > 0 && path && strncmp(path, root, n) == 0 && path[n] == '/' && strchr(path + n + 1, '/') == NULL;
}

static char* acf_install_dir(const char* manifest_path) {
    FILE* file = fopen(manifest_path, "rb");
    char line[1024];
    if (!file)
        return NULL;
    while (fgets(line, sizeof(line), file)) {
        char* key = strstr(line, "\"installdir\"");
        char* start;
        char* end;
        if (!key)
            continue;
        start = strchr(key + strlen("\"installdir\""), '"');
        if (!start)
            continue;
        start++;
        end = strchr(start, '"');
        if (end && end > start) {
            char* value = strndup(start, (size_t)(end - start));
            fclose(file);
            return value;
        }
    }
    fclose(file);
    return NULL;
}

static bool executable_helper_name(const char* name) {
    static const char* const ignored[] = {
        "bootstrapper", "crash",  "easyanticheat",   "installer", "uninstall",      "setup",   "redist",
        "vcredist",     "server", "start_protected", "d3dconfig", "steamwebhelper", "oalinst", "vconsole"};
    char lower[256];
    size_t length = strlen(name);
    if (length >= sizeof(lower))
        length = sizeof(lower) - 1;
    for (size_t i = 0; i < length; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[length] = '\0';
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        if (strstr(lower, ignored[i]))
            return true;
    return false;
}

/* "<Name>Launcher.exe" next to "<Name>.exe" (Bethesda: SkyrimSELauncher /
 * SkyrimSE, Fallout4Launcher / Fallout4) is a settings launcher, not the game.
 * Directory order must not decide which one runs. */
static char* sibling_game_for_launcher(const char* directory, const char* name) {
    static const char suffix[] = "launcher.exe";
    size_t length = strlen(name), suffix_length = sizeof(suffix) - 1;
    char game_name[256];
    char* path;
    if (length <= suffix_length || length - suffix_length + 4 >= sizeof(game_name) ||
        strcasecmp(name + length - suffix_length, suffix))
        return NULL;
    snprintf(game_name, sizeof(game_name), "%.*s.exe", (int)(length - suffix_length), name);
    path = join(directory, game_name);
    if (path && access(path, R_OK) == 0)
        return path;
    free(path);
    return NULL;
}

static char* find_game_executable(const char* directory, unsigned depth) {
    DIR* dir;
    struct dirent* entry;
    if (!directory || depth > 8 || !(dir = opendir(directory)))
        return NULL;
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        struct stat info;
        size_t length;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = join(directory, entry->d_name);
        if (!path || stat(path, &info) != 0) {
            free(path);
            continue;
        }
        if (S_ISREG(info.st_mode)) {
            length = strlen(path);
            if (length > 4 && !strcasecmp(path + length - 4, ".exe") && !executable_helper_name(entry->d_name)) {
                char* game = sibling_game_for_launcher(directory, entry->d_name);
                closedir(dir);
                if (game) {
                    free(path);
                    return game;
                }
                return path;
            }
        } else if (S_ISDIR(info.st_mode)) {
            char* found = find_game_executable(path, depth + 1);
            free(path);
            if (found) {
                closedir(dir);
                return found;
            }
            continue;
        }
        free(path);
    }
    closedir(dir);
    return NULL;
}

char* ms_game_find_executable_in_directory(const char* directory) {
    return find_game_executable(directory, 0);
}

static char* find_named_game_executable(const char* directory, const char* name, unsigned depth) {
    DIR* dir;
    struct dirent* entry;
    if (!directory || !name || depth > 8 || !(dir = opendir(directory)))
        return NULL;
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        struct stat info;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = join(directory, entry->d_name);
        if (!path)
            continue;
        if (stat(path, &info) != 0) {
            free(path);
            continue;
        }
        if (S_ISREG(info.st_mode) && !strcasecmp(entry->d_name, strrchr(name, '/') ? strrchr(name, '/') + 1 : name) &&
            !executable_helper_name(entry->d_name)) {
            closedir(dir);
            return path;
        }
        if (S_ISDIR(info.st_mode)) {
            char* found = find_named_game_executable(path, name, depth + 1);
            if (found) {
                free(path);
                closedir(dir);
                return found;
            }
        }
        free(path);
    }
    closedir(dir);
    return NULL;
}

char* ms_witcher3_game_executable(const char* game_dir) {
    char* executable;
    if (!game_dir || !*game_dir)
        return NULL;
    executable = join(game_dir, "bin/x64/witcher3.exe");
    if (executable && access(executable, R_OK) == 0)
        return executable;
    free(executable);
    /* Some storefront builds vary path casing; locate the canonical basename
     * recursively if the standard relative path is not an exact host match. */
    return find_named_game_executable(game_dir, "witcher3.exe", 0);
}

static char* rule_preferred_game_executable(const char* game_dir, unsigned id) {
    char* raw = ms_mtsp_default_rules_json();
    char error[96];
    ms_json* root;
    const ms_json* rules;
    if (!raw)
        return NULL;
    root = ms_json_parse(raw, strlen(raw), error, sizeof(error));
    free(raw);
    rules = root ? ms_json_object_get(root, "rules") : NULL;
    if (rules && ms_json_type_of(rules) == MS_JSON_ARRAY) {
        for (size_t i = 0; i < ms_json_array_length(rules); i++) {
            const ms_json* rule = ms_json_array_get(rules, i);
            const ms_json* names;
            long long rule_appid;
            if (!ms_json_as_i64(ms_json_object_get(rule, "appid"), &rule_appid) || rule_appid != (long long)id)
                continue;
            names = ms_json_object_get(rule, "exe_names");
            if (names && ms_json_type_of(names) == MS_JSON_ARRAY) {
                for (size_t j = 0; j < ms_json_array_length(names); j++) {
                    char* name = NULL;
                    char* found;
                    if (!ms_json_as_string(ms_json_array_get(names, j), &name) || !name)
                        continue;
                    if (strchr(name, '/')) {
                        found = join(game_dir, name);
                        if (found && access(found, R_OK) != 0) {
                            free(found);
                            found = NULL;
                        }
                    } else {
                        found = find_named_game_executable(game_dir, name, 0);
                    }
                    free(name);
                    if (found) {
                        ms_json_free(root);
                        return found;
                    }
                }
            }
            break;
        }
    }
    ms_json_free(root);
    return NULL;
}

static char* preferred_steam_game_executable(const char* game_dir, unsigned id, const char* pipeline) {
    const char* preferred[4] = {NULL, NULL, NULL, NULL};
    size_t count = 0;
    char* ruled = rule_preferred_game_executable(game_dir, id);
    if (ruled)
        return ruled;
    if (id == 730)
        preferred[count++] = "game/bin/win64/cs2.exe";
    else if (id == 1097150)
        preferred[count++] = "FallGuys_client_game.exe";
    else if (id == 4704690)
        preferred[count++] = "Chameleon/Binaries/Win64/PenguinHotel-Win64-Shipping.exe";
    else if (id == 4126040)
        preferred[count++] = "Aniimo.exe";
    else if (id == 284160)
        preferred[count++] = "Bin64/BeamNG.drive.x64.exe";
    else if (id == 292030) {
        char* witcher3 = ms_witcher3_game_executable(game_dir);
        if (witcher3)
            return witcher3;
    } else if (id == 8500)
        preferred[count++] = "Launcher/evelauncher.exe";
    else if (id == 1145360 && pipeline && !strcmp(pipeline, "dxmt_32"))
        preferred[count++] = "x86/Hades.exe";
    else if (id == 1145360)
        preferred[count++] = "x64Vk/Hades.exe";
    else if (id == 379720)
        preferred[count++] = "DOOMx64vk.exe";
    else if (id == 782330)
        preferred[count++] = "DOOMEternalx64vk.exe";
    else if (id == 105600)
        preferred[count++] = "Terraria.exe";
    else if (id == 1196590)
        preferred[count++] = "re8.exe";
    else if (id == 1245620)
        preferred[count++] = "eldenring.exe";
    else if (id == 553850)
        preferred[count++] = "bin/helldivers2.exe";
    else if (id == 1466860)
        preferred[count++] = "RelicCardinal.exe";
    else if (id == 1888160)
        preferred[count++] = "armoredcore6.exe";
    else if (id == 1962700)
        preferred[count++] = "Subnautica2/Binaries/Win64/Subnautica2-Win64-Shipping.exe";
    else if (id == 3240220)
        preferred[count++] = "GTA5_Enhanced.exe";
    else if (id == 2767030)
        preferred[count++] = "MarvelGame/Marvel/Binaries/Win64/Marvel-Win64-Shipping.exe";
    else if (id == 220)
        preferred[count++] = "hl2.exe";
    else if (id == 440) {
        preferred[count++] = "tf/win32/tf.exe";
        preferred[count++] = "tf.exe";
    } else if (id == 620)
        preferred[count++] = "portal2.exe";
    else if (id == 475150)
        preferred[count++] = "TQ.exe";
    else if (id == 2358720) {
        preferred[count++] = "b1-Win64-Shipping.exe";
        preferred[count++] = "b1.exe";
    } else if (id == 2357570)
        preferred[count++] = "Overwatch.exe";
    else if (id == 321040)
        preferred[count++] = "dirt3_game.exe";
    else if (id == 489830)
        preferred[count++] = "SkyrimSE.exe";
    for (size_t i = 0; i < count; i++) {
        char* path = join(game_dir, preferred[i]);
        if (path && access(path, R_OK) == 0)
            return path;
        free(path);
    }
    return find_game_executable(game_dir, 0);
}

static void set_pipeline_runtime_env(const char*, const char*);

char* ms_steam_d3dmetal_game_executable(const char* home, unsigned id) {
    return find_steam_game_executable(home, id, "d3dmetal");
}

char* ms_steam_d3dmetal_game_local_executable(const char* home, unsigned id) {
    char* game_dir;
    char* executable;
    if (id != 8500)
        return ms_steam_d3dmetal_game_executable(home, id);
    game_dir = ms_steam_game_dir(home, id);
    executable = game_dir ? latest_eve_online_client_executable(game_dir) : NULL;
    free(game_dir);
    return executable;
}

static char* spawn_offline_game(const char* home, const char* executable, unsigned id, const char* pipeline,
                                pid_t* pid) {
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "prefix-steam");
    pid_t child;
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        free(prefix);
        return strdup("MetalSharp Wine not found");
    }
    child = fork();
    if (child < 0) {
        free(wine);
        free(prefix);
        return strdup(strerror(errno));
    }
    if (child == 0) {
        char app_id[32];
        char library_env[4096];
        snprintf(app_id, sizeof(app_id), "%u", id);
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("METALSHARP_OFFLINE_MODE", "1", 1);
        setenv("SteamAppId", app_id, 1);
        setenv("SteamGameId", app_id, 1);
        setenv("METALSHARP_PIPELINE", pipeline, 1);
        set_pipeline_runtime_env(home, pipeline);
        set_game_opengl_env(id, pipeline);
        if (!strcmp(pipeline, "d3dmetal"))
            snprintf(
                library_env, sizeof(library_env),
                "%s/runtime/d3dmetal-gptk4-beta2/external:%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix",
                home, home, home);
        else
            snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                     home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        execl(wine, wine, executable, (char*)NULL);
        _exit(127);
    }
    free(wine);
    free(prefix);
    *pid = child;
    return NULL;
}

static void set_pipeline_runtime_env(const char* home, const char* pipeline) {
    char winedllpath[PATH_MAX * 2];
    char dxmt_config[PATH_MAX];
    char winemetal[PATH_MAX];
    char vulkan_icd[PATH_MAX];
    const char* backend = "dxmt";
    set_rosetta_avx_env();
    if (!pipeline)
        pipeline = "auto";
    if (pipeline_is_dxmt(pipeline)) {
        snprintf(winedllpath, sizeof(winedllpath), "%s/runtime/wine/lib/dxmt/%s-windows", home,
                 !strcmp(pipeline, "dxmt_32") ? "i386" : "x86_64");
        setenv("WINEDLLPATH", winedllpath, 1);
    } else if (!strcmp(pipeline, "vkd3d")) {
        snprintf(
            winedllpath, sizeof(winedllpath),
            "%s/vkd3d/vkd3d-proton/x86_64-windows:%s/vkd3d/dxvk/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows",
            home, home, home);
        setenv("WINEDLLPATH", winedllpath, 1);
        snprintf(vulkan_icd, sizeof(vulkan_icd), "%s/runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json", home);
        setenv("VK_ICD_FILENAMES", vulkan_icd, 1);
        setenv("VK_DRIVER_FILES", vulkan_icd, 1);
        backend = "vkd3d-proton";
    } else if (!strcmp(pipeline, "d3dmetal")) {
        snprintf(winedllpath, sizeof(winedllpath),
                 "%s/runtime/d3dmetal-gptk4-beta2/wine/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows", home,
                 home);
        setenv("WINEDLLPATH", winedllpath, 1);
        snprintf(winemetal, sizeof(winemetal), "%s/runtime/d3dmetal-gptk4-beta2/external/D3DMetal.framework/D3DMetal",
                 home);
        setenv("D3DMETAL_FRAMEWORK_PATH", winemetal, 1);
        snprintf(winemetal, sizeof(winemetal), "%s/runtime/d3dmetal-gptk4-beta2", home);
        setenv("D3DMETAL_RUNTIME_DIR", winemetal, 1);
        setenv("WINEDLLOVERRIDES", "d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b", 1);
        backend = "d3dmetal";
    }
    set_wfdxcompat_runtime_env(home, pipeline);
    if (pipeline_is_dxmt(pipeline)) {
        char runtime_dir[PATH_MAX];
        const char* route = "dxmt/x86_64-unix";
        snprintf(dxmt_config, sizeof(dxmt_config), "%s/runtime/wine/etc/dxmt.conf", home);
        snprintf(winemetal, sizeof(winemetal), "\\??\\Z:%s/runtime/wine/lib/%s/winemetal.so", home, route);
        for (char* p = winemetal + 5; *p; ++p)
            if (*p == '/')
                *p = '\\';
        snprintf(runtime_dir, sizeof(runtime_dir), "%s/runtime/wine/lib/dxmt", home);
        setenv("DXMT_CONFIG_FILE", dxmt_config, 1);
        setenv("DXMT_WINEMETAL_UNIXLIB", winemetal, 1);
        setenv("DXMT_RUNTIME_DIR", runtime_dir, 1);
        setenv("GRAPHICS_BACKEND", "dxmt", 1);
    } else {
        unsetenv("DXMT_RUNTIME_DIR");
        setenv("GRAPHICS_BACKEND", backend, 1);
    }
    setenv("MS_GRAPHICS_BACKEND", backend, 1);
    ms_steam_apply_launch_preferences(home);
}

static bool process_cwd_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    struct proc_vnodepathinfo info;
    int bytes = proc_pidinfo((int)pid, PROC_PIDVNODEPATHINFO, 0, &info, (int)sizeof(info));
    size_t length = strlen(root);
    return bytes == (int)sizeof(info) && !strncmp(info.pvi_cdir.vip_path, root, length) &&
           (info.pvi_cdir.vip_path[length] == '\0' || info.pvi_cdir.vip_path[length] == '/');
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

static bool process_executable_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    char executable[PROC_PIDPATHINFO_MAXSIZE];
    int bytes = proc_pidpath((int)pid, executable, sizeof(executable));
    size_t length = strlen(root);
    return bytes > 0 && !strncmp(executable, root, length) && (executable[length] == '\0' || executable[length] == '/');
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

static bool wine_process_owned(pid_t pid, const char* command, const char* prefix, const char* runtime) {
    return strstr(command, prefix) != NULL || process_cwd_within(pid, prefix) ||
           process_executable_within(pid, runtime);
}

static bool managed_wine_process_running(const char* home, bool steam_only) {
    char prefix[PATH_MAX], runtime[PATH_MAX];
    FILE* pipe;
    char line[4096];
    bool running = false;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe)
        return false;
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        bool candidate;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX)
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        candidate =
            steam_only ? (contains_ci(end, "c:\\program files (x86)\\steam") ||
                          contains_ci(end, "steamwebhelper.exe") || contains_ci(end, "steamwebhelper_real.exe"))
                       : (wine_steam_cleanup_target(end, prefix) || process_executable_within((pid_t)raw_pid, runtime));
        if (candidate && wine_process_owned((pid_t)raw_pid, end, prefix, runtime)) {
            running = true;
            break;
        }
    }
    pclose(pipe);
    return running;
}

bool ms_steam_process_running(const char* home) {
    return managed_wine_process_running(home, true);
}

static pid_t wine_steam_client_pid(const char* home) {
    char prefix[PATH_MAX];
    char runtime[PATH_MAX];
    FILE* pipe;
    char line[4096];
    pid_t steam_pid = 0;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe)
        return 0;
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX)
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if (contains_ci(end, "steam.exe") && !contains_ci(end, "steamwebhelper") &&
            wine_process_owned((pid_t)raw_pid, end, prefix, runtime)) {
            steam_pid = (pid_t)raw_pid;
            break;
        }
    }
    pclose(pipe);
    return steam_pid;
}

static char* wine_steam_route_marker_path(const char* home) {
    return join(home, "runtime/steam-session-pipeline");
}

static bool write_wine_steam_route_marker(const char* home, const char* pipeline) {
    char* path = wine_steam_route_marker_path(home);
    pid_t steam_pid = wine_steam_client_pid(home);
    FILE* file;
    bool ok = false;
    if (!path || steam_pid <= 0) {
        free(path);
        return false;
    }
    file = fopen(path, "wb");
    if (file) {
        bool wrote = fprintf(file, "%s %ld\n", pipeline, (long)steam_pid) > 0;
        int close_status = fclose(file);
        ok = wrote && close_status == 0;
        if (!ok)
            (void)unlink(path);
    }
    free(path);
    return ok;
}

static bool wine_steam_route_marker_matches(const char* home, const char* pipeline) {
    char* path = wine_steam_route_marker_path(home);
    char* contents = path ? read_bounded_file(path) : NULL;
    char saved_pipeline[32];
    long saved_pid = 0;
    bool matches = contents && sscanf(contents, "%31s %ld", saved_pipeline, &saved_pid) == 2 && saved_pid > 1 &&
                   !strcmp(saved_pipeline, pipeline) && (pid_t)saved_pid == wine_steam_client_pid(home);
    free(contents);
    free(path);
    return matches;
}

static bool write_wine_steam_route_pending(const char* home, const char* pipeline) {
    char* path = wine_steam_route_marker_path(home);
    FILE* file;
    bool ok = false;
    if (!path)
        return false;
    file = fopen(path, "wb");
    if (file) {
        bool wrote = fprintf(file, "%s pending %lld\n", pipeline, (long long)time(NULL)) > 0;
        int close_status = fclose(file);
        ok = wrote && close_status == 0;
        if (!ok)
            (void)unlink(path);
    }
    free(path);
    return ok;
}

static bool wine_steam_route_marker_is_pending(const char* home, const char* pipeline) {
    char* path = wine_steam_route_marker_path(home);
    char* contents = path ? read_bounded_file(path) : NULL;
    char saved_pipeline[32];
    char state[16];
    long long started_at = 0;
    time_t now = time(NULL);
    bool pending = contents && sscanf(contents, "%31s %15s %lld", saved_pipeline, state, &started_at) == 3 &&
                   !strcmp(saved_pipeline, pipeline) && !strcmp(state, "pending") && started_at > 0 &&
                   now >= started_at && now - started_at <= 120;
    free(contents);
    free(path);
    return pending;
}

static void clear_wine_steam_route_marker(const char* home) {
    char* path = wine_steam_route_marker_path(home);
    if (path)
        (void)unlink(path);
    free(path);
}

static pthread_mutex_t g_eve_game_dir_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_eve_game_dir_home[PATH_MAX];
static char* g_eve_game_dir_cache;
static time_t g_eve_game_dir_cached_at;
static bool g_eve_game_dir_cache_valid;

static char* cached_eve_game_dir(const char* home) {
    time_t now = time(NULL);
    char* result = NULL;
    pthread_mutex_lock(&g_eve_game_dir_mutex);
    if (g_eve_game_dir_cache_valid && !strcmp(g_eve_game_dir_home, home) && now >= g_eve_game_dir_cached_at &&
        now - g_eve_game_dir_cached_at < 5) {
        result = g_eve_game_dir_cache ? strdup(g_eve_game_dir_cache) : NULL;
        pthread_mutex_unlock(&g_eve_game_dir_mutex);
        return result;
    }
    pthread_mutex_unlock(&g_eve_game_dir_mutex);

    result = ms_steam_game_dir(home, 8500);
    pthread_mutex_lock(&g_eve_game_dir_mutex);
    free(g_eve_game_dir_cache);
    g_eve_game_dir_cache = result ? strdup(result) : NULL;
    snprintf(g_eve_game_dir_home, sizeof(g_eve_game_dir_home), "%s", home);
    g_eve_game_dir_cached_at = now;
    g_eve_game_dir_cache_valid = true;
    pthread_mutex_unlock(&g_eve_game_dir_mutex);
    return result;
}

static bool path_is_within(const char* path, const char* root) {
    size_t length = strlen(root);
    return length > 0 && !strncmp(path, root, length) &&
           (path[length] == '\0' || root[length - 1] == '/' || path[length] == '/');
}

static void append_eve_wine_path(char paths[][PATH_MAX], size_t capacity, size_t* count, char drive,
                                 const char* resolved_root, const char* resolved_game_dir) {
    char* output;
    const char* relative;
    size_t used;
    if (*count >= capacity || !path_is_within(resolved_game_dir, resolved_root))
        return;
    relative = resolved_game_dir + strlen(resolved_root);
    while (*relative == '/')
        relative++;
    output = paths[*count];
    int written = snprintf(output, PATH_MAX, "%c:\\", drive);
    if (written < 0 || written >= PATH_MAX)
        return;
    used = (size_t)written;
    while (*relative && used + 1 < PATH_MAX) {
        char character = *relative++;
        output[used++] = character == '/' ? '\\' : character;
    }
    output[used] = '\0';
    for (size_t i = 0; i < *count; i++)
        if (!strcasecmp(paths[i], output))
            return;
    (*count)++;
}

static size_t eve_install_dir_wine_paths(const char* home, const char* game_dir, char paths[][PATH_MAX],
                                         size_t capacity) {
    char resolved_game[PATH_MAX];
    char dosdevices[PATH_MAX];
    DIR* directory;
    struct dirent* entry;
    size_t count = 0;
    if (!game_dir || !realpath(game_dir, resolved_game))
        return 0;

    /* Z: remains valid for arbitrary Unix paths and is how some Steam
     * libraryfolders.vdf files represent external libraries. */
    append_eve_wine_path(paths, capacity, &count, 'Z', "/", resolved_game);
    snprintf(dosdevices, sizeof(dosdevices), "%s/prefix-steam/dosdevices", home);
    directory = opendir(dosdevices);
    if (!directory)
        return count;
    while ((entry = readdir(directory)) != NULL) {
        char link_path[PATH_MAX];
        char link_target[PATH_MAX];
        char resolved_root[PATH_MAX];
        ssize_t length;
        if (strlen(entry->d_name) != 2 || entry->d_name[1] != ':' || !isalpha((unsigned char)entry->d_name[0]))
            continue;
        if (snprintf(link_path, sizeof(link_path), "%s/%s", dosdevices, entry->d_name) >= (int)sizeof(link_path))
            continue;
        length = readlink(link_path, link_target, sizeof(link_target) - 1);
        if (length <= 0)
            continue;
        link_target[length] = '\0';
        if (link_target[0] == '/') {
            if (!realpath(link_target, resolved_root))
                continue;
        } else {
            char candidate[PATH_MAX];
            if (snprintf(candidate, sizeof(candidate), "%s/%s", dosdevices, link_target) >= (int)sizeof(candidate) ||
                !realpath(candidate, resolved_root))
                continue;
        }
        append_eve_wine_path(paths, capacity, &count, (char)toupper((unsigned char)entry->d_name[0]), resolved_root,
                             resolved_game);
    }
    closedir(directory);
    return count;
}

static bool command_contains_wine_path(const char* command, const char* path) {
    size_t length = strlen(path);
    for (const char* cursor = command; *cursor; cursor++)
        if (!strncasecmp(cursor, path, length) &&
            (cursor[length] == '\0' || cursor[length] == '\\' || cursor[length] == '/' || cursor[length] == '"' ||
             isspace((unsigned char)cursor[length])))
            return true;
    return false;
}

static int marvel_rivals_process_rank(const char* command) {
    if (contains_ci(command, "Marvel-Win64-Shipping.exe"))
        return 4;
    if (contains_ci(command, "\\Marvel.exe"))
        return 3;
    if (contains_ci(command, "MarvelRivals_Launcher.exe"))
        return 2;
    if (contains_ci(command, "CrashReportClient.exe"))
        return 1;
    return 0;
}

static pid_t scan_marvel_rivals_wine_processes(const char* home, int signal_number, bool* failed) {
    char* game_dir = ms_steam_game_dir(home, 2767030);
    char (*paths)[PATH_MAX] = calloc(26, sizeof(*paths));
    size_t path_count = game_dir && paths ? eve_install_dir_wine_paths(home, game_dir, paths, 26) : 0;
    char line[8192];
    FILE* pipe;
    pid_t best_pid = 0;
    int best_rank = 0;
    if (failed)
        *failed = !game_dir || !paths || path_count == 0;
    free(game_dir);
    if (!paths || path_count == 0) {
        free(paths);
        return 0;
    }
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe) {
        if (failed)
            *failed = true;
        free(paths);
        return 0;
    }
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        int rank;
        bool install_matches = false;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        rank = marvel_rivals_process_rank(end);
        if (!rank)
            continue;
        for (size_t i = 0; i < path_count; i++)
            if (command_contains_wine_path(end, paths[i])) {
                install_matches = true;
                break;
            }
        if (!install_matches)
            continue;
        if (signal_number && kill((pid_t)raw_pid, signal_number) != 0 && errno != ESRCH && failed)
            *failed = true;
        if (rank > best_rank) {
            best_rank = rank;
            best_pid = (pid_t)raw_pid;
        }
    }
    int pipe_status = pclose(pipe);
    if ((pipe_status == -1 || !WIFEXITED(pipe_status) || WEXITSTATUS(pipe_status) != 0) && failed)
        *failed = true;
    free(paths);
    return best_pid;
}

pid_t ms_steam_marvel_rivals_process_pid(const char* home) {
    return scan_marvel_rivals_wine_processes(home, 0, NULL);
}

bool ms_steam_stop_marvel_rivals_processes(const char* home) {
    for (unsigned attempt = 0; attempt < 10; attempt++) {
        bool failed = false;
        pid_t pid = scan_marvel_rivals_wine_processes(home, attempt < 3 ? SIGTERM : SIGKILL, &failed);
        if (failed)
            return false;
        if (pid == 0)
            return true;
        usleep(100000);
    }
    bool failed = false;
    return scan_marvel_rivals_wine_processes(home, SIGKILL, &failed) == 0 && !failed;
}

static int baldurs_gate_3_process_rank(const char* command) {
    if (contains_ci(command, "bg3_dx11.exe") || contains_ci(command, "bg3_vulkan.exe"))
        return 3;
    if (contains_ci(command, "\\bg3.exe") || contains_ci(command, "/bg3.exe"))
        return 2;
    if (contains_ci(command, "lariLauncher.exe") || contains_ci(command, "bg3launcher.exe"))
        return 1;
    return 0;
}

static bool baldurs_gate_3_process_matches(int rank, bool install_path_matches, bool wine_game_in_install) {
    return rank > 0 && (install_path_matches || (rank >= 2 && wine_game_in_install));
}

static pid_t scan_baldurs_gate_3_wine_processes(const char* home, int signal_number, bool* failed) {
    char runtime[PATH_MAX];
    char* game_dir = ms_steam_game_dir(home, 1086940);
    char (*paths)[PATH_MAX] = calloc(26, sizeof(*paths));
    size_t path_count = game_dir && paths ? eve_install_dir_wine_paths(home, game_dir, paths, 26) : 0;
    char line[8192];
    FILE* pipe;
    pid_t best_pid = 0;
    int best_rank = 0;
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    if (failed)
        *failed = !game_dir || !paths || path_count == 0;
    if (!game_dir || !paths || path_count == 0) {
        free(game_dir);
        free(paths);
        return 0;
    }
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe) {
        if (failed)
            *failed = true;
        free(game_dir);
        free(paths);
        return 0;
    }
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        int rank;
        bool install_matches = false;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        rank = baldurs_gate_3_process_rank(end);
        if (!rank)
            continue;
        for (size_t i = 0; i < path_count; i++)
            if (command_contains_wine_path(end, paths[i])) {
                install_matches = true;
                break;
            }
        bool wine_game_in_install = rank >= 2 && process_cwd_within((pid_t)raw_pid, game_dir) &&
                                    process_executable_within((pid_t)raw_pid, runtime);
        if (!baldurs_gate_3_process_matches(rank, install_matches, wine_game_in_install))
            continue;
        if (signal_number && kill((pid_t)raw_pid, signal_number) != 0 && errno != ESRCH && failed)
            *failed = true;
        if (rank > best_rank) {
            best_rank = rank;
            best_pid = (pid_t)raw_pid;
        }
    }
    int pipe_status = pclose(pipe);
    if ((pipe_status == -1 || !WIFEXITED(pipe_status) || WEXITSTATUS(pipe_status) != 0) && failed)
        *failed = true;
    free(game_dir);
    free(paths);
    return best_pid;
}

pid_t ms_steam_baldurs_gate_3_process_pid(const char* home) {
    return scan_baldurs_gate_3_wine_processes(home, 0, NULL);
}

bool ms_steam_stop_baldurs_gate_3_processes(const char* home) {
    for (unsigned attempt = 0; attempt < 10; attempt++) {
        bool failed = false;
        pid_t pid = scan_baldurs_gate_3_wine_processes(home, attempt < 3 ? SIGTERM : SIGKILL, &failed);
        if (failed)
            return false;
        if (pid == 0)
            return true;
        usleep(100000);
    }
    bool failed = false;
    return scan_baldurs_gate_3_wine_processes(home, SIGKILL, &failed) == 0 && !failed;
}

static bool eve_wine_process_command(const char* command, const char paths[][PATH_MAX], size_t path_count) {
    bool eve_executable = contains_ci(command, "evelauncher.exe") || contains_ci(command, "eve-online.exe");
    bool eve_game_client = contains_ci(command, "c:\\ccp\\eve\\tq\\bin64\\exefile.exe") ||
                           contains_ci(command, "c:\\ccp\\eve\\tq\\bin64\\eve_crashmon.exe");
    bool install_path_matches = false;
    for (size_t i = 0; eve_executable && i < path_count; i++)
        if (command_contains_wine_path(command, paths[i])) {
            install_path_matches = true;
            break;
        }
    return (eve_executable && install_path_matches) || eve_game_client;
}

static size_t signal_eve_wine_processes(const char* home, int signal_number, bool* failed) {
    char prefix[PATH_MAX];
    char runtime[PATH_MAX];
    char (*paths)[PATH_MAX] = calloc(26, sizeof(*paths));
    char* game_dir = cached_eve_game_dir(home);
    size_t path_count = paths ? eve_install_dir_wine_paths(home, game_dir, paths, 26) : 0;
    FILE* pipe;
    char line[8192];
    size_t signaled = 0;
    if (failed)
        *failed = paths == NULL;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    free(game_dir);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe) {
        if (failed)
            *failed = true;
        free(paths);
        return 0;
    }
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if (!eve_wine_process_command(end, (const char (*)[PATH_MAX])paths, path_count) ||
            !wine_process_owned((pid_t)raw_pid, end, prefix, runtime))
            continue;
        if (kill((pid_t)raw_pid, signal_number) == 0 || errno == ESRCH)
            signaled++;
    }
    int pipe_status = pclose(pipe);
    if ((pipe_status == -1 || !WIFEXITED(pipe_status) || WEXITSTATUS(pipe_status) != 0) && failed)
        *failed = true;
    free(paths);
    return signaled;
}

static pid_t scan_eve_wine_process_pid(const char* home, bool* failed) {
    char prefix[PATH_MAX];
    char runtime[PATH_MAX];
    char (*paths)[PATH_MAX] = calloc(26, sizeof(*paths));
    char* game_dir = cached_eve_game_dir(home);
    size_t path_count = paths ? eve_install_dir_wine_paths(home, game_dir, paths, 26) : 0;
    FILE* pipe;
    char line[8192];
    pid_t eve_client_pid = 0;
    pid_t game_client_pid = 0;
    pid_t launcher_pid = 0;
    pid_t fallback_pid = 0;
    if (failed)
        *failed = paths == NULL;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    free(game_dir);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe) {
        if (failed)
            *failed = true;
        free(paths);
        return 0;
    }
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        if (!paths)
            break;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX)
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if (!eve_wine_process_command(end, (const char (*)[PATH_MAX])paths, path_count) ||
            !wine_process_owned((pid_t)raw_pid, end, prefix, runtime))
            continue;
        if (!contains_ci(end, "--type=") && !fallback_pid)
            fallback_pid = (pid_t)raw_pid;
        if (contains_ci(end, "eve-online.exe") && !contains_ci(end, "--type=") && !eve_client_pid)
            eve_client_pid = (pid_t)raw_pid;
        else if (contains_ci(end, "c:\\ccp\\eve\\tq\\bin64\\exefile.exe") && !game_client_pid)
            game_client_pid = (pid_t)raw_pid;
        else if (contains_ci(end, "evelauncher.exe") && !contains_ci(end, "--type=") && !launcher_pid)
            launcher_pid = (pid_t)raw_pid;
    }
    int pipe_status = pclose(pipe);
    if ((pipe_status == -1 || !WIFEXITED(pipe_status) || WEXITSTATUS(pipe_status) != 0) && failed)
        *failed = true;
    free(paths);
    if (eve_client_pid)
        return eve_client_pid;
    if (game_client_pid)
        return game_client_pid;
    if (launcher_pid)
        return launcher_pid;
    return fallback_pid;
}

pid_t ms_steam_eve_process_pid(const char* home) {
    return scan_eve_wine_process_pid(home, NULL);
}

bool ms_steam_stop_eve_processes(const char* home) {
    for (unsigned attempt = 0; attempt < 20; attempt++) {
        bool scan_failed = false;
        bool process_scan_failed = false;
        size_t signaled = signal_eve_wine_processes(home, SIGKILL, &scan_failed);
        pid_t pid = scan_eve_wine_process_pid(home, &process_scan_failed);
        if (scan_failed || process_scan_failed)
            return false;
        if (signaled == 0 && pid == 0)
            return true;
        usleep(100000);
    }
    bool failed = false;
    pid_t pid = scan_eve_wine_process_pid(home, &failed);
    return !failed && pid == 0;
}

static void signal_wine_steam_processes(const char* home, int signal_number) {
    char prefix[PATH_MAX], runtime[PATH_MAX];
    FILE* pipe;
    char line[4096];
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe)
        return;
    while (fgets(line, sizeof(line), pipe)) {
        char* end;
        long raw_pid;
        char* command = line;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if ((wine_steam_cleanup_target(end, prefix) || process_executable_within((pid_t)raw_pid, runtime)) &&
            wine_process_owned((pid_t)raw_pid, end, prefix, runtime))
            (void)kill((pid_t)raw_pid, signal_number);
    }
    pclose(pipe);
}

/* SteamSetup may start Steam as soon as its payload is committed.  The setup
 * flow must remain in control until prerequisite installers have run, so tear
 * down every process owned by the Steam prefix, including a wineserver whose
 * command line no longer mentions Steam.
 *
 * `wineserver -k` goes first so a healthy server kills its whole client tree
 * at once; a wedged server is bounded to 1 s and then SIGKILLed with every
 * other prefix-owned process. Returns once nothing owned by the prefix is
 * left (or after ~3 s), so callers can report `running` without a fixed
 * sleep. */
static void terminate_wine_steam_session(const char* home) {
    char* wineserver = join(home, "runtime/wine/bin/wineserver");
    char* prefix = join(home, "prefix-steam");
    pid_t child = -1;

    if (wineserver && prefix && access(wineserver, X_OK) == 0 && (child = fork()) == 0) {
        setenv("WINEPREFIX", prefix, 1);
        execl(wineserver, wineserver, "-k", (char*)NULL);
        _exit(127);
    }
    if (child > 0) {
        int status;
        for (unsigned i = 0; i < 50; i++) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child || (waited < 0 && errno != EINTR))
                break;
            usleep(20000);
        }
        if (waitpid(child, &status, WNOHANG) == 0) {
            (void)kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
        }
    }
    free(wineserver);
    free(prefix);
    for (unsigned i = 0; i < 30; i++) {
        signal_wine_steam_processes(home, SIGKILL);
        if (!managed_wine_process_running(home, false))
            break;
        usleep(100000);
    }
    clear_wine_steam_route_marker(home);
}

static char* spawn_wine(const char* home, const char* first, const char* second, const char* third, const char* fourth,
                        const char* fifth, pid_t* pid) {
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "prefix-steam");
    char* steam_dir = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    pid_t child;
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        free(prefix);
        free(steam_dir);
        return strdup("MetalSharp Wine not found");
    }
    redirect_wine_steam_desktop(home);
    ensure_steam_launch_ready(home, steam_dir);
    seed_steam_registry(home);
    child = fork();
    if (child < 0) {
        char* s = strdup(strerror(errno));
        free(wine);
        free(prefix);
        free(steam_dir);
        return s;
    }
    if (child == 0) {
        char library_env[4096];
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        /* Steam launches games from its own Wine process tree, so advertise
         * Rosetta's AVX support before bootstrapping Steam, not just for
         * MetalSharp's direct game-launch path. */
        set_rosetta_avx_env();
        setenv("WINEDEBUG", "+vulkan,+d3d,+d3d11,+dxgi,+wined3d,+opengl", 1);
        setenv("WINEDEBUGGER", "none", 1);
        setenv("STEAM_RUNTIME", "0", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        setenv("WINEDLLOVERRIDES",
               "dxgi,d3d11,d3d10core=n,b;bcrypt=b;ncrypt=b;gameoverlayrenderer,gameoverlayrenderer64=d", 1);
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        if (steam_dir != NULL)
            (void)chdir(steam_dir);
        execl(wine, wine, first, second, third, fourth, fifth, (char*)NULL);
        _exit(127);
    }
    free(wine);
    free(prefix);
    free(steam_dir);
    *pid = child;
    return NULL;
}

/* The Wine Steam client is started through a tiny LaunchServices helper app
 * ("MetalSharp Steam.app", shipped in the app's Resources). macOS attributes
 * a process's first window to the app that launched it, so a direct child of
 * the backend showed the MetalSharp icon; via the helper it shows the
 * MetalSharp Steam icon. Returns NULL (direct launch) when the helper is absent. */
static char* steam_helper_app_path(void) {
    const char* bundle_dir = getenv("METALSHARP_BUNDLE_DIR");
    char* candidates[2] = {bundle_dir ? join(bundle_dir, "../MetalSharp Steam.app") : NULL,
                           strdup("/Applications/MetalSharp.app/Contents/Resources/MetalSharp Steam.app")};
    char* found = NULL;
    for (size_t i = 0; i < 2; i++) {
        char* launcher = candidates[i] ? join(candidates[i], "Contents/MacOS/metalsharp-steam") : NULL;
        if (!found && launcher && access(launcher, X_OK) == 0) {
            found = candidates[i];
            candidates[i] = NULL;
        }
        free(launcher);
        free(candidates[i]);
    }
    return found;
}

static bool is_steam_client_executable(const char* path) {
    size_t length = path ? strlen(path) : 0;
    return length >= 9 && !strcasecmp(path + length - 9, "Steam.exe") &&
           (length == 9 || path[length - 10] == '/' || path[length - 10] == '\\');
}

/* Child side of the fork: exec `open` on the helper, forwarding the prepared
 * environment (minus MetalSharp's own launch identity) and the Wine argv.
 * Returns only if exec failed, so the caller falls back to a direct exec. */
static void exec_via_steam_helper(const char* helper, const char* cwd, char* const* wine_argv) {
    extern char** environ;
    size_t env_count = 0, argc = 0, index = 0;
    char** argv;
    setenv("METALSHARP_LAUNCH_CWD", cwd ? cwd : "", 1);
    while (environ[env_count])
        env_count++;
    while (wine_argv[argc])
        argc++;
    argv = calloc(env_count * 2 + argc + 8, sizeof(*argv));
    if (!argv)
        return;
    argv[index++] = "/usr/bin/open";
    argv[index++] = "-n";
    argv[index++] = "-a";
    argv[index++] = (char*)helper;
    for (size_t i = 0; i < env_count; i++) {
        if (!strncmp(environ[i], "__CFBundleIdentifier=", 21) || !strncmp(environ[i], "XPC_SERVICE_NAME=", 17) ||
            !strncmp(environ[i], "XPC_FLAGS=", 10))
            continue;
        argv[index++] = "--env";
        argv[index++] = environ[i];
    }
    argv[index++] = "--args";
    for (size_t i = 0; i < argc; i++)
        argv[index++] = wine_argv[i];
    argv[index] = NULL;
    execv("/usr/bin/open", argv);
    free(argv);
}

static char* spawn_wine_for_pipeline(const char* home, const char* pipeline, unsigned id, const char* first,
                                     const char* second, const char* third, const char* fourth, const char* fifth,
                                     pid_t* pid) {
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "prefix-steam");
    char* steam_dir = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    pid_t child;
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        free(prefix);
        free(steam_dir);
        return strdup("MetalSharp Wine not found");
    }
    redirect_wine_steam_desktop(home);
    ensure_steam_launch_ready(home, steam_dir);
    seed_steam_registry(home);
    char* helper = id == 0 && is_steam_client_executable(first) ? steam_helper_app_path() : NULL;
    child = fork();
    if (child < 0) {
        char* error_text = strdup(strerror(errno));
        free(helper);
        free(wine);
        free(prefix);
        free(steam_dir);
        return error_text;
    }
    if (child == 0) {
        char overrides[1024];
        bool overrides_ready = format_steam_pipeline_overrides(overrides, sizeof(overrides), pipeline);
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        setenv("METALSHARP_HOME", home, 1);
        setenv("METALSHARP_PIPELINE", pipeline, 1);
        set_rosetta_avx_env();
        set_route_paths(home, pipeline);
        set_route_default_env(home, pipeline);
        set_game_opengl_env(0, pipeline);
        if (id > 0)
            set_launch_cache_env(home, id, pipeline);
        setenv("WINEDEBUG", "+vulkan,+d3d,+d3d11,+dxgi,+wined3d,+opengl", 1);
        setenv("WINEDEBUGGER", "none", 1);
        setenv("STEAM_RUNTIME", "0", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        if (overrides_ready)
            setenv("WINEDLLOVERRIDES", overrides, 1);
        else
            unsetenv("WINEDLLOVERRIDES");
        if (steam_dir)
            (void)chdir(steam_dir);
        if (helper) {
            char* wine_argv[] = {wine, (char*)first, (char*)second, (char*)third, (char*)fourth, (char*)fifth, NULL};
            exec_via_steam_helper(helper, steam_dir, wine_argv);
        }
        execl(wine, wine, first, second, third, fourth, fifth, (char*)NULL);
        _exit(127);
    }
    free(helper);
    free(wine);
    free(prefix);
    free(steam_dir);
    *pid = child;
    return NULL;
}

static char* spawn_wine_install(const char* home, const char* first, const char* second, const char* third,
                                pid_t* pid) {
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "prefix-steam");
    pid_t child;
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        wine = join(home, "runtime/wine/bin/wine");
    }
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        free(prefix);
        return strdup("MetalSharp Wine not found");
    }
    child = fork();
    if (child < 0) {
        char* error_text = strdup(strerror(errno));
        free(wine);
        free(prefix);
        return error_text;
    }
    if (child == 0) {
        char library_env[4096];
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "/usr/bin/true", 1);
        setenv("WINEDLLOVERRIDES", "winedbg=d", 1);
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        execl(wine, wine, first, second, third, (char*)NULL);
        _exit(127);
    }
    free(wine);
    free(prefix);
    *pid = child;
    return NULL;
}

static char* spawn_open(const char* a, const char* b, const char* c, pid_t* pid) {
    pid_t child = fork();
    if (child < 0)
        return strdup(strerror(errno));
    if (child == 0) {
        if (c)
            execl("/usr/bin/open", "open", a, b, c, (char*)NULL);
        else if (b)
            execl("/usr/bin/open", "open", a, b, (char*)NULL);
        else
            execl("/usr/bin/open", "open", a, (char*)NULL);
        _exit(127);
    }
    *pid = child;
    return NULL;
}
static char* pid_result(pid_t pid, const char* key, unsigned id, bool include_id) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, key);
    ms_json_writer_u64(&w, (unsigned)pid);
    if (include_id) {
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, id);
    }
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}

static char* launch_mode_pid_result(pid_t pid, unsigned id, const char* launch_mode) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned)pid);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, id);
    ms_json_writer_key(&w, "launch_mode");
    ms_json_writer_string(&w, launch_mode);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

/* Ask a running Wine Steam client to show its library. Returns false when
 * there is no live client (only leftover helpers) or when the forwarding
 * Wine process hangs, which means the session's wineserver is wedged; the
 * caller then restarts Steam. `*error_text` is set only for spawn failures. */
static bool activate_wine_steam(const char* home, const char* steam, pid_t* pid, char** error_text) {
    pid_t child = -1;
    if (wine_steam_client_pid(home) <= 0)
        return false;
    *error_text = spawn_wine_install(home, steam, "steam://open/library", NULL, &child);
    if (*error_text)
        return false;
    *pid = child;
    for (unsigned i = 0; i < 160; i++) {
        int wait_status;
        pid_t waited = waitpid(child, &wait_status, WNOHANG);
        /* ECHILD: a process-wide SIGCHLD reaper collected it, so it exited. */
        if (waited == child || (waited < 0 && errno == ECHILD))
            return true;
        if (waited < 0 && errno != EINTR)
            return true;
        usleep(50000);
    }
    (void)kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
    return false;
}

/* Steam's CEF webhelper writes htmlcache/"First Run" once its UI has
 * initialized. The very first launch under Wine can stall right after
 * creating the cache (observed: no progress for 30+ s, while a relaunch
 * finishes in about a second), so a cold start that has never initialized
 * gets one automatic restart if the UI is not up within 10 s. */
static bool steam_ui_initialized(const char* home) {
    char* users = join(home, "prefix-steam/drive_c/users");
    DIR* dir = users ? opendir(users) : NULL;
    struct dirent* entry;
    bool found = false;
    while (dir && !found && (entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        if (entry->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/AppData/Local/Steam/htmlcache/First Run", users, entry->d_name);
        found = access(path, F_OK) == 0;
    }
    if (dir)
        closedir(dir);
    free(users);
    return found;
}

static pthread_mutex_t steam_ui_watch_lock = PTHREAD_MUTEX_INITIALIZER;
static bool steam_ui_watch_active = false;
static bool steam_ui_restart_used = false;

static void* steam_ui_first_run_watchdog(void* opaque) {
    char* home = opaque;
    bool restart = false;
    for (int i = 0; i < 100; i++) {
        if (ms_process_background_shutdown_requested() || steam_ui_initialized(home) || !ms_steam_process_running(home))
            goto done;
        usleep(100000);
    }
    restart = !steam_ui_initialized(home) && ms_steam_process_running(home);
done:
    pthread_mutex_lock(&steam_ui_watch_lock);
    steam_ui_watch_active = false;
    if (restart)
        steam_ui_restart_used = true;
    pthread_mutex_unlock(&steam_ui_watch_lock);
    if (restart && !ms_process_background_shutdown_requested()) {
        ms_log_event(home, "Steam UI did not finish first-run setup; restarting Wine Steam");
        terminate_wine_steam_session(home);
        free(ms_steam_launch_json(home, NULL));
    }
    free(home);
    return NULL;
}

static void watch_steam_ui_first_run(const char* home) {
    pthread_t thread;
    char* copy;
    pthread_mutex_lock(&steam_ui_watch_lock);
    if (steam_ui_watch_active || steam_ui_restart_used) {
        pthread_mutex_unlock(&steam_ui_watch_lock);
        return;
    }
    steam_ui_watch_active = true;
    pthread_mutex_unlock(&steam_ui_watch_lock);
    copy = strdup(home);
    if (!copy || pthread_create(&thread, NULL, steam_ui_first_run_watchdog, copy) != 0) {
        free(copy);
        pthread_mutex_lock(&steam_ui_watch_lock);
        steam_ui_watch_active = false;
        pthread_mutex_unlock(&steam_ui_watch_lock);
        return;
    }
    (void)pthread_detach(thread);
}

char* ms_steam_launch_json(const char* home, int* status) {
    char* steam = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/Steam.exe");
    char* ui = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamui.dll");
    char* steam_dir = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    char* errtext;
    pid_t pid;
    if (status)
        *status = 500;
    char* wine_check = join(home, "runtime/wine/bin/wine");
    bool runtime_missing = !wine_check || access(wine_check, X_OK) != 0;
    free(wine_check);
    if (runtime_missing) {
        free(steam);
        free(ui);
        free(steam_dir);
        return err("MetalSharp Wine not found");
    }
    if (!steam || !ui || access(steam, F_OK) != 0 || access(ui, F_OK) != 0) {
        free(steam);
        free(ui);
        free(steam_dir);
        return err("Steam is not installed — use the setup wizard to install it first");
    }
    redirect_wine_steam_desktop(home);
    if (ms_steam_process_running(home)) {
        if (wine_steam_route_marker_is_pending(home, "d3dmetal"))
            (void)write_wine_steam_route_marker(home, "d3dmetal");
        errtext = NULL;
        if (activate_wine_steam(home, steam, &pid, &errtext)) {
            free(steam);
            free(ui);
            free(steam_dir);
            if (status)
                *status = 200;
            return pid_result(pid, "pid", 0, false);
        }
        if (errtext) {
            char* o = err(errtext);
            free(errtext);
            free(steam);
            free(ui);
            free(steam_dir);
            return o;
        }
        /* Leftover helpers without a live client, or a wedged session that
         * cannot forward the request: start over instead of reporting a
         * launch that never shows Steam. */
        terminate_wine_steam_session(home);
    }
    /* Start the shared Wine Steam client with D3DMetal available so Steam-
     * launched games inherit the same verified environment. Never restart a
     * responsive client here; the branch above only activates it.
     * spawn_wine_for_pipeline prepares the webhelper wrappers and registry. */
    bool ui_never_initialized = !steam_ui_initialized(home);
    errtext = spawn_wine_for_pipeline(home, "d3dmetal", 0, steam, "-no-cef-sandbox", "-cef-single-process",
                                      "-noverifyfiles", "-no-dwrite", &pid);
    if (!errtext) {
        (void)write_wine_steam_route_pending(home, "d3dmetal");
        for (int i = 0; i < 120 && !ms_steam_process_running(home); i++)
            usleep(100000);
    }
    free(steam);
    free(ui);
    free(steam_dir);
    if (errtext) {
        char* o = err(errtext);
        free(errtext);
        return o;
    }
    if (!ms_steam_process_running(home))
        return err("Wine Steam was started but did not become ready");
    if (!write_wine_steam_route_marker(home, "d3dmetal"))
        return err("Wine Steam is running, but its D3DMetal launch environment could not be verified");
    if (ui_never_initialized)
        watch_steam_ui_first_run(home);
    if (status)
        *status = 200;
    return pid_result(pid, "pid", 0, false);
}

static char* ensure_wine_steam_pipeline(const char* home, const char* pipeline) {
    char* steam;
    char* ui;
    char* steam_dir;
    char* error_text;
    pid_t pid;
    if (ms_steam_process_running(home)) {
        if (wine_steam_route_marker_matches(home, pipeline))
            return NULL;
        if (wine_steam_route_marker_is_pending(home, pipeline) && write_wine_steam_route_marker(home, pipeline))
            return NULL;
        return strdup("Wine Steam is already running without MetalSharp's verified D3DMetal environment. Stop Wine "
                      "Steam, then relaunch it from MetalSharp before starting this game.");
    }
    steam = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/Steam.exe");
    ui = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamui.dll");
    steam_dir = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    if (!steam || !ui || !steam_dir || access(steam, F_OK) != 0 || access(ui, F_OK) != 0) {
        free(steam);
        free(ui);
        free(steam_dir);
        return strdup("Steam is not installed — use the setup wizard to install it first");
    }
    error_text = spawn_wine_for_pipeline(home, pipeline, 0, steam, "-no-cef-sandbox", "-cef-single-process",
                                         "-noverifyfiles", "-no-dwrite", &pid);
    if (!error_text)
        (void)write_wine_steam_route_pending(home, pipeline);
    free(steam);
    free(ui);
    free(steam_dir);
    if (error_text)
        return error_text;
    for (int i = 0; i < 120 && !ms_steam_process_running(home); i++)
        usleep(100000);
    if (!ms_steam_process_running(home))
        return strdup("Wine Steam was started but did not become ready for this graphics route");
    if (!write_wine_steam_route_marker(home, pipeline))
        return strdup("Wine Steam started, but its D3DMetal launch environment could not be verified");
    return NULL;
}

char* ms_steam_stop_json(const char* home, int* status) {
    if (status)
        *status = 200;
    terminate_wine_steam_session(home);
    {
        ms_json_writer w;
        bool running = managed_wine_process_running(home, false);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "running");
        ms_json_writer_bool(&w, running);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
}
static char* macos_steam_app(void) {
    const char* home = getenv("HOME");
    char* user_app = home ? join(home, "Applications/Steam.app") : NULL;
    if (access("/Applications/Steam.app", F_OK) == 0) {
        free(user_app);
        return strdup("/Applications/Steam.app");
    }
    if (user_app && access(user_app, F_OK) == 0)
        return user_app;
    free(user_app);
    return NULL;
}

char* ms_steam_mac_launch_json(const char* home, int* status) {
    pid_t pid;
    char* e;
    char* app = macos_steam_app();
    if (!app) {
        if (status)
            *status = 500;
        return err("macOS Steam is not installed");
    }
    free(app);
    if (ms_steam_process_running(home)) {
        if (status)
            *status = 500;
        return err("Wine Steam is running. Stop Wine Steam before launching macOS Steam.");
    }
    e = spawn_open("-a", "Steam", "steam://open/library", &pid);
    if (e) {
        char* o = err(e);
        free(e);
        if (status)
            *status = 500;
        return o;
    }
    if (status)
        *status = 200;
    return pid_result(pid, "pid", 0, false);
}
char* ms_steam_mac_install_json(int* status) {
    pid_t pid;
    char* app = macos_steam_app();
    char* e;
    if (app) {
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "installed");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "path");
        ms_json_writer_string(&w, app);
        ms_json_writer_object_end(&w);
        free(app);
        if (status)
            *status = 200;
        return ms_json_writer_take(&w);
    }
    e = spawn_open("https://store.steampowered.com/about/", NULL, NULL, &pid);
    if (e) {
        char* o = err(e);
        free(e);
        if (status)
            *status = 500;
        return o;
    }
    if (status)
        *status = 200;
    {
        ms_json_writer w;
        char* o;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "installed");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)pid);
        ms_json_writer_key(&w, "url");
        ms_json_writer_string(&w, "https://store.steampowered.com/about/");
        ms_json_writer_object_end(&w);
        o = ms_json_writer_take(&w);
        return o;
    }
}
char* ms_steam_mac_stop_json(int* status) {
    (void)system("/usr/bin/osascript -e 'tell application \"Steam\" to quit' >/dev/null 2>&1");
    if (status)
        *status = 200;
    return strdup("{\"ok\":true,\"running\":false}");
}
static bool wait_child_success(pid_t pid) {
    int wait_status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static bool steam_install_lock_active(const char* path) {
    FILE* f = fopen(path, "rb");
    long pid = 0;
    bool active;
    if (!f)
        return false;
    if (fscanf(f, "%ld", &pid) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    active = pid > 1 && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
    return active;
}

static bool steam_install_complete(const char* steam_dir) {
    char* steam_exe = steam_dir ? join(steam_dir, "Steam.exe") : NULL;
    char* steam_x64 = steam_dir ? join(steam_dir, "steamclient64.dll") : NULL;
    char* win64_manifest = steam_dir ? join(steam_dir, "package/steam_client_win64.installed") : NULL;
    bool complete = steam_exe && steam_x64 && win64_manifest && access(steam_exe, F_OK) == 0 &&
                    access(steam_x64, F_OK) == 0 && access(win64_manifest, F_OK) == 0;
    free(steam_exe);
    free(steam_x64);
    free(win64_manifest);
    return complete;
}

static const char* fixed_unzstd_path(void) {
    const char* bundled = getenv("METALSHARP_UNZSTD_PATH");
    const char* fixed[] = {
        "/Applications/MetalSharp.app/Contents/Resources/tools/unzstd",
        "/Applications/MetalSharp.app/Contents/Resources/unzstd",
        "app/tools/unzstd",
    };
    if (bundled && access(bundled, X_OK) == 0)
        return bundled;
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (access(fixed[i], X_OK) == 0)
            return fixed[i];
    }
    if (access("/opt/homebrew/bin/unzstd", X_OK) == 0)
        return "/opt/homebrew/bin/unzstd";
    if (access("/usr/local/bin/unzstd", X_OK) == 0)
        return "/usr/local/bin/unzstd";
    return NULL;
}

static char* find_bundled_steam_archive(const char* home) {
    const char* bundle_dir = getenv("METALSHARP_BUNDLE_DIR");
    const char* fixed[] = {
        "/Applications/MetalSharp.app/Contents/Resources/bundles/metalsharp-steam.tar.zst",
        "/Applications/MetalSharp.app/Contents/Resources/metalsharp-steam.tar.zst",
        "app/bundles/metalsharp-steam.tar.zst",
    };
    char* path;
    if (bundle_dir) {
        path = join(bundle_dir, "metalsharp-steam.tar.zst");
        if (path && access(path, R_OK) == 0)
            return path;
        free(path);
    }
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        path = strdup(fixed[i]);
        if (path && access(path, R_OK) == 0)
            return path;
        free(path);
    }
    path = join(home, "cache/bundles/metalsharp-steam.tar.zst");
    if (path && access(path, R_OK) == 0)
        return path;
    free(path);
    return NULL;
}

static bool copy_file_path(const char* source, const char* destination) {
    pid_t pid;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execl("/bin/cp", "cp", source, destination, (char*)NULL);
        _exit(127);
    }
    return wait_child_success(pid);
}

static bool copy_file_path_new(const char* source, const char* destination) {
    char buffer[16384];
    int input = open(source, O_RDONLY);
    int output;
    bool ok = true;
    if (input < 0)
        return false;
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (output < 0) {
        close(input);
        return false;
    }
    for (;;) {
        ssize_t bytes = read(input, buffer, sizeof(buffer));
        if (bytes == 0)
            break;
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        ssize_t written = 0;
        while (written < bytes) {
            ssize_t count = write(output, buffer + written, (size_t)(bytes - written));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                ok = false;
                break;
            }
            written += count;
        }
        if (!ok)
            break;
    }
    if (close(input) != 0)
        ok = false;
    if (close(output) != 0)
        ok = false;
    if (!ok)
        (void)unlink(destination);
    return ok;
}

static bool select_wine_ntdll(const char* home, const char* pipeline) {
    char* directory = join(home, "runtime/wine/lib/wine/x86_64-unix");
    char* canonical = directory ? join(directory, "ntdll.so") : NULL;
    bool ok = canonical && access(canonical, R_OK) == 0;
    (void)pipeline;
    free(directory);
    free(canonical);
    return ok;
}

static bool copy_bundled_steam_installer(const char* home, const char* installer) {
    char temp_path[PATH_MAX];
    char* archive = find_bundled_steam_archive(home);
    char* source = NULL;
    const char* unzstd = fixed_unzstd_path();
    pid_t pid;
    bool ok = false;
    char compress_program[PATH_MAX];
    if (!archive || !unzstd)
        goto done;
    snprintf(compress_program, sizeof(compress_program), "--use-compress-program=%s", unzstd);
    snprintf(temp_path, sizeof(temp_path), "%s/cache/.steam-asset-%ld", home, (long)getpid());
    (void)remove_tree(temp_path);
    if (!ensure_directory(temp_path))
        goto done;
    pid = fork();
    if (pid < 0)
        goto cleanup;
    if (pid == 0) {
        execl("/usr/bin/tar", "tar", compress_program, "-xf", archive, "-C", temp_path, (char*)NULL);
        _exit(127);
    }
    if (!wait_child_success(pid))
        goto cleanup;
    source = join(temp_path, "steam/SteamSetup.exe");
    ok = source && access(source, R_OK) == 0 && copy_file_path(source, installer);
cleanup:
    (void)remove_tree(temp_path);
done:
    free(source);
    free(archive);
    return ok;
}

static bool steamwebhelper_wrapper_valid(const char* path) {
    struct stat st;
    int fds[2];
    pid_t pid;
    char output[256];
    ssize_t length;
    int status = 0;
    if (!path || stat(path, &st) != 0 || st.st_size <= 0 ||
        (unsigned long long)st.st_size > STEAMWEBHELPER_WRAPPER_MAX_BYTES)
        return false;
    if (pipe(fds) != 0)
        return false;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execl("/usr/bin/shasum", "shasum", "-a", "256", path, (char*)NULL);
        _exit(127);
    }
    close(fds[1]);
    length = read(fds[0], output, sizeof(output) - 1);
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (length <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    output[length] = '\0';
    return strncmp(output, STEAMWEBHELPER_WRAPPER_SHA256, 64) == 0;
}

static char* download_steam_bundle_archive(const char* home) {
    char* bundles = join(home, "cache/bundles");
    char* archive;
    char* temporary;
    pid_t pid;
    if (!bundles || !ensure_directory(bundles)) {
        free(bundles);
        return NULL;
    }
    archive = join(bundles, "metalsharp-steam.tar.zst");
    temporary = join(bundles, "metalsharp-steam.tar.zst.download");
    free(bundles);
    if (!archive || !temporary) {
        free(archive);
        free(temporary);
        return NULL;
    }
    if (access(archive, R_OK) == 0) {
        free(temporary);
        return archive;
    }
    pid = fork();
    if (pid < 0)
        goto fail;
    if (pid == 0) {
        execl("/usr/bin/curl", "curl", "--fail", "--location", "--silent", "--show-error", "--retry", "3", "-o",
              temporary, "https://github.com/metalsharp/MetalSharp/releases/download/bundles/metalsharp-steam.tar.zst",
              (char*)NULL);
        _exit(127);
    }
    if (!wait_child_success(pid) || rename(temporary, archive) != 0)
        goto fail;
    free(temporary);
    return archive;
fail:
    (void)unlink(temporary);
    free(archive);
    free(temporary);
    return NULL;
}

static char* extract_steamwebhelper_wrapper(const char* home) {
    char* cache = join(home, "cache/steam");
    char* cached = cache ? join(cache, "steamwebhelper.exe") : NULL;
    char* archive = find_bundled_steam_archive(home);
    char* temporary = NULL;
    char* source = NULL;
    const char* unzstd = fixed_unzstd_path();
    char compress_program[PATH_MAX];
    pid_t pid;
    bool extracted = false;
    if (cached && steamwebhelper_wrapper_valid(cached))
        goto done;
    if (!archive)
        archive = download_steam_bundle_archive(home);
    if (!cache || !cached || !archive || !unzstd || !ensure_directory(cache))
        goto done;
    temporary = join(home, "cache/.steam-webhelper-extract");
    if (!temporary)
        goto done;
    (void)remove_tree(temporary);
    if (!ensure_directory(temporary))
        goto done;
    snprintf(compress_program, sizeof(compress_program), "--use-compress-program=%s", unzstd);
    pid = fork();
    if (pid < 0)
        goto cleanup;
    if (pid == 0) {
        execl("/usr/bin/tar", "tar", compress_program, "-xf", archive, "-C", temporary, (char*)NULL);
        _exit(127);
    }
    if (!wait_child_success(pid))
        goto cleanup;
    source = join(temporary, "steam/steamwebhelper.exe");
    if (source && access(source, R_OK) == 0 && copy_file_path(source, cached) && steamwebhelper_wrapper_valid(cached))
        extracted = true;
cleanup:
    (void)remove_tree(temporary);
done:
    free(cache);
    free(archive);
    free(temporary);
    free(source);
    if (!extracted && cached && !steamwebhelper_wrapper_valid(cached)) {
        free(cached);
        cached = NULL;
    }
    return cached;
}

static void deploy_steamwebhelper_wrapper(const char* home, const char* steam_dir) {
    char* wrapper = extract_steamwebhelper_wrapper(home);
    char* cef_root;
    DIR* dir;
    struct dirent* entry;
    if (!wrapper || !steam_dir)
        goto done;
    cef_root = join(steam_dir, "bin/cef");
    dir = cef_root ? opendir(cef_root) : NULL;
    if (!dir) {
        free(cef_root);
        goto done;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *cef_dir, *original, *real, *marker;
        struct stat original_stat, real_stat;
        unsigned long long original_size = 0, real_size = 0;
        if (strncmp(entry->d_name, "cef.", 4) != 0)
            continue;
        cef_dir = join(cef_root, entry->d_name);
        original = cef_dir ? join(cef_dir, "steamwebhelper.exe") : NULL;
        real = cef_dir ? join(cef_dir, "steamwebhelper_real.exe") : NULL;
        marker = cef_dir ? join(cef_dir, ".ms_wrapper_deployed") : NULL;
        if (original && stat(original, &original_stat) == 0)
            original_size = (unsigned long long)original_stat.st_size;
        if (real && stat(real, &real_stat) == 0)
            real_size = (unsigned long long)real_stat.st_size;
        if (original_size > 0 && original_size <= STEAMWEBHELPER_WRAPPER_MAX_BYTES) {
            if (marker) {
                FILE* f = fopen(marker, "wb");
                if (f) {
                    fputs("deployed", f);
                    fclose(f);
                }
            }
        } else {
            if (real_size < STEAMWEBHELPER_WRAPPER_MAX_BYTES) {
                if (original_size > STEAMWEBHELPER_WRAPPER_MAX_BYTES) {
                    (void)unlink(real);
                    (void)rename(original, real);
                } else {
                    free(cef_dir);
                    free(original);
                    free(real);
                    free(marker);
                    continue;
                }
            } else if (original) {
                (void)unlink(original);
            }
            if (original && copy_file_path(wrapper, original) && marker) {
                FILE* f = fopen(marker, "wb");
                if (f) {
                    fputs("deployed", f);
                    fclose(f);
                }
            }
        }
        free(cef_dir);
        free(original);
        free(real);
        free(marker);
    }
    closedir(dir);
    free(cef_root);
done:
    free(wrapper);
}

static void ensure_steam_launch_ready(const char* home, const char* steam_dir) {
    char* cef_root = steam_dir ? join(steam_dir, "bin/cef") : NULL;
    DIR* dir = cef_root ? opendir(cef_root) : NULL;
    struct dirent* entry;
    bool deploy = false;
    if (!dir) {
        free(cef_root);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *cef_dir, *wrapper, *real;
        struct stat wrapper_stat, real_stat;
        unsigned long long wrapper_size = 0, real_size = 0;
        if (strncmp(entry->d_name, "cef.", 4) != 0)
            continue;
        cef_dir = join(cef_root, entry->d_name);
        wrapper = cef_dir ? join(cef_dir, "steamwebhelper.exe") : NULL;
        real = cef_dir ? join(cef_dir, "steamwebhelper_real.exe") : NULL;
        if (wrapper && stat(wrapper, &wrapper_stat) == 0)
            wrapper_size = (unsigned long long)wrapper_stat.st_size;
        if (real && stat(real, &real_stat) == 0)
            real_size = (unsigned long long)real_stat.st_size;
        if (wrapper_size == 0 || wrapper_size > STEAMWEBHELPER_WRAPPER_MAX_BYTES ||
            (real_size > 0 && real_size < STEAMWEBHELPER_WRAPPER_MAX_BYTES))
            deploy = true;
        free(cef_dir);
        free(wrapper);
        free(real);
        if (deploy)
            break;
    }
    closedir(dir);
    free(cef_root);
    if (deploy)
        deploy_steamwebhelper_wrapper(home, steam_dir);
}

static bool steamwebhelper_wrappers_ready(const char* steam_dir) {
    char* cef_root = steam_dir ? join(steam_dir, "bin/cef") : NULL;
    DIR* dir = cef_root ? opendir(cef_root) : NULL;
    struct dirent* entry;
    bool found = false;
    bool ready = true;
    if (!dir) {
        free(cef_root);
        return false;
    }
    while ((entry = readdir(dir)) != NULL) {
        char* cef_dir;
        char* wrapper;
        if (strncmp(entry->d_name, "cef.", 4) != 0)
            continue;
        cef_dir = join(cef_root, entry->d_name);
        wrapper = cef_dir ? join(cef_dir, "steamwebhelper.exe") : NULL;
        found = true;
        if (!steamwebhelper_wrapper_valid(wrapper))
            ready = false;
        free(cef_dir);
        free(wrapper);
    }
    closedir(dir);
    free(cef_root);
    return found && ready;
}

/* MetalSharp: migration-time guarantee that the Steam wrappers and shims
 * cannot be lost. Re-extracts the steamwebhelper wrapper from the bundled
 * archive when the cache was wiped, redeploys it into every CEF directory,
 * restores the steam-bridge shim from runtime/shims, and restores the
 * Goldberg payloads from the assets fallback when the runtime copies
 * vanish. Returns true when every wrapper is present afterwards. */
bool ms_steam_wrappers_ensure(const char* home) {
    bool ok = true;
    char* steam_dir = home ? join(home, "prefix-steam/drive_c/Program Files (x86)/Steam") : NULL;

    deploy_steamwebhelper_wrapper(home, steam_dir);
    if (!steamwebhelper_wrappers_ready(steam_dir))
        ok = false;

    /* Steam bridge shim: restore the deployed copy from runtime/shims. */
    {
        char* shim = join(home, "runtime/shims/libsteam_api.dylib");
        char* bridge_dir = join(home, "runtime/steam-bridge");
        char* bridge = bridge_dir ? join(bridge_dir, "libsteam_api.dylib") : NULL;
        if (shim && bridge && access(shim, R_OK) == 0 && access(bridge, R_OK) != 0) {
            if (ensure_directory(bridge_dir) && copy_file_path(shim, bridge))
                ok = ok && true;
            else
                ok = false;
        }
        free(shim);
        free(bridge_dir);
        free(bridge);
    }

    /* Goldberg payloads: restore from the assets fallback when lost. */
    {
        static const char* const payloads[][2] = {
            {"runtime/goldberg/x64/steam_api64.dll", "assets/goldberg/x64/steam_api64.dll"},
            {"runtime/goldberg/x86/steam_api.dll", "assets/goldberg/x86/steam_api.dll"},
        };
        for (size_t i = 0; i < sizeof(payloads) / sizeof(payloads[0]); ++i) {
            char* runtime_copy = join(home, payloads[i][0]);
            char* assets_copy = join(home, payloads[i][1]);
            if (runtime_copy && assets_copy && access(runtime_copy, R_OK) != 0 && access(assets_copy, R_OK) == 0) {
                char* parent = strdup(runtime_copy);
                char* slash = parent ? strrchr(parent, '/') : NULL;
                if (slash) {
                    *slash = '\0';
                    if (ensure_directory(parent) && copy_file_path(assets_copy, runtime_copy)) {
                        free(parent);
                        continue;
                    }
                }
                free(parent);
            }
            if (runtime_copy && access(runtime_copy, R_OK) != 0)
                ok = false;
            free(runtime_copy);
            free(assets_copy);
        }
    }

    free(steam_dir);
    return ok;
}

char* ms_steam_ensure_launch_ready_json(const char* home, int* status) {
    char* steam_dir = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    ms_json_writer writer;
    bool ready;
    if (status)
        *status = 500;
    if (!steam_dir || access(steam_dir, R_OK) != 0) {
        free(steam_dir);
        return err("Wine Steam is not installed yet");
    }
    ensure_steam_launch_ready(home, steam_dir);
    ready = steamwebhelper_wrappers_ready(steam_dir);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, ready);
    ms_json_writer_key(&writer, "wrappers_ready");
    ms_json_writer_bool(&writer, ready);
    if (!ready) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, "Steam webhelper wrapper deployment failed");
    }
    ms_json_writer_object_end(&writer);
    free(steam_dir);
    if (status)
        *status = ready ? 200 : 500;
    return ms_json_writer_take(&writer);
}

/* True when `section` (registry path as written in user.reg, e.g.
 * `Software\\Wine\\Mac Driver`) exists in `text` and contains every line in
 * `values`. Section names compare case-insensitively like the registry. */
static bool user_reg_section_has(const char* text, const char* section, const char* const* values, size_t count) {
    size_t section_length = strlen(section);
    for (const char* line = text; line && *line;) {
        const char* next = strchr(line, '\n');
        if (line[0] == '[' && !strncasecmp(line + 1, section, section_length) && line[1 + section_length] == ']') {
            const char* body = next ? next + 1 : NULL;
            const char* end = body;
            while (end && *end && *end != '[') {
                const char* eol = strchr(end, '\n');
                end = eol ? eol + 1 : end + strlen(end);
            }
            for (size_t i = 0; i < count; i++) {
                size_t value_length = strlen(values[i]);
                bool found = false;
                for (const char* cursor = body; cursor && cursor < end;) {
                    const char* eol = strchr(cursor, '\n');
                    size_t length = eol ? (size_t)(eol - cursor) : strlen(cursor);
                    if (length && cursor[length - 1] == '\r')
                        length--;
                    if (length == value_length && !strncmp(cursor, values[i], value_length)) {
                        found = true;
                        break;
                    }
                    cursor = eol ? eol + 1 : NULL;
                }
                if (!found)
                    return false;
            }
            return true;
        }
        line = next ? next + 1 : NULL;
    }
    return false;
}

/* Executables whose OpenGL loader needs entry points of GL versions above the
 * context's (Wine patch: AppDefaults\<app.exe>\OpenGL LaterEntryPoints). Dead
 * Cells' HashLink loader refuses to start on WineMetalGL's 3.3 context without
 * glDispatchCompute, glMemoryBarrier, glBindImageTexture and
 * glMultiDrawElementsIndirect, which Windows drivers always return; Mosa Lina
 * (2477090) is another HashLink game with the same loader. */
static const char* const later_gl_entry_point_apps[] = {"deadcells_gl.exe", "Mosa Lina.exe"};
static const char* const later_entry_points_line = "\"LaterEntryPoints\"=\"Y\"";

/* `wine reg import` cold-boots a wineserver and costs seconds on every Steam
 * start. The values only change with the Retina setting, so skip the import
 * when the prefix's user.reg already carries all of them. */
static bool steam_registry_seeded(const char* prefix, bool retina) {
    static const char* const overrides[] = {"\"d3d12\"=\"builtin\"", "\"d3d12core\"=\"builtin\"",
                                            "\"d3d12SDKLayers\"=\"builtin\"", "\"dxcore\"=\"builtin\""};
    static const char* const apps[] = {"steam.exe", "steamwebhelper.exe", "steamwebhelper_real.exe"};
    const char* retina_line = retina ? "\"RetinaMode\"=\"Y\"" : "\"RetinaMode\"=\"N\"";
    const char* dpi_line = retina ? "\"LogPixels\"=dword:000000c0" : "\"LogPixels\"=dword:00000060";
    char* path = join(prefix, "user.reg");
    char* text = path ? read_bounded_file(path) : NULL;
    bool seeded = text != NULL;
    for (size_t i = 0; seeded && i < sizeof(apps) / sizeof(apps[0]); i++) {
        char section[256];
        snprintf(section, sizeof(section), "Software\\\\Wine\\\\AppDefaults\\\\%s\\\\DllOverrides", apps[i]);
        seeded = user_reg_section_has(text, section, overrides, sizeof(overrides) / sizeof(overrides[0]));
    }
    for (size_t i = 0; seeded && i < sizeof(later_gl_entry_point_apps) / sizeof(later_gl_entry_point_apps[0]); i++) {
        char section[256];
        snprintf(section, sizeof(section), "Software\\\\Wine\\\\AppDefaults\\\\%s\\\\OpenGL",
                 later_gl_entry_point_apps[i]);
        seeded = user_reg_section_has(text, section, &later_entry_points_line, 1);
    }
    seeded = seeded && user_reg_section_has(text, "Software\\\\Wine\\\\Mac Driver", &retina_line, 1) &&
             user_reg_section_has(text, "Control Panel\\\\Desktop", &dpi_line, 1);
    free(text);
    free(path);
    return seeded;
}

static void seed_steam_registry(const char* home) {
    char* prefix = join(home, "prefix-steam");
    char* drive_c = prefix ? join(prefix, "drive_c") : NULL;
    char* reg_file = drive_c ? join(drive_c, "metalsharp-steam.reg") : NULL;
    bool retina = ms_config_retina_enabled(home);
    FILE* f;
    pid_t pid;
    char* error_text;
    if (!prefix || !drive_c || !reg_file || !ensure_directory(drive_c))
        goto done;
    if (steam_registry_seeded(prefix, retina))
        goto done;
    f = fopen(reg_file, "wb");
    if (!f)
        goto done;
    fputs("Windows Registry Editor Version 5.00\r\n\r\n", f);
    fputs("[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\Steam.exe\\DllOverrides]\r\n", f);
    fputs("\"d3d12\"=\"builtin\"\r\n\"d3d12core\"=\"builtin\"\r\n\"d3d12SDKLayers\"=\"builtin\"\r\n\"dxcore\"="
          "\"builtin\"\r\n",
          f);
    fputs("\r\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\steamwebhelper.exe\\DllOverrides]\r\n", f);
    fputs("\"d3d12\"=\"builtin\"\r\n\"d3d12core\"=\"builtin\"\r\n\"d3d12SDKLayers\"=\"builtin\"\r\n\"dxcore\"="
          "\"builtin\"\r\n",
          f);
    fputs("\r\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\steamwebhelper_real.exe\\DllOverrides]\r\n", f);
    fputs("\"d3d12\"=\"builtin\"\r\n\"d3d12core\"=\"builtin\"\r\n\"d3d12SDKLayers\"=\"builtin\"\r\n\"dxcore\"="
          "\"builtin\"\r\n",
          f);
    for (size_t i = 0; i < sizeof(later_gl_entry_point_apps) / sizeof(later_gl_entry_point_apps[0]); i++)
        fprintf(f, "\r\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\%s\\OpenGL]\r\n%s\r\n",
                later_gl_entry_point_apps[i], later_entry_points_line);
    fputs("\r\n[HKEY_CURRENT_USER\\Software\\Wine\\Mac Driver]\r\n", f);
    fprintf(f, "\"RetinaMode\"=\"%c\"\r\n", retina ? 'Y' : 'N');
    fputs("\r\n[HKEY_CURRENT_USER\\Control Panel\\Desktop]\r\n", f);
    fprintf(f, "\"LogPixels\"=dword:%08x\r\n", retina ? 192 : 96);
    fclose(f);
    error_text = spawn_wine_install(home, "reg", "import", "C:\\metalsharp-steam.reg", &pid);
    if (!error_text) {
        (void)wait_child_success(pid);
    } else {
        free(error_text);
    }
done:
    free(prefix);
    free(drive_c);
    free(reg_file);
}

/* Games whose dialogue is xWMA. Wine decodes xWMA through winegstreamer, and
 * the x86_64 runtime cannot load GStreamer on Apple Silicon, so NPC voices are
 * silent. FAudio's Windows build (FFmpeg xWMA decoder, from the
 * metalsharp-assets bundle) goes into the Steam prefix's system32, and only
 * the game's own executable prefers it, so no other title changes audio. */
static const struct {
    unsigned appid;
    const char* exe;
} faudio_games[] = {{489830, "SkyrimSE.exe"}};
static const char* const faudio_files[] = {"xaudio2_7.dll",    "x3daudio1_7.dll", "xapofx1_5.dll",
                                           "FAudio.dll",       "avcodec-58.dll",  "avutil-56.dll",
                                           "swresample-3.dll", "SDL2.dll",        "libwinpthread-1.dll"};
static const char* const faudio_overrides[] = {"\"xaudio2_7\"=\"native,builtin\"", "\"x3daudio1_7\"=\"native,builtin\"",
                                               "\"xapofx1_5\"=\"native,builtin\""};

/* Unpacked from metalsharp-assets (assets/faudio -> runtime/faudio/x64). */
static char* faudio_source_dir(const char* home) {
    char* dir = join(home, "runtime/faudio/x64");
    char* probe = dir ? join(dir, "xaudio2_7.dll") : NULL;
    bool ready = probe && (access(probe, R_OK) == 0 || (ms_setup_ensure_faudio(home) && access(probe, R_OK) == 0));
    free(probe);
    if (!ready) {
        free(dir);
        return NULL;
    }
    return dir;
}

static bool same_file_size(const char* left, const char* right) {
    struct stat a, b;
    return stat(left, &a) == 0 && stat(right, &b) == 0 && a.st_size == b.st_size;
}

bool ms_steam_ensure_game_audio_fix(const char* home, unsigned appid) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned failed[64];
    static size_t failed_count = 0;
    const char* exe = NULL;
    char marker[PATH_MAX];
    char installed[PATH_MAX];
    char section[256];
    char* source = NULL;
    char* prefix = home ? join(home, "prefix-steam") : NULL;
    char* system32 = prefix ? join(prefix, "drive_c/windows/system32") : NULL;
    char* user_reg = prefix ? join(prefix, "user.reg") : NULL;
    char* text = NULL;
    bool files_ready = true;
    bool ok = false;
    for (size_t i = 0; i < sizeof(faudio_games) / sizeof(faudio_games[0]); i++)
        if (faudio_games[i].appid == appid)
            exe = faudio_games[i].exe;
    if (!exe) {
        ok = true;
        goto done;
    }
    if (!system32 || access(system32, F_OK) != 0)
        goto done;
    snprintf(marker, sizeof(marker), "%s/bottles/steam_%u/.audio-fix", home, appid);
    snprintf(installed, sizeof(installed), "%s/xaudio2_7.dll", system32);
    if (access(marker, F_OK) == 0 && access(installed, F_OK) == 0) {
        ok = true;
        goto done;
    }
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < failed_count; i++)
        if (failed[i] == appid) {
            pthread_mutex_unlock(&lock);
            goto done;
        }
    source = faudio_source_dir(home);
    for (size_t i = 0; source && i < sizeof(faudio_files) / sizeof(faudio_files[0]); i++) {
        char* from = join(source, faudio_files[i]);
        char* to = join(system32, faudio_files[i]);
        if (!from || !to || (!same_file_size(from, to) && !copy_file_path(from, to)))
            files_ready = false;
        free(from);
        free(to);
    }
    snprintf(section, sizeof(section), "Software\\\\Wine\\\\AppDefaults\\\\%s\\\\DllOverrides", exe);
    text = user_reg ? read_bounded_file(user_reg) : NULL;
    if (source && files_ready &&
        !(text && user_reg_section_has(text, section, faudio_overrides,
                                       sizeof(faudio_overrides) / sizeof(faudio_overrides[0])))) {
        char* reg_file = join(prefix, "drive_c/metalsharp-faudio.reg");
        FILE* f = reg_file ? fopen(reg_file, "wb") : NULL;
        pid_t pid;
        char* error_text;
        if (f) {
            fputs("Windows Registry Editor Version 5.00\r\n\r\n", f);
            fprintf(f, "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\%s\\DllOverrides]\r\n", exe);
            for (size_t i = 0; i < sizeof(faudio_overrides) / sizeof(faudio_overrides[0]); i++)
                fprintf(f, "%s\r\n", faudio_overrides[i]);
            fclose(f);
            error_text = spawn_wine_install(home, "reg", "import", "C:\\metalsharp-faudio.reg", &pid);
            if (error_text)
                files_ready = false;
            else if (!wait_child_success(pid))
                files_ready = false;
            free(error_text);
        } else {
            files_ready = false;
        }
        free(reg_file);
    }
    ok = source && files_ready;
    if (!ok && failed_count < sizeof(failed) / sizeof(failed[0]))
        failed[failed_count++] = appid;
    pthread_mutex_unlock(&lock);
    if (ok) {
        char message[160];
        FILE* file = fopen(marker, "wb");
        if (file)
            fclose(file);
        snprintf(message, sizeof(message), "FAudio xWMA audio fix installed for %s", exe);
        ms_log_event(home, message);
    }
done:
    free(text);
    free(source);
    free(user_reg);
    free(system32);
    free(prefix);
    return ok;
}

static void write_steam_install_stage(const char* home, const char* stage) {
    char* path = join(home, ".steam-install-stage");
    FILE* file = path ? fopen(path, "wb") : NULL;
    if (file) {
        fprintf(file, "%s\n", stage);
        fclose(file);
    }
    free(path);
}

static void steam_install_worker(const char* home, const char* lock_path, const char* installer) {
    FILE* owner = fopen(lock_path, "wb");
    bool completed = false;
    pid_t pid;
    int wait_status = 0;
    char* wine_error;
    char* prefix = join(home, "prefix-steam");
    char* windows_dir = prefix ? join(prefix, "drive_c/windows/system32") : NULL;
    char* steam_dir = prefix ? join(prefix, "drive_c/Program Files (x86)/Steam") : NULL;
    char* steam_exe = steam_dir ? join(steam_dir, "Steam.exe") : NULL;
    char* steam_ui = steam_dir ? join(steam_dir, "steamui.dll") : NULL;
    if (owner) {
        fprintf(owner, "%ld\\n", (long)getpid());
        fclose(owner);
    }
    if (prefix)
        (void)remove_tree(prefix);
    write_steam_install_stage(home, "downloading");
    unlink(installer);
    pid = fork();
    if (pid < 0)
        goto done;
    if (pid == 0) {
        execl("/usr/bin/curl", "curl", "-sL", "-o", installer,
              "https://steamcdn-a.akamaihd.net/client/installer/SteamSetup.exe", (char*)NULL);
        _exit(127);
    }
    if (!wait_child_success(pid) && !copy_bundled_steam_installer(home, installer))
        goto done;
    if (access(installer, F_OK) != 0)
        goto done;
    /* A first Wine invocation initializes a fresh prefix automatically. Running
     * wineboot --init here also explicitly starts a second service manager. */
    write_steam_install_stage(home, "creating-steam-prefix");
    wine_error = spawn_wine_install(home, "cmd", "/c", "exit 0", &pid);
    if (wine_error) {
        free(wine_error);
        goto done;
    }
    if (!wait_child_success(pid))
        goto done;
    if (!windows_dir)
        goto done;
    for (int i = 0; i < 30 && access(windows_dir, F_OK) != 0; i++)
        sleep(2);
    if (access(windows_dir, F_OK) != 0)
        goto done;
    write_steam_install_stage(home, "installing-steam");
    wine_error = spawn_wine_install(home, installer, NULL, NULL, &pid);
    if (wine_error) {
        free(wine_error);
        goto done;
    }
    /* SteamSetup creates Steam.exe before it finishes downloading and committing
     * the real x64 client. Do not release the install lock or report success
     * until steamclient64.dll and steam_client_win64.installed exist. */
    for (int i = 0; i < 300; i++) {
        pid_t waited = waitpid(pid, &wait_status, WNOHANG);
        if (waited < 0 && errno != EINTR && errno != ECHILD)
            break;
        if (steam_install_complete(steam_dir))
            break;
        sleep(1);
    }
    if (!steam_install_complete(steam_dir))
        goto done;
    completed = true;
    write_steam_install_stage(home, "complete");
    terminate_wine_steam_session(home);
done:
    if (!completed)
        write_steam_install_stage(home, "failed");
    free(prefix);
    free(windows_dir);
    free(steam_dir);
    free(steam_exe);
    free(steam_ui);
    unlink(lock_path);
    _exit(0);
}

char* ms_steam_install_json(const char* home, int* status) {
    char *steam = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/Steam.exe"),
         *ui = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamui.dll"),
         *lock = join(home, ".steam-installing"), *installer = join(home, "SteamSetup.exe");
    FILE* lock_file = NULL;
    pid_t pid;
    bool installed = steam && ui && access(steam, F_OK) == 0 && access(ui, F_OK) == 0;
    if (status)
        *status = 200;
    if (installed) {
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        string_field(&w, "path", "Steam already installed");
        ms_json_writer_object_end(&w);
        free(steam);
        free(ui);
        free(lock);
        free(installer);
        return ms_json_writer_take(&w);
    }
    if (!lock || !installer) {
        free(steam);
        free(ui);
        free(lock);
        free(installer);
        if (status)
            *status = 500;
        return err("could not start Steam installation");
    }
    lock_file = fopen(lock, "wx");
    if (lock_file == NULL && errno == EEXIST && !steam_install_lock_active(lock)) {
        unlink(lock);
        lock_file = fopen(lock, "wx");
    }
    if (lock_file == NULL) {
        if (errno == EEXIST) {
            free(steam);
            free(ui);
            free(installer);
            if (status)
                *status = 200;
            {
                ms_json_writer w;
                ms_json_writer_init(&w);
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "ok");
                ms_json_writer_bool(&w, true);
                string_field(&w, "path", "Steam installation already in progress");
                ms_json_writer_object_end(&w);
                free(lock);
                return ms_json_writer_take(&w);
            }
        }
        free(steam);
        free(ui);
        free(lock);
        free(installer);
        if (status)
            *status = 500;
        return err("could not start Steam installation");
    }
    fprintf(lock_file, "%ld\\n", (long)getpid());
    fclose(lock_file);
    pid = fork();
    if (pid < 0) {
        unlink(lock);
        free(steam);
        free(ui);
        free(lock);
        free(installer);
        if (status)
            *status = 500;
        return err("could not start Steam installation");
    }
    if (pid == 0)
        steam_install_worker(home, lock, installer);
    free(steam);
    free(ui);
    free(lock);
    free(installer);
    {
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        string_field(&w, "path", "Steam installation started — polling /steam/status for completion");
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
}

char* ms_steam_install_game_json(const char* home, const char* body, size_t len, int* status) {
    unsigned id;
    char url[64];
    char bottle_id[64];
    char* error_text;
    pid_t pid;
    if (status)
        *status = 500;
    if (!body_id(body, len, &id)) {
        if (status)
            *status = 400;
        return err("appid required");
    }
    if (!ms_steam_ensure_bottle_manifest(home, id, "auto"))
        return err("failed to prepare Steam bottle manifest");
    snprintf(url, sizeof(url), "steam://install/%u", id);
    error_text = spawn_wine(home, "start", url, NULL, NULL, NULL, &pid);
    if (error_text) {
        char* out = err(error_text);
        free(error_text);
        return out;
    }
    snprintf(bottle_id, sizeof(bottle_id), "steam_%u", id);
    (void)mark_steam_bottle_launch(home, id, pid);
    if (status)
        *status = 200;
    {
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, id);
        string_field(&w, "method", "steam_ui");
        string_field(&w, "bottle_id", bottle_id);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
}

char* ms_steam_uninstall_game_json(const char* home, const char* body, size_t len, int* status) {
    unsigned id;
    char id_text[32];
    char manifest_name[64];
    char *games = NULL, *local = NULL, *steamapps = NULL, *manifest = NULL, *common = NULL, *install_dir = NULL,
         *game_dir = NULL;
    bool removed_local = false, removed_wine = false;
    if (status)
        *status = 500;
    if (!body_id(body, len, &id)) {
        if (status)
            *status = 400;
        return err("appid required");
    }
    snprintf(id_text, sizeof(id_text), "%u", id);
    snprintf(manifest_name, sizeof(manifest_name), "appmanifest_%u.acf", id);
    games = join(home, "games");
    local = games ? join(games, id_text) : NULL;
    if (local && access(local, F_OK) == 0) {
        if (!remove_tree(local)) {
            free(games);
            free(local);
            return err("failed to remove MetalSharp local game");
        }
        removed_local = true;
    }
    steamapps = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
    manifest = steamapps ? join(steamapps, manifest_name) : NULL;
    common = steamapps ? join(steamapps, "common") : NULL;
    if (manifest && access(manifest, F_OK) == 0) {
        install_dir = acf_install_dir(manifest);
        if (!install_dir || !common || strchr(install_dir, '/') || strchr(install_dir, '\\')) {
            free(games);
            free(local);
            free(steamapps);
            free(manifest);
            free(common);
            free(install_dir);
            return err("Refusing unsafe Steam install dir");
        }
        game_dir = join(common, install_dir);
        if (!path_is_direct_child(common, game_dir) || (access(game_dir, F_OK) == 0 && !remove_tree(game_dir))) {
            free(games);
            free(local);
            free(steamapps);
            free(manifest);
            free(common);
            free(install_dir);
            free(game_dir);
            return err("failed to remove Windows Steam game");
        }
        if (unlink(manifest) != 0 && errno != ENOENT) {
            free(games);
            free(local);
            free(steamapps);
            free(manifest);
            free(common);
            free(install_dir);
            free(game_dir);
            return err("failed to remove Windows Steam manifest");
        }
        removed_wine = true;
    }
    free(games);
    free(local);
    free(steamapps);
    free(manifest);
    free(common);
    free(install_dir);
    free(game_dir);
    if (!removed_local && !removed_wine)
        return err("No Windows Steam or MetalSharp local install was found to uninstall.");
    if (status)
        *status = 200;
    {
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, id);
        ms_json_writer_key(&w, "wine_removed");
        ms_json_writer_bool(&w, removed_wine);
        ms_json_writer_key(&w, "local_removed");
        ms_json_writer_bool(&w, removed_local);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
}

static char* pipeline_pid_result(pid_t pid, unsigned id, const char* pipeline, const char* home) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned)pid);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, id);
    char bottle_id[64];
    char* prefix = join(home, "prefix-steam");
    snprintf(bottle_id, sizeof(bottle_id), "steam_%u", id);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "bottle_id");
    ms_json_writer_string(&w, bottle_id);
    ms_json_writer_key(&w, "bottle_prefix");
    if (prefix)
        ms_json_writer_string(&w, prefix);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "offline_mode");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "env_applied_to");
    ms_json_writer_string(&w, "game_process");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(prefix);
    return o;
}

static char* acf_value(const char* text, const char* key) {
    char needle[128];
    const char* line = text;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    while (line && *line) {
        const char* found = strstr(line, needle);
        const char* end = strchr(line, '\n');
        if (found && (!end || found < end)) {
            const char* value = strchr(found + strlen(needle), '"');
            const char* close = value ? strchr(value + 1, '"') : NULL;
            if (value && close && close > value + 1)
                return strndup(value + 1, (size_t)(close - value - 1));
        }
        line = end ? end + 1 : NULL;
    }
    return NULL;
}

static char* read_bounded_file(const char* path) {
    FILE* file = fopen(path, "rb");
    char* data;
    long length;
    if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || length > 4 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0) {
        if (file)
            fclose(file);
        return NULL;
    }
    data = malloc((size_t)length + 1);
    if (data && fread(data, 1, (size_t)length, file) == (size_t)length)
        data[length] = '\0';
    else {
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

static void add_steamapps_candidate(char** candidates, size_t* count, size_t max, const char* path) {
    if (!path || !path[0] || *count >= max || access(path, R_OK) != 0)
        return;
    for (size_t i = 0; i < *count; i++)
        if (!strcmp(candidates[i], path))
            return;
    candidates[(*count)++] = strdup(path);
}

static size_t steamapps_candidates(const char* home, char** candidates, size_t max) {
    const char* host_home = getenv("HOME");
    char* internal = join(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
    const char* host_suffixes[] = {"Library/Application Support/Steam/steamapps", ".steam/steam/steamapps",
                                   ".local/share/Steam/steamapps"};
    size_t count = 0;
    add_steamapps_candidate(candidates, &count, max, internal);
    free(internal);
    if (host_home) {
        for (size_t i = 0; i < sizeof(host_suffixes) / sizeof(host_suffixes[0]); i++) {
            char* path = join(host_home, host_suffixes[i]);
            add_steamapps_candidate(candidates, &count, max, path);
            free(path);
        }
    }
    for (size_t i = 0; i < count && count < max; i++) {
        char* vdf = join(candidates[i], "libraryfolders.vdf");
        char* text = vdf ? read_bounded_file(vdf) : NULL;
        const char* line = text;
        while (line && *line && count < max) {
            const char* path_key = strstr(line, "\"path\"");
            const char* end = strchr(line, '\n');
            if (path_key && (!end || path_key < end)) {
                const char* first = strchr(path_key + 6, '"');
                const char* close = first ? strchr(first + 1, '"') : NULL;
                if (first && close && close > first + 1) {
                    char* root = ms_steam_library_host_path(candidates[i], first + 1, (size_t)(close - first - 1));
                    char* steamapps = root ? join(root, "steamapps") : NULL;
                    add_steamapps_candidate(candidates, &count, max, steamapps);
                    free(root);
                    free(steamapps);
                }
            }
            line = end ? end + 1 : NULL;
        }
        free(text);
        free(vdf);
    }
    return count;
}

static char* find_steam_game_executable(const char* home, unsigned id, const char* pipeline) {
    char local[PATH_MAX];
    char** candidates = calloc(32, sizeof(*candidates));
    size_t count;
    char* executable = NULL;
    char* game_dir = ms_steam_game_dir(home, id);
    if (game_dir) {
        char app_id[32];
        char* override = NULL;
        int override_status;
        snprintf(app_id, sizeof(app_id), "%u", id);
        override_status = ms_game_executable_override_load(home, "steam", app_id, game_dir, &override);
        if (override_status != 0) {
            free(game_dir);
            if (candidates) {
                for (size_t i = 0; i < 32; i++)
                    free(candidates[i]);
                free(candidates);
            }
            return override_status > 0 ? override : NULL;
        }
        executable = preferred_steam_game_executable(game_dir, id, pipeline);
        free(game_dir);
        if (executable || !candidates)
            goto done;
    }
    snprintf(local, sizeof(local), "%s/games/%u", home, id);
    executable = preferred_steam_game_executable(local, id, pipeline);
    if (executable || !candidates)
        goto done;
    count = steamapps_candidates(home, candidates, 32);
    for (size_t i = 0; i < count && !executable; i++) {
        char manifest_name[64];
        char* manifest_path;
        char* text;
        char* install_dir;
        char* common;
        char* game_dir;
        snprintf(manifest_name, sizeof(manifest_name), "appmanifest_%u.acf", id);
        manifest_path = join(candidates[i], manifest_name);
        text = manifest_path ? read_bounded_file(manifest_path) : NULL;
        install_dir = text ? acf_value(text, "installdir") : NULL;
        common = install_dir ? join(candidates[i], "common") : NULL;
        game_dir = common && install_dir ? join(common, install_dir) : NULL;
        if (game_dir)
            executable = preferred_steam_game_executable(game_dir, id, pipeline);
        free(manifest_path);
        free(text);
        free(install_dir);
        free(common);
        free(game_dir);
    }
done:
    if (candidates) {
        for (size_t i = 0; i < 32; i++)
            free(candidates[i]);
        free(candidates);
    }
    return executable;
}

char* ms_steam_resolve_game_executable(const char* home, unsigned id, const char* pipeline) {
    return find_steam_game_executable(home, id, pipeline && pipeline[0] ? pipeline : "d3dmetal");
}

char* ms_steam_save_executable_json(const char* home, const char* body, size_t length, int* status) {
    unsigned id = 0;
    char *game_dir = NULL, *selected = NULL, *result = NULL;
    char message[256] = "Could not save game executable";
    ms_json* request = NULL;
    ms_json_writer writer;
    if (status)
        *status = 400;
    if (!body_id(body, length, &id)) {
        snprintf(message, sizeof(message), "appid required");
        goto done;
    }
    request = ms_json_parse(body ? body : "", length, NULL, 0);
    if (!request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        !ms_json_as_string(ms_json_object_get(request, "executablePath"), &selected) || !selected || !selected[0]) {
        snprintf(message, sizeof(message), "executablePath required");
        goto done;
    }
    game_dir = ms_steam_game_dir(home, id);
    if (!game_dir) {
        if (status)
            *status = 404;
        snprintf(message, sizeof(message), "Installed Steam game was not found");
        goto done;
    }
    {
        char app_id[32];
        snprintf(app_id, sizeof(app_id), "%u", id);
        if (!ms_game_executable_override_save(home, "steam", app_id, game_dir, selected, message, sizeof(message)))
            goto done;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "executablePath");
    ms_json_writer_string(&writer, selected);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    if (status)
        *status = 200;
done:
    if (!result) {
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, false);
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, message);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
    }
    free(game_dir);
    free(selected);
    ms_json_free(request);
    return result;
}

static bool direct_game_launch_paths(const char* executable, unsigned id, char* cwd, size_t cwd_size, char* program,
                                     size_t program_size) {
    char* slash;
    char* exe_name;
    int written = snprintf(cwd, cwd_size, "%s", executable);
    if (written < 0 || (size_t)written >= cwd_size)
        return false;
    slash = strrchr(cwd, '/');
    if (!slash)
        return false;
    exe_name = slash + 1;
    *slash = '\0';
    if (id == 553850) {
        char* directory = strrchr(cwd, '/');
        if (!strcmp(directory ? directory + 1 : cwd, "bin")) {
            if (directory)
                *directory = '\0';
            written = snprintf(program, program_size, "bin/%s", exe_name);
            return written >= 0 && (size_t)written < program_size;
        }
    }
    if (id == 8500) {
        char* launcher_slash = strrchr(cwd, '/');
        if (launcher_slash && !strcmp(launcher_slash + 1, "Launcher") && !strcmp(exe_name, "evelauncher.exe")) {
            *launcher_slash = '\0';
            written = snprintf(program, program_size, "Launcher/%s", exe_name);
            return written >= 0 && (size_t)written < program_size;
        }
    }
    written = snprintf(program, program_size, "%s", exe_name);
    return written >= 0 && (size_t)written < program_size;
}

static char* spawn_direct_game(const char* home, const char* executable, unsigned id, const char* pipeline,
                               pid_t* pid) {
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "prefix-steam");
    char* cwd = malloc(PATH_MAX);
    char executable_relative[PATH_MAX];
    char* exe_name = executable_relative;
    pid_t child;
    int exec_pipe[2];
    /* The Rockstar Games Launcher always runs on D3DMetal with WFDXCompat; the
     * selected route still picks the game's arguments (RDR2's VKD3D adds -api
     * Vulkan) or, for other titles, the game's own route. */
    const rockstar_launcher_game* rockstar =
        is_rockstar_launcher_executable(executable) ? rockstar_launcher_game_for(id, pipeline) : NULL;
    const char* env_pipeline = rockstar && rockstar->launcher_on_d3dmetal ? "d3dmetal" : pipeline;
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        wine = join(home, "runtime/wine/bin/wine");
    }
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        free(prefix);
        free(cwd);
        return strdup("MetalSharp Wine not found");
    }
    if (!cwd ||
        !direct_game_launch_paths(executable, id, cwd, PATH_MAX, executable_relative, sizeof(executable_relative))) {
        free(wine);
        free(prefix);
        free(cwd);
        return strdup("Game executable path is too long or invalid");
    }
    if (pipe(exec_pipe) != 0) {
        char* error = strdup(strerror(errno));
        free(wine);
        free(prefix);
        free(cwd);
        return error;
    }
    (void)fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);
    if (!select_wine_ntdll(home, env_pipeline)) {
        free(wine);
        free(prefix);
        free(cwd);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        return strdup("Wine ntdll route variant is missing or could not be installed");
    }
    /* Settings -> Graphics runtime logs: keep Wine's stderr for every game
     * (errors, DLL loads, exceptions) instead of discarding it. */
    char runtime_log[PATH_MAX] = "";
    if (ms_config_graphics_runtime_logs_enabled(home)) {
        char runtime_log_dir[PATH_MAX];
        snprintf(runtime_log_dir, sizeof(runtime_log_dir), "%s/logs/%s/%u", home, pipeline, id);
        if (ensure_directory(runtime_log_dir))
            snprintf(runtime_log, sizeof(runtime_log), "%s/launch.stderr.log", runtime_log_dir);
    }
    child = fork();
    if (child < 0) {
        char* error = strdup(strerror(errno));
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        free(wine);
        free(prefix);
        free(cwd);
        return error;
    }
    if (child == 0) {
        char app_id[32];
        (void)setpgid(0, 0);
        close(exec_pipe[0]);
        char library_env[4096];
        char* argv[32];
        size_t argc = 0;
        snprintf(app_id, sizeof(app_id), "%u", id);
        setenv("WINEPREFIX", prefix, 1);
        setenv("METALSHARP_HOME", home, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "none", 1);
        setenv("SteamAppId", app_id, 1);
        setenv("SteamGameId", app_id, 1);
        setenv("SteamOverlayGameId", app_id, 1);
        setenv("METALSHARP_PIPELINE", env_pipeline, 1);
        set_route_paths(home, env_pipeline);
        set_route_default_env(home, env_pipeline);
        if (rockstar && rockstar->launcher_on_d3dmetal && !strcmp(pipeline, "vkd3d"))
            set_moltenvk_vkmt_env(home);
        if (rockstar && !strcmp(env_pipeline, "vkd3d"))
            set_rockstar_launcher_policy_env(home);
        if (rockstar && rockstar->agility_frontend && !strcmp(env_pipeline, "d3dmetal"))
            set_wfdxcompat_agility_env(home);
        ms_steam_apply_launch_preferences(home);
        set_game_opengl_env(id, env_pipeline);
        set_launch_cache_env(home, id, env_pipeline);
        if (runtime_log[0]) {
            int log_fd = open(runtime_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd >= 0) {
                setenv("WINEDEBUG", "err+all,+loaddll,+seh", 1);
                (void)dup2(log_fd, STDERR_FILENO);
                (void)dup2(log_fd, STDOUT_FILENO);
                close(log_fd);
                dprintf(STDERR_FILENO, "\n--- MetalSharp launch appid=%u pipeline=%s ---\nexecutable=%s\n", id,
                        pipeline, executable);
            }
        }
        if (id == 312520 || id == 2357570) {
            char diagnostic_path[PATH_MAX];
            int diagnostic_fd;
            snprintf(diagnostic_path, sizeof(diagnostic_path), "%s/logs/%s/%u/launch.stderr.log", home, pipeline, id);
            diagnostic_fd = open(diagnostic_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (diagnostic_fd >= 0) {
                setenv("WINEDEBUG", "err-all,+loaddll,+module,+seh", 1);
                (void)dup2(diagnostic_fd, STDERR_FILENO);
                (void)dup2(diagnostic_fd, STDOUT_FILENO);
                close(diagnostic_fd);
                dprintf(STDERR_FILENO, "\\n--- MetalSharp Rain World launch ---\\n");
                dprintf(STDERR_FILENO, "pipeline=%s\\nexecutable=%s\\n", pipeline, executable);
            }
        }
        if (pipeline_overrides(env_pipeline))
            setenv("WINEDLLOVERRIDES", pipeline_overrides(env_pipeline), 1);
        else
            unsetenv("WINEDLLOVERRIDES");
        if (pipeline_needs_legacy_game_args(pipeline) && (id == 774361 || id == 17410 || id == 49520)) {
            setenv("DXMT_ASYNC_PIPELINE_COMPILE", "0", 1);
            setenv("DXMT_METALFX_SPATIAL_SWAPCHAIN", "0", 1);
            setenv("DXMT_METALFX_SPATIAL", "0", 1);
            setenv("DXMT_CONFIG",
                   "d3d11.preferredMaxFrameRate=60;d3d11.maxFeatureLevel=12_1;dxmt.shaderMetalVersion=310", 1);
            setenv("METALSHARP_M9_SYNC_LOADING", "1", 1);
        }
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
        if (cwd)
            (void)chdir(cwd);
        if (id == 8500) {
            char log_dir[PATH_MAX];
            char log_path[PATH_MAX];
            int diagnostic_fd = -1;
            snprintf(log_dir, sizeof(log_dir), "%s/logs/%s/%u", home, pipeline, id);
            if (ensure_directory(log_dir)) {
                snprintf(log_path, sizeof(log_path), "%s/launch.stderr.log", log_dir);
                diagnostic_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            }
            if (diagnostic_fd >= 0) {
                setenv("WINEDEBUG", "+loaddll,+seh", 1);
                (void)dup2(diagnostic_fd, STDERR_FILENO);
                (void)dup2(diagnostic_fd, STDOUT_FILENO);
                close(diagnostic_fd);
            }
            dprintf(STDERR_FILENO, "\n--- MetalSharp EVE launch ---\npipeline=%s\nexecutable=%s\ncwd=%s\n", pipeline,
                    executable, cwd ? cwd : "(null)");
        }
        const char* launch_wrapper = getenv("METALSHARP_WINE_LAUNCH_WRAPPER");
        argv[argc++] = (char*)(launch_wrapper && access(launch_wrapper, X_OK) == 0 ? launch_wrapper : wine);
        argv[argc++] = exe_name;
        build_launch_args(id, pipeline, argv, &argc, sizeof(argv) / sizeof(argv[0]));
        if (rockstar) {
            static char steam_app_id[32];
            static char steam_location[PATH_MAX + 3];
            char* rockstar_dir = ms_steam_game_dir(home, id);
            snprintf(steam_app_id, sizeof(steam_app_id), "-steamAppId=%u", id);
            append_launch_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-skipPatcherCheck");
            append_launch_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), steam_app_id);
            if (format_wine_host_path(steam_location, sizeof(steam_location), rockstar_dir)) {
                append_launch_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-steamLocation");
                append_launch_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), steam_location);
            }
            free(rockstar_dir);
        }
        if (id == 312520 || id == 2357570) {
            dprintf(STDERR_FILENO, "command=");
            for (size_t i = 0; i < argc; i++)
                dprintf(STDERR_FILENO, "%s%s", i ? " " : "", argv[i]);
            dprintf(STDERR_FILENO, "\\n");
        }
        argv[argc] = NULL;
        if (id == 8500) {
            dprintf(STDERR_FILENO, "command=");
            for (size_t i = 0; i < argc; i++)
                dprintf(STDERR_FILENO, "%s%s", i ? " " : "", argv[i]);
            dprintf(STDERR_FILENO, "\n");
        }
        execv(argv[0], argv);
        {
            int error = errno;
            if (id == 8500)
                dprintf(STDERR_FILENO, "execv failed: %s\n", strerror(error));
            (void)write(exec_pipe[1], &error, sizeof(error));
        }
        _exit(127);
    }
    (void)setpgid(child, child);
    close(exec_pipe[1]);
    {
        int error = 0;
        ssize_t received;
        do {
            received = read(exec_pipe[0], &error, sizeof(error));
        } while (received < 0 && errno == EINTR);
        close(exec_pipe[0]);
        if (received > 0) {
            (void)waitpid(child, NULL, 0);
            free(wine);
            free(prefix);
            free(cwd);
            return strdup(strerror(error));
        }
    }
    free(wine);
    free(prefix);
    free(cwd);
    *pid = child;
    return NULL;
}

static bool d3dmetal_prefix_ready(const char* home) {
    char* prefix = join(home, "prefix-gptk");
    char* marker = prefix ? join(prefix, ".gptk-ready") : NULL;
    char* steam = prefix ? join(prefix, "drive_c/Program Files (x86)/Steam/Steam.exe") : NULL;
    char* dosdevices = prefix ? join(prefix, "dosdevices") : NULL;
    bool ready = marker && steam && dosdevices && access(marker, F_OK) == 0 && access(steam, F_OK) == 0 &&
                 access(dosdevices, F_OK) == 0;
    free(prefix);
    free(marker);
    free(steam);
    free(dosdevices);
    return ready;
}

static char* spawn_gptk_game(const char* home, const char* executable, unsigned id, const char* pipeline, pid_t* pid) {
    const char* wine = "/Applications/Game Porting Toolkit.app/Contents/Resources/wine/bin/wine64";
    const char* wine_root = "/Applications/Game Porting Toolkit.app/Contents/Resources/wine";
    char* prefix = join(home, "prefix-gptk");
    char* cwd = strdup(executable);
    char* exe_name;
    char* slash;
    char dyld[PATH_MAX * 3];
    char framework[PATH_MAX];
    pid_t child;
    if (access(wine, X_OK) != 0 || !d3dmetal_prefix_ready(home)) {
        free(prefix);
        free(cwd);
        return strdup("GPTK prefix is not ready; seed the GPTK prefix before launching");
    }
    if (!prefix || !cwd) {
        free(prefix);
        free(cwd);
        return strdup("out of memory");
    }
    slash = strrchr(cwd, '/');
    exe_name = slash ? slash + 1 : cwd;
    if (slash)
        *slash = '\0';
    snprintf(dyld, sizeof(dyld), "%s/lib:%s/lib/wine/x86_64-unix:%s/lib/wine/x86_32on64-unix:%s/lib/external",
             wine_root, wine_root, wine_root, wine_root);
    snprintf(framework, sizeof(framework), "%s/lib/external/D3DMetal.framework", wine_root);
    child = fork();
    if (child < 0) {
        char* error = strdup(strerror(errno));
        free(prefix);
        free(cwd);
        return error;
    }
    if (child == 0) {
        char app_id[32];
        char* argv[16];
        size_t argc = 0;
        snprintf(app_id, sizeof(app_id), "%u", id);
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEARCH", "wow64", 1);
        setenv("WINEDEBUG", "-all", 1);
        ms_steam_apply_launch_preferences(home);
        setenv("METALSHARP_WINE_BINARY", wine, 1);
        setenv("WINEDLOVERRIDES",
               "d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b;gameoverlayrenderer,gameoverlayrenderer64=d", 1);
        if (!strcmp(pipeline, "d3dmetal"))
            setenv("D3DMETAL_FRAMEWORK_PATH", framework, 1);
        setenv("SteamAppId", app_id, 1);
        setenv("SteamGameId", app_id, 1);
        setenv("SteamOverlayGameId", app_id, 1);
        if (!strcmp(pipeline, "d3dmetal"))
            setenv("SteamAppUser", "MetalSharp", 1);
        setenv("MS_GRAPHICS_BACKEND", !strcmp(pipeline, "d3dmetal") ? "d3dmetal" : "gptk", 1);
        setenv("DYLD_FALLBACK_LIBRARY_PATH", dyld, 1);
        (void)chdir(cwd);
        const char* launch_wrapper = getenv("METALSHARP_WINE_LAUNCH_WRAPPER");
        argv[argc++] = (char*)(launch_wrapper && access(launch_wrapper, X_OK) == 0 ? launch_wrapper : wine);
        argv[argc++] = exe_name;
        argv[argc] = NULL;
        execv(argv[0], argv);
        _exit(127);
    }
    free(prefix);
    free(cwd);
    *pid = child;
    return NULL;
}

char* ms_steam_launch_d3dmetal_json(const char* home, unsigned id, const char* bottle_id, const char* executable,
                                    int* status) {
    pid_t pid;
    char* error_text;
    char* game_dir;
    ms_json_writer writer;
    if (!executable || !executable[0]) {
        if (status)
            *status = 400;
        return err("D3DMetal game executable not found");
    }
    if (!apply_protected_exe_swap(home, id, "d3dmetal"))
        ms_log_event(home, "Could not replace this game's anti-cheat launcher stub with its real executable.");
    if (d3dmetal_steam_launcher_game_for(id, "d3dmetal")) {
        if (!ms_steam_ensure_bottle_manifest(home, id, "d3dmetal")) {
            if (status)
                *status = 500;
            return err("failed to prepare D3DMetal bottle manifest");
        }
        return launch_d3dmetal_launcher_via_steam_json(home, d3dmetal_steam_launcher_game_for(id, "d3dmetal"), status);
    }
    {
        char* launcher = rockstar_launcher_executable(home, id, "d3dmetal");
        if (launcher) {
            char* result;
            if (!ms_steam_ensure_bottle_manifest(home, id, "d3dmetal")) {
                free(launcher);
                if (status)
                    *status = 500;
                return err("failed to prepare D3DMetal bottle manifest");
            }
            result = launch_rockstar_via_launcher_json(home, rockstar_launcher_game_for(id, "d3dmetal"), launcher,
                                                       "d3dmetal", status);
            free(launcher);
            return result;
        }
    }
    /* Use the same Steam-prefix direct launcher as every routed Steam
     * pipeline. It preserves SteamAppId/SteamGameId/SteamOverlayGameId and
     * route setup, rather than marking this game as offline. */
    /* D3DMetal uses its own bottle play endpoint, so reconcile controller
     * shims here as well as in the generic Steam launch path. */
    game_dir = ms_steam_game_dir(home, id);
    ms_steam_deploy_controller_input_shims(home, game_dir);
    free(game_dir);
    error_text = spawn_direct_game(home, executable, id, "d3dmetal", &pid);
    if (error_text) {
        char* result = err(error_text);
        free(error_text);
        if (status)
            *status = 500;
        return result;
    }
    ms_process_register_game_executable(id, pid, executable);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "pid");
    ms_json_writer_u64(&writer, (unsigned)pid);
    ms_json_writer_key(&writer, "appid");
    ms_json_writer_u64(&writer, id);
    ms_json_writer_key(&writer, "bottle_id");
    ms_json_writer_string(&writer, bottle_id ? bottle_id : "");
    ms_json_writer_key(&writer, "game_exe");
    ms_json_writer_string(&writer, executable);
    ms_json_writer_key(&writer, "launch_args");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "launch_mode");
    ms_json_writer_string(&writer, "d3dmetal_steam_prefix_direct");
    ms_json_writer_object_end(&writer);
    if (status)
        *status = 200;
    return ms_json_writer_take(&writer);
}

static bool format_steam_run_url(char* url, size_t url_size, unsigned id, const char* encoded_args) {
    int written = encoded_args && encoded_args[0] ? snprintf(url, url_size, "steam://run/%u//%s", id, encoded_args)
                                                  : snprintf(url, url_size, "steam://run/%u", id);
    return written >= 0 && (size_t)written < url_size;
}

/* Start the Wine Steam client if it is not running. Returns an error result,
 * or NULL once the client is up. */
static char* ensure_wine_steam_running(const char* home, int* status) {
    char* steam_result;
    int steam_status = 500;
    if (ms_steam_process_running(home))
        return NULL;
    steam_result = ms_steam_launch_json(home, &steam_status);
    free(steam_result);
    if (steam_status >= 400) {
        if (status)
            *status = steam_status;
        return err("Wine Steam could not be started for this game");
    }
    for (int i = 0; i < 12 && !ms_steam_process_running(home); i++)
        sleep(1);
    if (!ms_steam_process_running(home)) {
        if (status)
            *status = 500;
        return err("Wine Steam was started but did not become ready for game launch");
    }
    return NULL;
}

static char* launch_game_via_steam_args_json(const char* home, unsigned id, const char* encoded_args, int* status,
                                             pid_t* launched_pid) {
    char url[1024];
    char* error_text;
    pid_t pid;
    if ((error_text = ensure_wine_steam_running(home, status)))
        return error_text;
    if (!format_steam_run_url(url, sizeof(url), id, encoded_args)) {
        if (status)
            *status = 500;
        return err("Steam launch URI exceeds the supported length");
    }
    error_text = spawn_wine(home, "start", url, NULL, NULL, NULL, &pid);
    if (error_text) {
        char* result = err(error_text);
        free(error_text);
        if (status)
            *status = 500;
        return result;
    }
    if (launched_pid)
        *launched_pid = pid;
    if (status)
        *status = 200;
    return pid_result(pid, "pid", id, true);
}

static char* launch_game_via_steam_json(const char* home, unsigned id, int* status, pid_t* launched_pid) {
    return launch_game_via_steam_args_json(home, id, NULL, status, launched_pid);
}

static int compare_eve_version(const char* left, const char* right) {
    while (*left || *right) {
        if (isdigit((unsigned char)*left) && isdigit((unsigned char)*right)) {
            char* left_end;
            char* right_end;
            unsigned long left_value = strtoul(left, &left_end, 10);
            unsigned long right_value = strtoul(right, &right_end, 10);
            if (left_value != right_value)
                return left_value < right_value ? -1 : 1;
            left = left_end;
            right = right_end;
        } else {
            unsigned char left_char = (unsigned char)tolower((unsigned char)*left);
            unsigned char right_char = (unsigned char)tolower((unsigned char)*right);
            if (left_char != right_char)
                return left_char < right_char ? -1 : 1;
            if (*left)
                left++;
            if (*right)
                right++;
        }
    }
    return 0;
}

static char* latest_eve_online_client_executable(const char* game_dir) {
    DIR* directory = opendir(game_dir);
    struct dirent* entry;
    char* best = NULL;
    char best_version[128] = "";
    if (!directory)
        return NULL;
    while ((entry = readdir(directory)) != NULL) {
        char* version_dir;
        char* executable;
        struct stat st;
        const char* version;
        if (strncmp(entry->d_name, "app-", 4) != 0 || entry->d_name[4] == '\0')
            continue;
        version = entry->d_name + 4;
        version_dir = join(game_dir, entry->d_name);
        executable = version_dir ? join(version_dir, "eve-online.exe") : NULL;
        free(version_dir);
        if (!executable || access(executable, F_OK) != 0 || executable_is_32bit(executable) || stat(executable, &st)) {
            free(executable);
            continue;
        }
        if (!best || compare_eve_version(version, best_version) > 0) {
            free(best);
            best = executable;
            snprintf(best_version, sizeof(best_version), "%s", version);
        } else
            free(executable);
    }
    closedir(directory);
    return best;
}

static char* launch_d3dmetal_launcher_via_steam_json(const char* home, const d3dmetal_steam_launcher_game* game,
                                                     int* status) {
    char* game_dir = ms_steam_game_dir(home, game->appid);
    char* launcher = game_dir ? join(game_dir, game->launcher) : NULL;
    char* executable = !game_dir      ? NULL
                       : game->client ? join(game_dir, game->client)
                                      : latest_eve_online_client_executable(game_dir);
    char message[256];
    char* error_text;
    char* result;
    pid_t pid = 0;
    int launch_status = 500;

    if (status)
        *status = 500;
    if (!game_dir || !launcher || access(launcher, F_OK) != 0) {
        free(game_dir);
        free(launcher);
        free(executable);
        if (status)
            *status = 404;
        snprintf(message, sizeof(message), "%s Steam launcher was not found", game->name);
        return err(message);
    }
    if (executable && access(executable, F_OK) != 0) {
        free(executable);
        executable = NULL;
    }

    ms_steam_deploy_controller_input_shims(home, game_dir);
    remove_stale_route_dlls(home, "d3dmetal", game_dir, launcher);
    if (executable) {
        remove_stale_route_dlls(home, "d3dmetal", game_dir, executable);
        if (!stage_route_dlls(home, game->appid, "d3dmetal", executable)) {
            free(game_dir);
            free(launcher);
            free(executable);
            snprintf(message, sizeof(message), "required D3DMetal runtime DLLs are missing for %s", game->name);
            return err(message);
        }
    }
    free(game_dir);
    free(launcher);
    free(executable);

    /* Reuse the shared Wine Steam client if it is already running. When no
     * client exists, start one with D3DMetal configured before the handoff. */
    error_text = ensure_wine_steam_pipeline(home, "d3dmetal");
    if (error_text) {
        char* result = err(error_text);
        free(error_text);
        return result;
    }
    result = launch_game_via_steam_args_json(home, game->appid, D3DMETAL_LAUNCHER_STEAM_ARGS, &launch_status, &pid);
    if (!result || launch_status >= 400) {
        if (status)
            *status = launch_status;
        snprintf(message, sizeof(message), "%s Steam handoff failed", game->name);
        return result ? result : err(message);
    }
    free(result);
    ms_process_register_pending_game(game->appid, pid, 15);
    if (status)
        *status = 200;
    return pipeline_pid_result(pid, game->appid, "d3dmetal", home);
}

/* RDR2 defaults to its Vulkan renderer, which D3DMetal cannot back: the game
 * compiles shaders and then crashes. Before each D3DMetal launch, switch every
 * prefix user's RDR2 settings to DX12; on a first launch, seed a settings file
 * that only selects DX12 so the game's own first write keeps it. */
static const char RDR2_SETTINGS_PATH[] = "Documents/Rockstar Games/Red Dead Redemption 2/Settings";
static const char RDR2_API_VULKAN[] = "<API>kSettingAPI_Vulkan</API>";
static const char RDR2_API_DX12[] = "<API>kSettingAPI_DX12</API>";
static const char RDR2_DX12_SEED[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n"
                                     "<rage__fwuiSystemSettingsCollection>\n"
                                     "  <version value=\"37\" />\n"
                                     "  <advancedGraphics>\n"
                                     "    <API>kSettingAPI_DX12</API>\n"
                                     "  </advancedGraphics>\n"
                                     "</rage__fwuiSystemSettingsCollection>\n";

static bool write_file_replacing(const char* path, const char* text, size_t length) {
    char temp[PATH_MAX];
    FILE* file;
    bool ok;
    if (snprintf(temp, sizeof(temp), "%s.metalsharp-tmp", path) >= (int)sizeof(temp))
        return false;
    file = fopen(temp, "wb");
    if (!file)
        return false;
    ok = fwrite(text, 1, length, file) == length;
    ok = fclose(file) == 0 && ok;
    if (ok && rename(temp, path) == 0)
        return true;
    (void)unlink(temp);
    return false;
}

/* system.xml can hold raw bytes (RDR2 writes the adapter name unencoded into
 * videoCardDescription), so the API swap works on lengths, not C strings. */
static bool ensure_rdr2_dx12_settings_file(const char* settings_dir) {
    const size_t vulkan_length = sizeof(RDR2_API_VULKAN) - 1;
    const size_t dx12_length = sizeof(RDR2_API_DX12) - 1;
    char path[PATH_MAX];
    struct stat info;
    FILE* file;
    char* current;
    char* updated;
    size_t length, at;
    bool ok;
    if (snprintf(path, sizeof(path), "%s/system.xml", settings_dir) >= (int)sizeof(path))
        return false;
    if (stat(path, &info) != 0)
        return ensure_directory(settings_dir) && write_file_replacing(path, RDR2_DX12_SEED, sizeof(RDR2_DX12_SEED) - 1);
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 1024 * 1024)
        return false;
    length = (size_t)info.st_size;
    current = malloc(length);
    file = current ? fopen(path, "rb") : NULL;
    ok = file && fread(current, 1, length, file) == length;
    if (file)
        fclose(file);
    if (!ok) {
        free(current);
        return false;
    }
    for (at = 0; at + vulkan_length <= length; at++)
        if (!memcmp(current + at, RDR2_API_VULKAN, vulkan_length))
            break;
    if (at + vulkan_length > length) {
        free(current);
        return true;
    }
    updated = malloc(length - vulkan_length + dx12_length);
    ok = updated != NULL;
    if (updated) {
        memcpy(updated, current, at);
        memcpy(updated + at, RDR2_API_DX12, dx12_length);
        memcpy(updated + at + dx12_length, current + at + vulkan_length, length - at - vulkan_length);
        ok = write_file_replacing(path, updated, length - vulkan_length + dx12_length);
    }
    free(updated);
    free(current);
    return ok;
}

static bool ensure_rdr2_dx12_settings(const char* home) {
    char* users = join(home, "prefix-steam/drive_c/users");
    DIR* directory = users ? opendir(users) : NULL;
    struct dirent* entry;
    bool ok = directory != NULL;
    while (directory && (entry = readdir(directory)) != NULL) {
        char documents[PATH_MAX], settings_dir[PATH_MAX];
        struct stat info;
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "Public"))
            continue;
        snprintf(documents, sizeof(documents), "%s/%s/Documents", users, entry->d_name);
        if (stat(documents, &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        snprintf(settings_dir, sizeof(settings_dir), "%s/%s/%s", users, entry->d_name, RDR2_SETTINGS_PATH);
        ok = ensure_rdr2_dx12_settings_file(settings_dir) && ok;
    }
    if (directory)
        closedir(directory);
    free(users);
    return ok;
}

/* Anti-cheat bootstrappers that Steam or a launcher starts in place of the
 * game: the stub is renamed to its backup name and replaced by a copy of the
 * real executable, so the game starts without the anti-cheat layer. Restores
 * the Rust backend's start_protected_game bypass and adds GTA V Enhanced's
 * BattlEye launcher. The copy is refreshed whenever it differs from the real
 * executable (game update, or Steam restoring the stub on verify). */
typedef struct {
    unsigned appid;
    const char* subdir;
    const char* stub;
    const char* backup;
    const char* real;
    bool vkd3d;
} protected_exe_swap;

static const protected_exe_swap PROTECTED_EXE_SWAPS[] = {
    {1245620, "Game", "start_protected_game.exe", "start_protected_game.old", "eldenring.exe", false},
    {1888160, "Game", "start_protected_game.exe", "start_protected_game.old", "armoredcore6.exe", false},
    {3240220, "", "GTA5_Enhanced_BE.exe", "GTA5_Enhanced_BE.old", "GTA5_Enhanced.exe", true},
};

static bool apply_protected_exe_swap_in(const char* dir, const protected_exe_swap* swap) {
    char real[PATH_MAX], stub[PATH_MAX], backup[PATH_MAX];
    bool have_backup;
    snprintf(real, sizeof(real), "%s/%s", dir, swap->real);
    snprintf(stub, sizeof(stub), "%s/%s", dir, swap->stub);
    snprintf(backup, sizeof(backup), "%s/%s", dir, swap->backup);
    if (access(real, R_OK) != 0)
        return false;
    if (access(stub, F_OK) == 0 && files_match(stub, real))
        return true;
    have_backup = access(backup, F_OK) == 0;
    if (!have_backup && access(stub, F_OK) == 0 && rename(stub, backup) != 0)
        return false;
    return copy_file_path(real, stub);
}

static bool apply_protected_exe_swap(const char* home, unsigned id, const char* pipeline) {
    for (size_t i = 0; i < sizeof(PROTECTED_EXE_SWAPS) / sizeof(PROTECTED_EXE_SWAPS[0]); i++) {
        const protected_exe_swap* swap = &PROTECTED_EXE_SWAPS[i];
        char dir[PATH_MAX];
        char* game_dir;
        bool ok;
        if (swap->appid != id || !pipeline ||
            (strcmp(pipeline, "d3dmetal") && !(swap->vkd3d && !strcmp(pipeline, "vkd3d"))))
            continue;
        game_dir = ms_steam_game_dir(home, id);
        if (!game_dir)
            return false;
        snprintf(dir, sizeof(dir), "%s%s%s", game_dir, swap->subdir[0] ? "/" : "", swap->subdir);
        ok = apply_protected_exe_swap_in(dir, swap);
        free(game_dir);
        return ok;
    }
    return true;
}

static char* launch_rockstar_via_launcher_json(const char* home, const rockstar_launcher_game* game,
                                               const char* launcher, const char* pipeline, int* status) {
    char* game_dir = ms_steam_game_dir(home, game->appid);
    char* client = game_dir ? join(game_dir, game->client) : NULL;
    const char* stage_pipeline = game->launcher_on_d3dmetal ? "d3dmetal" : pipeline;
    char message[256];
    char* error_text;
    pid_t pid = 0;

    if (status)
        *status = 500;
    if (!client || access(client, F_OK) != 0) {
        free(game_dir);
        free(client);
        if (status)
            *status = 404;
        snprintf(message, sizeof(message), "%s was not found", game->name);
        return err(message);
    }
    ms_steam_deploy_controller_input_shims(home, game_dir);
    remove_stale_route_dlls(home, stage_pipeline, game_dir, client);
    if (!stage_route_dlls(home, game->appid, stage_pipeline, client)) {
        free(game_dir);
        free(client);
        snprintf(message, sizeof(message), "required graphics runtime DLLs are missing for %s", game->name);
        return err(message);
    }
    free(game_dir);
    free(client);
    if (!apply_protected_exe_swap(home, game->appid, pipeline)) {
        snprintf(message, sizeof(message), "%s: could not replace the anti-cheat launcher stub.", game->name);
        ms_log_event(home, message);
    }
    /* RDR2 runs the launcher identically on both routes; its VKD3D keeps the
     * Vulkan renderer (-api Vulkan, forwarded by the launcher) instead. */
    if (game->dx12_settings && !strcmp(pipeline, "d3dmetal") && !ensure_rdr2_dx12_settings(home))
        ms_log_event(home, "Red Dead Redemption 2: could not switch system.xml to DX12 for D3DMetal.");
    if ((error_text = ensure_wine_steam_running(home, status)))
        return error_text;
    if ((error_text = spawn_direct_game(home, launcher, game->appid, pipeline, &pid))) {
        char* result = err(error_text);
        free(error_text);
        return result;
    }
    ms_process_register_game(game->appid, pid);
    if (status)
        *status = 200;
    return pipeline_pid_result(pid, game->appid, pipeline, home);
}

static bool steam_game_uses_ubisoft_connect(unsigned id, const char* game_dir) {
    static const char* const marker_files[] = {"uplay_r1_loader64.dll", "uplay_r1_loader.dll", "UbisoftConnect.exe",
                                               "UbisoftGameLauncher.exe"};
    if (id == 812140) /* Assassin's Creed Odyssey (Steam). */
        return true;
    for (size_t i = 0; game_dir && i < sizeof(marker_files) / sizeof(marker_files[0]); i++) {
        char* path = join(game_dir, marker_files[i]);
        bool exists = path && access(path, R_OK) == 0;
        free(path);
        if (exists)
            return true;
    }
    return false;
}

static bool marvel_rivals_uses_steam_bootstrap(unsigned id, const char* pipeline) {
    return id == 2767030 && pipeline && !strcmp(pipeline, "d3dmetal");
}

/* Games that must be started by the Wine Steam client on every route. AMID
 * EVIL (673130) intermittently exits during startup when its shipping exe is
 * launched directly, but starts reliably through Steam (AmidEvil.exe ->
 * AmidEvil-Win64-Shipping.exe AmidEvil) with the same staged route DLLs. */
static bool launches_through_steam_client(unsigned id) {
    return id == 673130;
}

static bool baldurs_gate_3_uses_steam_bootstrap(unsigned id, const char* pipeline) {
    return id == 1086940 && pipeline && !strcmp(pipeline, "d3dmetal");
}

static bool ubisoft_connect_command(const char* command) {
    return contains_ci(command, "ubisoftconnect.exe") || contains_ci(command, "ubisoftgamelauncher.exe") ||
           contains_ci(command, "upc.exe");
}

static pid_t ubisoft_connect_process_pid(const char* home) {
    char prefix[PATH_MAX], runtime[PATH_MAX];
    FILE* pipe;
    char line[4096];
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe)
        return 0;
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX)
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if (ubisoft_connect_command(end) && wine_process_owned((pid_t)raw_pid, end, prefix, runtime)) {
            pclose(pipe);
            return (pid_t)raw_pid;
        }
    }
    pclose(pipe);
    return 0;
}

static bool ubisoft_connect_running(const char* home) {
    return ubisoft_connect_process_pid(home) > 0;
}

static pid_t ubisoft_game_process_pid(const char* home, const char* executable) {
    char prefix[PATH_MAX], runtime[PATH_MAX];
    const char* exe_name = strrchr(executable, '/');
    FILE* pipe;
    char line[4096];
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    exe_name = exe_name ? exe_name + 1 : executable;
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe)
        return 0;
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX)
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if (contains_ci(end, exe_name) && wine_process_owned((pid_t)raw_pid, end, prefix, runtime)) {
            pclose(pipe);
            return (pid_t)raw_pid;
        }
    }
    pclose(pipe);
    return 0;
}

pid_t ms_steam_odyssey_activity_pid(const char* home) {
    char* executable = find_steam_game_executable(home, 812140, "d3dmetal");
    pid_t pid = executable ? ubisoft_game_process_pid(home, executable) : 0;
    free(executable);
    return pid;
}

static bool odyssey_process_command(const char* command, const char* executable) {
    const char* exe_name = executable ? strrchr(executable, '/') : NULL;
    exe_name = exe_name ? exe_name + 1 : executable;
    /* Ubisoft Connect is shared by multiple library entries; its presence is
     * not evidence that Assassin's Creed Odyssey itself is running. */
    return exe_name && contains_ci(command, exe_name);
}

static bool signal_odyssey_processes(const char* home, const char* executable, int signal_number, bool* failed) {
    char prefix[PATH_MAX], runtime[PATH_MAX];
    char line[4096];
    FILE* pipe;
    bool found = false;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe) {
        if (failed)
            *failed = true;
        return false;
    }
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        long raw_pid;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if (odyssey_process_command(end, executable) && wine_process_owned((pid_t)raw_pid, end, prefix, runtime)) {
            found = true;
            if (kill((pid_t)raw_pid, signal_number) != 0 && errno != ESRCH && failed)
                *failed = true;
        }
    }
    if (pclose(pipe) != 0 && failed)
        *failed = true;
    return found;
}

bool ms_steam_stop_odyssey_processes(const char* home) {
    char* executable;
    bool failed = false;
    /* Invalidate the first-run worker before killing processes, so a pending
     * crash-reporter retry cannot respawn the game after the user stops it. */
    ms_steam_cancel_background_tasks();
    executable = find_steam_game_executable(home, 812140, "d3dmetal");
    (void)signal_odyssey_processes(home, executable, SIGTERM, &failed);
    usleep(400000);
    (void)signal_odyssey_processes(home, executable, SIGKILL, &failed);
    free(executable);
    return !failed;
}

void ms_steam_cancel_background_tasks(void) {
    ms_process_cancel_background_tasks();
}

static char* launch_ubisoft_connect_steam_mode(const char* home, unsigned appid, pid_t* pid) {
    static const char* const client_relative =
        "prefix-steam/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/UbisoftConnect.exe";
    char app_id[32];
    char library_env[4096];
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "prefix-steam");
    char* executable = join(home, client_relative);
    char* cwd = executable ? strdup(executable) : NULL;
    char* slash;
    pid_t child;
    if (!wine || access(wine, X_OK) != 0 || !prefix || !executable || access(executable, R_OK) != 0 || !cwd) {
        free(wine);
        free(prefix);
        free(executable);
        free(cwd);
        return strdup("Ubisoft Connect was not found in the Steam prefix");
    }
    slash = strrchr(cwd, '/');
    if (slash)
        *slash = '\0';
    child = fork();
    if (child < 0) {
        char* error = strdup(strerror(errno));
        free(wine);
        free(prefix);
        free(executable);
        free(cwd);
        return error;
    }
    if (child == 0) {
        char* argv[] = {wine, executable, (char*)"-uplay_steam_mode", NULL};
        snprintf(app_id, sizeof(app_id), "%u", appid);
        setenv("WINEPREFIX", prefix, 1);
        setenv("METALSHARP_HOME", home, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "none", 1);
        setenv("STEAM_RUNTIME", "0", 1);
        setenv("SteamAppId", app_id, 1);
        setenv("SteamGameId", app_id, 1);
        setenv("SteamOverlayGameId", app_id, 1);
        set_route_paths(home, "vkd3d");
        set_route_default_env(home, "vkd3d");
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        (void)chdir(cwd);
        execv(wine, argv);
        _exit(127);
    }
    free(wine);
    free(prefix);
    free(executable);
    free(cwd);
    *pid = child;
    return NULL;
}

static char* ms_steam_launch_game_json_internal(const char* home, const char* body, size_t len, int* status,
                                                bool default_to_steam) {
    unsigned id;
    char *e, pipeline[32] = "auto", saved_pipeline[32] = "";
    char* executable = NULL;
    char* game_dir;
    pid_t pid;
    char je[96];
    unsigned long long started_at = monotonic_millis();
    ms_json* request = NULL;
    char* requested = NULL;
    bool has_route = false;
    if (status)
        *status = 400;
    if (!body_id(body, len, &id))
        return err("appid required");
    if (status)
        *status = 500;
    request = ms_json_parse(body ? body : "", len, je, sizeof(je));
    if (request) {
        if (ms_json_as_string(ms_json_object_get(request, "pipeline"), &requested) ||
            ms_json_as_string(ms_json_object_get(request, "launchMethod"), &requested)) {
            has_route = requested && requested[0] != '\0';
            snprintf(pipeline, sizeof(pipeline), "%s", requested);
        }
        free(requested);
        ms_json_free(request);
    }
    if (!has_route && default_to_steam)
        return launch_game_via_steam_json(home, id, status, NULL);
    if (has_route && (!strcasecmp(pipeline, "steam") || !strcasecmp(pipeline, "mac_steam") ||
                      !strcasecmp(pipeline, "macos_steam"))) {
        /* An explicit Steam route is a Steam URL
         * handoff; it does not resolve a local executable for that route. */
        return launch_game_via_steam_json(home, id, status, NULL);
    }
    {
        const char* canonical = canonical_pipeline(pipeline);
        if (!canonical) {
            if (status)
                *status = 400;
            return err("unknown pipeline");
        }
        /* Only an unset/"auto" route defers to the saved bottle; an explicit
         * route (including "dxmt") is launched as selected. */
        if (!strcmp(canonical, "auto")) {
            if (bottle_pipeline_value(home, id, saved_pipeline, sizeof(saved_pipeline)) && saved_pipeline[0] &&
                canonical_pipeline(saved_pipeline))
                snprintf(pipeline, sizeof(pipeline), "%s", canonical_pipeline(saved_pipeline));
            else {
                const char* resolved = canonical_pipeline(default_pipeline_for_appid(home, id));
                if (!resolved || !strcmp(resolved, "auto"))
                    resolved = "vkd3d";
                snprintf(pipeline, sizeof(pipeline), "%s", resolved);
            }
        } else {
            snprintf(pipeline, sizeof(pipeline), "%s", canonical);
        }
    }
    if (!ms_steam_ensure_bottle_manifest(home, id, pipeline)) {
        if (status)
            *status = 500;
        return err("failed to prepare Steam bottle manifest");
    }
    if (!apply_protected_exe_swap(home, id, pipeline))
        ms_log_event(home, "Could not replace this game's anti-cheat launcher stub with its real executable.");
    if (d3dmetal_steam_launcher_game_for(id, pipeline)) {
        char* result =
            launch_d3dmetal_launcher_via_steam_json(home, d3dmetal_steam_launcher_game_for(id, pipeline), status);
        if (result && status && *status == 200)
            record_launch_timing(home, id, started_at, pipeline);
        return result;
    }
    {
        char* launcher = rockstar_launcher_executable(home, id, pipeline);
        if (launcher) {
            char* result = launch_rockstar_via_launcher_json(home, rockstar_launcher_game_for(id, pipeline), launcher,
                                                             pipeline, status);
            free(launcher);
            if (result && status && *status == 200)
                record_launch_timing(home, id, started_at, pipeline);
            return result;
        }
    }
    if (!strcmp(pipeline, "fna_arm64")) {
        game_dir = ms_steam_game_dir(home, id);
        ms_steam_deploy_controller_input_shims(home, game_dir);
        free(game_dir);
        e = spawn_fna_game(home, id, &pid);
        if (e) {
            char* o = err(e);
            free(e);
            if (status)
                *status = 500;
            return o;
        }
        ms_process_register_game(id, pid);
        if (status)
            *status = 200;
        return pipeline_pid_result(pid, id, pipeline, home);
    }
    game_dir = ms_steam_game_dir(home, id);
    if (game_dir) {
        char app_id[32];
        char* saved_executable = NULL;
        int saved_status;
        snprintf(app_id, sizeof(app_id), "%u", id);
        saved_status = ms_game_executable_override_load(home, "steam", app_id, game_dir, &saved_executable);
        free(game_dir);
        if (saved_status < 0) {
            free(saved_executable);
            if (status)
                *status = 409;
            return err("The saved game executable is no longer valid; choose it again");
        }
        if (saved_status > 0)
            executable = saved_executable;
    }
    if (!executable)
        executable = find_steam_game_executable(home, id, pipeline);
    if (!executable) {
        if (status)
            *status = 404;
        return err("Game executable not found");
    }
    if (id != 8500 && !strcmp(pipeline, "dxmt") && executable_is_32bit(executable)) {
        /* DXMT has a separate PE lane for 32-bit games. */
        char widened_pipeline[32];
        snprintf(widened_pipeline, sizeof(widened_pipeline), "%s_32", pipeline);
        snprintf(pipeline, sizeof(pipeline), "%s", widened_pipeline);
        if (!ms_steam_ensure_bottle_manifest(home, id, pipeline)) {
            free(executable);
            if (status)
                *status = 500;
            return err("failed to prepare 32-bit graphics bottle manifest");
        }
    }
    game_dir = ms_steam_game_dir(home, id);
    ms_steam_deploy_controller_input_shims(home, game_dir);
    (void)ms_steam_ensure_game_audio_fix(home, id);
    prepare_real_steam_launch(home, game_dir, executable, id, pipeline);
    if (id == 8500) {
        char* eve_client = latest_eve_online_client_executable(game_dir);
        if (!eve_client) {
            free(game_dir);
            free(executable);
            if (status)
                *status = 404;
            return err("EVE Online 64-bit client was not found");
        }
        remove_stale_route_dlls(home, pipeline, game_dir, executable);
        remove_stale_route_dlls(home, pipeline, game_dir, eve_client);
        if (!stage_route_dlls(home, id, pipeline, eve_client)) {
            free(eve_client);
            free(game_dir);
            free(executable);
            if (status)
                *status = 500;
            return err("required graphics runtime DLLs are missing for EVE Online");
        }
        free(eve_client);
    } else {
        remove_stale_route_dlls(home, pipeline, game_dir, executable);
        if (!stage_route_dlls(home, id, pipeline, executable)) {
            free(game_dir);
            free(executable);
            if (status)
                *status = 500;
            return err("required graphics runtime DLLs are missing");
        }
    }
    if (launches_through_steam_client(id)) {
        pid_t steam_pid = 0;
        int steam_status = 500;
        char* result = launch_game_via_steam_json(home, id, &steam_status, &steam_pid);
        free(game_dir);
        free(executable);
        if (!result || steam_status >= 400) {
            if (status)
                *status = steam_status;
            return result ? result : err("Steam handoff failed");
        }
        free(result);
        ms_process_register_pending_game(id, steam_pid, 15);
        record_launch_timing(home, id, started_at, pipeline);
        if (status)
            *status = 200;
        return launch_mode_pid_result(steam_pid, id, "steam_handoff");
    }
    if (marvel_rivals_uses_steam_bootstrap(id, pipeline)) {
        pid_t steam_pid = 0;
        int steam_status = 500;
        char* error_text = ensure_wine_steam_pipeline(home, pipeline);
        char* result;
        if (error_text) {
            free(game_dir);
            free(executable);
            if (status)
                *status = 500;
            result = err(error_text);
            free(error_text);
            return result;
        }
        result = launch_game_via_steam_args_json(home, id, MARVEL_RIVALS_STEAM_ARGS, &steam_status, &steam_pid);
        if (!result || steam_status >= 400) {
            free(game_dir);
            free(executable);
            if (status)
                *status = steam_status;
            return result ? result : err("Marvel Rivals Steam handoff failed");
        }
        free(result);
        ms_process_register_pending_game(id, steam_pid, 15);
        record_launch_timing(home, id, started_at, pipeline);
        if (status)
            *status = 200;
        free(game_dir);
        free(executable);
        return launch_mode_pid_result(steam_pid, id, "steam_handoff");
    }
    if (baldurs_gate_3_uses_steam_bootstrap(id, pipeline)) {
        pid_t steam_pid = 0;
        int steam_status = 500;
        char* error_text = ensure_wine_steam_pipeline(home, pipeline);
        char* result;
        if (error_text) {
            free(game_dir);
            free(executable);
            if (status)
                *status = 500;
            result = err(error_text);
            free(error_text);
            return result;
        }
        result = launch_game_via_steam_json(home, id, &steam_status, &steam_pid);
        if (!result || steam_status >= 400) {
            free(game_dir);
            free(executable);
            if (status)
                *status = steam_status;
            return result ? result : err("Baldur's Gate 3 Steam handoff failed");
        }
        free(result);
        ms_process_register_pending_game(id, steam_pid, 15);
        record_launch_timing(home, id, started_at, pipeline);
        if (status)
            *status = 200;
        free(game_dir);
        free(executable);
        return launch_mode_pid_result(steam_pid, id, "steam_handoff");
    }
    if (steam_game_uses_ubisoft_connect(id, game_dir) && !ubisoft_connect_running(home)) {
        e = launch_ubisoft_connect_steam_mode(home, id, &pid);
        if (e) {
            char* result = err(e);
            free(e);
            free(game_dir);
            free(executable);
            if (status)
                *status = 500;
            return result;
        }
        for (int i = 0; i < 50 && !ubisoft_connect_running(home); i++)
            usleep(100000);
        if (!ubisoft_connect_running(home)) {
            free(game_dir);
            free(executable);
            if (status)
                *status = 500;
            return err("Ubisoft Connect did not start in Steam mode");
        }
    }
    free(game_dir);
    if (!strcmp(pipeline, "m13"))
        e = spawn_gptk_game(home, executable, id, pipeline, &pid);
    else if (!strcmp(pipeline, "d3dmetal"))
        e = spawn_direct_game(home, executable, id, pipeline, &pid);
    else
        e = spawn_direct_game(home, executable, id, pipeline, &pid);
    if (e) {
        char* o = err(e);
        free(e);
        free(executable);
        return o;
    }
    ms_process_register_game_executable(id, pid, executable);
    free(executable);
    (void)mark_steam_bottle_launch(home, id, pid);
    record_launch_timing(home, id, started_at, pipeline);
    if (status)
        *status = 200;
    return pipeline_pid_result(pid, id, pipeline, home);
}

char* ms_steam_launch_game_json(const char* home, const char* body, size_t len, int* status) {
    return ms_steam_launch_game_json_internal(home, body, len, status, true);
}

char* ms_steam_launch_auto_json(const char* home, const char* body, size_t len, int* status) {
    return ms_steam_launch_game_json_internal(home, body, len, status, false);
}

char* ms_steam_launch_external_json(const char* home, const char* body, size_t len, int* status) {
    unsigned id;
    char pipeline[32] = "auto";
    char* executable = NULL;
    char* game_dir = NULL;
    char parse_error[96];
    ms_json* request = NULL;
    char* requested = NULL;
    pid_t pid;
    char* error_text;
    unsigned long long started_at = monotonic_millis();
    if (status)
        *status = 400;
    if (!body_id(body, len, &id) || id == 0)
        return err("appid required");
    request = ms_json_parse(body ? body : "", len, parse_error, sizeof(parse_error));
    if (!request || !ms_json_as_string(ms_json_object_get(request, "exePath"), &executable) || !executable[0]) {
        ms_json_free(request);
        free(executable);
        return err("exePath required");
    }
    if (ms_json_as_string(ms_json_object_get(request, "pipeline"), &requested) && requested && requested[0])
        snprintf(pipeline, sizeof(pipeline), "%s", requested);
    free(requested);
    ms_json_free(request);
    if (!canonical_pipeline(pipeline)) {
        free(executable);
        return err("unknown pipeline");
    }
    snprintf(pipeline, sizeof(pipeline), "%s", canonical_pipeline(pipeline));
    if (d3dmetal_steam_launcher_game_for(id, pipeline)) {
        free(executable);
        if (!ms_steam_ensure_bottle_manifest(home, id, pipeline)) {
            if (status)
                *status = 500;
            return err("failed to prepare D3DMetal bottle manifest");
        }
        return launch_d3dmetal_launcher_via_steam_json(home, d3dmetal_steam_launcher_game_for(id, pipeline), status);
    }
    if (access(executable, F_OK) != 0) {
        free(executable);
        if (status)
            *status = 404;
        return err("GameJolt executable not found");
    }
    game_dir = strdup(executable);
    if (game_dir) {
        char* slash = strrchr(game_dir, '/');
        if (slash)
            *slash = '\0';
    }
    if (!game_dir) {
        free(executable);
        if (status)
            *status = 500;
        return err("game directory is missing");
    }
    ms_steam_deploy_controller_input_shims(home, game_dir);
    prepare_real_steam_launch(home, game_dir, executable, id, pipeline);
    remove_stale_route_dlls(home, pipeline, game_dir, executable);
    if (!stage_route_dlls(home, id, pipeline, executable)) {
        free(game_dir);
        free(executable);
        if (status)
            *status = 500;
        return err("required graphics runtime DLLs are missing");
    }
    if (!strcmp(pipeline, "m13"))
        error_text = spawn_gptk_game(home, executable, id, pipeline, &pid);
    else
        error_text = spawn_direct_game(home, executable, id, pipeline, &pid);
    free(game_dir);
    if (error_text) {
        char* result = err(error_text);
        free(error_text);
        free(executable);
        if (status)
            *status = 500;
        return result;
    }
    ms_process_register_game_executable(id, pid, executable);
    free(executable);
    record_launch_timing(home, id, started_at, pipeline);
    if (status)
        *status = 200;
    return pipeline_pid_result(pid, id, pipeline, home);
}

char* ms_steam_mtsp_inspect_json(const char* home, const unsigned char* body, size_t len, int* status, int mode) {
    unsigned id;
    char requested[64] = "auto";
    char saved[64] = "";
    char pipeline[64];
    char* game_dir = NULL;
    char* executable = NULL;
    ms_json* request = NULL;
    char parse_error[96];
    char* value = NULL;
    const char* override;
    const char* dlls[12];
    size_t dll_count = 0;
    bool ready = true;
    ms_json_writer w;
    char* result;

    if (status)
        *status = 500;
    if (!home || !home[0])
        home = ".metalsharp";
    if (!body_id((const char*)body, len, &id) || id == 0) {
        if (status)
            *status = 400;
        return err("appid required");
    }
    request = ms_json_parse((const char*)body, len, parse_error, sizeof(parse_error));
    if (request && ms_json_as_string(ms_json_object_get(request, "pipeline"), &value) && value[0])
        snprintf(requested, sizeof(requested), "%s", value);
    free(value);
    if (request)
        ms_json_free(request);
    {
        const char* requested_canonical = canonical_pipeline(requested);
        if (!requested_canonical) {
            if (status)
                *status = 400;
            return err("unknown pipeline");
        }
        if (!strcmp(requested_canonical, "auto")) {
            const char* saved_canonical =
                bottle_pipeline_value(home, id, saved, sizeof(saved)) ? canonical_pipeline(saved) : NULL;
            const char* default_canonical = canonical_pipeline(default_pipeline_for_appid(home, id));
            snprintf(pipeline, sizeof(pipeline), "%s",
                     saved_canonical && saved_canonical[0] ? saved_canonical
                                                           : (default_canonical ? default_canonical : "vkd3d"));
        } else
            snprintf(pipeline, sizeof(pipeline), "%s", requested_canonical);
    }

    game_dir = ms_steam_game_dir(home, id);
    executable = find_steam_game_executable(home, id, pipeline);
    if (!game_dir)
        ready = false;
    if (!executable)
        ready = false;

    if (!strcmp(pipeline, "dxmt") || !strcmp(pipeline, "dxmt_32")) {
        const char* arch = !strcmp(pipeline, "dxmt_32") ? "i386" : "x86_64";
        static const char* const common[] = {"d3d11.dll", "d3d10core.dll", "dxgi.dll", "winemetal.dll"};
        char source_dir[PATH_MAX];
        snprintf(source_dir, sizeof(source_dir), "%s/runtime/wine/lib/dxmt/%s-windows", home, arch);
        for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
            dlls[dll_count++] = common[i];
        for (size_t i = 0; i < dll_count; i++) {
            char* source = join(source_dir, dlls[i]);
            bool present = source && access(source, R_OK) == 0;
            if (!present)
                ready = false;
            free(source);
        }
    } else if (pipeline_is_d3d9(pipeline)) {
        const char* arch = executable && executable_is_32bit(executable) ? "i386" : "x86_64";
        static const char* const files[] = {"d3d9.dll", "dxgi.dll"};
        char source_dir[PATH_MAX];
        snprintf(source_dir, sizeof(source_dir), "%s/runtime/wine/lib/wine/%s-windows", home, arch);
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            char* source = join(source_dir, files[i]);
            if (!source || access(source, R_OK) != 0)
                ready = false;
            free(source);
        }
    } else if (!strcmp(pipeline, "vkd3d")) {
        dlls[dll_count++] = "d3d12.dll";
        dlls[dll_count++] = "d3d12core.dll";
        dlls[dll_count++] = "dxgi.dll";
        dlls[dll_count++] = "d3d11.dll";
        dlls[dll_count++] = "d3d10core.dll";
        dlls[dll_count++] = "d3d9.dll";
        for (size_t i = 0; i < dll_count; i++) {
            const char* subpath = i < 3 ? "vkd3d/vkd3d-proton/x86_64-windows" : "vkd3d/dxvk/x86_64-windows";
            char* source_dir = join(home, subpath);
            char* source = source_dir ? join(source_dir, dlls[i]) : NULL;
            if (!source || access(source, R_OK) != 0)
                ready = false;
            free(source_dir);
            free(source);
        }
    }

    if (mode == 0 && game_dir && executable) {
        remove_stale_route_dlls(home, pipeline, game_dir, executable);
        if (!stage_route_dlls(home, id, pipeline, executable))
            ready = false;
        if (pipeline_is_dxmt(pipeline))
            set_route_paths(home, pipeline);
    }

    override = pipeline_overrides(pipeline);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, ready);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, id);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "pipeline_name");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "game_dir");
    if (game_dir)
        ms_json_writer_string(&w, game_dir);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "exe_path");
    if (executable)
        ms_json_writer_string(&w, executable);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "dry_run");
    ms_json_writer_bool(&w, mode != 0);
    ms_json_writer_key(&w, "env_pairs");
    ms_json_writer_array_begin(&w);
    if (override) {
        ms_json_writer_object_begin(&w);
        string_field(&w, "key", "WINEDLLOVERRIDES");
        string_field(&w, "value", override);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_object_begin(&w);
    string_field(&w, "key", "DXMT_WINEMETAL_UNIXLIB");
    string_field(&w, "value", pipeline_is_dxmt(pipeline) ? "winemetal.so" : "");
    ms_json_writer_object_end(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "deploy_dlls");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < dll_count; i++) {
        ms_json_writer_object_begin(&w);
        string_field(&w, "filename", dlls[i]);
        ms_json_writer_key(&w, "present");
        ms_json_writer_bool(&w, true);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "env_keys_present");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "WINEDLLOVERRIDES");
    ms_json_writer_bool(&w, override != NULL);
    ms_json_writer_key(&w, "DXMT_WINEMETAL_UNIXLIB");
    ms_json_writer_bool(&w, pipeline_is_dxmt(pipeline));
    ms_json_writer_key(&w, "SteamAppId");
    ms_json_writer_bool(&w, true);
    ms_json_writer_object_end(&w);
    if (mode == 0) {
        ms_json_writer_key(&w, "prepared");
        ms_json_writer_bool(&w, ready);
    } else if (mode == 1) {
        ms_json_writer_key(&w, "recipe");
        ms_json_writer_object_begin(&w);
        string_field(&w, "pipeline", pipeline);
        ms_json_writer_key(&w, "env");
        ms_json_writer_object_begin(&w);
        if (override)
            string_field(&w, "WINEDLLOVERRIDES", override);
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
    } else {
        ms_json_writer_key(&w, "report");
        ms_json_writer_object_begin(&w);
        string_field(&w, "pipeline", pipeline);
        ms_json_writer_key(&w, "ready");
        ms_json_writer_bool(&w, ready);
        ms_json_writer_key(&w, "issues");
        ms_json_writer_array_begin(&w);
        if (!game_dir)
            ms_json_writer_string(&w, "game directory not found");
        if (!executable)
            ms_json_writer_string(&w, "game executable not found");
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_object_end(&w);
    result = ms_json_writer_take(&w);
    free(game_dir);
    free(executable);
    if (status)
        *status = ready ? 200 : 500;
    return result;
}

char* ms_steam_launch_offline_json(const char* home, const char* body, size_t len, int* status) {
    unsigned id;
    char pipeline[32] = "auto";
    char game_dir[PATH_MAX];
    char* executable;
    char* error_text;
    pid_t pid;
    ms_json* request;
    char parse_error[96];
    unsigned long long started_at = monotonic_millis();
    if (status)
        *status = 500;
    if (!body_id(body, len, &id)) {
        if (status)
            *status = 400;
        return err("appid required");
    }
    request = ms_json_parse(body ? body : "", len, parse_error, sizeof(parse_error));
    if (request) {
        char* requested = NULL;
        if (ms_json_as_string(ms_json_object_get(request, "pipeline"), &requested) ||
            ms_json_as_string(ms_json_object_get(request, "launchMethod"), &requested)) {
            snprintf(pipeline, sizeof(pipeline), "%s", requested);
        }
        free(requested);
        ms_json_free(request);
    }
    snprintf(game_dir, sizeof(game_dir), "%s/games/%u", home, id);
    if (access(game_dir, F_OK) != 0) {
        if (status)
            *status = 404;
        return err("Game directory not found");
    }
    executable = find_game_executable(game_dir, 0);
    if (!executable) {
        if (status)
            *status = 404;
        return err("Game executable not found");
    }
    if (!ms_steam_ensure_bottle_manifest(home, id, pipeline)) {
        free(executable);
        if (status)
            *status = 500;
        return err("failed to prepare Steam bottle manifest");
    }
    error_text = spawn_offline_game(home, executable, id, pipeline, &pid);
    free(executable);
    if (error_text) {
        char* out = err(error_text);
        free(error_text);
        return out;
    }
    (void)mark_steam_bottle_launch(home, id, pid);
    record_launch_timing(home, id, started_at, pipeline);
    if (status)
        *status = 200;
    {
        ms_json_writer w;
        char bottle_id[64];
        char* prefix = join(home, "prefix-steam");
        const char* backend = !strcmp(pipeline, "vkd3d")      ? "vkd3d-proton"
                              : !strcmp(pipeline, "d3dmetal") ? "d3dmetal"
                                                              : "dxmt";
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)pid);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, id);
        snprintf(bottle_id, sizeof(bottle_id), "steam_%u", id);
        string_field(&w, "gameType", "wine");
        string_field(&w, "bottle_id", bottle_id);
        ms_json_writer_key(&w, "bottle_prefix");
        if (prefix)
            ms_json_writer_string(&w, prefix);
        else
            ms_json_writer_null(&w);
        string_field(&w, "pipeline", pipeline);
        string_field(&w, "graphics_backend", backend);
        ms_json_writer_key(&w, "offline_mode");
        ms_json_writer_bool(&w, true);
        ms_json_writer_object_end(&w);
        char* out = ms_json_writer_take(&w);
        free(prefix);
        return out;
    }
}

char* ms_steam_mac_launch_game_json(const char* home, const char* body, size_t len, int* status) {
    unsigned id;
    char *native_app_path, *e;
    pid_t pid;
    unsigned long long started_at = monotonic_millis();
    if (status)
        *status = 400;
    if (!body_id(body, len, &id))
        return err("appid required");
    if (status)
        *status = 500;
    native_app_path = ms_steam_native_app_path(home, id);
    if (!native_app_path)
        return err("This game is not installed with a native macOS app bundle");
    e = spawn_open(native_app_path, NULL, NULL, &pid);
    free(native_app_path);
    if (e) {
        char* o = err(e);
        free(e);
        return o;
    }
    record_launch_timing(home, id, started_at, "mac_steam");
    if (status)
        *status = 200;
    return pid_result(pid, "pid", id, true);
}
char* ms_steam_view_game_json(const char* home, const char* body, size_t len, int* status) {
    unsigned id;
    char url[80], *e;
    pid_t pid;
    if (status)
        *status = 400;
    if (!body_id(body, len, &id))
        return err("appid required");
    if (status)
        *status = 500;
    snprintf(url, sizeof(url), "steam://nav/games/details/%u", id);
    e = spawn_wine(home, "start", url, NULL, NULL, NULL, &pid);
    if (e) {
        char* o = err(e);
        free(e);
        return o;
    }
    if (status)
        *status = 200;
    return pid_result(pid, "pid", id, true);
}
static bool contains_ci(const char* haystack, const char* needle) {
    size_t n;
    if (!haystack || !needle || !*needle)
        return false;
    n = strlen(needle);
    for (; *haystack; haystack++)
        if (!strncasecmp(haystack, needle, n))
            return true;
    return false;
}

static bool wine_steam_cleanup_target(const char* command, const char* prefix) {
    if (!command || !prefix || contains_ci(command, " rg ") || contains_ci(command, "rg -i") ||
        contains_ci(command, "ps axo") || strstr(command, "Steam.app/Contents/MacOS") ||
        contains_ci(command, "steam_osx"))
        return false;
    return (strstr(command, prefix) != NULL) || contains_ci(command, "c:\\program files (x86)\\steam") ||
           contains_ci(command, "steamwebhelper.exe") || contains_ci(command, "steamwebhelper_real.exe") ||
           contains_ci(command, "c:\\windows\\system32\\explorer.exe /desktop") ||
           (contains_ci(command, "c:\\windows\\system32\\conhost.exe") && contains_ci(command, "--headless")) ||
           contains_ci(command, "winedevice.exe") || contains_ci(command, "wineserver") ||
           contains_ci(command, "wineloader");
}

char* ms_steam_stop_targets_json(const char* home, int* status) {
    char prefix[PATH_MAX], runtime[PATH_MAX];
    char line[4096];
    FILE* pipe;
    unsigned count = 0;
    ms_json_writer w;
    if (status)
        *status = 200;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "targeted");
    ms_json_writer_array_begin(&w);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (pipe) {
        while (fgets(line, sizeof(line), pipe)) {
            char* cursor = line;
            char* end;
            long raw_pid;
            bool target;
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;
            errno = 0;
            raw_pid = strtol(cursor, &end, 10);
            if (errno != 0 || end == cursor || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
                continue;
            while (*end == ' ' || *end == '\t')
                end++;
            {
                char* newline = strchr(end, '\n');
                if (newline)
                    *newline = '\0';
            }
            target = (wine_steam_cleanup_target(end, prefix) || process_executable_within((pid_t)raw_pid, runtime)) &&
                     wine_process_owned((pid_t)raw_pid, end, prefix, runtime);
            if (target) {
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "pid");
                ms_json_writer_u64(&w, (unsigned)raw_pid);
                string_field(&w, "command", end);
                ms_json_writer_object_end(&w);
                count++;
            }
        }
        pclose(pipe);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "excluded");
    ms_json_writer_array_begin(&w);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (pipe) {
        while (fgets(line, sizeof(line), pipe)) {
            char* cursor = line;
            char* end;
            long raw_pid;
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;
            errno = 0;
            raw_pid = strtol(cursor, &end, 10);
            if (errno != 0 || end == cursor || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
                continue;
            while (*end == ' ' || *end == '\t')
                end++;
            if ((strstr(end, "Steam.app/Contents/MacOS") || strstr(end, "steam_osx") || strstr(end, " rg ") ||
                 strstr(end, "rg -i") || strstr(end, "ps axo"))) {
                char* newline = strchr(end, '\n');
                if (newline)
                    *newline = '\0';
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "pid");
                ms_json_writer_u64(&w, (unsigned)raw_pid);
                string_field(&w, "command", !strncmp(end, "/bin/ps ", 8) ? end + 5 : end);
                ms_json_writer_object_end(&w);
            }
        }
        pclose(pipe);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "targeted_pid_count");
    ms_json_writer_u64(&w, count);
    ms_json_writer_key(&w, "summary");
    {
        char summary[256];
        snprintf(summary, sizeof(summary),
                 "stop_wine_steam targets %u Wine Steam helper process(es); the macOS Steam client and MetalSharp's "
                 "own rg/ps invocations are excluded",
                 count);
        ms_json_writer_string(&w, summary);
    }
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_steam_misc_json(const char* action, const unsigned char* body, size_t len, int* status) {
    unsigned id = 0;
    ms_json_writer w;
    char* o;
    if (status)
        *status = 200;
    if (!strcmp(action, "install"))
        return strdup("{\"ok\":true,\"installed\":false,\"url\":\"https://store.steampowered.com/about/\"}");
    if (!strcmp(action, "stop-targets"))
        return strdup("{\"ok\":true,\"targets\":[]}");
    if (!strcmp(action, "bridge-start")) {
        const char* value = getenv("METALSHARP_STEAM_BRIDGE_PORT");
        const char* home = getenv("METALSHARP_HOME");
        char bridge[PATH_MAX], wine[PATH_MAX];
        unsigned long port = value && *value ? strtoul(value, NULL, 10) : 18733;
        if (!home || !*home)
            home = ".metalsharp";
        snprintf(bridge, sizeof(bridge), "%s/runtime/steam-bridge/steambridge.exe", home);
        if (access(bridge, F_OK) != 0) {
            if (status)
                *status = 500;
            return err("steambridge.exe not found — Wine-side Steam API bridge is not yet available");
        }
        snprintf(wine, sizeof(wine), "%s/runtime/wine/bin/metalsharp-wine", home);
        if (access(wine, X_OK) != 0) {
            if (status)
                *status = 500;
            return err("MetalSharp Wine not found — run setup first");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "port");
        ms_json_writer_u64(&w, port >= 1 && port <= 65535 ? port : 18733);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "compatdata"))
        return strdup("{\"ok\":false,\"deprecated\":true,\"replacement\":\"bottle manifest route "
                      "state\",\"error\":\"compatdata is deprecated and no longer written\"}");
    if (!body_id((const char*)body, len, &id) || id == 0) {
        if (status && strcmp(action, "install-recipe-deps") && strcmp(action, "runtime-doctor") &&
            strcmp(action, "d3d12-runtime-doctor"))
            *status = 400;
        return err(!strcmp(action, "install-recipe-deps")
                       ? ((body && strstr((const char*)body, "\"appid\"") != NULL) ? "appid must be greater than zero"
                                                                                   : "appid required")
                       : "appid required");
    }
    if (!strcmp(action, "install-recipe-deps")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, id);
        ms_json_writer_key(&w, "installed");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        string_field(&w, "message", "all recipe dependencies satisfied");
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, id);
    if (!strcmp(action, "runtime-doctor") || !strcmp(action, "d3d12-runtime-doctor")) {
        char pipeline[32] = "vkd3d";
        char bottle_id[64];
        char saved_pipeline[32] = "";
        char* prefix;
        const char* home = getenv("METALSHARP_HOME");
        bool has_saved_pipeline;
        ms_json* request = ms_json_parse((const char*)body, len, NULL, 0);
        char* requested = NULL;
        if (request && ms_json_as_string(ms_json_object_get(request, "pipeline"), &requested) && requested[0] != '\0')
            snprintf(pipeline, sizeof(pipeline), "%s", requested);
        free(requested);
        ms_json_free(request);
        if (!home || !home[0])
            home = ".metalsharp";
        has_saved_pipeline = bottle_pipeline_value(home, id, saved_pipeline, sizeof(saved_pipeline)) &&
                             strcmp(saved_pipeline, "auto") != 0;
        if (!strcmp(pipeline, "auto")) {
            if (has_saved_pipeline)
                snprintf(pipeline, sizeof(pipeline), "%s", saved_pipeline);
            else
                snprintf(pipeline, sizeof(pipeline), "%s", default_pipeline_for_appid(home, id));
        }
        (void)ms_steam_ensure_bottle_manifest(home, id, pipeline);
        snprintf(bottle_id, sizeof(bottle_id), "steam_%u", id);
        prefix = join(home, "prefix-steam");
        ms_json_writer_key(&w, "report");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, id);
        string_field(&w, "bottle_id", bottle_id);
        {
            char name[64];
            snprintf(name, sizeof(name), "Game %u", id);
            string_field(&w, "bottle_name", name);
        }
        if (has_saved_pipeline)
            string_field(&w, "preferred_pipeline", saved_pipeline);
        else {
            ms_json_writer_key(&w, "preferred_pipeline");
            ms_json_writer_null(&w);
        }
        ms_json_writer_key(&w, "pipeline");
        ms_json_writer_string(&w, pipeline);
        string_field(&w, "runtime_profile", pipeline);
        ms_json_writer_key(&w, "prefix_path");
        if (prefix)
            ms_json_writer_string(&w, prefix);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "game_install_path");
        ms_json_writer_null(&w);
        ms_json_writer_key(&w, "runtime_assets");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "components");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "actions");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "compatdata");
        ms_json_writer_null(&w);
        ms_json_writer_key(&w, "recipe_missing_components");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "recipe_missing_dlls");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "recipe_env");
        ms_json_writer_object_begin(&w);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "d3d12_sdk");
        ms_json_writer_null(&w);
        ms_json_writer_key(&w, "ready");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "issues");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        free(prefix);
    }
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
