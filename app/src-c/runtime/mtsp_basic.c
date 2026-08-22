#include "metalsharp_backend/mtsp_basic.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <ctype.h>
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
    bool experimental;
    bool requires_wine;
};

static const struct pipeline pipelines[] = {
    {"m12", "M12", "D3D12 -> Metal via DXMT", "dxmt", false, true},
    {"vkd3d", "VKD3D", "Direct3D 12 via VKD3D-Proton and the bundled MoltenVK Vulkan driver", "vulkan", false, true},
    {"m11", "M11", "D3D11 -> Metal via DXMT", "dxmt", false, true},
    {"m11_32", "M11(32)", "D3D11 -> Metal via DXMT (32-bit / i386)", "dxmt", false, true},
    {"m10", "M10", "D3D10 -> Metal via DXMT", "dxmt", false, true},
    {"m10_32", "M10(32)", "D3D10 -> Metal via DXMT (32-bit / i386)", "dxmt", false, true},
    {"m9", "M9", "D3D9 -> Metal via DXMT launch family", "dxmt", false, true},
    {"d3dmetal", "D3DMetal", "D3D11/D3D12 via Apple D3DMetal 4.0 (GPTK Wine)", "d3dmetal", true, false},
    {"fna_arm64", "Mono/FNA", "Windows XNA/FNA via MetalSharp Mono runtime", "mono", false, false},
};

static unsigned long long query_appid(const char* query) {
    const char* p = query == NULL ? NULL : strstr(query, "appid=");
    char* end;
    if (p == NULL)
        return 0;
    p += 6;
    return strtoull(p, &end, 10);
}

char* ms_mtsp_pipelines_json(const char* query) {
    ms_json_writer writer;
    char* result;
    size_t i;
    unsigned long long appid = query_appid(query);
    (void)appid;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "appid");
    ms_json_writer_u64(&writer, appid);
    ms_json_writer_key(&writer, "recommended");
    ms_json_writer_string(&writer, appid == 620 ? "m12" : "vkd3d");
    ms_json_writer_key(&writer, "recommended_name");
    ms_json_writer_string(&writer, appid == 620 ? "M12" : "VKD3D");
    ms_json_writer_key(&writer, "preferred");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "preferred_name");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "pipelines");
    ms_json_writer_array_begin(&writer);
    for (i = 0; i < sizeof(pipelines) / sizeof(pipelines[0]); ++i) {
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "id");
        ms_json_writer_string(&writer, pipelines[i].id);
        ms_json_writer_key(&writer, "name");
        ms_json_writer_string(&writer, pipelines[i].name);
        ms_json_writer_key(&writer, "description");
        ms_json_writer_string(&writer, pipelines[i].description);
        ms_json_writer_key(&writer, "backend");
        ms_json_writer_string(&writer, pipelines[i].backend);
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
    const char* candidates[16] = {"configs/mtsp-rules.toml", "app/configs/mtsp-rules.toml",
                                  "/Volumes/AverySSD/MetalSharp/configs/mtsp-rules.toml",
                                  "/Volumes/AverySSD/MetalSharp/app/configs/mtsp-rules.toml", NULL};
    char home_candidate[1024], ancestor_paths[8][1024];
    const char* home = getenv("METALSHARP_HOME");
    size_t candidate_count = 4;
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

static char* mtsp_error(const char* message) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
static bool mtsp_body(const unsigned char* body, size_t length, unsigned long long* appid, char* pipeline,
                      size_t pipeline_size, int* status) {
    char error[128];
    ms_json* v;
    const ms_json* x;
    long long n;
    char* s = NULL;
    v = ms_json_parse((const char*)(body == NULL ? (const unsigned char*)"{}" : body), body == NULL ? 2 : length, error,
                      sizeof(error));
    if (!v || ms_json_type_of(v) != MS_JSON_OBJECT) {
        ms_json_free(v);
        if (status)
            *status = 400;
        return false;
    }
    x = ms_json_object_get(v, "appid");
    if (!ms_json_as_i64(x, &n) || n <= 0) {
        ms_json_free(v);
        if (status)
            *status = 400;
        return false;
    }
    *appid = (unsigned long long)n;
    x = ms_json_object_get(v, "launchMethod");
    if (!x)
        x = ms_json_object_get(v, "pipeline");
    if (x && ms_json_as_string(x, &s))
        snprintf(pipeline, pipeline_size, "%s", s);
    else
        snprintf(pipeline, pipeline_size, "m12");
    free(s);
    ms_json_free(v);
    if (status)
        *status = 200;
    return true;
}
static bool known_pipeline(const char* id) {
    size_t i;
    for (i = 0; i < sizeof(pipelines) / sizeof(pipelines[0]); i++)
        if (strcmp(id, pipelines[i].id) == 0)
            return true;
    return strcmp(id, "auto") == 0 || strcmp(id, "wine_bare") == 0 || strcmp(id, "steam") == 0 ||
           strcmp(id, "mac_steam") == 0;
}
static char* mtsp_simple_result(const unsigned char* body, size_t length, int* status, const char* kind) {
    unsigned long long appid;
    char pipeline[64];
    ms_json_writer w;
    char* o;
    if (!mtsp_body(body, length, &appid, pipeline, sizeof(pipeline), status))
        return mtsp_error("appid required");
    if (!known_pipeline(pipeline)) {
        if (status)
            *status = 400;
        return mtsp_error("unknown pipeline");
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, kind);
    if (strcmp(kind, "recipe") == 0) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "pipeline");
        ms_json_writer_string(&w, pipeline);
        ms_json_writer_key(&w, "env");
        ms_json_writer_object_begin(&w);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "dlls");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
    } else if (strcmp(kind, "report") == 0) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "pipeline");
        ms_json_writer_string(&w, pipeline);
        ms_json_writer_key(&w, "ready");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "issues");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
    } else {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "pipeline");
        ms_json_writer_string(&w, pipeline);
        ms_json_writer_key(&w, "prepared");
        ms_json_writer_bool(&w, true);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
char* ms_mtsp_prepare_json(const unsigned char* body, size_t length, int* status) {
    return mtsp_simple_result(body, length, status, "result");
}
char* ms_mtsp_recipe_json(const unsigned char* body, size_t length, int* status) {
    return mtsp_simple_result(body, length, status, "recipe");
}
char* ms_mtsp_doctor_json(const unsigned char* body, size_t length, int* status) {
    return mtsp_simple_result(body, length, status, "report");
}

static void mtsp_default_pipeline_for(unsigned long long appid, char* output, size_t output_size) {
    snprintf(output, output_size, "%s",
             appid == 620                            ? "m12"
             : (appid == 1145360 || appid == 475150) ? "m11_32"
                                                     : "vkd3d");
    if (appid == 620)
        return;
    const char* paths[] = {"configs/mtsp-rules.toml", "app/configs/mtsp-rules.toml"};
    FILE* file = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        file = fopen(paths[i], "r");
        if (file)
            break;
    }
    if (!file)
        return;
    char line[512];
    bool matching = false;
    while (fgets(line, sizeof(line), file)) {
        char section_id[64];
        char* section = strstr(line, "[overrides.");
        bool root = false;
        if (section && sscanf(section, "[overrides.%63[^]]]", section_id) == 1 && section_id[0]) {
            root = true;
            for (size_t k = 0; section_id[k]; k++)
                if (!isdigit((unsigned char)section_id[k]))
                    root = false;
        }
        if (root) {
            unsigned long long current = strtoull(section_id, NULL, 10);
            matching = current == appid;
            continue;
        }
        if (matching && (!strncmp(line, "pipeline", 8) || !strncmp(line, "pipeline ", 9))) {
            char* q = strchr(line, '"');
            if (q) {
                q++;
                char* end = strchr(q, '"');
                if (end) {
                    size_t n = (size_t)(end - q);
                    if (n >= output_size)
                        n = output_size - 1;
                    memcpy(output, q, n);
                    output[n] = 0;
                    break;
                }
            }
        }
    }
    fclose(file);
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
