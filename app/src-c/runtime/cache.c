#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/cache.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/wine_runtime.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct cache_stats {
    unsigned long long bytes;
    unsigned long long files;
    unsigned long long directories;
    unsigned long long apps;
    unsigned long long newest;
};

static char* join_path(const char* left, const char* right) {
    size_t a = strlen(left), b = strlen(right);
    bool slash = a > 0 && left[a - 1] != '/';
    char* path = (char*)malloc(a + b + (slash ? 2 : 1));
    if (path != NULL)
        (void)snprintf(path, a + b + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return path;
}

static bool numeric_name(const char* name) {
    size_t i;
    if (name == NULL || name[0] == '\0')
        return false;
    for (i = 0; name[i] != '\0'; ++i)
        if (name[i] < '0' || name[i] > '9')
            return false;
    return true;
}

static void collect_stats(const char* path, unsigned depth, struct cache_stats* stats) {
    struct stat st;
    DIR* dir;
    struct dirent* entry;
    if (lstat(path, &st) != 0)
        return;
    if ((unsigned long long)st.st_mtime > stats->newest && depth > 0)
        stats->newest = (unsigned long long)st.st_mtime;
    if (S_ISREG(st.st_mode)) {
        stats->files++;
        if (st.st_size > 0)
            stats->bytes += (unsigned long long)st.st_size;
        return;
    }
    if (!S_ISDIR(st.st_mode))
        return;
    if (depth > 0) {
        stats->directories++;
        if (depth == 2 && numeric_name(strrchr(path, '/') == NULL ? path : strrchr(path, '/') + 1))
            stats->apps++;
    }
    dir = opendir(path);
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        child = join_path(path, entry->d_name);
        if (child != NULL) {
            collect_stats(child, depth + 1, stats);
            free(child);
        }
    }
    closedir(dir);
}

static bool remove_tree(const char* path) {
    struct stat st;
    DIR* dir;
    struct dirent* entry;
    bool ok = true;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (dir == NULL)
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        child = join_path(path, entry->d_name);
        if (child == NULL || !remove_tree(child))
            ok = false;
        free(child);
    }
    closedir(dir);
    if (rmdir(path) != 0)
        ok = false;
    return ok;
}

static char* cache_path(const char* home, const char* type) {
    return join_path(home, strcmp(type, "pipeline") == 0 ? "pipeline-cache" : "shader-cache");
}

static void write_time_or_null(ms_json_writer* writer, unsigned long long timestamp) {
    char date[64];
    time_t raw = (time_t)timestamp;
    struct tm local;
    ms_json_writer_key(writer, "last_modified");
    if (timestamp == 0 || localtime_r(&raw, &local) == NULL ||
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %Z", &local) == 0) {
        ms_json_writer_null(writer);
    } else {
        ms_json_writer_string(writer, date);
    }
}

static void write_summary(ms_json_writer* writer, const char* path) {
    struct stat st;
    struct cache_stats stats = {0};
    bool exists = stat(path, &st) == 0;
    const char* status;
    if (exists)
        collect_stats(path, 0, &stats);
    status = !exists ? "missing" : stats.files == 0 ? "empty" : "active";
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "bytes");
    ms_json_writer_u64(writer, stats.bytes);
    ms_json_writer_key(writer, "files");
    ms_json_writer_u64(writer, stats.files);
    ms_json_writer_key(writer, "directories");
    ms_json_writer_u64(writer, stats.directories);
    ms_json_writer_key(writer, "apps");
    ms_json_writer_u64(writer, stats.apps);
    ms_json_writer_key(writer, "path");
    ms_json_writer_string(writer, path);
    ms_json_writer_key(writer, "status");
    ms_json_writer_string(writer, status);
    write_time_or_null(writer, stats.newest);
    ms_json_writer_object_end(writer);
}

/* The FEX build's caches, per graphics lane. Each game's go in shader-cache/fex-<lane>/<appid> and
 * pipeline-cache/fex-<lane>/<appid> (steam_actions.c set_launch_cache_env). Mullion is "dxmt"
 * (and "dxmt_32"); Metal's own pipeline cache for its games is in the user's cache folder,
 * mullion/<exe>/com.apple.metal, beside Mullion's shader databases for games started without
 * MetalSharp's environment. */
static const char* const fex_cache_lanes[] = {"dxmt", "vkd3d", "dxvk"};

static bool fex_cache_lane(const char* lane) {
    for (size_t i = 0; lane && i < sizeof(fex_cache_lanes) / sizeof(fex_cache_lanes[0]); i++)
        if (!strcmp(lane, fex_cache_lanes[i]))
            return true;
    return false;
}

/* The folders MetalSharp owns for a lane, per game: up to four, written to roots[]; returns how many. */
static size_t fex_lane_roots(const char* home, const char* lane, char roots[6][PATH_MAX]) {
    static const char* const kinds[] = {"shader-cache", "pipeline-cache"};
    size_t count = 0;
    for (size_t k = 0; k < 2; k++) {
        snprintf(roots[count++], PATH_MAX, "%s/%s/fex-%s", home, kinds[k], lane);
        if (!strcmp(lane, "dxmt"))
            snprintf(roots[count++], PATH_MAX, "%s/%s/fex-dxmt_32", home, kinds[k]);
    }
    return count;
}

/* Mullion's folder in the user's cache folder: Metal's pipeline cache for every Mullion game (one
 * folder per executable, whoever started it), which MetalSharp does not own. */
static bool mullion_shared_cache(char* out, size_t out_size) {
    char user_cache[PATH_MAX];
    size_t length = confstr(_CS_DARWIN_USER_CACHE_DIR, user_cache, sizeof(user_cache));
    if (length == 0 || length > sizeof(user_cache) || !user_cache[0])
        return false;
    return snprintf(out, out_size, "%s%smullion", user_cache, user_cache[strlen(user_cache) - 1] == '/' ? "" : "/") <
           (int)out_size;
}

/* Games with a cache in a lane's shader or pipeline folders (numeric app ids, each counted once). */
static unsigned long long fex_lane_apps(const char* home, const char* lane) {
    char roots[6][PATH_MAX];
    size_t count = fex_lane_roots(home, lane, roots);
    char seen[256][24];
    size_t seen_count = 0;
    for (size_t r = 0; r < count; r++) {
        DIR* dir;
        struct dirent* entry;
        if (strstr(roots[r], "-cache/fex-") == NULL || (dir = opendir(roots[r])) == NULL)
            continue;
        while ((entry = readdir(dir)) != NULL) {
            bool known = false;
            if (!numeric_name(entry->d_name) || strlen(entry->d_name) >= sizeof(seen[0]))
                continue;
            for (size_t i = 0; i < seen_count && !known; i++)
                known = !strcmp(seen[i], entry->d_name);
            if (!known && seen_count < sizeof(seen) / sizeof(seen[0]))
                snprintf(seen[seen_count++], sizeof(seen[0]), "%s", entry->d_name);
        }
        closedir(dir);
    }
    return seen_count;
}

static void write_fex_lane_summary(ms_json_writer* writer, const char* home, const char* lane) {
    char roots[6][PATH_MAX], shared[PATH_MAX];
    size_t count = fex_lane_roots(home, lane, roots);
    struct cache_stats stats = {0}, shared_stats = {0};
    bool has_shared = !strcmp(lane, "dxmt") && mullion_shared_cache(shared, sizeof(shared));
    for (size_t r = 0; r < count; r++)
        collect_stats(roots[r], 1, &stats);
    if (has_shared) {
        collect_stats(shared, 1, &shared_stats);
        stats.bytes += shared_stats.bytes;
        stats.files += shared_stats.files;
        if (shared_stats.newest > stats.newest)
            stats.newest = shared_stats.newest;
    }
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "bytes");
    ms_json_writer_u64(writer, stats.bytes);
    ms_json_writer_key(writer, "files");
    ms_json_writer_u64(writer, stats.files);
    ms_json_writer_key(writer, "apps");
    ms_json_writer_u64(writer, fex_lane_apps(home, lane));
    ms_json_writer_key(writer, "path");
    ms_json_writer_string(writer, roots[0]);
    ms_json_writer_key(writer, "paths");
    ms_json_writer_array_begin(writer);
    for (size_t r = 0; r < count; r++)
        ms_json_writer_string(writer, roots[r]);
    ms_json_writer_array_end(writer);
    /* Of the total, Metal's cache that every Mullion game shares (cleared only when asked for). */
    if (has_shared) {
        ms_json_writer_key(writer, "shared_bytes");
        ms_json_writer_u64(writer, shared_stats.bytes);
        ms_json_writer_key(writer, "shared_path");
        ms_json_writer_string(writer, shared);
    }
    ms_json_writer_key(writer, "status");
    ms_json_writer_string(writer, stats.files == 0 ? "empty" : "active");
    write_time_or_null(writer, stats.newest);
    ms_json_writer_object_end(writer);
}

/* Clears MetalSharp's own folders of a lane; with `shared`, also Mullion's folder in the user's
 * cache, which every Mullion game uses (the app asks first). */
static char* fex_lane_clear_json(const char* home, const char* lane, bool shared) {
    char roots[6][PATH_MAX], shared_path[PATH_MAX];
    size_t count = fex_lane_roots(home, lane, roots);
    struct cache_stats stats = {0};
    ms_json_writer writer;
    for (size_t r = 0; r < count; r++) {
        collect_stats(roots[r], 1, &stats);
        (void)remove_tree(roots[r]);
        (void)mkdir(roots[r], 0755);
    }
    /* Mullion makes its user-cache folder itself. */
    if (shared && !strcmp(lane, "dxmt") && mullion_shared_cache(shared_path, sizeof(shared_path))) {
        collect_stats(shared_path, 1, &stats);
        (void)remove_tree(shared_path);
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "cache_type");
    ms_json_writer_string(&writer, "lane");
    ms_json_writer_key(&writer, "lane");
    ms_json_writer_string(&writer, lane);
    ms_json_writer_key(&writer, "files_removed");
    ms_json_writer_u64(&writer, stats.files);
    ms_json_writer_key(&writer, "bytes_freed");
    ms_json_writer_u64(&writer, stats.bytes);
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

char* ms_cache_size_json(const char* metalsharp_home) {
    char* shader = cache_path(metalsharp_home, "shader");
    char* pipeline = cache_path(metalsharp_home, "pipeline");
    ms_json_writer writer;
    char* result;
    if (shader == NULL || pipeline == NULL) {
        free(shader);
        free(pipeline);
        return NULL;
    }
    (void)mkdir(shader, 0755);
    (void)mkdir(pipeline, 0755);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "shader_cache");
    write_summary(&writer, shader);
    ms_json_writer_key(&writer, "pipeline_cache");
    write_summary(&writer, pipeline);
    /* The FEX build also reports each lane's caches. */
    if (ms_wine_is_fex()) {
        ms_json_writer_key(&writer, "lanes");
        ms_json_writer_object_begin(&writer);
        for (size_t i = 0; i < sizeof(fex_cache_lanes) / sizeof(fex_cache_lanes[0]); i++) {
            ms_json_writer_key(&writer, fex_cache_lanes[i]);
            write_fex_lane_summary(&writer, metalsharp_home, fex_cache_lanes[i]);
        }
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(shader);
    free(pipeline);
    return result;
}

char* ms_cache_clear_json(const char* metalsharp_home, const unsigned char* body, size_t body_length) {
    char* type = strdup("shader");
    char* path;
    ms_json* request = NULL;
    char error[128];
    struct cache_stats stats = {0};
    ms_json_writer writer;
    char* result;
    if (body != NULL && body_length > 0)
        request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
    if (request != NULL) {
        char* requested = NULL;
        if (ms_json_as_string(ms_json_object_get(request, "type"), &requested)) {
            free(type);
            type = requested;
        }
    }
    /* The FEX build clears one lane's caches: {"type":"lane","lane":"dxmt"|"vkd3d"|"dxvk"}, and with
     * "shared":true Mullion's shared Metal cache too. */
    if (ms_wine_is_fex() && type && !strcmp(type, "lane")) {
        char* lane = NULL;
        char* lane_result;
        bool shared = false;
        (void)ms_json_as_string(ms_json_object_get(request, "lane"), &lane);
        (void)ms_json_as_bool(ms_json_object_get(request, "shared"), &shared);
        lane_result = fex_cache_lane(lane) ? fex_lane_clear_json(metalsharp_home, lane, shared)
                                           : strdup("{\"ok\":false,\"error\":\"unknown cache lane\"}");
        free(lane);
        ms_json_free(request);
        free(type);
        return lane_result;
    }
    if (strcmp(type, "pipeline") != 0) {
        free(type);
        type = strdup("shader");
    }
    path = cache_path(metalsharp_home, type);
    if (path == NULL) {
        ms_json_free(request);
        free(type);
        return NULL;
    }
    collect_stats(path, 0, &stats);
    (void)remove_tree(path);
    (void)mkdir(path, 0755);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "cache_type");
    ms_json_writer_string(&writer, type);
    ms_json_writer_key(&writer, "files_removed");
    ms_json_writer_u64(&writer, stats.files);
    ms_json_writer_key(&writer, "bytes_freed");
    ms_json_writer_u64(&writer, stats.bytes);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    ms_json_free(request);
    free(type);
    free(path);
    return result;
}
