#include "metalsharp_backend/steam_basic.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

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

static void write_library_game(ms_json_writer* w, const steam_game* game) {
    char cover[256], header[256];
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
    ms_json_writer_string(w, "auto");
    ms_json_writer_key(w, "launch_method_name");
    ms_json_writer_string(w, "Auto");
    ms_json_writer_key(w, "preferred_pipeline");
    ms_json_writer_null(w);
    ms_json_writer_key(w, "available_pipelines");
    ms_json_writer_array_begin(w);
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_string(w, "auto");
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, "Auto");
    ms_json_writer_key(w, "recommended");
    ms_json_writer_bool(w, true);
    ms_json_writer_object_end(w);
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
                collect_steam_games(path, &games, &count, &capacity);
                free(path);
            }
        }
    }
    path = join_path(metalsharp_home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
    if (path != NULL) {
        collect_steam_games(path, &games, &count, &capacity);
        free(path);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "total");
    ms_json_writer_u64(&w, count);
    ms_json_writer_key(&w, "installed_count");
    ms_json_writer_u64(&w, count);
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
        write_library_game(&w, &games[i]);
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
                collect_steam_games(path, &games, &count, &capacity);
                free(path);
            }
        }
    }
    {
        char* path = join_path(metalsharp_home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
        if (path != NULL) {
            collect_steam_games(path, &games, &count, &capacity);
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
    char* install_lock = join_path(metalsharp_home, ".steam-installing");
    char* mac_path = home == NULL ? NULL : join_path(home, "Library/Application Support/Steam/steamapps");
    bool windows_installed = wine_exe != NULL && access(wine_exe, F_OK) == 0;
    bool installing = install_lock != NULL && access(install_lock, F_OK) == 0;
    bool mac_installed =
        (mac_path != NULL && access(mac_path, F_OK) == 0) || access("/Applications/Steam.app", F_OK) == 0;
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
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "mac_install_url");
    ms_json_writer_string(&writer, "https://store.steampowered.com/about/");
    ms_json_writer_key(&writer, "mac_running");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "running");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "metalsharp_wine_available");
    ms_json_writer_bool(&writer, wine != NULL && access(wine, F_OK) == 0);
    ms_json_writer_key(&writer, "installing");
    ms_json_writer_bool(&writer, installing);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(wine_prefix);
    free(wine_exe);
    free(wine);
    free(install_lock);
    free(mac_path);
    return result;
}
