#include "metalsharp_backend/steam.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/mtsp.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char* join_path(const char* left, const char* right) {
    size_t a = strlen(left), b = strlen(right);
    bool slash = a > 0 && left[a - 1] != '/';
    char* path = (char*)malloc(a + b + (slash ? 2 : 1));
    if (path != NULL)
        (void)snprintf(path, a + b + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return path;
}

static bool mkdir_p(const char* path) {
    char* copy = strdup(path);
    size_t i;
    if (copy == NULL)
        return false;
    for (i = 1; copy[i] != '\0'; ++i) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            copy[i] = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static char* read_file(const char* path, size_t* length_out) {
    FILE* file = fopen(path, "rb");
    long size;
    char* data;
    size_t length;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL)
            fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (char*)malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    length = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[length] = '\0';
    if (length_out != NULL)
        *length_out = length;
    return data;
}

static char* saved_key(const char* home) {
    char* path = join_path(home, "cache/steam_config.json");
    char* text;
    size_t length;
    char error[128];
    ms_json* json;
    char* key = NULL;
    if (path == NULL)
        return NULL;
    text = read_file(path, &length);
    free(path);
    if (text == NULL)
        return strdup("");
    json = ms_json_parse(text, length, error, sizeof(error));
    free(text);
    if (json != NULL)
        (void)ms_json_as_string(ms_json_object_get(json, "steam_api_key"), &key);
    ms_json_free(json);
    return key == NULL ? strdup("") : key;
}

char* ms_steam_api_key_json(const char* metalsharp_home) {
    char* key = saved_key(metalsharp_home);
    ms_json_writer writer;
    char* result;
    if (key == NULL)
        return NULL;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "key");
    ms_json_writer_string(&writer, key);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(key);
    return result;
}

typedef struct {
    unsigned appid;
    char* name;
    char* game_dir;
    bool installed;
} steam_game;

static char* acf_field(const char* text, const char* key) {
    char prefix[64];
    const char* line = text;
    (void)snprintf(prefix, sizeof(prefix), "\"%s\"", key);
    while (line != NULL && *line != '\0') {
        const char* end = strchr(line, '\n');
        size_t length = end == NULL ? strlen(line) : (size_t)(end - line);
        while (length > 0 && isspace((unsigned char)line[0])) {
            line++;
            length--;
        }
        if (length > strlen(prefix) && strncmp(line, prefix, strlen(prefix)) == 0) {
            const char* first = strchr(line + strlen(prefix), '\"');
            const char* last = first == NULL ? NULL : strchr(first + 1, '\"');
            if (first != NULL && last != NULL && last > first)
                return strndup(first + 1, (size_t)(last - first - 1));
        }
        line = end == NULL ? NULL : end + 1;
    }
    return NULL;
}

static bool game_seen(const steam_game* games, size_t count, unsigned appid) {
    size_t i;
    for (i = 0; i < count; ++i)
        if (games[i].appid == appid)
            return true;
    return false;
}

static void collect_steam_games(const char* steamapps, steam_game** games, size_t* count, size_t* capacity) {
    DIR* dir = opendir(steamapps);
    struct dirent* entry;
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        char *manifest, *text, *id_text, *name, *install, *common, *game_dir;
        char* end;
        unsigned long id;
        if (n < 17 || strncmp(entry->d_name, "appmanifest_", 12) != 0 || strcmp(entry->d_name + n - 4, ".acf") != 0)
            continue;
        manifest = join_path(steamapps, entry->d_name);
        text = manifest == NULL ? NULL : read_file(manifest, NULL);
        free(manifest);
        if (text == NULL)
            continue;
        id_text = acf_field(text, "appid");
        name = acf_field(text, "name");
        install = acf_field(text, "installdir");
        id = id_text == NULL ? 0 : strtoul(id_text, &end, 10);
        free(id_text);
        if (id == 0 || id > 0xffffffffUL || name == NULL || install == NULL ||
            game_seen(*games, *count, (unsigned)id)) {
            free(text);
            free(name);
            free(install);
            continue;
        }
        common = join_path(steamapps, "common");
        game_dir = common == NULL ? NULL : join_path(common, install);
        free(common);
        if (*count == *capacity) {
            size_t next = *capacity == 0 ? 16 : *capacity * 2;
            steam_game* p = realloc(*games, next * sizeof(*p));
            if (p == NULL) {
                free(game_dir);
                free(name);
                free(install);
                free(text);
                continue;
            }
            *games = p;
            *capacity = next;
        }
        (*games)[*count].appid = (unsigned)id;
        (*games)[*count].name = name;
        (*games)[*count].game_dir = game_dir;
        (*games)[*count].installed = true;
        (*count)++;
        free(install);
        free(text);
    }
    closedir(dir);
}

static char* vdf_path_value(const char* line) {
    const char* value;
    const char* end;
    char* result;
    size_t length;
    while (*line != '\0' && isspace((unsigned char)*line))
        line++;
    if (strncmp(line, "\"path\"", 6) != 0)
        return NULL;
    value = line + 6;
    while (*value != '\0' && isspace((unsigned char)*value))
        value++;
    if (*value != '\"')
        return NULL;
    value++;
    end = value;
    while (*end != '\0' && *end != '\"')
        end++;
    if (*end != '\"')
        return NULL;
    length = (size_t)(end - value);
    result = strndup(value, length);
    if (result == NULL)
        return NULL;
    for (size_t i = 0; i < length; i++)
        if (result[i] == '\\')
            result[i] = '/';
    return result;
}

static void collect_steam_library_tree(const char* steamapps, steam_game** games, size_t* count, size_t* capacity) {
    char* folders_path = join_path(steamapps, "libraryfolders.vdf");
    char* folders = folders_path == NULL ? NULL : read_file(folders_path, NULL);
    const char* line;
    if (folders_path != NULL)
        free(folders_path);

    collect_steam_games(steamapps, games, count, capacity);
    if (folders == NULL)
        return;

    line = folders;
    while (*line != '\0') {
        const char* next = strchr(line, '\n');
        size_t line_length = next == NULL ? strlen(line) : (size_t)(next - line);
        char* line_copy = strndup(line, line_length);
        char* library_path = line_copy == NULL ? NULL : vdf_path_value(line_copy);
        char* library_steamapps = library_path == NULL ? NULL : join_path(library_path, "steamapps");
        if (library_steamapps != NULL && strcmp(library_steamapps, steamapps) != 0 &&
            access(library_steamapps, F_OK) == 0)
            collect_steam_games(library_steamapps, games, count, capacity);
        free(library_steamapps);
        free(library_path);
        free(line_copy);
        if (next == NULL)
            break;
        line = next + 1;
    }
    free(folders);
}

static bool hidden_library_game(const steam_game* game) {
    return game->appid == 228980 ||
           (game->name != NULL && strcasecmp(game->name, "Steamworks Common Redistributables") == 0);
}

static bool bottle_string_value(const char* home, unsigned appid, const char* key, char* out, size_t out_size) {
    char path[PATH_MAX], raw[1024 * 1024];
    FILE* file;
    size_t length;
    char error[96];
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
    if (!ms_json_as_string(ms_json_object_get(manifest, key), &value) || !value || !value[0]) {
        free(value);
        ms_json_free(manifest);
        return false;
    }
    snprintf(out, out_size, "%s", value);
    free(value);
    ms_json_free(manifest);
    return true;
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

static const char* pipeline_display_name(const char* pipeline) {
    if (!pipeline)
        return "Auto";
    if (!strcmp(pipeline, "m12"))
        return "M12";
    if (!strcmp(pipeline, "m11"))
        return "M11";
    if (!strcmp(pipeline, "m11_32"))
        return "M11(32)";
    if (!strcmp(pipeline, "m10"))
        return "M10";
    if (!strcmp(pipeline, "m10_32"))
        return "M10(32)";
    if (!strcmp(pipeline, "m9"))
        return "M9";
    if (!strcmp(pipeline, "d3dmetal"))
        return "D3DMetal";
    if (!strcmp(pipeline, "fna_arm64"))
        return "Mono/FNA";
    return "VKD3D";
}

static void write_library_game(ms_json_writer* w, const char* home, const steam_game* game) {
    char cover[256], header[256];
    char preferred[64] = "";
    const char* recommended = default_pipeline_for_appid(game->appid);
    const char* effective = bottle_string_value(home, game->appid, "preferred_pipeline", preferred, sizeof(preferred))
                                ? preferred
                                : recommended;
    snprintf(cover, sizeof(cover), "https://steamcdn-a.akamaihd.net/steam/apps/%u/library_600x900.jpg", game->appid);
    snprintf(header, sizeof(header), "https://steamcdn-a.akamaihd.net/steam/apps/%u/header.jpg", game->appid);
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "appid");
    ms_json_writer_u64(w, game->appid);
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, game->name);
    ms_json_writer_key(w, "installed");
    ms_json_writer_bool(w, game->installed);
    ms_json_writer_key(w, "state");
    ms_json_writer_string(w, game->installed ? "installed" : "not_installed");
    ms_json_writer_key(w, "can_uninstall");
    ms_json_writer_bool(w, game->installed);
    ms_json_writer_key(w, "launch_method");
    ms_json_writer_string(w, effective);
    ms_json_writer_key(w, "launch_method_name");
    ms_json_writer_string(w, pipeline_display_name(effective));
    ms_json_writer_key(w, "preferred_pipeline");
    if (preferred[0])
        ms_json_writer_string(w, preferred);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "available_pipelines");
    ms_json_writer_array_begin(w);
    {
        static const char* pipeline_ids[] = {"m12", "vkd3d", "m11", "m11_32", "m10", "m10_32", "m9", "d3dmetal",
                                             "fna_arm64"};
        for (size_t i = 0; i < sizeof(pipeline_ids) / sizeof(pipeline_ids[0]); i++) {
            ms_json_writer_object_begin(w);
            ms_json_writer_key(w, "id");
            ms_json_writer_string(w, pipeline_ids[i]);
            ms_json_writer_key(w, "name");
            ms_json_writer_string(w, pipeline_display_name(pipeline_ids[i]));
            ms_json_writer_key(w, "recommended");
            ms_json_writer_bool(w, !strcmp(pipeline_ids[i], recommended));
            ms_json_writer_object_end(w);
        }
    }
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "has_native_build");
    ms_json_writer_bool(w, false);
    ms_json_writer_key(w, "native_app_path");
    ms_json_writer_null(w);
    ms_json_writer_key(w, "wine_game_path");
    if (game->game_dir == NULL)
        ms_json_writer_null(w);
    else
        ms_json_writer_string(w, game->game_dir);
    ms_json_writer_key(w, "bottle_id");
    ms_json_writer_null(w);
    ms_json_writer_key(w, "bottle_health");
    ms_json_writer_null(w);
    ms_json_writer_key(w, "bottle_runtime_assets");
    ms_json_writer_u64(w, 0);
    ms_json_writer_key(w, "cover_url");
    ms_json_writer_string(w, cover);
    ms_json_writer_key(w, "header_url");
    ms_json_writer_string(w, header);
    ms_json_writer_object_end(w);
}

char* ms_steam_library_json(const char* metalsharp_home) {
    const char* home = getenv("HOME");
    steam_game* games = NULL;
    size_t count = 0, capacity = 0, i;
    char* path;
    ms_json_writer w;
    char* sync_text = NULL;
    char* result;
    if (home != NULL) {
        const char* suffixes[] = {"Library/Application Support/Steam/steamapps", ".steam/steam/steamapps",
                                  ".local/share/Steam/steamapps"};
        for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
            path = join_path(home, suffixes[i]);
            if (path != NULL) {
                collect_steam_library_tree(path, &games, &count, &capacity);
                free(path);
            }
        }
    }
    path = join_path(metalsharp_home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
    if (path != NULL) {
        collect_steam_library_tree(path, &games, &count, &capacity);
        free(path);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    size_t visible_count = 0;
    for (i = 0; i < count; ++i)
        if (!hidden_library_game(&games[i]))
            visible_count++;
    ms_json_writer_key(&w, "total");
    ms_json_writer_u64(&w, visible_count);
    ms_json_writer_key(&w, "installed_count");
    ms_json_writer_u64(&w, visible_count);
    ms_json_writer_key(&w, "sync");
    sync_text =
        ms_steam_api_key_json(metalsharp_home); /* Replace the key response with the library sync object below. */
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "api_key_set");
    {
        char* key = saved_key(metalsharp_home);
        ms_json_writer_bool(&w, key != NULL && key[0] != '\0');
        free(key);
    }
    ms_json_writer_key(&w, "steam_id_detected");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "steam_id");
    ms_json_writer_string(&w, "");
    ms_json_writer_key(&w, "owned_games_cache");
    ms_json_writer_bool(&w, false);
    ms_json_writer_object_end(&w);
    free(sync_text);
    ms_json_writer_key(&w, "games");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < count; ++i)
        if (!hidden_library_game(&games[i]))
            write_library_game(&w, metalsharp_home, &games[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    result = ms_json_writer_take(&w);
    for (i = 0; i < count; ++i) {
        free(games[i].name);
        free(games[i].game_dir);
    }
    free(games);
    return result;
}

char* ms_steam_game_dir(const char* metalsharp_home, unsigned appid) {
    const char* home = getenv("HOME");
    steam_game* games = NULL;
    size_t count = 0, capacity = 0;
    char* result = NULL;
    if (home != NULL) {
        const char* suffixes[] = {"Library/Application Support/Steam/steamapps", ".steam/steam/steamapps",
                                  ".local/share/Steam/steamapps"};
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            char* path = join_path(home, suffixes[i]);
            if (path) {
                collect_steam_library_tree(path, &games, &count, &capacity);
                free(path);
            }
        }
    }
    {
        char* path = join_path(metalsharp_home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
        if (path) {
            collect_steam_library_tree(path, &games, &count, &capacity);
            free(path);
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (games[i].appid == appid && games[i].game_dir && access(games[i].game_dir, R_OK) == 0) {
            result = strdup(games[i].game_dir);
            break;
        }
    }
    for (size_t i = 0; i < count; i++) {
        free(games[i].name);
        free(games[i].game_dir);
    }
    free(games);
    return result;
}

static bool contains_ci(const char* text, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0)
        return true;
    for (; *text != '\0'; ++text)
        if (strncasecmp(text, needle, n) == 0)
            return true;
    return false;
}

char* ms_steam_is_running_json(const char* metalsharp_home) {
    char prefix[PATH_MAX];
    FILE* pipe = popen("/bin/ps axo command=", "r");
    char line[2048];
    bool running = false;
    ms_json_writer w;
    char* result;
    (void)snprintf(prefix, sizeof(prefix), "%s/prefix-steam", metalsharp_home);
    if (pipe != NULL) {
        while (fgets(line, sizeof(line), pipe) != NULL) {
            if ((strstr(line, prefix) != NULL && contains_ci(line, "steam.exe")) ||
                (contains_ci(line, "c:\\\\program files (x86)\\\\steam") && contains_ci(line, "steam.exe"))) {
                running = true;
                break;
            }
        }
        (void)pclose(pipe);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, running);
    ms_json_writer_object_end(&w);
    result = ms_json_writer_take(&w);
    return result;
}

char* ms_steam_bridge_status_json(const char* metalsharp_home) {
    const char* port = getenv("METALSHARP_STEAM_BRIDGE_PORT");
    ms_json_writer w;
    char* result;
    (void)metalsharp_home;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "port");
    ms_json_writer_u64(&w, port == NULL ? 18733 : strtoull(port, NULL, 10));
    ms_json_writer_object_end(&w);
    result = ms_json_writer_take(&w);
    return result;
}

char* ms_steam_watch_json(const char* metalsharp_home) {
    char* cache = join_path(metalsharp_home, "cache/steam_appids.cache");
    char* old = cache == NULL ? NULL : read_file(cache, NULL);
    unsigned old_ids[1024];
    size_t old_count = 0, i;
    steam_game* games = NULL;
    size_t count = 0, capacity = 0;
    ms_json_writer w;
    char* result;
    const char* home = getenv("HOME");
    if (old != NULL) {
        char* p = old;
        while (*p != '\0' && old_count < 1024) {
            while (*p != '\0' && !isdigit((unsigned char)*p))
                p++;
            if (*p == '\0')
                break;
            old_ids[old_count++] = (unsigned)strtoul(p, &p, 10);
        }
    }
    if (home != NULL) {
        const char* suffixes[] = {"Library/Application Support/Steam/steamapps", ".steam/steam/steamapps",
                                  ".local/share/Steam/steamapps"};
        for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
            char* path = join_path(home, suffixes[i]);
            if (path != NULL) {
                collect_steam_library_tree(path, &games, &count, &capacity);
                free(path);
            }
        }
    }
    {
        char* path = join_path(metalsharp_home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
        if (path != NULL) {
            collect_steam_library_tree(path, &games, &count, &capacity);
            free(path);
        }
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "new_appids");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < count; ++i) {
        size_t j;
        bool found = false;
        for (j = 0; j < old_count; ++j)
            if (old_ids[j] == games[i].appid)
                found = true;
        if (!found)
            ms_json_writer_u64(&w, games[i].appid);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    result = ms_json_writer_take(&w);
    if (cache != NULL) {
        FILE* f;
        char* parent = join_path(metalsharp_home, "cache");
        (void)mkdir_p(parent == NULL ? metalsharp_home : parent);
        free(parent);
        f = fopen(cache, "wb");
        if (f != NULL) {
            fputs("{\"version\":1,\"appids\":[", f);
            for (i = 0; i < count; i++)
                fprintf(f, "%s%u", i ? "," : "", games[i].appid);
            fputs("]}", f);
            fclose(f);
        }
    }
    free(cache);
    free(old);
    for (i = 0; i < count; i++) {
        free(games[i].name);
        free(games[i].game_dir);
    }
    free(games);
    return result;
}

char* ms_steam_save_api_key_json(const char* metalsharp_home, const unsigned char* body, size_t body_length,
                                 int* status) {
    ms_json* request = NULL;
    char error[128];
    char* key = NULL;
    char* cache = join_path(metalsharp_home, "cache");
    char* path = join_path(metalsharp_home, "cache/steam_config.json");
    char* owned = join_path(metalsharp_home, "cache/owned_games.json");
    FILE* file;
    ms_json_writer writer;
    char* json;
    char* result;
    if (status != NULL)
        *status = 500;
    if (body != NULL && body_length > 0)
        request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
    if (request != NULL)
        (void)ms_json_as_string(ms_json_object_get(request, "key"), &key);
    if (key == NULL)
        key = strdup("");
    if (cache == NULL || path == NULL || owned == NULL || !mkdir_p(cache))
        goto fail;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "steam_api_key");
    ms_json_writer_string(&writer, key);
    ms_json_writer_key(&writer, "steam_id");
    ms_json_writer_string(&writer, "");
    ms_json_writer_object_end(&writer);
    json = ms_json_writer_take(&writer);
    if (json == NULL)
        goto fail;
    file = fopen(path, "wb");
    if (file == NULL || fputs(json, file) < 0 || fclose(file) != 0) {
        if (file != NULL)
            fclose(file);
        free(json);
        goto fail;
    }
    free(json);
    (void)unlink(owned);
    result = ms_steam_library_json(metalsharp_home);
    if (result != NULL) {
        char* library = result;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, true);
        ms_json_writer_key(&writer, "library");
        ms_json_writer_raw(&writer, library);
        ms_json_writer_key(&writer, "sync");
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "api_key_set");
        ms_json_writer_bool(&writer, key[0] != '\0');
        ms_json_writer_key(&writer, "steam_id_detected");
        ms_json_writer_bool(&writer, false);
        ms_json_writer_key(&writer, "steam_id");
        ms_json_writer_string(&writer, "");
        ms_json_writer_key(&writer, "owned_games_cache");
        ms_json_writer_bool(&writer, false);
        ms_json_writer_object_end(&writer);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
        free(library);
    }
    if (status != NULL)
        *status = result == NULL ? 500 : 200;
    free(cache);
    free(path);
    free(owned);
    free(key);
    ms_json_free(request);
    return result;
fail:
    if (status != NULL)
        *status = 500;
    free(cache);
    free(path);
    free(owned);
    free(key);
    ms_json_free(request);
    return strdup("{\"ok\":false,\"error\":\"failed to save Steam API key\"}");
}

char* ms_steam_status_json(const char* metalsharp_home) {
    const char* home = getenv("HOME");
    char* wine_prefix = join_path(metalsharp_home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    char* wine_exe = wine_prefix == NULL ? NULL : join_path(wine_prefix, "Steam.exe");
    char* wine = join_path(metalsharp_home, "runtime/wine/bin/wine");
    char* wine_wrapper = join_path(metalsharp_home, "runtime/wine/bin/metalsharp-wine");
    char* install_lock = join_path(metalsharp_home, ".steam-installing");
    char* mac_app = home == NULL ? NULL : join_path(home, "Applications/Steam.app");
    char* mac_bundle = home == NULL ? NULL : join_path(home, "Library/Application Support/Steam/Steam.AppBundle/Steam/Steam.app");
    bool windows_installed = wine_exe != NULL && access(wine_exe, F_OK) == 0;
    bool installing = install_lock != NULL && access(install_lock, F_OK) == 0;
    bool mac_installed = access("/Applications/Steam.app", F_OK) == 0 ||
                         (mac_app != NULL && access(mac_app, F_OK) == 0) ||
                         (mac_bundle != NULL && access(mac_bundle, F_OK) == 0);
    bool running = false;
    bool mac_running = false;
    FILE* process_pipe = popen("/bin/ps axo command=", "r");
    char process_line[2048];
    if (process_pipe != NULL) {
        while (fgets(process_line, sizeof(process_line), process_pipe) != NULL) {
            if (strstr(process_line, metalsharp_home) != NULL && contains_ci(process_line, "steam.exe")) {
                running = true;
            }
            if (strstr(process_line, "/Steam.app/Contents/MacOS/steam_osx") != NULL ||
                strstr(process_line, "Steam Helper.app/Contents/MacOS") != NULL)
                mac_running = true;
        }
        (void)pclose(process_pipe);
    }
    ms_json_writer writer;
    char* result;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "installed");
    ms_json_writer_bool(&writer, windows_installed);
    ms_json_writer_key(&writer, "path");
    if (windows_installed)
        ms_json_writer_string(&writer, wine_prefix);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "login_state");
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "state");
    ms_json_writer_string(&writer, "unknown");
    ms_json_writer_key(&writer, "account");
    ms_json_writer_null(&writer);
    ms_json_writer_object_end(&writer);
    ms_json_writer_key(&writer, "mac_installed");
    ms_json_writer_bool(&writer, mac_installed);
    ms_json_writer_key(&writer, "mac_path");
    if (access("/Applications/Steam.app", F_OK) == 0)
        ms_json_writer_string(&writer, "/Applications/Steam.app");
    else if (mac_app && access(mac_app, F_OK) == 0)
        ms_json_writer_string(&writer, mac_app);
    else if (mac_bundle && access(mac_bundle, F_OK) == 0)
        ms_json_writer_string(&writer, mac_bundle);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "mac_install_url");
    ms_json_writer_string(&writer, "https://store.steampowered.com/about/");
    ms_json_writer_key(&writer, "mac_running");
    ms_json_writer_bool(&writer, mac_running);
    ms_json_writer_key(&writer, "running");
    ms_json_writer_bool(&writer, running);
    ms_json_writer_key(&writer, "metalsharp_wine_available");
    ms_json_writer_bool(&writer, (wine != NULL && access(wine, F_OK) == 0) ||
                                      (wine_wrapper != NULL && access(wine_wrapper, F_OK) == 0));
    ms_json_writer_key(&writer, "installing");
    ms_json_writer_bool(&writer, installing);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(wine_prefix);
    free(wine_exe);
    free(wine);
    free(wine_wrapper);
    free(install_lock);
    free(mac_app);
    free(mac_bundle);
    return result;
}
