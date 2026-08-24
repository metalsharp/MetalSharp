#include "metalsharp_backend/gamejolt.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/steam_actions.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char* join_path(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* out = malloc(x + y + (slash ? 1 : 0) + 1);
    if (out)
        snprintf(out, x + y + (slash ? 1 : 0) + 1, "%s%s%s", a, slash ? "/" : "", b);
    return out;
}

static bool mkdir_p(const char* path) {
    char* copy = strdup(path);
    if (!copy)
        return false;
    for (char* p = copy + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(copy, 0755);
            *p = '/';
        }
    }
    bool ok = mkdir(copy, 0755) == 0 || errno == EEXIST;
    free(copy);
    return ok;
}

static char* read_text(const char* path) {
    FILE* file = fopen(path, "rb");
    long size;
    char* text;
    size_t got;
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file)
            fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = '\0';
    return text;
}

static char* field(const ms_json* object, const char* key, const char* fallback) {
    char* value = NULL;
    if (object)
        (void)ms_json_as_string(ms_json_object_get(object, key), &value);
    return value ? value : strdup(fallback ? fallback : "");
}

static char* storage_config_path(const char* home) {
    char* dir = join_path(home, "gamejolt");
    char* path = dir ? join_path(dir, "storage.json") : NULL;
    free(dir);
    return path;
}

static char* names_config_path(const char* home) {
    char* dir = join_path(home, "gamejolt");
    char* path = dir ? join_path(dir, "names.json") : NULL;
    free(dir);
    return path;
}

static char* engines_config_path(const char* home) {
    char* dir = join_path(home, "gamejolt");
    char* path = dir ? join_path(dir, "engines.json") : NULL;
    free(dir);
    return path;
}

static char* custom_game_name(const char* home, const char* id, const char* fallback) {
    char* config_path = names_config_path(home);
    char* raw = config_path ? read_text(config_path) : NULL;
    char error[96];
    ms_json* config = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    char* name = field(config, id, fallback);
    free(config_path);
    free(raw);
    ms_json_free(config);
    return name;
}

static char* custom_game_engine(const char* home, const char* id, const char* fallback) {
    char* config_path = engines_config_path(home);
    char* raw = config_path ? read_text(config_path) : NULL;
    char error[96];
    ms_json* config = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    char* engine = field(config, id, fallback);
    free(config_path);
    free(raw);
    ms_json_free(config);
    return engine;
}

static char* configured_root(const char* home) {
    char* config_path = storage_config_path(home);
    char* raw = config_path ? read_text(config_path) : NULL;
    char error[96];
    ms_json* config = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    char* root = field(config, "rootPath", "");
    free(config_path);
    free(raw);
    ms_json_free(config);
    if (!root || !root[0]) {
        free(root);
        return strdup(home);
    }
    return root;
}

static char* gamejolt_dir(const char* home) {
    char* root = configured_root(home);
    char* result;
    size_t length;
    if (!root)
        return NULL;
    length = strlen(root);
    while (length > 1 && root[length - 1] == '/')
        root[--length] = '\0';
    if (length >= 8 && strcmp(root + length - 8, "/GameJolt") == 0)
        result = strdup(root);
    else
        result = join_path(root, "GameJolt");
    free(root);
    return result;
}

static char* error_json(const char* message) {
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "error");
    ms_json_writer_string(&writer, message);
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

static bool has_suffix(const char* value, const char* suffix) {
    size_t value_len = strlen(value), suffix_len = strlen(suffix);
    return value_len >= suffix_len && strcasecmp(value + value_len - suffix_len, suffix) == 0;
}

static unsigned long long path_hash(const char* value) {
    unsigned long long hash = 1469598103934665603ULL;
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool is_regular_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool path_within(const char* directory, const char* path) {
    size_t length = directory ? strlen(directory) : 0;
    return directory && path && length > 0 && strncmp(path, directory, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static char* archive_stem(const char* filename) {
    char* stem = strdup(filename);
    if (!stem)
        return NULL;
    char* slash = strrchr(stem, '/');
    char* name = slash ? slash + 1 : stem;
    if (has_suffix(name, ".tar.gz"))
        name[strlen(name) - strlen(".tar.gz")] = '\0';
    else if (has_suffix(name, ".tar.bz2"))
        name[strlen(name) - strlen(".tar.bz2")] = '\0';
    else if (has_suffix(name, ".tar.xz"))
        name[strlen(name) - strlen(".tar.xz")] = '\0';
    else {
        char* dot = strrchr(name, '.');
        if (dot)
            *dot = '\0';
    }
    return stem;
}

static bool extract_archive(const char* archive, const char* destination) {
    pid_t pid = fork();
    int status = 0;
    if (pid < 0)
        return false;
    if (pid == 0) {
        const char* name = strrchr(archive, '/');
        name = name ? name + 1 : archive;
        if (has_suffix(name, ".zip"))
            execl("/usr/bin/ditto", "ditto", "-x", "-k", archive, destination, (char*)NULL);
        else if (has_suffix(name, ".tar.gz") || has_suffix(name, ".tgz"))
            execl("/usr/bin/tar", "tar", "-xzf", archive, "-C", destination, (char*)NULL);
        else if (has_suffix(name, ".tar.bz2"))
            execl("/usr/bin/tar", "tar", "-xjf", archive, "-C", destination, (char*)NULL);
        else if (has_suffix(name, ".rar"))
            execl("/usr/bin/bsdtar", "bsdtar", "-xf", archive, "-C", destination, (char*)NULL);
        else
            execl("/usr/bin/tar", "tar", "-xf", archive, "-C", destination, (char*)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void extract_archives(const char* directory) {
    DIR* dir = opendir(directory);
    struct dirent* entry;
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char* archive;
        char* stem;
        char* destination;
        struct stat st;
        if (!has_suffix(entry->d_name, ".zip") && !has_suffix(entry->d_name, ".rar") &&
            !has_suffix(entry->d_name, ".tar.gz") && !has_suffix(entry->d_name, ".tgz") &&
            !has_suffix(entry->d_name, ".tar.bz2") && !has_suffix(entry->d_name, ".tar.xz"))
            continue;
        archive = join_path(directory, entry->d_name);
        if (!archive || stat(archive, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(archive);
            continue;
        }
        stem = archive_stem(entry->d_name);
        destination = stem ? join_path(directory, stem) : NULL;
        if (destination && mkdir_p(destination) && extract_archive(archive, destination))
            (void)unlink(archive);
        free(archive);
        free(stem);
        free(destination);
    }
    closedir(dir);
}

static char* native_app_cover(const char* game_directory, const char* app_path) {
    char* resources = join_path(app_path, "Contents/Resources");
    char* cache = join_path(game_directory, ".metalsharp-cover.png");
    DIR* dir;
    struct dirent* entry;
    if (!resources || !cache) {
        free(resources);
        free(cache);
        return NULL;
    }
    if (is_regular_file(cache)) {
        free(resources);
        return cache;
    }
    dir = opendir(resources);
    if (!dir) {
        free(resources);
        free(cache);
        return NULL;
    }
    while ((entry = readdir(dir)) != NULL) {
        char* icon;
        pid_t pid;
        int status = 0;
        if (!has_suffix(entry->d_name, ".icns"))
            continue;
        icon = join_path(resources, entry->d_name);
        if (!icon)
            continue;
        pid = fork();
        if (pid == 0) {
            execl("/usr/bin/sips", "sips", "-s", "format", "png", icon, "--out", cache, (char*)NULL);
            _exit(127);
        }
        if (pid > 0) {
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
        }
        free(icon);
        if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && is_regular_file(cache)) {
            closedir(dir);
            free(resources);
            return cache;
        }
    }
    closedir(dir);
    free(resources);
    free(cache);
    return NULL;
}

static const char* find_tool(const char* name) {
    static const char* const prefixes[] = {"/opt/homebrew/bin/", "/usr/local/bin/", "/usr/bin/"};
    static char paths[2][PATH_MAX];
    size_t slot = strcmp(name, "icotool") == 0 ? 1 : 0;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        snprintf(paths[slot], sizeof(paths[slot]), "%s%s", prefixes[i], name);
        if (access(paths[slot], X_OK) == 0)
            return paths[slot];
    }
    return NULL;
}

static bool wait_success(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static char* windows_exe_cover(const char* game_directory, const char* executable) {
    const char* wrestool = find_tool("wrestool");
    const char* icotool = find_tool("icotool");
    char* cache = join_path(game_directory, ".metalsharp-cover.png");
    char* output = join_path(game_directory, ".metalsharp-icon");
    char* resource = output ? join_path(output, "resource.ico") : NULL;
    DIR* dir;
    struct dirent* entry;
    if (!wrestool || !icotool || !cache || !output || !resource || !mkdir_p(output)) {
        free(cache);
        free(output);
        free(resource);
        return NULL;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(resource, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        char* const args[] = {(char*)wrestool, "-x", "--type=14", (char*)executable, NULL};
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0)
            _exit(127);
        close(fd);
        execv(wrestool, args);
        _exit(127);
    }
    if (!wait_success(pid))
        goto fail;
    pid = fork();
    if (pid == 0) {
        char* const args[] = {(char*)icotool, "-x", "-o", output, resource, NULL};
        execv(icotool, args);
        _exit(127);
    }
    if (!wait_success(pid))
        goto fail;
    dir = opendir(output);
    if (!dir)
        goto fail;
    while ((entry = readdir(dir)) != NULL) {
        char* image;
        if (!has_suffix(entry->d_name, ".png"))
            continue;
        image = join_path(output, entry->d_name);
        if (image && rename(image, cache) == 0) {
            free(image);
            closedir(dir);
            unlink(resource);
            rmdir(output);
            free(output);
            free(resource);
            return cache;
        }
        free(image);
    }
    closedir(dir);
fail:
    unlink(resource);
    free(cache);
    free(output);
    free(resource);
    return NULL;
}

static void prepare_native_app(const char* app_path) {
    char* macos = join_path(app_path, "Contents/MacOS");
    DIR* dir = macos ? opendir(macos) : NULL;
    struct dirent* entry;
    if (!dir) {
        free(macos);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = join_path(macos, entry->d_name);
        if (path && stat(path, &st) == 0 && S_ISREG(st.st_mode))
            (void)chmod(path, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        free(path);
    }
    closedir(dir);
    free(macos);
}


static bool contains_case_insensitive(const char* value, const char* needle) {
    size_t needle_length = strlen(needle);
    for (const char* p = value; p && *p; ++p)
        if (!strncasecmp(p, needle, needle_length))
            return true;
    return false;
}

static bool preferred_game_executable(const char* name) {
    return !contains_case_insensitive(name, "unitycrashhandler") && !contains_case_insensitive(name, "crashreport") &&
           !contains_case_insensitive(name, "crashreportclient") && !contains_case_insensitive(name, "unrealcefsubprocess") &&
           !contains_case_insensitive(name, "uninstall") && !contains_case_insensitive(name, "ue4prereq") &&
           !contains_case_insensitive(name, "setup");
}

static void find_game_assets(const char* directory, unsigned depth, char** executable, bool* native, char** cover) {
    DIR* dir;
    struct dirent* entry;
    if (depth > 6 || !directory || !executable || !native || !cover)
        return;
    dir = opendir(directory);
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = join_path(directory, entry->d_name);
        if (!path)
            continue;
        if (has_suffix(entry->d_name, ".app")) {
            if (!*executable) {
                *executable = path;
                *native = true;
                path = NULL;
            }
            free(path);
            continue;
        }
        if (is_regular_file(path)) {
            if (has_suffix(entry->d_name, ".png") || has_suffix(entry->d_name, ".jpg") ||
                has_suffix(entry->d_name, ".jpeg") || has_suffix(entry->d_name, ".webp")) {
                bool preferred = strncasecmp(entry->d_name, "cover", 5) == 0 ||
                                 strncasecmp(entry->d_name, "icon", 4) == 0 ||
                                 strncasecmp(entry->d_name, "art", 3) == 0 ||
                                 strncasecmp(entry->d_name, "banner", 6) == 0;
                if (!*cover || preferred) {
                    free(*cover);
                    *cover = strdup(path);
                }
            }
            if (has_suffix(entry->d_name, ".exe") && preferred_game_executable(entry->d_name)) {
                const char* current_name = *executable ? strrchr(*executable, '/') : NULL;
                current_name = current_name ? current_name + 1 : *executable;
                if (!*executable || (!preferred_game_executable(current_name) && preferred_game_executable(entry->d_name))) {
                    free(*executable);
                    *executable = path;
                    *native = false;
                    path = NULL;
                }
            }
            free(path);
        } else if (depth < 6) {
            find_game_assets(path, depth + 1, executable, native, cover);
            free(path);
        } else {
            free(path);
        }
    }
    closedir(dir);
}

static void write_pipeline_options(ms_json_writer* writer) {
    static const char* const options[][2] = {{"auto", "Auto"},       {"m9", "M9"},
                                             {"m10", "M10"},         {"m10_32", "M10 (32-bit)"},
                                             {"m11", "M11"},         {"m11_32", "M11 (32-bit)"},
                                             {"vkd3d", "VKD3D"},    {"d3dmetal", "D3DMetal"}};
    ms_json_writer_array_begin(writer);
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        ms_json_writer_object_begin(writer);
        ms_json_writer_key(writer, "id");
        ms_json_writer_string(writer, options[i][0]);
        ms_json_writer_key(writer, "name");
        ms_json_writer_string(writer, options[i][1]);
        ms_json_writer_key(writer, "recommended");
        ms_json_writer_bool(writer, i == 0);
        ms_json_writer_object_end(writer);
    }
    ms_json_writer_array_end(writer);
}

static void write_game(ms_json_writer* writer, const char* home, const char* directory, const char* executable,
                       bool native, const char* cover) {
    char id[64];
    const char* slash = strrchr(directory, '/');
    const char* name = slash ? slash + 1 : directory;
    snprintf(id, sizeof(id), "gamejolt_%llx", path_hash(directory));
    char* display_name = custom_game_name(home, id, name);
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "id");
    ms_json_writer_string(writer, id);
    ms_json_writer_key(writer, "name");
    ms_json_writer_string(writer, display_name ? display_name : name);
    ms_json_writer_key(writer, "install_dir");
    ms_json_writer_string(writer, directory);
    ms_json_writer_key(writer, "exe_path");
    ms_json_writer_string(writer, executable);
    ms_json_writer_key(writer, "installed");
    ms_json_writer_bool(writer, true);
    ms_json_writer_key(writer, "native");
    ms_json_writer_bool(writer, native);
    ms_json_writer_key(writer, "engine");
    {
        char* engine = custom_game_engine(home, id, native ? "native" : "auto");
        ms_json_writer_string(writer, native ? "native" : engine);
        free(engine);
    }
    ms_json_writer_key(writer, "cover_path");
    if (cover)
        ms_json_writer_string(writer, cover);
    else
        ms_json_writer_null(writer);
    ms_json_writer_key(writer, "bottle_id");
    ms_json_writer_string(writer, native ? "" : id);
    ms_json_writer_key(writer, "available_pipelines");
    if (native)
        ms_json_writer_array_begin(writer), ms_json_writer_array_end(writer);
    else
        write_pipeline_options(writer);
    ms_json_writer_object_end(writer);
    free(display_name);
}

char* ms_gamejolt_storage_json(const char* home) {
    char* root = configured_root(home);
    char* directory = gamejolt_dir(home);
    ms_json_writer writer;
    if (!root || !directory) {
        free(root);
        free(directory);
        return error_json("failed to resolve GameJolt storage");
    }
    (void)mkdir_p(directory);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "mode");
    ms_json_writer_string(&writer, strcmp(root, home) == 0 ? "internal" : "external");
    ms_json_writer_key(&writer, "rootPath");
    ms_json_writer_string(&writer, root);
    ms_json_writer_key(&writer, "gamejoltDir");
    ms_json_writer_string(&writer, directory);
    ms_json_writer_object_end(&writer);
    free(root);
    free(directory);
    return ms_json_writer_take(&writer);
}

char* ms_gamejolt_set_storage_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2,
                                      error, sizeof(error));
    char* root = field(request, "rootPath", "");
    char* config_dir = join_path(home, "gamejolt");
    char* config_path = config_dir ? join_path(config_dir, "storage.json") : NULL;
    ms_json_writer writer;
    char* raw;
    FILE* file;
    bool ok;
    if (!request || !root || !root[0] || !config_dir || !config_path || !mkdir_p(config_dir)) {
        ms_json_free(request);
        free(root);
        free(config_dir);
        free(config_path);
        return error_json("a valid GameJolt storage folder is required");
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "rootPath");
    ms_json_writer_string(&writer, root);
    ms_json_writer_object_end(&writer);
    raw = ms_json_writer_take(&writer);
    file = fopen(config_path, "wb");
    ok = file && raw && fputs(raw, file) >= 0;
    if (file)
        fclose(file);
    free(raw);
    free(root);
    free(config_dir);
    free(config_path);
    ms_json_free(request);
    if (!ok)
        return error_json("failed to save GameJolt storage location");
    return ms_gamejolt_storage_json(home);
}

char* ms_gamejolt_set_name_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2,
                                     error, sizeof(error));
    char* id = field(request, "id", "");
    char* name = field(request, "name", "");
    char* config_path = names_config_path(home);
    char* config_dir = join_path(home, "gamejolt");
    char* raw = config_path ? read_text(config_path) : NULL;
    ms_json* previous = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    ms_json_writer writer;
    char* serialized;
    FILE* file;
    bool found = false;
    if (!request || !id[0] || !name[0] || strlen(id) > 128 || strlen(name) > 160 || !config_path || !config_dir ||
        !mkdir_p(config_dir)) {
        ms_json_free(request);
        ms_json_free(previous);
        free(id);
        free(name);
        free(config_path);
        free(config_dir);
        free(raw);
        return error_json("a non-empty GameJolt name is required");
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    if (previous && ms_json_type_of(previous) == MS_JSON_OBJECT) {
        for (size_t i = 0; i < ms_json_object_length(previous); ++i) {
            const char* key = ms_json_object_key_at(previous, i);
            char* value = NULL;
            ms_json_writer_key(&writer, key);
            if (key && !strcmp(key, id)) {
                ms_json_writer_string(&writer, name);
                found = true;
            } else if (ms_json_as_string(ms_json_object_value_at(previous, i), &value)) {
                ms_json_writer_string(&writer, value);
                free(value);
            } else {
                char* value_json = ms_json_stringify(ms_json_object_value_at(previous, i));
                ms_json_writer_raw(&writer, value_json ? value_json : "null");
                free(value_json);
            }
        }
    }
    if (!found) {
        ms_json_writer_key(&writer, id);
        ms_json_writer_string(&writer, name);
    }
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    file = fopen(config_path, "wb");
    if (!file || !serialized || fputs(serialized, file) < 0) {
        if (file)
            fclose(file);
        free(serialized);
        ms_json_free(request);
        ms_json_free(previous);
        free(id);
        free(name);
        free(config_path);
        free(config_dir);
        free(raw);
        return error_json("failed to save GameJolt name");
    }
    fclose(file);
    free(serialized);
    ms_json_free(request);
    ms_json_free(previous);
    free(config_path);
    free(config_dir);
    free(raw);
    {
        ms_json_writer result;
        ms_json_writer_init(&result);
        ms_json_writer_object_begin(&result);
        ms_json_writer_key(&result, "ok");
        ms_json_writer_bool(&result, true);
        ms_json_writer_key(&result, "id");
        ms_json_writer_string(&result, id);
        ms_json_writer_key(&result, "name");
        ms_json_writer_string(&result, name);
        ms_json_writer_object_end(&result);
        free(id);
        free(name);
        return ms_json_writer_take(&result);
    }
}

char* ms_gamejolt_set_engine_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2,
                                     error, sizeof(error));
    char* id = field(request, "id", "");
    char* engine = field(request, "engine", "");
    char* path = engines_config_path(home);
    char* dir = join_path(home, "gamejolt");
    char* raw = path ? read_text(path) : NULL;
    ms_json* previous = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    ms_json_writer writer;
    char* serialized;
    FILE* file;
    bool valid = !strcmp(engine, "auto") || !strcmp(engine, "m9") || !strcmp(engine, "m10") ||
                 !strcmp(engine, "m10_32") || !strcmp(engine, "m11") || !strcmp(engine, "m11_32") ||
                 !strcmp(engine, "vkd3d") || !strcmp(engine, "d3dmetal");
    bool found = false;
    if (!request || !id[0] || !engine[0] || !valid || !path || !dir || !mkdir_p(dir)) {
        ms_json_free(request); ms_json_free(previous); free(id); free(engine); free(path); free(dir); free(raw);
        return error_json("invalid GameJolt engine");
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    if (previous && ms_json_type_of(previous) == MS_JSON_OBJECT) {
        for (size_t i = 0; i < ms_json_object_length(previous); ++i) {
            const char* key = ms_json_object_key_at(previous, i);
            char* value = NULL;
            ms_json_writer_key(&writer, key);
            if (key && !strcmp(key, id)) {
                ms_json_writer_string(&writer, engine);
                found = true;
            } else if (ms_json_as_string(ms_json_object_value_at(previous, i), &value)) {
                ms_json_writer_string(&writer, value);
                free(value);
            }
        }
    }
    if (!found) {
        ms_json_writer_key(&writer, id);
        ms_json_writer_string(&writer, engine);
    }
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    file = fopen(path, "wb");
    if (!file || !serialized || fputs(serialized, file) < 0) {
        if (file) fclose(file);
        free(serialized); ms_json_free(request); ms_json_free(previous); free(id); free(engine); free(path); free(dir); free(raw);
        return error_json("failed to save GameJolt engine");
    }
    fclose(file); free(serialized); ms_json_free(request); ms_json_free(previous); free(path); free(dir); free(raw);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok"); ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "id"); ms_json_writer_string(&writer, id);
    ms_json_writer_key(&writer, "engine"); ms_json_writer_string(&writer, engine);
    ms_json_writer_object_end(&writer);
    free(id); free(engine);
    return ms_json_writer_take(&writer);
}

char* ms_gamejolt_uninstall_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2,
                                     error, sizeof(error));
    char* target = field(request, "installDir", "");
    char* directory = gamejolt_dir(home);
    char* root = configured_root(home);
    char* resolved_target = target[0] ? realpath(target, NULL) : NULL;
    char* resolved_directory = directory ? realpath(directory, NULL) : NULL;
    bool legacy_root = false;
    bool valid = false;
    if (root) {
        const char* root_name = strrchr(root, '/');
        legacy_root = root_name && !strcasecmp(root_name + 1, "GameJolt");
    }
    if (resolved_target && resolved_directory && strcmp(resolved_target, resolved_directory) != 0)
        valid = path_within(resolved_directory, resolved_target);
    if (!valid && legacy_root) {
        char* resolved_root = realpath(root, NULL);
        if (resolved_root && resolved_target && strcmp(resolved_target, resolved_root) != 0)
            valid = path_within(resolved_root, resolved_target);
        free(resolved_root);
    }
    if (!request || !target[0] || !valid) {
        ms_json_free(request); free(target); free(directory); free(root); free(resolved_target); free(resolved_directory);
        return error_json("invalid GameJolt uninstall path");
    }
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", "--", resolved_target, (char*)NULL);
        _exit(127);
    }
    bool removed = wait_success(pid);
    ms_json_free(request); free(target); free(directory); free(root); free(resolved_target); free(resolved_directory);
    if (!removed)
        return error_json("failed to uninstall GameJolt game");
    {
        ms_json_writer writer;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok"); ms_json_writer_bool(&writer, true);
        ms_json_writer_object_end(&writer);
        return ms_json_writer_take(&writer);
    }
}

static void write_games_from_directory(ms_json_writer* writer, const char* home, const char* directory,
                                       const char* skip_directory) {
    DIR* dir = opendir(directory);
    struct dirent* entry;
    if (!dir)
        return;
    extract_archives(directory);
    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL) {
        char* game_dir;
        char* executable = NULL;
        char* cover = NULL;
        bool native = false;
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            (skip_directory && !strcmp(entry->d_name, skip_directory)))
            continue;
        game_dir = join_path(directory, entry->d_name);
        if (!game_dir || stat(game_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            free(game_dir);
            continue;
        }
        if (has_suffix(entry->d_name, ".rar") || has_suffix(entry->d_name, ".zip") ||
            has_suffix(entry->d_name, ".7z")) {
            char* stem = archive_stem(entry->d_name);
            char* normalized = stem ? join_path(directory, stem) : NULL;
            if (normalized && access(normalized, F_OK) != 0 && rename(game_dir, normalized) == 0) {
                free(game_dir);
                game_dir = normalized;
            } else {
                free(normalized);
            }
            free(stem);
        }
        extract_archives(game_dir);
        native = has_suffix(entry->d_name, ".app");
        executable = native ? strdup(game_dir) : NULL;
        if (!executable)
            find_game_assets(game_dir, 0, &executable, &native, &cover);
        if (native && executable && !cover)
            cover = native_app_cover(game_dir, executable);
        if (!native && executable) {
            char* embedded_cover = windows_exe_cover(game_dir, executable);
            if (embedded_cover) {
                free(cover);
                cover = embedded_cover;
            }
        }
        if (executable)
            write_game(writer, home, game_dir, executable, native, cover);
        free(game_dir);
        free(executable);
        free(cover);
    }
    closedir(dir);
}

char* ms_gamejolt_json(const char* home) {
    char* root = configured_root(home);
    char* directory = gamejolt_dir(home);
    ms_json_writer writer;
    if (!directory || !mkdir_p(directory)) {
        free(directory);
        return error_json("failed to create GameJolt folder");
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "storage");
    {
        char* storage = ms_gamejolt_storage_json(home);
        ms_json_writer_raw(&writer, storage ? storage : "{}");
        free(storage);
    }
    ms_json_writer_key(&writer, "games");
    ms_json_writer_array_begin(&writer);
    write_games_from_directory(&writer, home, directory, NULL);
    if (root && strcmp(root, directory) != 0) {
        const char* root_name = strrchr(root, '/');
        if (root_name && !strcasecmp(root_name + 1, "GameJolt"))
            write_games_from_directory(&writer, home, root, "GameJolt");
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    free(root);
    free(directory);
    return ms_json_writer_take(&writer);
}

char* ms_gamejolt_cover_path(const char* home, const char* id) {
    char* games = ms_gamejolt_json(home);
    char error[96];
    ms_json* root = games ? ms_json_parse(games, strlen(games), error, sizeof(error)) : NULL;
    const ms_json* array = root ? ms_json_object_get(root, "games") : NULL;
    char* result = NULL;
    if (array && id) {
        for (size_t i = 0; i < ms_json_array_length(array); ++i) {
            char* item_id = field(ms_json_array_get(array, i), "id", "");
            if (!strcmp(item_id, id)) {
                char* cover = field(ms_json_array_get(array, i), "cover_path", "");
                if (cover[0] && is_regular_file(cover))
                    result = cover;
                else
                    free(cover);
                free(item_id);
                break;
            }
            free(item_id);
        }
    }
    ms_json_free(root);
    free(games);
    return result;
}

char* ms_gamejolt_launch_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2,
                                      error, sizeof(error));
    char* id = field(request, "id", "");
    char* executable = field(request, "exePath", "");
    char* engine = field(request, "engine", "auto");
    char* directory = gamejolt_dir(home);
    char* root = configured_root(home);
    bool legacy_root = false;
    bool valid_path;
    pid_t pid;
    ms_json_writer writer;
    if (root) {
        const char* root_name = strrchr(root, '/');
        legacy_root = root_name && !strcasecmp(root_name + 1, "GameJolt");
    }
    valid_path = path_within(directory, executable) || (legacy_root && path_within(root, executable));
    if (!request || !id[0] || !executable[0] || !directory || !valid_path) {
        ms_json_free(request);
        free(id);
        free(executable);
        free(engine);
        free(directory);
        free(root);
        return error_json("invalid GameJolt game path");
    }
    if (access(executable, F_OK) != 0) {
        ms_json_free(request);
        free(id);
        free(executable);
        free(engine);
        free(directory);
        free(root);
        return error_json("GameJolt executable not found");
    }
    bool native = has_suffix(executable, ".app");
    if (native)
        prepare_native_app(executable);
    if (!native) {
        unsigned synthetic_id = (unsigned)path_hash(executable);
        ms_json_writer request_writer;
        char* launch_body;
        char* result;
        ms_json_writer_init(&request_writer);
        ms_json_writer_object_begin(&request_writer);
        ms_json_writer_key(&request_writer, "appid");
        ms_json_writer_u64(&request_writer, synthetic_id ? synthetic_id : 1);
        ms_json_writer_key(&request_writer, "exePath");
        ms_json_writer_string(&request_writer, executable);
        ms_json_writer_key(&request_writer, "pipeline");
        ms_json_writer_string(&request_writer, !strcmp(engine, "auto") ? "vkd3d" : engine);
        ms_json_writer_object_end(&request_writer);
        launch_body = ms_json_writer_take(&request_writer);
        result = launch_body
                     ? ms_steam_launch_external_json(home, launch_body, strlen(launch_body), NULL)
                     : error_json("failed to prepare GameJolt launch");
        free(launch_body);
        ms_json_free(request);
        free(id);
        free(executable);
        free(engine);
        free(directory);
        free(root);
        return result;
    }
    pid = fork();
    if (pid == 0) {
        char* slash = strrchr(executable, '/');
        if (native) {
            execl("/usr/bin/open", "open", "-W", executable, (char*)NULL);
        } else {
            char* wine = join_path(home, "runtime/wine/bin/metalsharp-wine");
            if (slash)
                *slash = '\0';
            char* prefix = join_path(home, "sharp-prefix");
            if (prefix) {
                setenv("WINEPREFIX", prefix, 1);
                free(prefix);
            }
            setenv("MS_GRAPHICS_BACKEND", engine, 1);
            setenv("MS_GAMEJOLT", "1", 1);
            (void)chdir(slash ? executable : directory);
            if (wine) {
                execl(wine, wine, slash ? slash + 1 : executable, (char*)NULL);
                free(wine);
            }
        }
        _exit(127);
    }
    if (pid < 0) {
        free(id);
        free(executable);
        free(engine);
        free(directory);
        free(root);
        ms_json_free(request);
        return error_json("failed to launch GameJolt game");
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "pid");
    ms_json_writer_u64(&writer, (unsigned long long)pid);
    ms_json_writer_key(&writer, "pipeline");
    ms_json_writer_string(&writer, native ? "native" : engine);
    ms_json_writer_object_end(&writer);
    free(id);
    free(executable);
    free(engine);
    free(directory);
    free(root);
    ms_json_free(request);
    return ms_json_writer_take(&writer);
}

char* ms_gamejolt_pid_status_json(const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2,
                                     error, sizeof(error));
    long long pid_value = 0;
    bool running = false;
    ms_json_writer writer;
    if (!request || !ms_json_as_i64(ms_json_object_get(request, "pid"), &pid_value) || pid_value <= 0 ||
        pid_value > INT_MAX) {
        ms_json_free(request);
        return error_json("a valid process id is required");
    }
    {
        pid_t child_state = waitpid((pid_t)pid_value, NULL, WNOHANG);
        if (child_state == (pid_t)pid_value)
            running = false;
        else if (child_state < 0)
            running = kill((pid_t)pid_value, 0) == 0 || errno == EPERM;
        else
            running = true;
    }
    ms_json_free(request);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "running");
    ms_json_writer_bool(&writer, running);
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}
