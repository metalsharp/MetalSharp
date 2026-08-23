#include "metalsharp_backend/game.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/steam.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char* bad(const char* s) {
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
static bool parse_appid(const unsigned char* body, size_t len, unsigned long long* id, ms_json** parsed) {
    char e[64];
    ms_json* j = ms_json_parse(body ? (const char*)body : "", body ? len : 0, e, sizeof(e));
    long long n;
    bool ok = j && ms_json_type_of(j) == MS_JSON_OBJECT && ms_json_as_i64(ms_json_object_get(j, "appid"), &n) && n > 0;
    if (ok)
        *id = (unsigned long long)n;
    if (parsed)
        *parsed = j;
    else
        ms_json_free(j);
    return ok;
}
static char* manifest_pipeline(const char* home, unsigned long long id) {
    char path[2048], *raw, *pipeline = NULL;
    FILE* f;
    long n;
    size_t got;
    char e[64];
    ms_json* j;
    snprintf(path, sizeof(path), "%s/bottles/steam_%llu/bottle.json", home, id);
    f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f)
            fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    raw = malloc((size_t)n + 1);
    if (!raw) {
        fclose(f);
        return NULL;
    }
    got = fread(raw, 1, (size_t)n, f);
    fclose(f);
    raw[got] = 0;
    j = ms_json_parse(raw, got, e, sizeof(e));
    free(raw);
    if (j && ms_json_type_of(j) == MS_JSON_OBJECT) {
        ms_json_as_string(ms_json_object_get(j, "preferred_pipeline"), &pipeline);
        if (!pipeline)
            ms_json_as_string(ms_json_object_get(j, "launch_pipeline"), &pipeline);
    }
    ms_json_free(j);
    return pipeline;
}
static const char* pipeline_name(const char* pipeline) {
    if (!strcmp(pipeline, "vkd3d"))
        return "VKD3D-Proton";
    if (!strcmp(pipeline, "d3dmetal"))
        return "D3DMetal (GPTK)";
    if (!strcmp(pipeline, "fna_arm64"))
        return "FNA / Mono ARM64";
    if (!strcmp(pipeline, "m12"))
        return "M12";
    if (!strcmp(pipeline, "m11"))
        return "M11";
    if (!strcmp(pipeline, "m10"))
        return "M10";
    if (!strcmp(pipeline, "m9"))
        return "M9";
    return "Wine";
}
static const char* graphics_backend(const char* pipeline) {
    if (!strcmp(pipeline, "vkd3d"))
        return "vkd3d-proton";
    if (!strcmp(pipeline, "d3dmetal"))
        return "d3dmetal";
    if (!strcmp(pipeline, "wine_bare"))
        return "wine";
    if (!strcmp(pipeline, "fna_arm64"))
        return "fna_arm64";
    return "dxmt";
}
char* ms_game_resolve_json(const char* home, const unsigned char* body, size_t len, int* status) {
    unsigned long long id;
    ms_json* j = NULL;
    char *preferred, *pipeline;
    ms_json_writer w;
    char* o;
    if (!parse_appid(body, len, &id, &j)) {
        if (status)
            *status = 400;
        return bad("appid required");
    }
    preferred = manifest_pipeline(home, id);
    pipeline = preferred ? strdup(preferred) : strdup("vkd3d");
    if (!pipeline) {
        free(preferred);
        ms_json_free(j);
        if (status)
            *status = 500;
        return bad("out of memory");
    }
    if (!strcmp(pipeline, "auto"))
        snprintf(pipeline, 16, "vkd3d");
    if (status)
        *status = 200;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, id);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "pipeline_name");
    ms_json_writer_string(&w, pipeline_name(pipeline));
    ms_json_writer_key(&w, "preferred_pipeline");
    if (preferred)
        ms_json_writer_string(&w, preferred);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "graphics_backend");
    ms_json_writer_string(&w, graphics_backend(pipeline));
    ms_json_writer_key(&w, "backend");
    ms_json_writer_string(&w, graphics_backend(pipeline));
    ms_json_writer_key(&w, "offline_capable");
    ms_json_writer_bool(&w, !strcmp(pipeline, "d3dmetal"));
    ms_json_writer_key(&w, "recipe");
    ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(preferred);
    free(pipeline);
    ms_json_free(j);
    return o;
}
static char* join_game_path(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    char* p = malloc(x + y + 2);
    if (p)
        snprintf(p, x + y + 2, "%s/%s", a, b);
    return p;
}
static char* find_named_suffix(const char* root, const char* suffix, unsigned depth) {
    DIR* d;
    struct dirent* e;
    if (depth > 5)
        return NULL;
    d = opendir(root);
    if (!d)
        return NULL;
    while ((e = readdir(d)) != NULL) {
        char* p;
        struct stat st;
        size_t n;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        p = join_game_path(root, e->d_name);
        if (!p)
            continue;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            n = strlen(e->d_name);
            if (n > strlen(suffix) && !strcasecmp(e->d_name + n - strlen(suffix), suffix)) {
                closedir(d);
                return p;
            }
        }
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            char* found = find_named_suffix(p, suffix, depth + 1);
            if (found) {
                free(p);
                closedir(d);
                return found;
            }
        }
        free(p);
    }
    closedir(d);
    return NULL;
}
static char* find_app_bundle(const char* root, unsigned depth) {
    DIR* d;
    struct dirent* e;
    if (depth > 2)
        return NULL;
    d = opendir(root);
    if (!d)
        return NULL;
    while ((e = readdir(d)) != NULL) {
        char* p;
        struct stat st;
        size_t n;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        p = join_game_path(root, e->d_name);
        if (!p)
            continue;
        n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".app") && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            closedir(d);
            return p;
        }
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            char* found = find_app_bundle(p, depth + 1);
            if (found) {
                free(p);
                closedir(d);
                return found;
            }
        }
        free(p);
    }
    closedir(d);
    return NULL;
}
static char* acf_install_dir(const char* steamapps, unsigned long long id) {
    char path[2048], *raw, *name = NULL;
    FILE* f;
    long n;
    size_t got;
    snprintf(path, sizeof(path), "%s/appmanifest_%llu.acf", steamapps, id);
    f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f)
            fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    raw = malloc((size_t)n + 1);
    if (!raw) {
        fclose(f);
        return NULL;
    }
    got = fread(raw, 1, (size_t)n, f);
    fclose(f);
    raw[got] = 0;
    for (char* line = strtok(raw, "\n"); line; line = strtok(NULL, "\n")) {
        char* key = strstr(line, "\"installdir\"");
        if (key) {
            char* open = strchr(key + strlen("\"installdir\""), '\"');
            if (open) {
                char* value = open + 1;
                char* end = strchr(value, '\"');
                if (end) {
                    *end = 0;
                    name = strdup(value);
                    break;
                }
            }
        }
    }
    free(raw);
    return name;
}
static void inspect_dual_dir(const char* dir, char** mac_dir, char** mac_app, char** wine_dir) {
    char *app = find_app_bundle(dir, 0), *exe = find_named_suffix(dir, ".exe", 0);
    if (app && !*mac_dir) {
        *mac_dir = strdup(dir);
        *mac_app = app;
    } else
        free(app);
    if (exe && !*wine_dir)
        *wine_dir = strdup(dir);
    free(exe);
}
static char* steam_common_game(const char* steamapps, unsigned long long id) {
    char *name = acf_install_dir(steamapps, id), *common, *dir;
    if (!name)
        return NULL;
    common = join_game_path(steamapps, "common");
    dir = common ? join_game_path(common, name) : NULL;
    free(common);
    free(name);
    return dir;
}
char* ms_game_dual_json(const char* home, const char* query, int* status) {
    const char* p = query ? strstr(query, "appid=") : NULL;
    unsigned long long id = p ? strtoull(p + 6, NULL, 10) : 0;
    char *mac_dir = NULL, *mac_app = NULL, *wine_dir = NULL, *dir, *common;
    const char* user = getenv("HOME");
    const char* suffixes[] = {"Library/Application Support/Steam/steamapps", ".steam/steam/steamapps",
                              ".local/share/Steam/steamapps"};
    ms_json_writer w;
    char* o;
    if (!id) {
        if (status)
            *status = 400;
        return bad("appid required");
    }
    dir = join_game_path(home, "games");
    {
        char id_text[64];
        snprintf(id_text, sizeof(id_text), "%llu", id);
        common = dir ? join_game_path(dir, id_text) : NULL;
    }
    free(dir);
    if (common) {
        inspect_dual_dir(common, &mac_dir, &mac_app, &wine_dir);
        free(common);
    }
    if (user)
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            dir = join_game_path(user, suffixes[i]);
            if (!dir)
                continue;
            common = steam_common_game(dir, id);
            if (common) {
                inspect_dual_dir(common, &mac_dir, &mac_app, &wine_dir);
                free(common);
            }
            free(dir);
        }
    dir = join_game_path(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
    if (dir) {
        common = steam_common_game(dir, id);
        if (common) {
            inspect_dual_dir(common, &mac_dir, &mac_app, &wine_dir);
            free(common);
        }
        free(dir);
    }
    if (status)
        *status = 200;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, id);
    ms_json_writer_key(&w, "has_native_build");
    ms_json_writer_bool(&w, mac_app != NULL);
    ms_json_writer_key(&w, "macos_dir");
    if (mac_dir)
        ms_json_writer_string(&w, mac_dir);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "macos_app");
    if (mac_app)
        ms_json_writer_string(&w, mac_app);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "wine_dir");
    if (wine_dir)
        ms_json_writer_string(&w, wine_dir);
    else
        ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(mac_dir);
    free(mac_app);
    free(wine_dir);
    return o;
}
static bool copy_file(const char* src, const char* dst) {
    FILE *in = fopen(src, "rb"), *out;
    unsigned char buf[16384];
    size_t n;
    struct stat st;
    bool ok = true;
    if (!in || stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (in)
            fclose(in);
        return false;
    }
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    fclose(out);
    return ok;
}
static bool goldberg_active(const char* home, unsigned long long id) {
    char p[2048];
    snprintf(p, sizeof(p), "%s/goldberg/%llu/active", home, id);
    return access(p, F_OK) == 0;
}
static char* goldberg_source(const char* home) {
    char p[2048];
    snprintf(p, sizeof(p), "%s/runtime/goldberg/x64/steam_api64.dll", home);
    if (access(p, R_OK) == 0)
        return strdup(p);
    snprintf(p, sizeof(p), "%s/assets/goldberg/x64/steam_api64.dll", home);
    if (access(p, R_OK) == 0)
        return strdup(p);
    snprintf(p, sizeof(p), "%s/Assets/goldberg/x64/steam_api64.dll", home);
    if (access(p, R_OK) == 0)
        return strdup(p);
    return NULL;
}
static char* goldberg_status_json(const char* home, unsigned long long id, int* status) {
    bool active = goldberg_active(home, id);
    ms_json_writer w;
    char* o;
    if (status)
        *status = 200;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, id);
    ms_json_writer_key(&w, "goldberg_active");
    ms_json_writer_bool(&w, active);
    ms_json_writer_key(&w, "persisted_active");
    ms_json_writer_bool(&w, active);
    ms_json_writer_key(&w, "cache_files_ok");
    ms_json_writer_bool(&w, active);
    ms_json_writer_key(&w, "backed_up_at");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "cache_files");
    ms_json_writer_array_begin(&w);
    if (active)
        ms_json_writer_string(&w, "steam_api64.dll");
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, "vkd3d");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
char* ms_goldberg_json(const char* home, const char* query, int* status) {
    const char* p = query ? strstr(query, "appid=") : NULL;
    unsigned long long id = p ? strtoull(p + 6, NULL, 10) : 0;
    if (!id)
        return goldberg_status_json(home, 0, status);
    return goldberg_status_json(home, id, status);
}
char* ms_goldberg_toggle_json(const char* home, const unsigned char* body, size_t len, int* status) {
    unsigned long long id;
    ms_json* j = NULL;
    bool enable = true;
    char game_dir[2048], marker_dir[2048], marker[2048];
    char* discovered_game_dir = NULL;
    FILE* f;
    if (!parse_appid(body, len, &id, &j)) {
        if (status)
            *status = 400;
        return bad("appid required");
    }
    ms_json_as_bool(ms_json_object_get(j, "enable"), &enable);
    discovered_game_dir = ms_steam_game_dir(home, (unsigned)id);
    if (discovered_game_dir) {
        snprintf(game_dir, sizeof(game_dir), "%s", discovered_game_dir);
        free(discovered_game_dir);
    } else
        snprintf(game_dir, sizeof(game_dir), "%s/games/%llu", home, id);
    if (access(game_dir, F_OK) != 0) {
        ms_json_free(j);
        if (status)
            *status = 404;
        return bad("game directory not found");
    }
    snprintf(marker_dir, sizeof(marker_dir), "%s/goldberg/%llu", home, id);
    snprintf(marker, sizeof(marker), "%s/active", marker_dir);
    if (enable) {
        char goldberg_root[2048], target[2048], backup[2048];
        char* source;
        snprintf(goldberg_root, sizeof(goldberg_root), "%s/goldberg", home);
        snprintf(target, sizeof(target), "%s/steam_api64.dll", game_dir);
        snprintf(backup, sizeof(backup), "%s/steam_api64.dll.original", marker_dir);
        source = goldberg_source(home);
        if (!source) {
            free(source);
            ms_json_free(j);
            if (status)
                *status = 500;
            return bad("Goldberg steam_api64.dll source not found");
        }
        mkdir(goldberg_root, 0755);
        mkdir(marker_dir, 0755);
        if (access(backup, F_OK) != 0 && access(target, F_OK) == 0 && !copy_file(target, backup)) {
            free(source);
            ms_json_free(j);
            if (status)
                *status = 500;
            return bad("failed to back up steam_api64.dll");
        }
        if (!copy_file(source, target)) {
            free(source);
            ms_json_free(j);
            if (status)
                *status = 500;
            return bad("failed to deploy Goldberg steam_api64.dll");
        }
        free(source);
        f = fopen(marker, "wb");
        if (f) {
            fputs("active", f);
            fclose(f);
        }
    } else {
        char target[2048], backup[2048];
        snprintf(target, sizeof(target), "%s/steam_api64.dll", game_dir);
        snprintf(backup, sizeof(backup), "%s/steam_api64.dll.original", marker_dir);
        if (access(backup, F_OK) == 0) {
            unlink(target);
            copy_file(backup, target);
        } else
            unlink(target);
        unlink(marker);
    }
    ms_json_free(j);
    return goldberg_status_json(home, id, status);
}
