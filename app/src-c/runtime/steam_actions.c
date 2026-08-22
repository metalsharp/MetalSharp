#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char* join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (p)
        snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
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
            if (length > 4 && !strcasecmp(path + length - 4, ".exe")) {
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
        }
        free(path);
    }
    closedir(dir);
    return NULL;
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
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX)
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        if (strstr(end, prefix) && (strstr(end, "Steam.exe") || strstr(end, "steam.exe")))
            (void)kill((pid_t)raw_pid, signal_number);
    }
    pclose(pipe);
}

static char* spawn_wine(const char* home, const char* first, const char* second, const char* third, pid_t* pid) {
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
        char* s = strdup(strerror(errno));
        free(wine);
        free(prefix);
        return s;
    }
    if (child == 0) {
        char library_env[4096];
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "none", 1);
        set_pipeline_runtime_env(home, getenv("METALSHARP_PIPELINE"));
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        chdir(home);
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
        return err("MetalSharp Wine not found");
    }
    if (!steam || !ui || access(steam, F_OK) != 0 || access(ui, F_OK) != 0) {
        free(steam);
        free(ui);
        return err("Steam is not installed — use the setup wizard to install it first");
    }
    if (wine_steam_running(home)) {
        free(steam);
        free(ui);
        if (status)
            *status = 200;
        return strdup("{\"ok\":true,\"message\":\"Steam already running\"}");
    }
    errtext = spawn_wine(home, steam, "-no-cef-sandbox", NULL, &pid);
    free(steam);
    free(ui);
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
    if (wine_steam_running(home))
        signal_wine_steam_processes(home, SIGKILL);
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

static void steam_install_worker(const char* home, const char* lock_path, const char* installer) {
    FILE* owner = fopen(lock_path, "wb");
    pid_t pid;
    if (owner) {
        fprintf(owner, "%ld\\n", (long)getpid());
        fclose(owner);
    }
    int wait_status;
    char* wine_error;
    pid = fork();
    if (pid < 0)
        goto done;
    if (pid == 0) {
        execl("/usr/bin/curl", "curl", "--fail", "--silent", "--show-error", "--location", "-o", installer,
              "https://steamcdn-a.akamaihd.net/client/installer/SteamSetup.exe", (char*)NULL);
        _exit(127);
    }
    if (!wait_child_success(pid))
        goto done;
    if (access(installer, F_OK) != 0)
        goto done;
    wine_error = spawn_wine(home, "wineboot", "--init", NULL, &pid);
    if (wine_error) {
        free(wine_error);
        goto done;
    }
    if (!wait_child_success(pid))
        goto done;
    wine_error = spawn_wine(home, installer, NULL, NULL, &pid);
    if (wine_error) {
        free(wine_error);
        goto done;
    }
    (void)waitpid(pid, &wait_status, 0);
done:
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
    error_text = spawn_wine(home, "start", url, NULL, &pid);
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

char* ms_steam_launch_game_json(const char* home, const char* body, size_t len, int* status) {
    unsigned id;
    char url[64], *e, pipeline[32] = "auto";
    pid_t pid;
    char je[96];
    unsigned long long started_at = monotonic_millis();
    ms_json* request = NULL;
    char* requested = NULL;
    if (status)
        *status = 400;
    if (!body_id(body, len, &id))
        return err("appid required");
    if (status)
        *status = 500;
    request = ms_json_parse(body ? body : "", len, je, sizeof(je));
    if (request) {
        if (ms_json_as_string(ms_json_object_get(request, "pipeline"), &requested) ||
            ms_json_as_string(ms_json_object_get(request, "launchMethod"), &requested))
            snprintf(pipeline, sizeof(pipeline), "%s", requested);
        free(requested);
        ms_json_free(request);
    }
    if (!ensure_steam_bottle_manifest(home, id, pipeline)) {
        if (status)
            *status = 500;
        return err("failed to prepare Steam bottle manifest");
    }
    setenv("METALSHARP_PIPELINE", pipeline, 1);
    snprintf(url, sizeof(url), "steam://run/%u", id);
    e = spawn_wine(home, "start", url, NULL, &pid);
    unsetenv("METALSHARP_PIPELINE");
    if (e) {
        char* o = err(e);
        free(e);
        return o;
    }
    (void)mark_steam_bottle_launch(home, id, pid);
    record_launch_timing(home, id, started_at, pipeline);
    if (status)
        *status = 200;
    return pipeline_pid_result(pid, id, pipeline, home);
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
    e = spawn_wine(home, "start", url, NULL, &pid);
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
            bool search_command, macos_steam, target;
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
            search_command = strstr(end, " rg ") || strstr(end, "rg -i") || strstr(end, "ps axo");
            macos_steam = strstr(end, "Steam.app/Contents/MacOS") || strstr(end, "steam_osx");
            target = !search_command && !macos_steam &&
                     ((strstr(end, prefix) && (contains_ci(end, "Steam.exe") || contains_ci(end, "steam.exe"))) ||
                      contains_ci(end, "steamwebhelper.exe") || contains_ci(end, "steamwebhelper_real.exe") ||
                      contains_ci(end, "winedevice.exe") || contains_ci(end, "wineserver") ||
                      contains_ci(end, "wineloader") || contains_ci(end, "c:\\program files (x86)\\steam"));
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
        ms_json_writer_key(&w, "report");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "pipeline");
        ms_json_writer_string(&w, "m12");
        ms_json_writer_key(&w, "ready");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "issues");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
