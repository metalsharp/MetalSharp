#include "metalsharp_backend/cache.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <dirent.h>
#include <errno.h>
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
