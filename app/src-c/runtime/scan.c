#include "metalsharp_backend/scan.h"

#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/steam_basic.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char* path;
    unsigned depth;
    bool preferred;
} exe_candidate;

static char* join_path(const char* left, const char* right) {
    size_t a = strlen(left), b = strlen(right);
    bool slash = a > 0 && left[a - 1] != '/';
    char* path = (char*)malloc(a + b + (slash ? 2 : 1));
    if (path != NULL)
        (void)snprintf(path, a + b + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return path;
}

static bool valid_exe(const char* name) {
    char lower[256];
    size_t i, length = strlen(name);
    if (length >= sizeof(lower))
        length = sizeof(lower) - 1;
    for (i = 0; i < length; ++i)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[length] = '\0';
    if (strstr(lower, "setup") || strstr(lower, "redist") || strstr(lower, "dotnet") || strstr(lower, "installer") ||
        strstr(lower, "uninstall") || strstr(lower, "vcredist") || strstr(lower, "crashhandler") ||
        strstr(lower, "server"))
        return false;
    return true;
}

static bool preferred_exe(const char* name) {
    char lower[256];
    size_t i, length = strlen(name);
    if (length >= sizeof(lower))
        length = sizeof(lower) - 1;
    for (i = 0; i < length; ++i)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[length] = '\0';
    return strncmp(lower, "rain", 4) == 0 || strncmp(lower, "terraria", 8) == 0 || strncmp(lower, "hl2", 3) == 0 ||
           strcmp(lower, "game.exe") == 0;
}

static void find_exe(const char* root, unsigned depth, exe_candidate* best) {
    DIR* dir;
    struct dirent* entry;
    struct stat st;
    if (depth > 3 || lstat(root, &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    dir = opendir(root);
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        struct stat child_stat = {0};
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        path = join_path(root, entry->d_name);
        if (path == NULL)
            continue;
        if (lstat(path, &child_stat) == 0 && S_ISREG(child_stat.st_mode)) {
            size_t length = strlen(entry->d_name);
            if (length >= 4 && strcasecmp(entry->d_name + length - 4, ".exe") == 0 && valid_exe(entry->d_name)) {
                bool preferred = preferred_exe(entry->d_name);
                if (best->path == NULL || (preferred && !best->preferred)) {
                    free(best->path);
                    best->path = strdup(path);
                    best->preferred = preferred;
                }
            }
        } else if (child_stat.st_mode != 0 && S_ISDIR(child_stat.st_mode)) {
            find_exe(path, depth + 1, best);
        }
        free(path);
        if (best->preferred)
            break;
    }
    closedir(dir);
}

static unsigned long long dir_size(const char* root) {
    DIR* dir;
    struct dirent* entry;
    struct stat st;
    unsigned long long total = 0;
    if (lstat(root, &st) != 0)
        return 0;
    if (S_ISREG(st.st_mode))
        return st.st_size > 0 ? (unsigned long long)st.st_size : 0;
    if (!S_ISDIR(st.st_mode))
        return 0;
    dir = opendir(root);
    if (dir == NULL)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        path = join_path(root, entry->d_name);
        if (path != NULL) {
            total += dir_size(path);
            free(path);
        }
    }
    closedir(dir);
    return total;
}

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    long size;
    char* text;
    size_t length;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL)
            fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > 4 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = (char*)malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }
    length = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[length] = '\0';
    return text;
}

static char* acf_value(const char* text, const char* key) {
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
            const char* quote = strchr(line + strlen(prefix), '"');
            if (quote != NULL) {
                const char* last = strrchr(quote + 1, '"');
                if (last != NULL && last > quote)
                    return strndup(quote + 1, (size_t)(last - quote - 1));
            }
        }
        line = end == NULL ? NULL : end + 1;
    }
    return NULL;
}

static void write_game(ms_json_writer* writer, const char* id, const char* name, const char* exe, const char* platform,
                       bool has_appid, unsigned long long appid, unsigned long long size) {
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "id");
    ms_json_writer_string(writer, id);
    ms_json_writer_key(writer, "name");
    ms_json_writer_string(writer, name);
    ms_json_writer_key(writer, "exe_path");
    ms_json_writer_string(writer, exe == NULL ? "" : exe);
    ms_json_writer_key(writer, "platform");
    ms_json_writer_string(writer, platform);
    ms_json_writer_key(writer, "steam_app_id");
    if (has_appid)
        ms_json_writer_u64(writer, appid);
    else
        ms_json_writer_null(writer);
    ms_json_writer_key(writer, "size_bytes");
    ms_json_writer_u64(writer, size);
    ms_json_writer_key(writer, "metalsharp_compatible");
    ms_json_writer_bool(writer, true);
    ms_json_writer_object_end(writer);
}

static void scan_steam_library(ms_json_writer* writer, const char* steamapps) {
    DIR* dir = opendir(steamapps);
    struct dirent* entry;
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char *manifest, *text, *appid_text, *name, *install_dir, *game_dir;
        exe_candidate best = {0};
        unsigned long long appid;
        char* end;
        size_t length = strlen(entry->d_name);
        if (length < 14 || strncmp(entry->d_name, "appmanifest_", 12) != 0 ||
            strcmp(entry->d_name + length - 4, ".acf") != 0)
            continue;
        manifest = join_path(steamapps, entry->d_name);
        text = manifest == NULL ? NULL : read_file(manifest);
        free(manifest);
        if (text == NULL)
            continue;
        appid_text = acf_value(text, "appid");
        name = acf_value(text, "name");
        install_dir = acf_value(text, "installdir");
        if (appid_text == NULL || name == NULL || install_dir == NULL) {
            free(text);
            free(appid_text);
            free(name);
            free(install_dir);
            continue;
        }
        appid = strtoull(appid_text, &end, 10);
        game_dir = join_path(steamapps, "common");
        if (game_dir != NULL) {
            char* full = join_path(game_dir, install_dir);
            free(game_dir);
            game_dir = full;
        }
        if (game_dir != NULL)
            find_exe(game_dir, 0, &best);
        {
            char id[64];
            (void)snprintf(id, sizeof(id), "steam_%llu", appid);
            write_game(writer, id, name, best.path, "steam", true, appid, game_dir == NULL ? 0 : dir_size(game_dir));
        }
        free(best.path);
        free(game_dir);
        free(text);
        free(appid_text);
        free(name);
        free(install_dir);
    }
    closedir(dir);
}

static void scan_local_games(ms_json_writer* writer, const char* home) {
    char* games = join_path(home, "games");
    DIR* dir;
    struct dirent* entry;
    if (games == NULL)
        return;
    dir = opendir(games);
    if (dir == NULL) {
        free(games);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        exe_candidate best = {0};
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        path = join_path(games, entry->d_name);
        if (path == NULL)
            continue;
        find_exe(path, 0, &best);
        if (best.path != NULL) {
            char id[256];
            (void)snprintf(id, sizeof(id), "local_%s", entry->d_name);
            write_game(writer, id, entry->d_name, best.path, "local", false, 0, dir_size(path));
        }
        free(best.path);
        free(path);
    }
    closedir(dir);
    free(games);
}

char* ms_scan_all_json(const char* metalsharp_home) {
    const char* home = getenv("HOME");
    const char* candidates[3];
    ms_json_writer writer;
    char* status;
    char* result;
    size_t i;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "data");
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "games");
    ms_json_writer_array_begin(&writer);
    if (home != NULL) {
        char* p1 = join_path(home, "Library/Application Support/Steam/steamapps");
        char* p2 = join_path(home, ".steam/steam/steamapps");
        char* p3 = join_path(home, ".local/share/Steam/steamapps");
        candidates[0] = p1;
        candidates[1] = p2;
        candidates[2] = p3;
        for (i = 0; i < 3; ++i)
            if (candidates[i] != NULL) {
                scan_steam_library(&writer, candidates[i]);
                free((void*)candidates[i]);
            }
    }
    scan_local_games(&writer, metalsharp_home);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "steam");
    status = ms_steam_status_json(metalsharp_home);
    ms_json_writer_raw(&writer, status == NULL ? "{}" : status);
    free(status);
    ms_json_writer_object_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    return result;
}
