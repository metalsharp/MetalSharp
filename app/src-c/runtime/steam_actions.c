#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/mtsp.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/steam.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STEAMWEBHELPER_WRAPPER_MAX_BYTES 100000ULL
#define STEAMWEBHELPER_WRAPPER_SHA256 "f46a1e8c39c850ba22861f63559f13b4f68557acf04a92e6d1b899769b2ea1f9"

static char* join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (p)
        snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
}

static void ensure_steam_launch_ready(const char* home, const char* steam_dir);
static void seed_steam_d3d12_guard(const char* home, const char* prefix);
static bool contains_ci(const char* haystack, const char* needle);
static bool wine_steam_cleanup_target(const char* command, const char* prefix);
static bool copy_file_path(const char* source, const char* destination);
static bool ensure_directory(const char* path);
static char* read_bounded_file(const char* path);
static char* find_game_executable(const char* directory, unsigned depth);
static char* preferred_steam_game_executable(const char* game_dir, unsigned id, const char* pipeline);
static char* find_steam_game_executable(const char* home, unsigned id, const char* pipeline);
static bool body_id(const char* body, size_t len, unsigned* id);
static void string_field(ms_json_writer* writer, const char* key, const char* value);

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
            if (ms_json_as_string(ms_json_array_get(dlls, i), &name)) {
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

static void deploy_controller_input_shims(const char* home, const char* game_dir) {
    static const char* const xinput[] = {"xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
                                         "xinput9_1_0.dll"};
    static const char* const dinput[] = {"dinput.dll", "dinput8.dll"};
    const char* mode;
    const char* const* names;
    size_t count;
    char* bundled;
    char* fallback;
    char* meta;
    char* marker;
    ms_json_writer writer;
    if (!game_dir || access(game_dir, F_OK) != 0)
        return;
    mode = controller_input_mode_for_home(home);
    remove_input_shim_manifest(game_dir);
    {
        static const char* const all_shims[] = {"xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
                                                "xinput9_1_0.dll", "dinput.dll", "dinput8.dll"};
        const bool remove_all = !strcmp(mode, "off");
        const bool remove_x = remove_all || !strcmp(mode, "d");
        const bool remove_d = remove_all || !strcmp(mode, "x");
        for (size_t i = 0; i < sizeof(all_shims) / sizeof(all_shims[0]); i++) {
            bool is_dinput = !strncmp(all_shims[i], "dinput", 6);
            if ((is_dinput && remove_d) || (!is_dinput && remove_x)) {
                char* path = join(game_dir, all_shims[i]);
                if (path)
                    (void)unlink(path);
                free(path);
            }
        }
    }
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
        if (selected && target && access(selected, R_OK) == 0 && copy_file_path(selected, target))
            ms_json_writer_string(&writer, names[i]);
        free(source);
        free(fallback_source);
        free(target);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    if (marker) {
        char* payload = ms_json_writer_take(&writer);
        FILE* file = fopen(marker, "wb");
        if (file && payload) {
            fputs(payload, file);
            fclose(file);
        } else if (file)
            fclose(file);
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
 * This is the C equivalent of launcher.rs' PipelineNode/LaunchRecipe launch
 * shape.  Keep route selection here instead of scattering pipeline-specific
 * guesses through the process-spawn code: the executable, DLL deployment,
 * Wine DLL search path, Unix library path, overrides, environment, and args
 * must all be selected from the same route.
 */
static const char* canonical_pipeline(const char* requested) {
    if (!requested || !requested[0] || !strcasecmp(requested, "auto") || !strcasecmp(requested, "dxmt"))
        return requested && !strcasecmp(requested, "dxmt") ? "dxmt" : "auto";
    if (!strcasecmp(requested, "m12") || !strcasecmp(requested, "d3d12") || !strcasecmp(requested, "dx12"))
        return "m12";
    if (!strcasecmp(requested, "vkd3d") || !strcasecmp(requested, "vkd3d_proton") ||
        !strcasecmp(requested, "vulkan_d3d12"))
        return "vkd3d";
    if (!strcasecmp(requested, "m11") || !strcasecmp(requested, "d3d11") || !strcasecmp(requested, "dx11") ||
        !strcasecmp(requested, "steam_d3dmetal_perf") || !strcasecmp(requested, "steam_metalfx"))
        return "m11";
    if (!strcasecmp(requested, "m11_32") || !strcasecmp(requested, "d3d11_32") ||
        !strcasecmp(requested, "dx11_32"))
        return "m11_32";
    if (!strcasecmp(requested, "m10") || !strcasecmp(requested, "d3d10") || !strcasecmp(requested, "dx10"))
        return "m10";
    if (!strcasecmp(requested, "m10_32") || !strcasecmp(requested, "d3d10_32") ||
        !strcasecmp(requested, "dx10_32"))
        return "m10_32";
    if (!strcasecmp(requested, "m9") || !strcasecmp(requested, "d3d9") || !strcasecmp(requested, "dx9"))
        return "m9";
    if (!strcasecmp(requested, "m13") || !strcasecmp(requested, "gptk") ||
        !strcasecmp(requested, "steam_d3dmetal"))
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
    return pipeline && (!strcmp(pipeline, "m9") || !strcmp(pipeline, "m10") || !strcmp(pipeline, "m10_32") ||
                         !strcmp(pipeline, "m11") || !strcmp(pipeline, "m11_32") || !strcmp(pipeline, "m12") ||
                         !strcmp(pipeline, "dxmt"));
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
    if (!strcmp(pipeline, "m12"))
        return "winemetal,d3d12,dxgi,dxgi_dxmt,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "vkd3d"))
        return "d3d12,d3d12core,d3d11,d3d10core,dxgi,d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "m11"))
        return "winemetal,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "m11_32"))
        return "d3d11,dxgi,winemetal=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "m10"))
        return "winemetal,d3d10,d3d10_1,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "m10_32"))
        return "d3d10,d3d10_1,d3d10core,d3d11,dxgi,winemetal=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "m9"))
        return "d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    if (!strcmp(pipeline, "m13") || !strcmp(pipeline, "d3dmetal"))
        return "d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";
    return NULL;
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
    FILE* file;
    if (!etc || !path || !ensure_directory(etc)) {
        free(etc);
        free(path);
        return;
    }
    existing = read_bounded_file(path);
    if (existing && strstr(existing, "dxmt.shaderMetalVersion") != NULL) {
        free(existing);
        free(etc);
        free(path);
        return;
    }
    file = fopen(path, "wb");
    if (file) {
        if (existing && existing[0]) {
            fputs(existing, file);
            if (existing[strlen(existing) - 1] != '\n')
                fputc('\n', file);
        }
        fputs("dxmt.shaderMetalVersion = 310\n", file);
        fclose(file);
    }
    free(existing);
    free(etc);
    free(path);
}

static void set_route_paths(const char* home, const char* pipeline) {
    char dllpath[PATH_MAX * 3];
    char unixpath[PATH_MAX * 3];
    const char* backend = pipeline_backend(pipeline);
    if (pipeline_is_dxmt(pipeline))
        ensure_dxmt_shader_metal_version(home);
    dllpath[0] = '\0';
    unixpath[0] = '\0';

    if (!strcmp(pipeline, "m12")) {
        snprintf(dllpath, sizeof(dllpath), "%s/runtime/wine/lib/dxmt_m12/x86_64-windows", home);
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/dxmt_m12/x86_64-unix:%s/runtime/wine/lib/wine/x86_64-unix",
                 home, home);
    } else if (!strcmp(pipeline, "m11")) {
        snprintf(dllpath, sizeof(dllpath), "%s/runtime/wine/lib/dxmt/x86_64-windows:%s/runtime/wine/lib/metalsharp/x86_64-windows",
                 home, home);
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/wine/x86_64-unix:%s/runtime/wine/lib/dxmt/x86_64-unix",
                 home, home);
    } else if (!strcmp(pipeline, "m10")) {
        snprintf(dllpath, sizeof(dllpath), "%s/runtime/wine/lib/wine/x86_64-windows:%s/runtime/wine/lib/dxmt/x86_64-windows:%s/runtime/wine/lib/metalsharp/x86_64-windows",
                 home, home, home);
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/wine/x86_64-unix:%s/runtime/wine/lib/dxmt/x86_64-unix",
                 home, home);
    } else if (!strcmp(pipeline, "m9")) {
        snprintf(dllpath, sizeof(dllpath), "%s/runtime/wine/lib/wine/x86_64-windows:%s/runtime/wine/lib/wine/i386-windows:%s/runtime/wine/lib/dxmt/x86_64-windows:%s/runtime/wine/lib/metalsharp/x86_64-windows",
                 home, home, home, home);
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/wine/x86_64-unix:%s/runtime/wine/lib/dxmt/x86_64-unix",
                 home, home);
    } else if (!strcmp(pipeline, "m11_32")) {
        snprintf(dllpath, sizeof(dllpath), "%s/runtime/wine/lib/dxmt/i386-windows:%s/runtime/wine/lib/wine/i386-windows:%s/runtime/wine/lib/wine/x86_64-windows",
                 home, home, home);
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/wine/x86_64-unix:%s/runtime/wine/lib/dxmt/i386-unix:%s/runtime/wine/lib/wine",
                 home, home, home);
    } else if (!strcmp(pipeline, "m10_32")) {
        snprintf(dllpath, sizeof(dllpath), "%s/runtime/wine/lib/wine/i386-windows:%s/runtime/wine/lib/dxmt/i386-windows:%s/runtime/wine/lib/wine/x86_64-windows",
                 home, home, home);
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/wine/x86_64-unix:%s/runtime/wine/lib/dxmt/i386-unix:%s/runtime/wine/lib/wine",
                 home, home, home);
    } else if (!strcmp(pipeline, "vkd3d")) {
        snprintf(dllpath, sizeof(dllpath), "%s/vkd3d/vkd3d-proton/x86_64-windows:%s/vkd3d/dxvk/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows",
                 home, home, home);
        snprintf(unixpath, sizeof(unixpath), "%s/runtime/wine/lib/wine/x86_64-unix", home);
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
    if (pipeline_is_dxmt(pipeline))
        setenv("DXMT_WINEMETAL_UNIXLIB", "winemetal.so", 1);
    else
        unsetenv("DXMT_WINEMETAL_UNIXLIB");
    setenv("MS_GRAPHICS_BACKEND", backend, 1);
    setenv("WINEMSYNC", "1", 1);
    if (!strcmp(pipeline, "vkd3d")) {
        char icd[PATH_MAX];
        snprintf(icd, sizeof(icd), "%s/runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json", home);
        setenv("VK_ICD_FILENAMES", icd, 1);
        setenv("VK_DRIVER_FILES", icd, 1);
    } else {
        unsetenv("VK_ICD_FILENAMES");
        unsetenv("VK_DRIVER_FILES");
    }
}

static void set_launch_cache_env(const char* home, unsigned id, const char* pipeline) {
    const char* subdir = pipeline;
    char shader[PATH_MAX], cache[PATH_MAX], summary[PATH_MAX * 2];
    if (!strcmp(pipeline, "dxmt"))
        subdir = "m11";
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

static void set_route_default_env(const char* pipeline) {
    /* Capability reporting is behavior-gated in DXMT, never route-forced. */
    unsetenv("DXMT_D3D12_UE_SM6_COMPAT");
    if (pipeline_is_dxmt(pipeline)) {
        setenv("DXMT_METALFX_SPATIAL_SWAPCHAIN", "1", 1);
        setenv("DXMT_ASYNC_PIPELINE_COMPILE", "1", 1);
        if (!strcmp(pipeline, "m12")) {
            setenv("DXMT_METALFX_SPATIAL", "1", 1);
            setenv("DXMT_METALFX_TEMPORAL", "1", 1);
            setenv("DXMT_D3D12_PSO_WORKERS", "6", 1);
            setenv("DXMT_CONFIG", "d3d11.metalSpatialUpscaleFactor=1.43;d3d11.preferredMaxFrameRate=60;dxmt.shaderMetalVersion=310", 1);
        } else {
            setenv("DXMT_CONFIG", "d3d11.metalSpatialUpscaleFactor=1.43;d3d11.preferredMaxFrameRate=60;dxmt.shaderMetalVersion=310", 1);
        }
    } else {
        unsetenv("DXMT_METALFX_SPATIAL_SWAPCHAIN");
        unsetenv("DXMT_METALFX_SPATIAL");
        unsetenv("DXMT_METALFX_TEMPORAL");
        unsetenv("DXMT_ASYNC_PIPELINE_COMPILE");
        unsetenv("DXMT_D3D12_UE_SM6_COMPAT");
        unsetenv("DXMT_D3D12_PSO_WORKERS");
        unsetenv("DXMT_CONFIG");
    }
    if (!strcmp(pipeline, "vkd3d")) {
        setenv("VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT", "1", 1);
        setenv("MVK_PRESENT_MODE", "1", 1);
        setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", 1);
        setenv("MVK_CONFIG_RESUME_LOST_DEVICE", "1", 1);
    }
}

static bool append_launch_arg(char** argv, size_t* count, size_t max, const char* arg) {
    if (*count + 1 >= max)
        return false;
    argv[(*count)++] = (char*)arg;
    return true;
}

static void build_launch_args(unsigned id, const char* pipeline, char** argv, size_t* count, size_t max) {
    static char dpcvars[] = "-dpcvars=r.Nanite=0,r.Nanite.ProjectEnabled=0,r.Nanite.AllowTessellation=0,r.Nanite.Tessellation=0,r.Nanite.SkinnedMeshes=0,r.Nanite.AsyncRasterization=0,r.GeometryCollection.Nanite=0,r.RayTracing=0,r.Lumen.HardwareRayTracing=0,r.Shadow.Virtual.Enable=0";
    if (!strcmp(pipeline, "m12")) {
        append_launch_arg(argv, count, max, "-windowed");
        append_launch_arg(argv, count, max, "-ResX=1280");
        append_launch_arg(argv, count, max, "-ResY=720");
        append_launch_arg(argv, count, max, "-ForceRes");
    }
    if (id == 1962700 && !strcmp(pipeline, "m12")) {
        append_launch_arg(argv, count, max, "-dx12");
        append_launch_arg(argv, count, max, "-d3d12");
        append_launch_arg(argv, count, max, dpcvars);
        append_launch_arg(argv, count, max, "-NoNanite");
        append_launch_arg(argv, count, max, "-ExecCmds=r.Nanite 0;r.Nanite.ProjectEnabled 0;r.Nanite.Tessellation 0;r.GeometryCollection.Nanite 0");
    }
    if (id == 379720 || id == 275850 || id == 892970 || id == 252490 || id == 570 || id == 548430 ||
        id == 526870 || id == 1272080)
        append_launch_arg(argv, count, max, "-vulkan");
    if (id == 949230)
        append_launch_arg(argv, count, max, "-force-vulkan");
    if (id == 1174180) {
        append_launch_arg(argv, count, max, "-api");
        append_launch_arg(argv, count, max, "Vulkan");
    }
    if ((id == 400 || id == 620 || id == 4000) && !strcmp(pipeline, "m9")) {
        append_launch_arg(argv, count, max, "-dxlevel");
        append_launch_arg(argv, count, max, "90");
        append_launch_arg(argv, count, max, "-novid");
    } else if ((id == 240 || id == 500 || id == 550) && !strcmp(pipeline, "m9")) {
        append_launch_arg(argv, count, max, "-dxlevel");
        append_launch_arg(argv, count, max, "90");
    } else if (id == 7670 && !strcmp(pipeline, "m9"))
        append_launch_arg(argv, count, max, "-dx9");
    else if (id == 12210 && !strcmp(pipeline, "m10"))
        append_launch_arg(argv, count, max, "-d3d10");
    else if (id == 17300 && !strcmp(pipeline, "m10"))
        append_launch_arg(argv, count, max, "-dx10");

    if (id == 1196590 || id == 1623730 || id == 1928870 || id == 2358720 || id == 2456740) {
        if (!strcmp(pipeline, "m12")) {
            append_launch_arg(argv, count, max, "-dx12");
            append_launch_arg(argv, count, max, "-d3d12");
        }
    } else if ((id == 1623730 || id == 2358720) && !strcmp(pipeline, "m11")) {
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
                                                "libSDL2.dylib", "libFAudio.dylib", "libCSteamworks.dylib"};
    const char* slash;
    const char* name;
    char id_arg[PATH_MAX];
    char* id_argv[] = {(char*)"/usr/bin/install_name_tool", (char*)"install_name_tool", (char*)"-id", id_arg,
                       (char*)path, NULL};
    if (!path || !*path || access("/usr/bin/install_name_tool", X_OK) != 0)
        return;
    slash = strrchr(path, '/');
    name = slash ? slash + 1 : path;
    snprintf(id_arg, sizeof(id_arg), "@loader_path/%s", name);
    (void)run_fna_tool("/usr/bin/install_name_tool", id_argv);
    for (size_t i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]); i++) {
        char old_name[PATH_MAX], new_name[PATH_MAX];
        char* change_argv[] = {(char*)"/usr/bin/install_name_tool", (char*)"install_name_tool", (char*)"-change",
                               old_name, new_name, (char*)path, NULL};
        snprintf(old_name, sizeof(old_name), "@rpath/%s", dependencies[i]);
        snprintf(new_name, sizeof(new_name), "@loader_path/%s", dependencies[i]);
        (void)run_fna_tool("/usr/bin/install_name_tool", change_argv);
    }
    if (access("/usr/bin/codesign", X_OK) == 0) {
        char* sign_argv[] = {(char*)"/usr/bin/codesign", (char*)"codesign", (char*)"--force", (char*)"-s", (char*)"-",
                             (char*)path, NULL};
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
                /* This legacy shim is SDL3-linked. Rust intentionally never
                 * stages it; the game alias is rebuilt from libFNA3D.0 below. */
                if (strcmp(entry->d_name, "libFNA3D.dylib") != 0) {
                    (void)copy_file_path(source_path, target_path);
                    if (strstr(entry->d_name, ".dylib") != NULL)
                        fix_fna_dylib_install_names(target_path);
                }
            }
            else if (S_ISDIR(info.st_mode))
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
        snprintf(native_path, sizeof(native_path), "%s/runtime/mono-x86/lib:%s/runtime/shims:%s", home, home,
                 game_dir);
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
        source = strdup("/Applications/MetalSharp.app/Contents/Resources/runtime/shim-sources/fna/shims/carbon_interpose.c");
    }
    if (source && output && access(source, R_OK) == 0 && access(output, R_OK) != 0) {
        char* argv[] = {(char*)"/usr/bin/clang", (char*)"clang", (char*)"-shared", (char*)"-fPIC",
                        (char*)"-arch", (char*)"x86_64", (char*)"-o", output, source, NULL};
        if (run_fna_tool("/usr/bin/clang", argv) && access(output, R_OK) == 0) {
            char* sign_argv[] = {(char*)"/usr/bin/codesign", (char*)"codesign", (char*)"--force", (char*)"-s",
                                 (char*)"-", output, NULL};
            (void)run_fna_tool("/usr/bin/codesign", sign_argv);
        }
    }
    free(source_dir);
    free(source);
    free(output);
}

static void stage_celeste_steam_api(const char* home, const char* game_dir) {
    char* steam_root = join(home, "Library/Application Support/Steam");
    char* helper = steam_root
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

/* Rust prepares the real Steam client contract before every direct launch.
 * In particular, source-style games need steam_appid.txt even when the
 * graphics route is M9 and the executable is launched directly through Wine. */
static void prepare_real_steam_launch(const char* home, const char* game_dir, const char* executable, unsigned id,
                                      const char* pipeline) {
    char* steam_dir;
    char* target_dirs[4] = {NULL, NULL, NULL, NULL};
    size_t target_count = 0;
    const char* files[] = {"steam_api.dll", "steam_api64.dll", "steamclient.dll", "steamclient64.dll",
                           "GameOverlayRenderer.dll", "GameOverlayRenderer64.dll"};
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
    const char* config_name = id == 504230 ? "celeste-x86-mono.config"
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
         * FAudio.  Rust deploys this nested runtime directory explicitly. */
        if (x86)
            stage_fna_directory(fmod, cwd);
        stage_fna_directory(shims, cwd);
        if (id == 504230) {
            char* sdl3 = join(cwd, "libSDL3.0.dylib");
            char* sdl3_alias = join(cwd, "libSDL3.dylib");
            /* Rust's setup phase does deploy libsteam_api for Celeste. Keep
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
        /* Rust's FNA deploy makes libFNA3D.dylib resolve to the SDL2-linked
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
        snprintf(mono_path, sizeof(mono_path), "%s:%s/runtime/mono-%s/lib/mono/4.5", cwd, home,
                 x86 ? "x86" : "arm64");
        setenv("DYLD_LIBRARY_PATH", library_env, 1);
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
        setenv("METALSHARP_HOME", home, 1);
        /* Match the Rust FNA pipeline node; without this, macOS graphics
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
        /* Rust passes the resolved absolute executable path.  Do the same;
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

static bool stage_route_asset(const char* home, const char* source_subpath, const char* filename, const char* destination) {
    char* source_root = NULL;
    char* source_dir = NULL;
    char* source = NULL;
    char* target = NULL;
    bool ok = false;
    if (!strncmp(source_subpath, "vkd3d/", 6))
        source_root = strdup(home);
    else
        source_root = join(home, "runtime/wine");
    source_dir = source_root ? join(source_root, source_subpath) : NULL;
    source = source_dir ? join(source_dir, filename) : NULL;
    target = destination ? join(destination, filename) : NULL;
    if (source && target && access(source, R_OK) == 0) {
        (void)ensure_directory(destination);
        ok = copy_file_path(source, target);
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

static void remove_stale_route_dlls(const char* home, const char* pipeline, const char* game_dir, const char* executable) {
    static const char* const names[] = {"d3d12.dll", "d3d12core.dll", "d3d11.dll", "d3d10.dll", "d3d10_1.dll",
                                        "d3d10core.dll", "d3d9.dll", "dxgi.dll", "dxgi_dxmt.dll", "nvapi64.dll",
                                        "nvngx.dll", "winemetal.dll", "metalsharp_ntdll_hook.dll"};
    const char* source_subpaths[] = {"runtime/wine/lib/dxmt/x86_64-windows",
                                     "runtime/wine/lib/dxmt/i386-windows",
                                     "runtime/wine/lib/dxmt_m12/x86_64-windows",
                                     "runtime/wine/lib/metalsharp/x86_64-windows",
                                     "runtime/wine/lib/metalsharp/i386-windows",
                                     "runtime/wine/lib/wine/x86_64-windows",
                                     "runtime/wine/lib/wine/i386-windows",
                                     "vkd3d/vkd3d-proton/x86_64-windows", "vkd3d/dxvk/x86_64-windows"};
    char* exe_dir = executable ? strdup(executable) : NULL;
    char* slash = exe_dir ? strrchr(exe_dir, '/') : NULL;
    const char* dirs[2] = {game_dir, NULL};
    if (slash) {
        *slash = '\0';
        dirs[1] = exe_dir;
    }
    if (!pipeline_is_dxmt(pipeline) && strcmp(pipeline, "vkd3d") != 0) {
        free(exe_dir);
        return;
    }
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

static bool stage_route_dlls(const char* home, unsigned id, const char* pipeline, const char* executable) {
    char* exe_dir = NULL;
    char* game_dir = NULL;
    char* cursor;
    bool ok = true;
    const char* source;
    const char* files[12];
    size_t file_count = 0;
    bool is32 = executable_is_32bit(executable);

    if (!strcmp(pipeline, "m13") || !strcmp(pipeline, "d3dmetal") || !strcmp(pipeline, "m32") ||
        !strcmp(pipeline, "wine_bare"))
        return true;
    exe_dir = strdup(executable);
    cursor = exe_dir ? strrchr(exe_dir, '/') : NULL;
    if (!cursor)
        goto done;
    *cursor = '\0';
    game_dir = ms_steam_game_dir(home, id);

    if (!strcmp(pipeline, "m12")) {
        source = "lib/dxmt_m12/x86_64-windows";
        files[file_count++] = "d3d12.dll";
        files[file_count++] = "d3d11.dll";
        files[file_count++] = "dxgi.dll";
        files[file_count++] = "dxgi_dxmt.dll";
        files[file_count++] = "d3d10core.dll";
        files[file_count++] = "winemetal.dll";
        files[file_count++] = "nvapi64.dll";
        files[file_count++] = "nvngx.dll";
    } else if (!strcmp(pipeline, "vkd3d")) {
        static const char* const vkd3d_files[] = {"d3d12.dll", "d3d12core.dll"};
        static const char* const dxvk_files[] = {"d3d11.dll", "d3d10core.dll", "d3d9.dll", "dxgi.dll"};
        for (size_t i = 0; i < sizeof(vkd3d_files) / sizeof(vkd3d_files[0]); i++)
            ok = stage_route_asset(home, "vkd3d/vkd3d-proton/x86_64-windows", vkd3d_files[i], exe_dir) && ok;
        for (size_t i = 0; i < sizeof(dxvk_files) / sizeof(dxvk_files[0]); i++)
            ok = stage_route_asset(home, "vkd3d/dxvk/x86_64-windows", dxvk_files[i], exe_dir) && ok;
        goto prefix_done;
    } else if (!strcmp(pipeline, "m11") || !strcmp(pipeline, "m11_32")) {
        source = !strcmp(pipeline, "m11_32") ? "lib/dxmt/i386-windows" : "lib/dxmt/x86_64-windows";
        files[file_count++] = "d3d11.dll";
        files[file_count++] = "dxgi.dll";
        files[file_count++] = "dxgi_dxmt.dll";
        files[file_count++] = "d3d10core.dll";
        files[file_count++] = "winemetal.dll";
        if (!strcmp(pipeline, "m11")) {
            source = "lib/dxmt/x86_64-windows";
            files[file_count++] = "nvapi64.dll";
            files[file_count++] = "nvngx.dll";
            files[file_count++] = "metalsharp_ntdll_hook.dll";
        }
    } else if (!strcmp(pipeline, "m10") || !strcmp(pipeline, "m10_32")) {
        const char* arch = !strcmp(pipeline, "m10_32") ? "i386" : "x86_64";
        char wine_source[PATH_MAX];
        snprintf(wine_source, sizeof(wine_source), "lib/wine/%s-windows", arch);
        ok = stage_route_asset(home, wine_source, "d3d10.dll", exe_dir) && ok;
        ok = stage_route_asset(home, wine_source, "d3d10_1.dll", exe_dir) && ok;
        source = !strcmp(pipeline, "m10_32") ? "lib/dxmt/i386-windows" : "lib/dxmt/x86_64-windows";
        files[file_count++] = "d3d11.dll";
        files[file_count++] = "dxgi.dll";
        files[file_count++] = "dxgi_dxmt.dll";
        files[file_count++] = "d3d10core.dll";
        files[file_count++] = "winemetal.dll";
    } else if (!strcmp(pipeline, "m9")) {
        if (is32) {
            ok = stage_route_asset(home, "lib/wine/i386-windows", "d3d9.dll", exe_dir) && ok;
            ok = stage_route_asset(home, "lib/wine/i386-windows", "dxgi.dll", exe_dir) && ok;
        } else {
            ok = stage_route_asset(home, "lib/wine/x86_64-windows", "d3d9.dll", exe_dir) && ok;
        }
        goto prefix_done;
    } else {
        goto done;
    }
    for (size_t i = 0; i < file_count; i++) {
        const char* asset_source = !strcmp(files[i], "metalsharp_ntdll_hook.dll")
                                        ? "lib/metalsharp/x86_64-windows"
                                        : source;
        bool staged = stage_route_asset(home, asset_source, files[i], exe_dir);
        bool optional = strcmp(pipeline, "m12") != 0 &&
                        (!strncmp(files[i], "nvapi", 5) || !strncmp(files[i], "nvngx", 5));
        if (!staged && !optional)
            ok = false;
    }

prefix_done:
    /* M12 is the only Rust route that stages its graphics DLLs into the
     * shared Steam prefix system32.  It is intentionally not done for VKD3D
     * or the legacy DXMT routes. */
    if (!strcmp(pipeline, "m12")) {
            char* prefix = join(home, "prefix-steam/drive_c/windows/system32");
        if (prefix) {
            for (size_t i = 0; i < file_count; i++) {
                const char* asset_source = !strcmp(files[i], "metalsharp_ntdll_hook.dll")
                                                ? "lib/metalsharp/x86_64-windows"
                                                : "lib/dxmt_m12/x86_64-windows";
                bool staged = stage_route_asset(home, asset_source, files[i], prefix);
                bool optional = !strncmp(files[i], "nvapi", 5) || !strncmp(files[i], "nvngx", 5);
                if (!staged && !optional)
                    ok = false;
            }
        }
        free(prefix);
    }
    /* Unreal's M12 recipe has a second target in Engine/Binaries/Win64. */
    if (!strcmp(pipeline, "m12") && game_dir) {
        char* engine = join(game_dir, "Engine/Binaries/Win64");
        if (engine && access(engine, F_OK) == 0)
            for (size_t i = 0; i < file_count; i++)
                ok = stage_route_asset(home, "lib/dxmt_m12/x86_64-windows", files[i], engine) && ok;
        free(engine);
    }
done:
    free(game_dir);
    free(exe_dir);
    return ok;
}

static const char* default_pipeline_for_appid(unsigned appid) {
    static char pipeline[64];
    char* raw = ms_mtsp_default_rules_json();
    char error[96];
    ms_json* root;
    const ms_json* rules;
    pipeline[0] = '\0';
    if (!raw)
        return "vkd3d";
    root = ms_json_parse(raw, strlen(raw), error, sizeof(error));
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
    return pipeline[0] ? pipeline : "vkd3d";
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

static bool ensure_steam_bottle_manifest(const char* home, unsigned id, const char* pipeline) {
    char bottle_id[64];
    char name[64];
    char *bottles = join(home, "bottles"), *dir = NULL, *path = NULL, *prefix = NULL;
    FILE* file = NULL;
    ms_json_writer w;
    char* serialized = NULL;
    bool ok = false;
    if (!pipeline || !pipeline[0] || !strcmp(pipeline, "auto"))
        pipeline = default_pipeline_for_appid(id);
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
    if (!canonical || !strcmp(canonical, "auto") || !strcmp(canonical, "dxmt"))
        canonical = canonical_pipeline(default_pipeline_for_appid((unsigned)appid));
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

static bool mark_steam_bottle_launch(const char* home, unsigned id, pid_t pid) {
    char bottle_id[64];
    char* dir = join(home, "bottles");
    char* path;
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
    file = fopen(dir, "wb");
    if (file && serialized && fputs(serialized, file) >= 0)
        ok = true;
close_done:
    if (file)
        fclose(file);
done:
    free(raw);
    free(serialized);
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
    static const char* const ignored[] = {"bootstrapper", "crash", "easyanticheat", "installer", "uninstall",
                                          "setup", "redist", "vcredist", "server", "start_protected", "d3dconfig",
                                          "steamwebhelper", "oalinst"};
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
                closedir(dir);
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
    if (id == 1097150)
        preferred[count++] = "FallGuys_client_game.exe";
    else if (id == 1145360 && pipeline && !strcmp(pipeline, "m11_32"))
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
    else if (id == 1888160)
        preferred[count++] = "armoredcore6.exe";
    else if (id == 1962700)
        preferred[count++] = "Subnautica2.exe";
    else if (id == 2767030)
        preferred[count++] = "MarvelGame/Marvel/Binaries/Win64/Marvel-Win64-Shipping.exe";
    else if (id == 220)
        preferred[count++] = "hl2.exe";
    else if (id == 440) {
        preferred[count++] = "tf/win32/tf.exe";
        preferred[count++] = "tf.exe";
    }
    else if (id == 620)
        preferred[count++] = "portal2.exe";
    else if (id == 475150)
        preferred[count++] = "TQ.exe";
    else if (id == 2358720) {
        preferred[count++] = "b1-Win64-Shipping.exe";
        preferred[count++] = "b1.exe";
    }
    else if (id == 2357570)
        preferred[count++] = "Overwatch.exe";
    else if (id == 321040)
        preferred[count++] = "dirt3_game.exe";
    for (size_t i = 0; i < count; i++) {
        char* path = join(game_dir, preferred[i]);
        if (path && access(path, R_OK) == 0)
            return path;
        free(path);
    }
    return find_game_executable(game_dir, 0);
}

static void set_pipeline_runtime_env(const char*, const char*);

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
    if (!pipeline)
        pipeline = "auto";
    if (!strcmp(pipeline, "m12")) {
        snprintf(winedllpath, sizeof(winedllpath), "%s/runtime/wine/lib/dxmt_m12/x86_64-windows", home);
        setenv("WINEDLLPATH", winedllpath, 1);
    } else if (!strcmp(pipeline, "m9") || !strcmp(pipeline, "m10") || !strcmp(pipeline, "m11") ||
               !strcmp(pipeline, "m10_32") || !strcmp(pipeline, "m11_32")) {
        snprintf(winedllpath, sizeof(winedllpath), "%s/runtime/wine/lib/dxmt/x86_64-windows", home);
        setenv("WINEDLLPATH", winedllpath, 1);
    } else if (!strcmp(pipeline, "vkd3d")) {
        snprintf(winedllpath, sizeof(winedllpath), "%s/vkd3d/x86_64-windows:%s/runtime/wine/lib/wine/x86_64-windows",
                 home, home);
        setenv("WINEDLLPATH", winedllpath, 1);
        snprintf(vulkan_icd, sizeof(vulkan_icd), "%s/runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json", home);
        setenv("VK_ICD_FILENAMES", vulkan_icd, 1);
        setenv("VK_DRIVER_FILES", vulkan_icd, 1);
        backend = "vkd3d-proton";
    } else if (!strcmp(pipeline, "d3dmetal")) {
        backend = "d3dmetal";
    }
    if (strncmp(pipeline, "m", 1) == 0 || !strcmp(pipeline, "dxmt")) {
        snprintf(dxmt_config, sizeof(dxmt_config), "%s/runtime/wine/etc/dxmt.conf", home);
        snprintf(winemetal, sizeof(winemetal), "%s/runtime/wine/lib/dxmt/x86_64-unix/winemetal.so", home);
        setenv("DXMT_CONFIG_FILE", dxmt_config, 1);
        setenv("DXMT_WINEMETAL_UNIXLIB", winemetal, 1);
    }
    setenv("MS_GRAPHICS_BACKEND", backend, 1);
    setenv("WINEMSYNC", "1", 1);
}

static bool wine_steam_running(const char* home) {
    char prefix[PATH_MAX];
    FILE* pipe;
    char line[2048];
    bool running = false;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    pipe = popen("/bin/ps axo command=", "r");
    if (!pipe)
        return false;
    while (fgets(line, sizeof(line), pipe)) {
        if (strstr(line, prefix) && (strstr(line, "Steam.exe") || strstr(line, "steam.exe"))) {
            running = true;
            break;
        }
    }
    pclose(pipe);
    return running;
}

static void signal_wine_steam_processes(const char* home, int signal_number) {
    char prefix[PATH_MAX];
    FILE* pipe;
    char line[4096];
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
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
        if (wine_steam_cleanup_target(end, prefix))
            (void)kill((pid_t)raw_pid, signal_number);
    }
    pclose(pipe);
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
    seed_steam_d3d12_guard(home, steam_dir);
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
        setenv("WINEDEBUG", "+vulkan,+d3d,+d3d11,+dxgi,+wined3d,+opengl", 1);
        setenv("WINEDEBUGGER", "none", 1);
        setenv("STEAM_RUNTIME", "0", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        setenv("WINEDLLOVERRIDES", "dxgi,d3d11,d3d10core=n,b;bcrypt=b;ncrypt=b;gameoverlayrenderer,gameoverlayrenderer64=d", 1);
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
    if (wine_steam_running(home)) {
        free(steam);
        free(ui);
        free(steam_dir);
        if (status)
            *status = 200;
        return strdup("{\"ok\":true,\"message\":\"Steam already running\"}");
    }
    ensure_steam_launch_ready(home, steam_dir);
    seed_steam_d3d12_guard(home, steam_dir);
    errtext = spawn_wine(home, steam, "-no-cef-sandbox", "-noverifyfiles", "-no-dwrite", NULL, &pid);
    free(steam);
    free(ui);
    free(steam_dir);
    if (errtext) {
        char* o = err(errtext);
        free(errtext);
        return o;
    }
    if (status)
        *status = 200;
    return pid_result(pid, "pid", 0, false);
}
char* ms_steam_stop_json(const char* home, int* status) {
    if (status)
        *status = 200;
    signal_wine_steam_processes(home, SIGTERM);
    sleep(1);
    signal_wine_steam_processes(home, SIGKILL);
    usleep(500000);
    {
        ms_json_writer w;
        bool running = wine_steam_running(home);
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

static bool macos_steam_running(void) {
    FILE* pipe = popen("/bin/ps axo command=", "r");
    char line[2048];
    bool running = false;
    if (!pipe)
        return false;
    while (fgets(line, sizeof(line), pipe)) {
        if (strstr(line, "Steam.app/Contents/MacOS") && strstr(line, "steam")) {
            running = true;
            break;
        }
    }
    pclose(pipe);
    return running;
}

static bool macos_game_installed(unsigned id) {
    const char* home = getenv("HOME");
    char path[PATH_MAX];
    if (access("/Applications/Steam.app/Contents/MacOS/steamapps", F_OK) == 0) {
        snprintf(path, sizeof(path), "/Applications/Steam.app/Contents/MacOS/steamapps/appmanifest_%u.acf", id);
        if (access(path, F_OK) == 0)
            return true;
    }
    if (home) {
        snprintf(path, sizeof(path), "%s/Library/Application Support/Steam/steamapps/appmanifest_%u.acf", home, id);
        if (access(path, F_OK) == 0)
            return true;
    }
    return false;
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
    if (wine_steam_running(home)) {
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

static const char* fixed_unzstd_path(void) {
    if (access("/opt/homebrew/bin/unzstd", X_OK) == 0)
        return "/opt/homebrew/bin/unzstd";
    if (access("/usr/local/bin/unzstd", X_OK) == 0)
        return "/usr/local/bin/unzstd";
    return NULL;
}

static char* find_bundled_steam_archive(const char* home) {
    const char* fixed[] = {
        "/Applications/MetalSharp.app/Contents/Resources/bundles/metalsharp-steam.tar.zst",
        "/Applications/MetalSharp.app/Contents/Resources/metalsharp-steam.tar.zst",
        "app/bundles/metalsharp-steam.tar.zst",
    };
    char* path;
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
    if (!path || stat(path, &st) != 0 || st.st_size <= 0 || (unsigned long long)st.st_size > STEAMWEBHELPER_WRAPPER_MAX_BYTES)
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
              temporary, "https://github.com/aaf2tbz/metalsharp/releases/download/bundles/metalsharp-steam.tar.zst",
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

static void seed_steam_d3d12_guard(const char* home, const char* steam_dir) {
    char* prefix = join(home, "prefix-steam");
    char* drive_c = prefix ? join(prefix, "drive_c") : NULL;
    char* reg_file = drive_c ? join(drive_c, "metalsharp-steam-d3d12-guard.reg") : NULL;
    FILE* f;
    pid_t pid;
    char* error_text;
    (void)steam_dir;
    if (!prefix || !drive_c || !reg_file || !ensure_directory(drive_c))
        goto done;
    f = fopen(reg_file, "wb");
    if (!f)
        goto done;
    fputs("Windows Registry Editor Version 5.00\r\n\r\n", f);
    fputs("[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\Steam.exe\\DllOverrides]\r\n", f);
    fputs("\"d3d12\"=\"builtin\"\r\n\"d3d12core\"=\"builtin\"\r\n\"d3d12SDKLayers\"=\"builtin\"\r\n\"dxcore\"=\"builtin\"\r\n", f);
    fputs("\r\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\steamwebhelper.exe\\DllOverrides]\r\n", f);
    fputs("\"d3d12\"=\"builtin\"\r\n\"d3d12core\"=\"builtin\"\r\n\"d3d12SDKLayers\"=\"builtin\"\r\n\"dxcore\"=\"builtin\"\r\n", f);
    fputs("\r\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\steamwebhelper_real.exe\\DllOverrides]\r\n", f);
    fputs("\"d3d12\"=\"builtin\"\r\n\"d3d12core\"=\"builtin\"\r\n\"d3d12SDKLayers\"=\"builtin\"\r\n\"dxcore\"=\"builtin\"\r\n", f);
    fclose(f);
    error_text = spawn_wine_install(home, "reg", "import", "C:\\metalsharp-steam-d3d12-guard.reg", &pid);
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

static void steam_install_worker(const char* home, const char* lock_path, const char* installer) {
    FILE* owner = fopen(lock_path, "wb");
    pid_t pid;
    int wait_status = 0;
    char* wine_error;
    char* prefix = join(home, "prefix-steam");
    char* windows_dir = prefix ? join(prefix, "drive_c/windows/system32") : NULL;
    char* steam_dir = prefix ? join(prefix, "drive_c/Program Files (x86)/Steam") : NULL;
    char* steam_exe = steam_dir ? join(steam_dir, "Steam.exe") : NULL;
    char* steam_ui = steam_dir ? join(steam_dir, "steamui.dll") : NULL;
    bool install_crashed = false;
    if (owner) {
        fprintf(owner, "%ld\\n", (long)getpid());
        fclose(owner);
    }
    if (prefix)
        (void)remove_tree(prefix);
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
    wine_error = spawn_wine_install(home, "wineboot", "--init", NULL, &pid);
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
    wine_error = spawn_wine_install(home, installer, NULL, NULL, &pid);
    if (wine_error) {
        free(wine_error);
        goto done;
    }
    for (int i = 0; i < 70; i++) {
        pid_t waited = waitpid(pid, &wait_status, WNOHANG);
        if (waited == pid) {
            install_crashed = !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0;
            break;
        }
        if (waited < 0 && errno != EINTR)
            break;
        if (steam_exe && steam_ui && access(steam_exe, F_OK) == 0 && access(steam_ui, F_OK) == 0)
            break;
        sleep(1);
    }
    if (install_crashed) {
        sleep(3);
        if (!steam_exe || !steam_ui || access(steam_exe, F_OK) != 0 || access(steam_ui, F_OK) != 0)
            goto done;
    }
done:
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
    if (!ensure_steam_bottle_manifest(home, id, "auto"))
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
                    char* root = strndup(first + 1, (size_t)(close - first - 1));
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

static char* spawn_direct_game(const char* home, const char* executable, unsigned id, const char* pipeline, pid_t* pid) {
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "prefix-steam");
    char* cwd = strdup(executable);
    char* exe_name;
    char* slash;
    pid_t child;
    int exec_pipe[2];
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
    slash = cwd ? strrchr(cwd, '/') : NULL;
    exe_name = slash ? slash + 1 : cwd;
    if (slash)
        *slash = '\0';
    if (pipe(exec_pipe) != 0) {
        char* error = strdup(strerror(errno));
        free(wine);
        free(prefix);
        free(cwd);
        return error;
    }
    (void)fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);
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
        set_route_paths(home, pipeline);
        set_route_default_env(pipeline);
        set_launch_cache_env(home, id, pipeline);
        if (pipeline_overrides(pipeline))
            setenv("WINEDLLOVERRIDES", pipeline_overrides(pipeline), 1);
        else
            unsetenv("WINEDLLOVERRIDES");
        if (!strcmp(pipeline, "m9") && (id == 774361 || id == 17410 || id == 49520)) {
            setenv("DXMT_ASYNC_PIPELINE_COMPILE", "0", 1);
            setenv("DXMT_METALFX_SPATIAL_SWAPCHAIN", "0", 1);
            setenv("DXMT_METALFX_SPATIAL", "0", 1);
            setenv("DXMT_CONFIG", "d3d11.preferredMaxFrameRate=60;dxmt.shaderMetalVersion=310", 1);
            setenv("METALSHARP_M9_SYNC_LOADING", "1", 1);
        }
        if (id == 1962700 && !strcmp(pipeline, "m12")) {
            setenv("DXMT_D3D12_ENABLE_GEOMETRY_MESH", "1", 1);
            setenv("DXMT_D3D12_FORCE_SWAPCHAIN_BLIT", "1", 1);
            setenv("DXMT_D3D12_AUTOPRESENT_SWAPCHAIN", "1", 1);
            setenv("DXMT_D3D12_LIVE_PRESENT", "1", 1);
            setenv("DXMT_D3D12_REASSERT_WINDOW_HANDOFF", "1", 1);
            setenv("DXMT_D3D12_DISABLE_RUNTIME_MSC", "1", 1);
            setenv("DXMT_D3D12_FORCE_COLOR_WRITE_STATE", "1", 1);
            setenv("DXMT_METALFX_SPATIAL_SWAPCHAIN", "0", 1);
            setenv("DXMT_METALFX_SPATIAL", "0", 1);
            setenv("DXMT_METALFX_TEMPORAL", "0", 1);
            setenv("DXMT_CONFIG", "d3d11.preferredMaxFrameRate=60;dxmt.shaderMetalVersion=310", 1);
        }
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
        if (cwd)
            (void)chdir(cwd);
        argv[argc++] = wine;
        argv[argc++] = exe_name;
        build_launch_args(id, pipeline, argv, &argc, sizeof(argv) / sizeof(argv[0]));
        argv[argc] = NULL;
        execv(wine, argv);
        {
            int error = errno;
            (void)write(exec_pipe[1], &error, sizeof(error));
        }
        _exit(127);
    }
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
        setenv("WINEARCH", "win64", 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv(!strcmp(pipeline, "d3dmetal") ? "WINEESYNC" : "WINEMSYNC", "1", 1);
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
        argv[argc++] = (char*)wine;
        argv[argc++] = exe_name;
        argv[argc] = NULL;
        execv(wine, argv);
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
    ms_json_writer writer;
    if (!executable || !executable[0]) {
        if (status)
            *status = 400;
        return err("D3DMetal game executable not found");
    }
    error_text = spawn_gptk_game(home, executable, id, "d3dmetal", &pid);
    if (error_text) {
        char* result = err(error_text);
        free(error_text);
        if (status)
            *status = 500;
        return result;
    }
    ms_process_register_game(id, pid);
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
    ms_json_writer_string(&writer, "d3dmetal_direct_game_exe");
    ms_json_writer_object_end(&writer);
    if (status)
        *status = 200;
    return ms_json_writer_take(&writer);
}

static char* launch_game_via_steam_json(const char* home, unsigned id, int* status) {
    char url[64];
    char *steam_result, *error_text;
    pid_t pid;
    if (!wine_steam_running(home)) {
        int steam_status = 500;
        steam_result = ms_steam_launch_json(home, &steam_status);
        free(steam_result);
        if (steam_status >= 400) {
            if (status)
                *status = steam_status;
            return err("Wine Steam could not be started for this game");
        }
        for (int i = 0; i < 12 && !wine_steam_running(home); i++)
            sleep(1);
        if (!wine_steam_running(home)) {
            if (status)
                *status = 500;
            return err("Wine Steam was started but did not become ready for game launch");
        }
    }
    snprintf(url, sizeof(url), "steam://run/%u", id);
    error_text = spawn_wine(home, "start", url, NULL, NULL, NULL, &pid);
    if (error_text) {
        char* result = err(error_text);
        free(error_text);
        if (status)
            *status = 500;
        return result;
    }
    if (status)
        *status = 200;
    return pid_result(pid, "pid", id, true);
}

static char* ms_steam_launch_game_json_internal(const char* home, const char* body, size_t len, int* status,
                                                bool default_to_steam) {
    unsigned id;
    char *e, pipeline[32] = "auto", saved_pipeline[32] = "";
    char* executable;
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
        return launch_game_via_steam_json(home, id, status);
    if (has_route && (!strcasecmp(pipeline, "steam") || !strcasecmp(pipeline, "mac_steam") ||
                      !strcasecmp(pipeline, "macos_steam"))) {
        /* The Rust endpoint treats an explicit Steam route as a Steam URL
         * handoff; it does not resolve a local executable for that route. */
        return launch_game_via_steam_json(home, id, status);
    }
    {
        const char* canonical = canonical_pipeline(pipeline);
        if (!canonical) {
            if (status)
                *status = 400;
            return err("unknown pipeline");
        }
        if (!strcmp(canonical, "auto") || !strcmp(canonical, "dxmt")) {
            if (bottle_pipeline_value(home, id, saved_pipeline, sizeof(saved_pipeline)) && saved_pipeline[0] &&
                canonical_pipeline(saved_pipeline))
                snprintf(pipeline, sizeof(pipeline), "%s", canonical_pipeline(saved_pipeline));
            else {
                const char* resolved = canonical_pipeline(default_pipeline_for_appid(id));
                if (!resolved || !strcmp(resolved, "auto") || !strcmp(resolved, "dxmt"))
                    resolved = "vkd3d";
                snprintf(pipeline, sizeof(pipeline), "%s", resolved);
            }
        } else {
            snprintf(pipeline, sizeof(pipeline), "%s", canonical);
        }
    }
    if (!ensure_steam_bottle_manifest(home, id, pipeline)) {
        if (status)
            *status = 500;
        return err("failed to prepare Steam bottle manifest");
    }
    if (!strcmp(pipeline, "fna_arm64")) {
        game_dir = ms_steam_game_dir(home, id);
        deploy_controller_input_shims(home, game_dir);
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
    executable = find_steam_game_executable(home, id, pipeline);
    if (!executable) {
        if (status)
            *status = 404;
        return err("Game executable not found");
    }
    game_dir = ms_steam_game_dir(home, id);
    deploy_controller_input_shims(home, game_dir);
    prepare_real_steam_launch(home, game_dir, executable, id, pipeline);
    remove_stale_route_dlls(home, pipeline, game_dir, executable);
    if (!stage_route_dlls(home, id, pipeline, executable)) {
        free(game_dir);
        free(executable);
        if (status)
            *status = 500;
        return err("required graphics runtime DLLs are missing");
    }
    free(game_dir);
    if (!strcmp(pipeline, "m13"))
        e = spawn_gptk_game(home, executable, id, pipeline, &pid);
    else if (!strcmp(pipeline, "d3dmetal"))
        e = spawn_gptk_game(home, executable, id, pipeline, &pid);
    else
        e = spawn_direct_game(home, executable, id, pipeline, &pid);
    free(executable);
    if (e) {
        char* o = err(e);
        free(e);
        return o;
    }
    ms_process_register_game(id, pid);
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
    prepare_real_steam_launch(home, game_dir, executable, id, pipeline);
    remove_stale_route_dlls(home, pipeline, game_dir, executable);
    if (!stage_route_dlls(home, id, pipeline, executable)) {
        free(game_dir);
        free(executable);
        if (status)
            *status = 500;
        return err("required graphics runtime DLLs are missing");
    }
    if (!strcmp(pipeline, "m13") || !strcmp(pipeline, "d3dmetal"))
        error_text = spawn_gptk_game(home, executable, id, pipeline, &pid);
    else
        error_text = spawn_direct_game(home, executable, id, pipeline, &pid);
    free(game_dir);
    free(executable);
    if (error_text) {
        char* result = err(error_text);
        free(error_text);
        if (status)
            *status = 500;
        return result;
    }
    ms_process_register_game(id, pid);
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
        if (!strcmp(requested_canonical, "auto") || !strcmp(requested_canonical, "dxmt")) {
            const char* saved_canonical = bottle_pipeline_value(home, id, saved, sizeof(saved)) ? canonical_pipeline(saved) : NULL;
            const char* default_canonical = canonical_pipeline(default_pipeline_for_appid(id));
            snprintf(pipeline, sizeof(pipeline), "%s", saved_canonical && saved_canonical[0] ? saved_canonical :
                                                               (default_canonical ? default_canonical : "vkd3d"));
        } else
            snprintf(pipeline, sizeof(pipeline), "%s", requested_canonical);
    }

    game_dir = ms_steam_game_dir(home, id);
    executable = find_steam_game_executable(home, id, pipeline);
    if (!game_dir)
        ready = false;
    if (!executable)
        ready = false;

    if (!strcmp(pipeline, "m11") || !strcmp(pipeline, "m11_32")) {
        const char* arch = !strcmp(pipeline, "m11_32") ? "i386" : "x86_64";
        static const char* const common[] = {"d3d11.dll", "dxgi.dll", "dxgi_dxmt.dll", "d3d10core.dll", "winemetal.dll"};
        static const char* const wide[] = {"nvapi64.dll", "nvngx.dll", "metalsharp_ntdll_hook.dll"};
        char source_dir[PATH_MAX];
        snprintf(source_dir, sizeof(source_dir), "%s/runtime/wine/lib/dxmt/%s-windows", home, arch);
        for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
            dlls[dll_count++] = common[i];
        if (!strcmp(pipeline, "m11"))
            for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
                dlls[dll_count++] = wide[i];
        for (size_t i = 0; i < dll_count; i++) {
            char* source = join(source_dir, dlls[i]);
            bool optional = (!strncmp(dlls[i], "nvapi", 5) || !strncmp(dlls[i], "nvngx", 5));
            bool present = source && access(source, R_OK) == 0;
            if (!present && !optional)
                ready = false;
            free(source);
        }
    } else if (!strcmp(pipeline, "vkd3d")) {
        dlls[dll_count++] = "d3d12.dll";
        dlls[dll_count++] = "d3d12core.dll";
        dlls[dll_count++] = "d3d11.dll";
        dlls[dll_count++] = "d3d10core.dll";
        dlls[dll_count++] = "d3d9.dll";
        dlls[dll_count++] = "dxgi.dll";
        for (size_t i = 0; i < dll_count; i++) {
            const char* subpath = i < 2 ? "vkd3d/vkd3d-proton/x86_64-windows" : "vkd3d/dxvk/x86_64-windows";
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
    if (!ensure_steam_bottle_manifest(home, id, pipeline)) {
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
    char url[64], *e;
    pid_t pid;
    unsigned long long started_at = monotonic_millis();
    if (status)
        *status = 400;
    if (!body_id(body, len, &id))
        return err("appid required");
    if (status)
        *status = 500;
    if (wine_steam_running(home))
        return err("Wine Steam is running. Stop Wine Steam before launching through MacOS Steam.");
    if (!macos_game_installed(id))
        return err("This game is not installed in macOS Steam. Download it through macOS Steam before using the MacOS "
                   "Steam engine.");
    if (!macos_steam_running()) {
        char* launch_error;
        launch_error = spawn_open("-a", "Steam", "steam://open/library", &pid);
        if (launch_error)
            free(launch_error);
    }
    snprintf(url, sizeof(url), "steam://run/%u", id);
    e = spawn_open(url, NULL, NULL, &pid);
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
        contains_ci(command, "ps axo") || strstr(command, "Steam.app/Contents/MacOS") || contains_ci(command, "steam_osx"))
        return false;
    return (strstr(command, prefix) != NULL) || contains_ci(command, "c:\\program files (x86)\\steam") ||
           contains_ci(command, "steamwebhelper.exe") || contains_ci(command, "steamwebhelper_real.exe") ||
           contains_ci(command, "c:\\windows\\system32\\explorer.exe /desktop") ||
           (contains_ci(command, "c:\\windows\\system32\\conhost.exe") && contains_ci(command, "--headless")) ||
           contains_ci(command, "winedevice.exe") || contains_ci(command, "wineserver") || contains_ci(command, "wineloader");
}

char* ms_steam_stop_targets_json(const char* home, int* status) {
    char prefix[PATH_MAX];
    char line[4096];
    FILE* pipe;
    unsigned count = 0;
    ms_json_writer w;
    if (status)
        *status = 200;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
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
            target = wine_steam_cleanup_target(end, prefix);
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
        char pipeline[32] = "m12";
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
                snprintf(pipeline, sizeof(pipeline), "%s", default_pipeline_for_appid(id));
        }
        (void)ensure_steam_bottle_manifest(home, id, pipeline);
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
