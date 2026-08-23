#include "metalsharp_backend/mtsp.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/steam_actions.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct pipeline {
    const char* id;
    const char* name;
    const char* description;
    const char* backend;
    const char* graphics_backend;
    bool experimental;
    bool requires_wine;
};

static const struct pipeline pipelines[] = {
    {"dxmt", "DXMT", "Auto-selected D3D9/D3D10/D3D11/D3D12 -> Metal via unified DXMT runtime", "dxmt", "dxmt", false, true},
    {"m12", "M12", "D3D12 -> Metal via DXMT", "dxmt", "dxmt", false, true},
    {"vkd3d", "VKD3D", "Direct3D 12 via VKD3D-Proton and the bundled MoltenVK Vulkan driver", "vulkan", "vulkan", false, true},
    {"m11", "M11", "D3D11 -> Metal via DXMT", "dxmt", "dxmt", false, true},
    {"m11_32", "M11(32)", "D3D11 -> Metal via DXMT (32-bit / i386)", "dxmt", "dxmt", false, true},
    {"m10", "M10", "D3D10 -> Metal via DXMT", "dxmt", "dxmt", false, true},
    {"m10_32", "M10(32)", "D3D10 -> Metal via DXMT (32-bit / i386)", "dxmt", "dxmt", false, true},
    {"m9", "M9", "D3D9 -> Metal via DXMT launch family", "dxmt", "dxmt", false, true},
    {"m13", "M13", "D3D11/D3D12 via Apple Game Porting Toolkit", "gptk", "gptk", false, true},
    {"d3dmetal", "D3DMetal", "D3D11/D3D12 via Apple D3DMetal 4.0 (GPTK Wine)", "d3dmetal", "d3dmetal", true, false},
    {"m32", "M32", "32-bit Wine fallback", "wine32", "wine", false, true},
    {"fna_arm64", "Mono/FNA", "Windows XNA/FNA via MetalSharp Mono runtime", "mono", "native", false, false},
    {"steam", "Steam", "Wine Steam", "wine-steam", "wine", false, false},
    {"mac_steam", "MacOS Steam", "Native macOS Steam", "macos-steam", "native", false, false},
    {"wine_bare", "Wine", "Plain Wine (Custom Library)", "wine", "wine", false, true},
};

static const char* mtsp_pipeline_name(const char* id);

static bool mtsp_pipeline_user_selectable(const char* id) {
    return !strcmp(id, "m12") || !strcmp(id, "vkd3d") || !strcmp(id, "m11") || !strcmp(id, "m11_32") ||
           !strcmp(id, "m10") || !strcmp(id, "m10_32") || !strcmp(id, "m9") || !strcmp(id, "d3dmetal") ||
           !strcmp(id, "fna_arm64");
}

static unsigned long long query_appid(const char* query) {
    const char* p = query == NULL ? NULL : strstr(query, "appid=");
    char* end;
    if (p == NULL)
        return 0;
    p += 6;
    return strtoull(p, &end, 10);
}

static bool mtsp_bottle_preferred(unsigned appid, char* out, size_t out_size) {
    char path[PATH_MAX], raw[1024 * 1024], error[96];
    FILE* file;
    size_t length;
    ms_json* manifest;
    char* value = NULL;
    const char* home = getenv("METALSHARP_HOME");
    if (!home || !*home)
        return false;
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

static const char* mtsp_default_pipeline(unsigned appid) {
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

char* ms_mtsp_pipelines_json(const char* query) {
    ms_json_writer writer;
    char* result;
    size_t i;
    unsigned long long appid = query_appid(query);
    char preferred[64] = "";
    const char* recommended = mtsp_default_pipeline((unsigned)appid);
    bool has_preferred = mtsp_bottle_preferred((unsigned)appid, preferred, sizeof(preferred));
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "appid");
    ms_json_writer_u64(&writer, appid);
    ms_json_writer_key(&writer, "recommended");
    ms_json_writer_string(&writer, recommended);
    ms_json_writer_key(&writer, "recommended_name");
    ms_json_writer_string(&writer, mtsp_pipeline_name(recommended));
    ms_json_writer_key(&writer, "preferred");
    if (has_preferred)
        ms_json_writer_string(&writer, preferred);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "preferred_name");
    if (has_preferred)
        ms_json_writer_string(&writer, mtsp_pipeline_name(preferred));
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "pipelines");
    ms_json_writer_array_begin(&writer);
    for (i = 0; i < sizeof(pipelines) / sizeof(pipelines[0]); ++i) {
        if (!mtsp_pipeline_user_selectable(pipelines[i].id))
            continue;
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "id");
        ms_json_writer_string(&writer, pipelines[i].id);
        ms_json_writer_key(&writer, "name");
        ms_json_writer_string(&writer, pipelines[i].name);
        ms_json_writer_key(&writer, "description");
        ms_json_writer_string(&writer, pipelines[i].description);
        ms_json_writer_key(&writer, "backend");
        ms_json_writer_string(&writer, pipelines[i].backend);
        ms_json_writer_key(&writer, "graphics_backend");
        ms_json_writer_string(&writer, pipelines[i].graphics_backend);
        ms_json_writer_key(&writer, "experimental");
        ms_json_writer_bool(&writer, pipelines[i].experimental);
        ms_json_writer_key(&writer, "requires_wine");
        ms_json_writer_bool(&writer, pipelines[i].requires_wine);
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    return result;
}

static const char* mtsp_pipeline_name(const char* id) {
    if (!strcmp(id, "dxmt"))
        return "DXMT";
    if (!strcmp(id, "m11_32"))
        return "M11(32)";
    if (!strcmp(id, "m10_32"))
        return "M10(32)";
    if (!strcmp(id, "fna_arm64") || !strcmp(id, "fna_x86"))
        return "Mono/FNA";
    if (!strcmp(id, "wine_bare"))
        return "Wine bare";
    if (!strcmp(id, "vkd3d"))
        return "VKD3D";
    if (!strcmp(id, "d3dmetal"))
        return "D3DMetal";
    if (!strcmp(id, "m13"))
        return "M13";
    if (!strcmp(id, "m32"))
        return "M32";
    if (!strcmp(id, "steam"))
        return "Steam";
    if (!strcmp(id, "mac_steam") || !strcmp(id, "macos_steam"))
        return "MacOS Steam";
    if (!strcmp(id, "m11"))
        return "M11";
    if (!strcmp(id, "m10"))
        return "M10";
    if (!strcmp(id, "m9"))
        return "M9";
    return "M12";
}

static void mtsp_catalog_entry(ms_json_writer* writer, unsigned long long appid, const char* pipeline, const char* name,
                               const char* exe, bool offline) {
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "appid");
    ms_json_writer_u64(writer, appid);
    ms_json_writer_key(writer, "name");
    ms_json_writer_string(writer, name);
    ms_json_writer_key(writer, "default_pipeline");
    ms_json_writer_string(writer, pipeline);
    ms_json_writer_key(writer, "default_pipeline_name");
    ms_json_writer_string(writer, mtsp_pipeline_name(pipeline));
    ms_json_writer_key(writer, "custom_exe_fix");
    ms_json_writer_bool(writer, exe && *exe);
    ms_json_writer_key(writer, "exe_names");
    ms_json_writer_array_begin(writer);
    if (exe && *exe)
        ms_json_writer_string(writer, exe);
    ms_json_writer_array_end(writer);
    ms_json_writer_key(writer, "offline_capable");
    ms_json_writer_bool(writer, offline);
    ms_json_writer_key(writer, "components");
    ms_json_writer_array_begin(writer);
    ms_json_writer_array_end(writer);
    ms_json_writer_key(writer, "check_dlls");
    ms_json_writer_array_begin(writer);
    ms_json_writer_array_end(writer);
    ms_json_writer_key(writer, "env");
    ms_json_writer_object_begin(writer);
    ms_json_writer_object_end(writer);
    ms_json_writer_key(writer, "launch_shape");
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "wine_overrides");
    ms_json_writer_string(writer, "");
    ms_json_writer_key(writer, "dyld_paths");
    ms_json_writer_array_begin(writer);
    ms_json_writer_array_end(writer);
    ms_json_writer_key(writer, "winedllpath_dirs");
    ms_json_writer_array_begin(writer);
    ms_json_writer_array_end(writer);
    ms_json_writer_key(writer, "deploy_dlls");
    ms_json_writer_array_begin(writer);
    ms_json_writer_array_end(writer);
    ms_json_writer_object_end(writer);
    ms_json_writer_object_end(writer);
}

char* ms_mtsp_default_rules_json_legacy(void) {
    const char* candidates[16] = {"configs/mtsp-rules.toml", "app/configs/mtsp-rules.toml", NULL};
    char home_candidate[1024], ancestor_paths[8][1024];
    const char* home = getenv("METALSHARP_HOME");
    size_t candidate_count = 2;
    if (home && *home) {
        snprintf(home_candidate, sizeof(home_candidate), "%s/configs/mtsp-rules.toml", home);
        candidates[candidate_count++] = home_candidate;
    }
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        char current[1024];
        snprintf(current, sizeof(current), "%s", cwd);
        for (size_t i = 0; i < 8 && candidate_count < 12; i++) {
            snprintf(ancestor_paths[i], sizeof(ancestor_paths[i]), "%s/configs/mtsp-rules.toml", current);
            candidates[candidate_count++] = ancestor_paths[i];
            char* slash = strrchr(current, '/');
            if (!slash || slash == current)
                break;
            *slash = 0;
        }
    }
    FILE* file = NULL;
    char line[512], pipeline[64] = "auto", name[192] = "", exe[192] = "";
    unsigned long long appid = 0;
    size_t count = 0;
    bool offline = false, have = false;
    ms_json_writer writer;
    char* result;
    for (size_t i = 0; i < candidate_count; i++) {
        file = fopen(candidates[i], "r");
        if (file)
            break;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "rules");
    ms_json_writer_array_begin(&writer);
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            unsigned long long next_id = 0;
            char section_id[64];
            char* section = strstr(line, "[overrides.");
            bool root_section = false;
            if (section && sscanf(section, "[overrides.%63[^]]]", section_id) == 1 && section_id[0]) {
                root_section = true;
                for (size_t k = 0; section_id[k]; k++)
                    if (!isdigit((unsigned char)section_id[k]))
                        root_section = false;
                if (root_section)
                    next_id = strtoull(section_id, NULL, 10);
            }
            if (root_section) {
                if (have) {
                    mtsp_catalog_entry(&writer, appid, pipeline, name[0] ? name : "MetalSharp rule", exe, offline);
                    count++;
                }
                appid = next_id;
                snprintf(pipeline, sizeof(pipeline), "auto");
                name[0] = 0;
                exe[0] = 0;
                offline = false;
                have = true;
                continue;
            }
            if (!have)
                continue;
            char* value = strchr(line, '=');
            if (!value)
                continue;
            value++;
            while (isspace((unsigned char)*value))
                value++;
            if (strstr(line, "pipeline") == line || !strncmp(line, "pipeline", 8)) {
                char* quote = strchr(value, '"');
                if (quote) {
                    quote++;
                    char* end = strchr(quote, '"');
                    if (end) {
                        size_t n = (size_t)(end - quote);
                        if (n >= sizeof(pipeline))
                            n = sizeof(pipeline) - 1;
                        memcpy(pipeline, quote, n);
                        pipeline[n] = 0;
                    }
                }
            } else if (!strncmp(line, "name", 4)) {
                char* quote = strchr(value, '"');
                if (quote) {
                    quote++;
                    char* end = strchr(quote, '"');
                    if (end) {
                        size_t n = (size_t)(end - quote);
                        if (n >= sizeof(name))
                            n = sizeof(name) - 1;
                        memcpy(name, quote, n);
                        name[n] = 0;
                    }
                }
            } else if (!strncmp(line, "exe_names", 9)) {
                char* quote = strchr(value, '"');
                if (quote) {
                    quote++;
                    char* end = strchr(quote, '"');
                    if (end) {
                        size_t n = (size_t)(end - quote);
                        if (n >= sizeof(exe))
                            n = sizeof(exe) - 1;
                        memcpy(exe, quote, n);
                        exe[n] = 0;
                    }
                }
            } else if (!strncmp(line, "offline_capable", 15) && strstr(value, "true"))
                offline = true;
        }
        if (have) {
            mtsp_catalog_entry(&writer, appid, pipeline, name[0] ? name : "MetalSharp rule", exe, offline);
            count++;
        }
        fclose(file);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "count");
    ms_json_writer_u64(&writer, count);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    return result;
}

char* ms_mtsp_prepare_json(const unsigned char* body, size_t length, int* status) {
    return ms_steam_mtsp_inspect_json(getenv("METALSHARP_HOME"), body, length, status, 0);
}
char* ms_mtsp_recipe_json(const unsigned char* body, size_t length, int* status) {
    return ms_steam_mtsp_inspect_json(getenv("METALSHARP_HOME"), body, length, status, 1);
}
char* ms_mtsp_doctor_json(const unsigned char* body, size_t length, int* status) {
    return ms_steam_mtsp_inspect_json(getenv("METALSHARP_HOME"), body, length, status, 2);
}

static void mtsp_default_pipeline_for(unsigned long long appid, char* output, size_t output_size) {
    snprintf(output, output_size, "%s", mtsp_default_pipeline((unsigned)appid));
}

char* ms_mtsp_launch_shape_json(const char* query) {
    unsigned long long appid = query_appid(query);
    char default_pipeline[64];
    mtsp_default_pipeline_for(appid, default_pipeline, sizeof(default_pipeline));
    const char* pipeline = strstr(query ? query : "", "pipeline=");
    char requested[64];
    const char* id;
    if (pipeline) {
        const char* end = strchr(pipeline + 9, '&');
        size_t length = end ? (size_t)(end - (pipeline + 9)) : strlen(pipeline + 9);
        if (length >= sizeof(requested))
            length = sizeof(requested) - 1;
        memcpy(requested, pipeline + 9, length);
        requested[length] = 0;
        id = !strcmp(requested, "auto") || !requested[0] ? default_pipeline : requested;
    } else
        id = default_pipeline;
    const char* name = !strcmp(id, "vkd3d")       ? "VKD3D"
                       : !strcmp(id, "d3dmetal")  ? "D3DMetal"
                       : !strcmp(id, "m11")       ? "M11"
                       : !strcmp(id, "m11_32")    ? "M11(32)"
                       : !strcmp(id, "m10")       ? "M10"
                       : !strcmp(id, "m10_32")    ? "M10(32)"
                       : !strcmp(id, "m9")        ? "M9"
                       : !strcmp(id, "fna_arm64") ? "Mono/FNA"
                                                  : "M12";
    const char* backend = !strcmp(id, "vkd3d")       ? "vulkan"
                          : !strcmp(id, "d3dmetal")  ? "d3dmetal"
                          : !strcmp(id, "fna_arm64") ? "mono"
                                                     : "dxmt";
    const char* custom_exe = appid == 1145360 ? "x86/Hades.exe" : NULL;
    const char* graphics = !strcmp(id, "vkd3d")       ? "vulkan"
                           : !strcmp(id, "d3dmetal")  ? "d3dmetal"
                           : !strcmp(id, "fna_arm64") ? "mono"
                                                      : "dxmt";
    bool default_rule = !strcmp(id, default_pipeline) && strcmp(id, "vkd3d") != 0;
    ms_json_writer writer;
    char* result;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "appid");
    ms_json_writer_u64(&writer, appid);
    ms_json_writer_key(&writer, "pipeline");
    ms_json_writer_string(&writer, id);
    ms_json_writer_key(&writer, "pipeline_name");
    ms_json_writer_string(&writer, name);
    ms_json_writer_key(&writer, "backend");
    ms_json_writer_string(&writer, backend);
    ms_json_writer_key(&writer, "graphics_backend");
    ms_json_writer_string(&writer, graphics);
    ms_json_writer_key(&writer, "requires_wine");
    ms_json_writer_bool(&writer, strcmp(id, "d3dmetal") != 0);
    ms_json_writer_key(&writer, "is_default_rule");
    ms_json_writer_bool(&writer, default_rule);
    ms_json_writer_key(&writer, "custom_exe_fix");
    ms_json_writer_bool(&writer, custom_exe != NULL);
    ms_json_writer_key(&writer, "exe_names");
    ms_json_writer_array_begin(&writer);
    if (custom_exe)
        ms_json_writer_string(&writer, custom_exe);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "launch_shape");
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "deploy_dlls");
    ms_json_writer_array_begin(&writer);
    if (!strcmp(id, "vkd3d")) {
        const char* files[] = {"d3d12.dll", "d3d12core.dll", "d3d11.dll", "d3d10core.dll", "d3d9.dll", "dxgi.dll"};
        const char* sources[] = {"vkd3d-proton/x86_64-windows", "vkd3d-proton/x86_64-windows", "dxvk/x86_64-windows",
                                 "dxvk/x86_64-windows",         "dxvk/x86_64-windows",         "dxvk/x86_64-windows"};
        for (size_t i = 0; i < 6; i++) {
            ms_json_writer_object_begin(&writer);
            ms_json_writer_key(&writer, "arch");
            ms_json_writer_string(&writer, "64-bit");
            ms_json_writer_key(&writer, "dest_filename");
            ms_json_writer_null(&writer);
            ms_json_writer_key(&writer, "filename");
            ms_json_writer_string(&writer, files[i]);
            ms_json_writer_key(&writer, "source_subpath");
            ms_json_writer_string(&writer, sources[i]);
            ms_json_writer_object_end(&writer);
        }
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "dyld_paths");
    ms_json_writer_array_begin(&writer);
    if (!strcmp(id, "vkd3d"))
        ms_json_writer_string(&writer, "lib/wine/x86_64-unix");
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "wine_overrides");
    ms_json_writer_string(
        &writer, !strcmp(id, "vkd3d")
                     ? "d3d12,d3d12core,d3d11,d3d10core,dxgi,d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"
                     : "");
    ms_json_writer_key(&writer, "winedllpath_dirs");
    ms_json_writer_array_begin(&writer);
    if (!strcmp(id, "vkd3d")) {
        ms_json_writer_string(&writer, "vkd3d-proton/x86_64-windows");
        ms_json_writer_string(&writer, "dxvk/x86_64-windows");
        ms_json_writer_string(&writer, "lib/wine/x86_64-windows");
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    return result;
}
