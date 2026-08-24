#include "metalsharp_backend/shadps4.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x00000100
#endif

#define SHADPS4_MAX_GAMES   512
#define SHADPS4_MAX_ROOTS   32
#define SHADPS4_MAX_CAPTURE (8 * 1024 * 1024)
#define SHADPS4_STABLE_REPO "shadps4-emu/shadPS4"
#define SHADPS4_MIN_MODULES 1

typedef struct {
    char id[96];
    char title_id[32];
    char title[256];
    char version[48];
    char category[24];
    char path[4096];
    char launch_path[4096];
    char sfo_path[4096];
    char icon_path[4096];
    bool installed;
    bool has_update;
} shadps4_game;

typedef struct {
    shadps4_game items[SHADPS4_MAX_GAMES];
    size_t count;
    size_t scanned_entries;
} shadps4_games;

typedef struct {
    char* tag;
    char* version;
    char* asset_name;
    char* url;
    char* digest;
    unsigned long long size;
    char* published_at;
} shadps4_release;

typedef struct {
    pthread_mutex_t mutex;
    bool running;
    int percent;
    char status[32];
    char message[256];
    char error[256];
    char target[160];
    pid_t worker_pid;
} shadps4_update_state;

static shadps4_update_state g_update = {PTHREAD_MUTEX_INITIALIZER, false, 0, "idle", "", "", "", 0};

static char* join_path(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (p)
        snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
}

static bool mkdir_p(const char* path) {
    char* copy = strdup(path);
    if (!copy)
        return false;
    for (size_t i = 1; copy[i]; ++i) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            copy[i] = '/';
        }
    }
    bool ok = mkdir(copy, 0755) == 0 || errno == EEXIST;
    free(copy);
    return ok;
}

static char* read_file(const char* path, size_t limit, size_t* length_out) {
    FILE* f = fopen(path, "rb");
    long n;
    char* data;
    size_t got;
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f)
            fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || (size_t)n > limit || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)n + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }
    got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    if (length_out)
        *length_out = got;
    return data;
}

static bool write_atomic(const char* path, const char* data) {
    size_t n = strlen(path) + 32;
    char* tmp = malloc(n);
    FILE* f;
    bool ok;
    if (!tmp)
        return false;
    snprintf(tmp, n, "%s.tmp.%ld", path, (long)getpid());
    f = fopen(tmp, "wb");
    ok = f && fwrite(data, 1, strlen(data), f) == strlen(data) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0)
        ok = false;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
    free(tmp);
    return ok;
}

static char* error_json(const char* message) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message ? message : "SHADPS4 operation failed");
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static ms_json* parse_body(const unsigned char* body, size_t length) {
    char error[160];
    ms_json* root = ms_json_parse((const char*)(body ? body : (const unsigned char*)""), length, error, sizeof(error));
    if (!root || ms_json_type_of(root) != MS_JSON_OBJECT) {
        ms_json_free(root);
        return NULL;
    }
    return root;
}

static char* json_string(const ms_json* root, const char* key) {
    char* value = NULL;
    (void)ms_json_as_string(ms_json_object_get(root, key), &value);
    return value;
}

static bool json_bool(const ms_json* root, const char* key, bool fallback) {
    bool value;
    return ms_json_as_bool(ms_json_object_get(root, key), &value) ? value : fallback;
}

static bool remove_tree(const char* path);

static char* emulator_root(const char* home) {
    return join_path(home, "emulators/shadps4");
}

static void cleanup_interrupted_updates(const char* root) {
    bool running;
    pthread_mutex_lock(&g_update.mutex);
    running = g_update.running;
    pthread_mutex_unlock(&g_update.mutex);
    if (running)
        return;
    const char* names[] = {"downloads", "staging"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char* directory = join_path(root, names[i]);
        DIR* d = directory ? opendir(directory) : NULL;
        struct dirent* entry;
        if (!d) {
            free(directory);
            continue;
        }
        while ((entry = readdir(d))) {
            size_t n = strlen(entry->d_name);
            bool stale = (i == 0 && n > 5 && !strcmp(entry->d_name + n - 5, ".part")) ||
                         (i == 1 && !strncmp(entry->d_name, "update-", 7));
            if (stale) {
                char* path = join_path(directory, entry->d_name);
                if (path) {
                    (void)remove_tree(path);
                    free(path);
                }
            }
        }
        closedir(d);
        free(directory);
    }
}

static bool ensure_environment(const char* home) {
    char *root = emulator_root(home), *versions, *downloads, *staging, *logs, *sessions, *state_home, *manifest;
    bool ok = false;
    if (!root)
        return false;
    versions = join_path(root, "versions");
    downloads = join_path(root, "downloads");
    staging = join_path(root, "staging");
    logs = join_path(root, "logs");
    sessions = join_path(root, "sessions");
    state_home = join_path(root, "home/Library/Application Support/shadPS4");
    manifest = join_path(root, "environment.json");
    ok = versions && downloads && staging && logs && sessions && state_home && mkdir_p(versions) &&
         mkdir_p(downloads) && mkdir_p(staging) && mkdir_p(logs) && mkdir_p(sessions) && mkdir_p(state_home);
    if (ok)
        cleanup_interrupted_updates(root);
    if (ok && manifest && access(manifest, F_OK) != 0)
        ok = write_atomic(manifest, "{\"schemaVersion\":1,\"provider\":\"shadps4\",\"managedRuntime\":true,"
                                    "\"isolatedHome\":true,\"channel\":\"stable\"}\n");
    free(root);
    free(versions);
    free(downloads);
    free(staging);
    free(logs);
    free(sessions);
    free(state_home);
    free(manifest);
    return ok;
}

static bool is_safe_component(const char* value) {
    size_t n = value ? strlen(value) : 0;
    if (n == 0 || n > 150 || !strcmp(value, ".") || !strcmp(value, ".."))
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z') ||
              (value[i] >= '0' && value[i] <= '9') || value[i] == '.' || value[i] == '_' || value[i] == '-'))
            return false;
    return true;
}

static char* current_tag(const char* home) {
    char *root = emulator_root(home), *current = root ? join_path(root, "current") : NULL;
    char target[4096];
    ssize_t n = current ? readlink(current, target, sizeof(target) - 1) : -1;
    char* result = NULL;
    if (n > 0) {
        target[n] = '\0';
        const char* slash = strrchr(target, '/');
        result = strdup(slash ? slash + 1 : target);
    }
    free(root);
    free(current);
    return result;
}

static char* executable_path(const char* home) {
    const char* override = getenv("METALSHARP_SHADPS4_BIN");
    char *root, *path;
    if (override && override[0])
        return strdup(override);
    root = emulator_root(home);
    path = root ? join_path(root, "current/shadps4") : NULL;
    free(root);
    return path;
}

static const char* machine_arch(void) {
    const char* override = getenv("METALSHARP_SHADPS4_HOST_ARCH");
    static char result[32];
    struct utsname info;
    if (override && override[0])
        return override;
    if (!result[0]) {
        if (uname(&info) == 0)
            snprintf(result, sizeof(result), "%s", !strcmp(info.machine, "arm64") ? "arm64" : "x86_64");
        else
            snprintf(result, sizeof(result), "unknown");
    }
    return result;
}

static int run_wait(const char* const argv[], const char* output_path, const char* home_override) {
    pid_t pid = fork();
    int status = 0;
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (output_path) {
            int fd = open(output_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        if (home_override)
            setenv("HOME", home_override, 1);
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static char* run_capture(const char* const argv[], size_t limit) {
    int fds[2];
    pid_t pid;
    char* data;
    size_t used = 0, capacity = 8192;
    int status;
    if (pipe(fds) != 0)
        return NULL;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    close(fds[1]);
    data = malloc(capacity);
    if (!data) {
        close(fds[0]);
        waitpid(pid, &status, 0);
        return NULL;
    }
    for (;;) {
        ssize_t got;
        if (used + 4097 > capacity) {
            size_t next = capacity * 2;
            char* grown;
            if (next > limit + 1)
                next = limit + 1;
            if (next <= capacity)
                break;
            grown = realloc(data, next);
            if (!grown)
                break;
            data = grown;
            capacity = next;
        }
        got = read(fds[0], data + used, capacity - used - 1);
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        used += (size_t)got;
        if (used >= limit)
            break;
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(data);
        return NULL;
    }
    data[used] = '\0';
    return data;
}

static void release_free(shadps4_release* release) {
    if (!release)
        return;
    free(release->tag);
    free(release->version);
    free(release->asset_name);
    free(release->url);
    free(release->digest);
    free(release->published_at);
    memset(release, 0, sizeof(*release));
}

static char* release_field(const ms_json* object, const char* key) {
    char* value = NULL;
    (void)ms_json_as_string(ms_json_object_get(object, key), &value);
    return value;
}

static bool load_release(const char* home, shadps4_release* out, char* error, size_t error_size, bool force) {
    const char* fixture = getenv("METALSHARP_SHADPS4_RELEASE_JSON");
    char *text = NULL, parse_error[160];
    char *environment = NULL, *cache = NULL;
    ms_json* root;
    const ms_json* assets;
    const ms_json* asset = NULL;
    if (fixture && fixture[0])
        text = read_file(fixture, SHADPS4_MAX_CAPTURE, NULL);
    else {
        struct stat cache_stat;
        environment = home ? emulator_root(home) : NULL;
        cache = environment ? join_path(environment, "release-cache.json") : NULL;
        if (!force && cache && stat(cache, &cache_stat) == 0 && S_ISREG(cache_stat.st_mode) &&
            time(NULL) - cache_stat.st_mtime >= 0 && time(NULL) - cache_stat.st_mtime < 12 * 60 * 60)
            text = read_file(cache, SHADPS4_MAX_CAPTURE, NULL);
        char url[256];
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", SHADPS4_STABLE_REPO);
        const char* argv[] = {"/usr/bin/curl",      "--fail",     "--silent", "--show-error",
                              "--location",         "--max-time", "20",       "-A",
                              "MetalSharp-shadPS4", url,          NULL};
        if (!text) {
            text = run_capture(argv, SHADPS4_MAX_CAPTURE);
            if (text && cache) {
                (void)ensure_environment(home);
                (void)write_atomic(cache, text);
            }
        }
    }
    free(environment);
    free(cache);
    if (!text) {
        snprintf(error, error_size, "failed to fetch the official SHADPS4 release");
        return false;
    }
    root = ms_json_parse(text, strlen(text), parse_error, sizeof(parse_error));
    free(text);
    if (!root || ms_json_type_of(root) != MS_JSON_OBJECT) {
        ms_json_free(root);
        snprintf(error, error_size, "failed to parse the SHADPS4 release response");
        return false;
    }
    out->tag = release_field(root, "tag_name");
    out->published_at = release_field(root, "published_at");
    assets = ms_json_object_get(root, "assets");
    for (size_t i = 0; i < ms_json_array_length(assets); ++i) {
        const ms_json* candidate = ms_json_array_get(assets, i);
        char* name = release_field(candidate, "name");
        bool match = name && !strncmp(name, "shadps4-macos-sdl-", 18) && strstr(name, ".zip") &&
                     !strcmp(name + strlen(name) - 4, ".zip");
        free(name);
        if (match) {
            asset = candidate;
            break;
        }
    }
    if (!out->tag || !asset || !is_safe_component(out->tag)) {
        ms_json_free(root);
        release_free(out);
        snprintf(error, error_size, "the official release has no valid macOS asset");
        return false;
    }
    out->asset_name = release_field(asset, "name");
    out->url = release_field(asset, "browser_download_url");
    out->digest = release_field(asset, "digest");
    long long size = 0;
    if (ms_json_as_i64(ms_json_object_get(asset, "size"), &size) && size > 0)
        out->size = (unsigned long long)size;
    out->version = out->tag ? strdup(out->tag) : NULL;
    ms_json_free(root);
    if (!out->asset_name || !is_safe_component(out->asset_name) || !out->url || !out->digest || out->size == 0 ||
        strncmp(out->digest, "sha256:", 7) != 0 || strlen(out->digest + 7) != 64 ||
        strncmp(out->url, "https://github.com/shadps4-emu/shadPS4/releases/download/", 57) != 0) {
        release_free(out);
        snprintf(error, error_size, "the SHADPS4 release metadata is incomplete or untrusted");
        return false;
    }
    return true;
}

typedef struct {
    char* pinned_tag;
    char* skipped_tag;
} shadps4_update_policy;

static void load_update_policy(const char* home, shadps4_update_policy* policy) {
    char *environment = emulator_root(home), *path = environment ? join_path(environment, "update-policy.json") : NULL;
    char* text = path ? read_file(path, 64 * 1024, NULL) : NULL;
    memset(policy, 0, sizeof(*policy));
    if (text) {
        char error[128];
        ms_json* root = ms_json_parse(text, strlen(text), error, sizeof(error));
        policy->pinned_tag = json_string(root, "pinnedTag");
        policy->skipped_tag = json_string(root, "skippedTag");
        ms_json_free(root);
    }
    free(text);
    free(path);
    free(environment);
}

static void free_update_policy(shadps4_update_policy* policy) {
    free(policy->pinned_tag);
    free(policy->skipped_tag);
    memset(policy, 0, sizeof(*policy));
}

static bool save_update_policy(const char* home, const shadps4_update_policy* policy) {
    char *environment = emulator_root(home), *path = environment ? join_path(environment, "update-policy.json") : NULL;
    ms_json_writer w;
    bool ok;
    if (!path || !ensure_environment(home)) {
        free(environment);
        free(path);
        return false;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "pinnedTag");
    if (policy->pinned_tag)
        ms_json_writer_string(&w, policy->pinned_tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "skippedTag");
    if (policy->skipped_tag)
        ms_json_writer_string(&w, policy->skipped_tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    ok = text && write_atomic(path, text);
    free(text);
    free(environment);
    free(path);
    return ok;
}

static char* release_json(const char* home, bool force) {
    shadps4_release release = {0};
    shadps4_update_policy policy;
    char error[256], *current = current_tag(home);
    ms_json_writer w;
    if (!load_release(home, &release, error, sizeof(error), force)) {
        free(current);
        return error_json(error);
    }
    load_update_policy(home, &policy);
    bool newer = !current || strcmp(current, release.tag);
    bool pinned = policy.pinned_tag && current && !strcmp(policy.pinned_tag, current);
    bool skipped = policy.skipped_tag && !strcmp(policy.skipped_tag, release.tag);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "shadps4");
    ms_json_writer_key(&w, "currentTag");
    if (current)
        ms_json_writer_string(&w, current);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "latestTag");
    ms_json_writer_string(&w, release.tag);
    ms_json_writer_key(&w, "latestVersion");
    ms_json_writer_string(&w, release.version);
    ms_json_writer_key(&w, "available");
    ms_json_writer_bool(&w, newer && !pinned && !skipped);
    ms_json_writer_key(&w, "pinnedTag");
    if (policy.pinned_tag)
        ms_json_writer_string(&w, policy.pinned_tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "skippedTag");
    if (policy.skipped_tag)
        ms_json_writer_string(&w, policy.skipped_tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "suppressed");
    ms_json_writer_string(&w, pinned ? "pinned" : skipped ? "skipped" : "none");
    ms_json_writer_key(&w, "assetName");
    ms_json_writer_string(&w, release.asset_name);
    ms_json_writer_key(&w, "downloadSize");
    ms_json_writer_u64(&w, release.size);
    ms_json_writer_key(&w, "digest");
    ms_json_writer_string(&w, release.digest);
    ms_json_writer_key(&w, "publishedAt");
    ms_json_writer_string(&w, release.published_at ? release.published_at : "");
    ms_json_writer_object_end(&w);
    free(current);
    free_update_policy(&policy);
    release_free(&release);
    return ms_json_writer_take(&w);
}

static void update_set(const char* status, int percent, const char* message, const char* error) {
    pthread_mutex_lock(&g_update.mutex);
    snprintf(g_update.status, sizeof(g_update.status), "%s", status ? status : "idle");
    g_update.percent = percent;
    snprintf(g_update.message, sizeof(g_update.message), "%s", message ? message : "");
    snprintf(g_update.error, sizeof(g_update.error), "%s", error ? error : "");
    if (!strcmp(g_update.status, "completed") || !strcmp(g_update.status, "failed") || !strcmp(g_update.status, "idle"))
        g_update.running = false;
    pthread_mutex_unlock(&g_update.mutex);
}

static char* update_progress_json(void) {
    ms_json_writer w;
    pthread_mutex_lock(&g_update.mutex);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "status");
    ms_json_writer_string(&w, g_update.status);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, g_update.running);
    ms_json_writer_key(&w, "percent");
    ms_json_writer_i64(&w, g_update.percent);
    ms_json_writer_key(&w, "message");
    ms_json_writer_string(&w, g_update.message);
    ms_json_writer_key(&w, "error");
    if (g_update.error[0])
        ms_json_writer_string(&w, g_update.error);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "targetTag");
    if (g_update.target[0])
        ms_json_writer_string(&w, g_update.target);
    else
        ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    pthread_mutex_unlock(&g_update.mutex);
    return ms_json_writer_take(&w);
}

static bool remove_tree(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR* d = opendir(path);
        struct dirent* entry;
        if (!d)
            return false;
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char* child = join_path(path, entry->d_name);
            bool ok = child && remove_tree(child);
            free(child);
            if (!ok) {
                closedir(d);
                return false;
            }
        }
        closedir(d);
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

static char* find_stage_file(const char* root, const char* name, unsigned depth) {
    DIR* d;
    struct dirent* entry;
    if (depth > 3 || !(d = opendir(root)))
        return NULL;
    while ((entry = readdir(d))) {
        struct stat st;
        char *path, *found;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = join_path(root, entry->d_name);
        if (!path)
            continue;
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && !strcmp(entry->d_name, name)) {
            closedir(d);
            return path;
        }
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            found = find_stage_file(path, name, depth + 1);
            if (found) {
                free(path);
                closedir(d);
                return found;
            }
        }
        free(path);
    }
    closedir(d);
    return NULL;
}

static bool symlinks_stay_inside(const char* root, const char* path) {
    DIR* d = opendir(path);
    struct dirent* entry;
    char resolved_root[4096];
    if (!realpath(root, resolved_root) || !d)
        return false;
    while ((entry = readdir(d))) {
        char* child;
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = join_path(path, entry->d_name);
        if (!child) {
            closedir(d);
            return false;
        }
        if (lstat(child, &st) != 0) {
            free(child);
            closedir(d);
            return false;
        }
        if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
            free(child);
            closedir(d);
            return false;
        }
        if (S_ISDIR(st.st_mode) && !symlinks_stay_inside(root, child)) {
            free(child);
            closedir(d);
            return false;
        }
        free(child);
    }
    closedir(d);
    return true;
}

static bool zip_entry_safe(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= 4096 || name[0] == '/' || strchr(name, '\\'))
        return false;
    const char* part = name;
    for (size_t i = 0; i <= n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if ((c > 0 && c < 32) || c == 127)
            return false;
        if (name[i] == '/' || name[i] == '\0') {
            size_t length = (size_t)(name + i - part);
            if ((length == 0 && i < n) || (length == 1 && part[0] == '.') ||
                (length == 2 && part[0] == '.' && part[1] == '.'))
                return false;
            part = name + i + 1;
        }
    }
    return true;
}

static bool zip_entries_safe(const char* archive) {
    const char* argv[] = {"/usr/bin/unzip", "-Z1", archive, NULL};
    char* listing = run_capture(argv, SHADPS4_MAX_CAPTURE);
    char* names[4096] = {0};
    size_t count = 0;
    bool ok = listing != NULL;
    char* save = NULL;
    for (char* line = ok ? strtok_r(listing, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r')
            line[--n] = '\0';
        if (!zip_entry_safe(line) || count >= 4096) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(names[i], line)) {
                ok = false;
                break;
            }
        if (!ok || !(names[count] = strdup(line))) {
            ok = false;
            break;
        }
        count++;
    }
    for (size_t i = 0; i < count; ++i)
        free(names[i]);
    free(listing);
    return ok && count > 0;
}

static bool approved_download_url(const char* url) {
    return url &&
           (!strncmp(url, "https://github.com/", 19) || !strncmp(url, "https://objects.githubusercontent.com/", 38) ||
            !strncmp(url, "https://release-assets.githubusercontent.com/", 45));
}

static bool file_sha256(const char* path, char output[65]) {
    const char* argv[] = {"/usr/bin/shasum", "-a", "256", path, NULL};
    char* text = run_capture(argv, 4096);
    bool ok = text && strlen(text) >= 64;
    if (ok) {
        memcpy(output, text, 64);
        output[64] = '\0';
        for (size_t i = 0; i < 64; ++i)
            if (!((output[i] >= '0' && output[i] <= '9') || (output[i] >= 'a' && output[i] <= 'f') ||
                  (output[i] >= 'A' && output[i] <= 'F')))
                ok = false;
    }
    free(text);
    return ok;
}

static int version_major(const char* text) {
    if (!text)
        return -1;
    while (*text && (*text < '0' || *text > '9'))
        ++text;
    return *text ? (int)strtol(text, NULL, 10) : -1;
}

static unsigned long long host_sysctl_u64(const char* name) {
    const char* argv[] = {"/usr/sbin/sysctl", "-n", name, NULL};
    char* output = run_capture(argv, 4096);
    unsigned long long value = output ? strtoull(output, NULL, 10) : 0;
    free(output);
    return value;
}

static int host_macos_major(void) {
    const char* override = getenv("METALSHARP_SHADPS4_HOST_MACOS");
    if (override && override[0])
        return version_major(override);
    const char* argv[] = {"/usr/bin/sw_vers", "-productVersion", NULL};
    char* output = run_capture(argv, 4096);
    int major = version_major(output);
    free(output);
    return major;
}

static int macho_minimum_macos(const char* path) {
    const char* argv[] = {"/usr/bin/otool", "-l", path, NULL};
    char* output = run_capture(argv, 256 * 1024);
    int major = -1;
    if (output) {
        char* marker = strstr(output, "minos ");
        if (marker)
            major = version_major(marker + 6);
    }
    free(output);
    return major;
}

static bool validate_macho_dependencies(const char* path) {
    const char* argv[] = {"/usr/bin/otool", "-L", path, NULL};
    char* output = run_capture(argv, 512 * 1024);
    bool ok = output != NULL;
    char* save = NULL;
    size_t line_number = 0;
    for (char* line = ok ? strtok_r(output, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        if (line_number++ == 0)
            continue;
        while (*line == ' ' || *line == '\t')
            ++line;
        char* end = strchr(line, ' ');
        if (end)
            *end = '\0';
        if (!strncmp(line, "/usr/lib/", 9) || !strncmp(line, "/System/Library/", 16) ||
            !strncmp(line, "@rpath/libvulkan", 16))
            continue;
        ok = false;
        break;
    }
    free(output);
    return ok && line_number > 1;
}

static bool validate_macho_x86_64(const char* path) {
    const char* argv[] = {"/usr/bin/lipo", "-archs", path, NULL};
    char* output = run_capture(argv, 4096);
    bool ok = output && strstr(output, "x86_64") && !strstr(output, "arm64");
    free(output);
    return ok;
}

static bool rosetta_available(void) {
    const char* forced = getenv("METALSHARP_SHADPS4_ROSETTA");
    if (forced)
        return !strcmp(forced, "1") || !strcasecmp(forced, "true");
    const char* argv[] = {"/usr/bin/arch", "-x86_64", "/usr/bin/true", NULL};
    return run_wait(argv, NULL, NULL) == 0;
}

static bool switch_version(const char* root, const char* tag, char* error, size_t error_size) {
    char *current = join_path(root, "current"), *previous = join_path(root, "previous"),
         *temp = join_path(root, "current.new");
    char old[4096];
    ssize_t old_len = current ? readlink(current, old, sizeof(old) - 1) : -1;
    char target[256];
    bool ok = false;
    snprintf(target, sizeof(target), "versions/%s", tag);
    if (!current || !previous || !temp) {
        snprintf(error, error_size, "failed to allocate version paths");
        goto done;
    }
    unlink(temp);
    if (symlink(target, temp) != 0) {
        snprintf(error, error_size, "failed to stage the active SHADPS4 version");
        goto done;
    }
    if (old_len > 0) {
        old[old_len] = '\0';
        char* previous_temp = join_path(root, "previous.new");
        if (!previous_temp) {
            unlink(temp);
            snprintf(error, error_size, "failed to stage rollback metadata");
            goto done;
        }
        unlink(previous_temp);
        if (symlink(old, previous_temp) != 0 || rename(previous_temp, previous) != 0) {
            unlink(previous_temp);
            unlink(temp);
            free(previous_temp);
            snprintf(error, error_size, "failed to preserve the rollback version");
            goto done;
        }
        free(previous_temp);
    }
    if (getenv("METALSHARP_SHADPS4_FAIL_ACTIVATION")) {
        unlink(temp);
        snprintf(error, error_size, "shadPS4 activation was interrupted by the validation hook");
        goto done;
    }
    if (rename(temp, current) != 0) {
        unlink(temp);
        snprintf(error, error_size, "failed to activate the SHADPS4 version");
        goto done;
    }
    ok = true;
done:
    free(current);
    free(previous);
    free(temp);
    return ok;
}

typedef struct {
    char* home;
    shadps4_release release;
} update_job;

static bool any_session_running(const char* home);

static bool write_source_manifest(const char* version_dir, const shadps4_release* release) {
    char* path = join_path(version_dir, "source.json");
    ms_json_writer w;
    bool ok;
    if (!path)
        return false;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "shadps4");
    ms_json_writer_key(&w, "repository");
    ms_json_writer_string(&w, SHADPS4_STABLE_REPO);
    ms_json_writer_key(&w, "tag");
    ms_json_writer_string(&w, release->tag);
    ms_json_writer_key(&w, "assetName");
    ms_json_writer_string(&w, release->asset_name);
    ms_json_writer_key(&w, "assetUrl");
    ms_json_writer_string(&w, release->url);
    ms_json_writer_key(&w, "assetSize");
    ms_json_writer_u64(&w, release->size);
    ms_json_writer_key(&w, "assetDigest");
    ms_json_writer_string(&w, release->digest);
    ms_json_writer_key(&w, "locallyAdHocSigned");
    ms_json_writer_bool(&w, true);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static bool write_capability_manifest(const char* version_dir, const char* tag) {
    char* path = join_path(version_dir, "capabilities.json");
    if (!path)
        return false;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schemaVersion");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "shadps4");
    ms_json_writer_key(&w, "runtimeTag");
    ms_json_writer_string(&w, tag);
    ms_json_writer_key(&w, "runtimeArchitecture");
    ms_json_writer_string(&w, "x86_64");
    ms_json_writer_key(&w, "cli");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "--game");
    ms_json_writer_string(&w, "--fullscreen");
    ms_json_writer_string(&w, "--config-global");
    ms_json_writer_string(&w, "--add-game-folder");
    ms_json_writer_string(&w, "--set-addon-folder");
    ms_json_writer_string(&w, "--override-root");
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "content");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "cusa-directory");
    ms_json_writer_string(&w, "update-directory");
    ms_json_writer_string(&w, "console-dumped-modules");
    ms_json_writer_string(&w, "console-dumped-fonts");
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "packageExtraction");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "zarDiscovery");
    ms_json_writer_bool(&w, false);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    bool ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static bool icd_manifest_valid(const char* path) {
    char* text = read_file(path, 64 * 1024, NULL);
    bool valid = false;
    if (text) {
        char error[128];
        ms_json* root = ms_json_parse(text, strlen(text), error, sizeof(error));
        const ms_json* icd = ms_json_object_get(root, "ICD");
        char* library = json_string(icd, "library_path");
        valid = library && !strcmp(library, "./libvulkan_kosmickrisp.dylib");
        free(library);
        ms_json_free(root);
    }
    free(text);
    return valid;
}

static bool probe_runtime(const char* executable, const char* version_dir, const char* isolated_home) {
    int fds[2], status = 0;
    pid_t pid;
    char output[64 * 1024];
    size_t used = 0;
    if (pipe(fds) != 0)
        return false;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (chdir(version_dir) != 0)
            _exit(126);
        setenv("HOME", isolated_home, 1);
        setenv("VK_DRIVER_FILES", "./kosmickrisp_mesa_icd.json", 1);
        alarm(10);
        execl("/usr/bin/arch", "/usr/bin/arch", "-x86_64", executable, "--help", (char*)NULL);
        _exit(127);
    }
    close(fds[1]);
    while (used + 1 < sizeof(output)) {
        ssize_t got = read(fds[0], output + used, sizeof(output) - used - 1);
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        used += (size_t)got;
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    output[used] = '\0';
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && strstr(output, "--fullscreen") &&
           strstr(output, "--add-game-folder") && strstr(output, "--config-global");
}

static void* update_worker(void* raw) {
    update_job* job = raw;
    shadps4_release* release = &job->release;
    char *root = emulator_root(job->home), *downloads = root ? join_path(root, "downloads") : NULL;
    char *staging = root ? join_path(root, "staging") : NULL, *versions = root ? join_path(root, "versions") : NULL;
    char *archive = NULL, *download = NULL, *stage = NULL, *version_dir = NULL;
    char *source_exe = NULL, *source_loader = NULL, *source_driver = NULL, *source_icd = NULL;
    char *dest_exe = NULL, *dest_loader = NULL, *dest_driver = NULL, *dest_icd = NULL;
    char *isolated_home = root ? join_path(root, "home") : NULL, *license_path = NULL;
    char error[256] = "", sha[65];
    struct stat st;
    bool ok = false;
    if (!root || !downloads || !staging || !versions || !isolated_home || access("/usr/bin/unzip", X_OK) != 0) {
        snprintf(error, sizeof(error), "failed to prepare the shadPS4 update environment");
        goto done;
    }
    archive = join_path(downloads, release->asset_name);
    if (archive) {
        size_t download_size = strlen(archive) + 6;
        download = malloc(download_size);
        if (download)
            snprintf(download, download_size, "%s.part", archive);
    }
    char stage_name[96];
    snprintf(stage_name, sizeof(stage_name), "update-%ld-%lld", (long)getpid(), (long long)time(NULL));
    stage = join_path(staging, stage_name);
    version_dir = join_path(versions, release->tag);
    if (!archive || !download || !stage || !version_dir || !mkdir_p(stage)) {
        snprintf(error, sizeof(error), "failed to create shadPS4 update staging");
        goto done;
    }

    update_set("downloading", 15, "Downloading the official stable shadPS4 core", NULL);
    unlink(download);
    const char* download_fixture = getenv("METALSHARP_SHADPS4_DOWNLOAD_FILE");
    const char* curl_argv[] = {"/usr/bin/curl", "--fail",   "--location",    "--max-redirs", "5",
                               "--proto",       "=https",   "--proto-redir", "=https",       "--silent",
                               "--show-error",  "--output", download,        "--write-out",  "%{url_effective}",
                               release->url,    NULL};
    const char* copy_argv[] = {"/bin/cp", download_fixture, download, NULL};
    char* final_url = NULL;
    int download_status;
    if (download_fixture && download_fixture[0])
        download_status = run_wait(copy_argv, NULL, NULL);
    else {
        final_url = run_capture(curl_argv, 4096);
        download_status = approved_download_url(final_url) ? 0 : -1;
    }
    free(final_url);
    if (download_status != 0 || stat(download, &st) != 0 || !S_ISREG(st.st_mode) ||
        (unsigned long long)st.st_size != release->size) {
        snprintf(error, sizeof(error), "shadPS4 download failed or had an unexpected size");
        goto done;
    }
    update_set("verifying", 40, "Verifying the official shadPS4 SHA-256", NULL);
    if (!file_sha256(download, sha) || strcasecmp(sha, release->digest + 7)) {
        snprintf(error, sizeof(error), "shadPS4 download digest did not match the official release");
        goto done;
    }
    if (rename(download, archive) != 0 || !zip_entries_safe(archive)) {
        snprintf(error, sizeof(error), "shadPS4 ZIP failed path-safety validation");
        goto done;
    }

    update_set("extracting", 55, "Extracting shadPS4 into isolated staging", NULL);
    const char* extract_argv[] = {"/usr/bin/unzip", "-qq", archive, "-d", stage, NULL};
    if (run_wait(extract_argv, NULL, NULL) != 0 || !symlinks_stay_inside(stage, stage)) {
        snprintf(error, sizeof(error), "shadPS4 ZIP extraction failed validation");
        goto done;
    }
    source_exe = find_stage_file(stage, "shadps4", 0);
    source_loader = find_stage_file(stage, "libvulkan.dylib", 0);
    source_driver = find_stage_file(stage, "libvulkan_kosmickrisp.dylib", 0);
    source_icd = find_stage_file(stage, "kosmickrisp_mesa_icd.json", 0);
    if (!source_exe || !source_loader || !source_driver || !source_icd || access(source_exe, X_OK) != 0 ||
        !icd_manifest_valid(source_icd)) {
        snprintf(error, sizeof(error), "shadPS4 release is missing its validated core or Vulkan runtime");
        goto done;
    }
    if (!validate_macho_x86_64(source_exe) || !validate_macho_x86_64(source_loader) ||
        !validate_macho_x86_64(source_driver)) {
        snprintf(error, sizeof(error), "shadPS4 release contains an unsupported Mach-O architecture");
        goto done;
    }
    if (!validate_macho_dependencies(source_exe) || !validate_macho_dependencies(source_loader) ||
        !validate_macho_dependencies(source_driver)) {
        snprintf(error, sizeof(error), "shadPS4 release contains an untrusted Mach-O dependency");
        goto done;
    }
    int min_macos = macho_minimum_macos(source_exe);
    int host_macos = host_macos_major();
    if (min_macos < 0 || host_macos < min_macos) {
        snprintf(error, sizeof(error), "shadPS4 requires macOS %d or newer", min_macos > 0 ? min_macos : 26);
        goto done;
    }
    if (access(version_dir, F_OK) == 0 && !remove_tree(version_dir)) {
        snprintf(error, sizeof(error), "failed to replace an incomplete shadPS4 version");
        goto done;
    }
    if (!mkdir_p(version_dir)) {
        snprintf(error, sizeof(error), "failed to create the shadPS4 version directory");
        goto done;
    }
    dest_exe = join_path(version_dir, "shadps4");
    dest_loader = join_path(version_dir, "libvulkan.dylib");
    dest_driver = join_path(version_dir, "libvulkan_kosmickrisp.dylib");
    dest_icd = join_path(version_dir, "kosmickrisp_mesa_icd.json");
    if (!dest_exe || !dest_loader || !dest_driver || !dest_icd || rename(source_exe, dest_exe) != 0 ||
        rename(source_loader, dest_loader) != 0 || rename(source_driver, dest_driver) != 0 ||
        rename(source_icd, dest_icd) != 0) {
        snprintf(error, sizeof(error), "failed to move shadPS4 into the version store");
        goto done;
    }

    update_set("validating", 72, "Locally signing and probing the verified shadPS4 core", NULL);
    const char* binaries[] = {dest_exe, dest_loader, dest_driver};
    const char* codesign = getenv("METALSHARP_SHADPS4_CODESIGN_BIN");
    if (!codesign || !codesign[0])
        codesign = "/usr/bin/codesign";
    for (size_t i = 0; i < sizeof(binaries) / sizeof(binaries[0]); ++i) {
        const char* sign_argv[] = {codesign, "--force", "--sign", "-", binaries[i], NULL};
        const char* verify_argv[] = {codesign, "--verify", "--strict", binaries[i], NULL};
        if (run_wait(sign_argv, NULL, NULL) != 0 || run_wait(verify_argv, NULL, NULL) != 0) {
            snprintf(error, sizeof(error), "failed to locally sign the verified shadPS4 runtime");
            goto done;
        }
    }
    license_path = join_path(version_dir, "LICENSE");
    if (!license_path) {
        snprintf(error, sizeof(error), "failed to prepare shadPS4 license provenance");
        goto done;
    }
    const char* license_fixture = getenv("METALSHARP_SHADPS4_LICENSE_FILE");
    char license_url[512];
    snprintf(license_url, sizeof(license_url), "https://raw.githubusercontent.com/shadps4-emu/shadPS4/%s/LICENSE",
             release->tag);
    const char* license_copy[] = {"/bin/cp", license_fixture, license_path, NULL};
    const char* license_curl[] = {"/usr/bin/curl", "--fail",   "--silent",   "--show-error", "--proto",
                                  "=https",        "--output", license_path, license_url,    NULL};
    int license_status =
        license_fixture && license_fixture[0] ? run_wait(license_copy, NULL, NULL) : run_wait(license_curl, NULL, NULL);
    if (license_status != 0 || !write_source_manifest(version_dir, release) ||
        !probe_runtime(dest_exe, version_dir, isolated_home) || !write_capability_manifest(version_dir, release->tag)) {
        snprintf(error, sizeof(error), "shadPS4 provenance or CLI capability validation failed");
        goto done;
    }

    while (any_session_running(job->home)) {
        update_set("waiting_for_exit", 85, "shadPS4 update will activate after active games exit", NULL);
        sleep(1);
    }
    update_set("activating", 92, "Atomically activating shadPS4", NULL);
    if (!switch_version(root, release->tag, error, sizeof(error)))
        goto done;
    ok = true;

done:
    if (stage)
        remove_tree(stage);
    if (!ok && version_dir)
        remove_tree(version_dir);
    if (!ok && archive)
        unlink(archive);
    if (download)
        unlink(download);
    update_set(ok ? "completed" : "failed", ok ? 100 : 0, ok ? "shadPS4 is ready" : "shadPS4 update failed",
               ok ? NULL : error);
    free(root);
    free(downloads);
    free(staging);
    free(versions);
    free(archive);
    free(download);
    free(stage);
    free(version_dir);
    free(source_exe);
    free(source_loader);
    free(source_driver);
    free(source_icd);
    free(dest_exe);
    free(dest_loader);
    free(dest_driver);
    free(dest_icd);
    free(isolated_home);
    free(license_path);
    release_free(release);
    free(job->home);
    free(job);
    return NULL;
}

static char* start_update(const char* home) {
    update_job* job;
    pthread_t thread;
    char error[256] = "";
    char* installed_tag;
    if (strcmp(machine_arch(), "arm64"))
        return error_json("shadPS4 for macOS requires an Apple Silicon host");
    if (host_macos_major() < 26)
        return error_json("the current shadPS4 runtime requires macOS 26 or newer");
    if (!rosetta_available())
        return error_json("Rosetta 2 is required to run shadPS4");
    if (!ensure_environment(home))
        return error_json("failed to create the isolated shadPS4 environment");
    pthread_mutex_lock(&g_update.mutex);
    if (g_update.running) {
        pthread_mutex_unlock(&g_update.mutex);
        return error_json("a shadPS4 update is already running");
    }
    g_update.running = true;
    g_update.percent = 0;
    snprintf(g_update.status, sizeof(g_update.status), "starting");
    g_update.error[0] = '\0';
    pthread_mutex_unlock(&g_update.mutex);
    job = calloc(1, sizeof(*job));
    if (!job || !(job->home = strdup(home)) || !load_release(home, &job->release, error, sizeof(error), false)) {
        if (job) {
            free(job->home);
            free(job);
        }
        update_set("failed", 0, "SHADPS4 update failed", error[0] ? error : "failed to prepare update");
        return error_json(error[0] ? error : "failed to prepare update");
    }
    installed_tag = current_tag(home);
    char* installed_executable = executable_path(home);
    bool installed_ready = installed_executable && access(installed_executable, X_OK) == 0;
    free(installed_executable);
    if (installed_ready && installed_tag && !strcmp(installed_tag, job->release.tag)) {
        free(installed_tag);
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("idle", 0, "SHADPS4 is already up to date", NULL);
        return error_json("SHADPS4 is already up to date");
    }
    free(installed_tag);
    pthread_mutex_lock(&g_update.mutex);
    snprintf(g_update.target, sizeof(g_update.target), "%s", job->release.tag);
    pthread_mutex_unlock(&g_update.mutex);
    if (pthread_create(&thread, NULL, update_worker, job) != 0) {
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("failed", 0, "SHADPS4 update failed", "failed to start update worker");
        return error_json("failed to start SHADPS4 update worker");
    }
    pthread_detach(thread);
    return update_progress_json();
}

static char* rollback_update(const char* home) {
    char *root = emulator_root(home), *previous;
    char previous_target[4096], error[256];
    ssize_t length;
    bool ok = false;
    if (!root)
        return error_json("failed to resolve SHADPS4 environment");
    if (any_session_running(home)) {
        free(root);
        return error_json("stop SHADPS4 before rolling back");
    }
    previous = join_path(root, "previous");
    length = previous ? readlink(previous, previous_target, sizeof(previous_target) - 1) : -1;
    if (length > 0) {
        previous_target[length] = '\0';
        const char* prefix = "versions/";
        const char* tag = previous_target + strlen(prefix);
        char* candidate = !strncmp(previous_target, prefix, strlen(prefix)) && is_safe_component(tag)
                              ? join_path(root, previous_target)
                              : NULL;
        char* executable = candidate ? join_path(candidate, "shadps4") : NULL;
        if (executable && access(executable, X_OK) == 0)
            ok = switch_version(root, tag, error, sizeof(error));
        free(candidate);
        free(executable);
    }
    free(root);
    free(previous);
    if (!ok)
        return error_json("no valid previous SHADPS4 version is available");
    return ms_shadps4_status_json(home);
}

static uint16_t le16(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool sfo_value(const unsigned char* data, size_t size, const char* wanted, char* output, size_t output_size) {
    if (size < 20 || data[0] != 0 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F')
        return false;
    uint32_t key_start = le32(data + 8), value_start = le32(data + 12), count = le32(data + 16);
    if (count > 1024 || key_start >= size || value_start >= size || 20ULL + (uint64_t)count * 16ULL > size)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* entry = data + 20 + i * 16;
        uint16_t key_offset = le16(entry), format = le16(entry + 2);
        uint32_t value_len = le32(entry + 4), value_offset = le32(entry + 12);
        if ((uint64_t)key_start + key_offset >= size || (uint64_t)value_start + value_offset >= size)
            continue;
        const char* key = (const char*)data + key_start + key_offset;
        size_t key_room = size - (key_start + key_offset);
        if (!memchr(key, '\0', key_room) || strcmp(key, wanted))
            continue;
        if (format != 0x0204 && format != 0x0004)
            continue;
        size_t available = size - (value_start + value_offset);
        size_t n = value_len < available ? value_len : available;
        if (n >= output_size)
            n = output_size - 1;
        memcpy(output, data + value_start + value_offset, n);
        output[n] = '\0';
        while (n > 0 &&
               (output[n - 1] == '\0' || output[n - 1] == '\r' || output[n - 1] == '\n' || output[n - 1] == ' '))
            output[--n] = '\0';
        return n > 0;
    }
    return false;
}

static unsigned long long path_hash(const char* value) {
    unsigned long long h = 1469598103934665603ULL;
    while (*value) {
        h ^= (unsigned char)*value++;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool game_exists(const shadps4_games* games, const char* sfo) {
    for (size_t i = 0; i < games->count; ++i)
        if (!strcmp(games->items[i].sfo_path, sfo))
            return true;
    return false;
}

static bool valid_cusa_id(const char* value) {
    if (!value || strncasecmp(value, "CUSA", 4) || strlen(value) != 9)
        return false;
    for (size_t i = 4; i < 9; ++i)
        if (value[i] < '0' || value[i] > '9')
            return false;
    return true;
}

static bool suffix_case(const char* value, const char* suffix) {
    size_t n = strlen(value), m = strlen(suffix);
    return n >= m && !strcasecmp(value + n - m, suffix);
}

static void add_sfo_game(shadps4_games* games, const char* sfo, bool installed) {
    size_t size = 0;
    unsigned char* data;
    char sce_sys[4096], game_root[4096];
    const char* slash;
    shadps4_game* game;
    struct stat st;
    (void)installed;
    if (games->count >= SHADPS4_MAX_GAMES || game_exists(games, sfo))
        return;
    slash = strrchr(sfo, '/');
    if (!slash || (size_t)(slash - sfo) >= sizeof(sce_sys))
        return;
    memcpy(sce_sys, sfo, (size_t)(slash - sfo));
    sce_sys[slash - sfo] = '\0';
    const char* sce_name = strrchr(sce_sys, '/');
    if (!sce_name || strcasecmp(sce_name + 1, "sce_sys"))
        return;
    size_t root_len = (size_t)(sce_name - sce_sys);
    if (!root_len || root_len >= sizeof(game_root))
        return;
    memcpy(game_root, sce_sys, root_len);
    game_root[root_len] = '\0';
    const char* base = strrchr(game_root, '/');
    base = base ? base + 1 : game_root;
    if (suffix_case(base, "-patch") || suffix_case(base, "-update"))
        return;
    char* eboot = join_path(game_root, "eboot.bin");
    if (!eboot || lstat(eboot, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        free(eboot);
        return;
    }
    data = (unsigned char*)read_file(sfo, 1024 * 1024, &size);
    if (!data) {
        free(eboot);
        return;
    }
    game = &games->items[games->count];
    memset(game, 0, sizeof(*game));
    snprintf(game->sfo_path, sizeof(game->sfo_path), "%s", sfo);
    snprintf(game->path, sizeof(game->path), "%s", game_root);
    snprintf(game->launch_path, sizeof(game->launch_path), "%s", eboot);
    (void)sfo_value(data, size, "TITLE_ID", game->title_id, sizeof(game->title_id));
    (void)sfo_value(data, size, "TITLE", game->title, sizeof(game->title));
    (void)sfo_value(data, size, "APP_VER", game->version, sizeof(game->version));
    (void)sfo_value(data, size, "CATEGORY", game->category, sizeof(game->category));
    free(data);
    if (!valid_cusa_id(game->title_id)) {
        free(eboot);
        memset(game, 0, sizeof(*game));
        return;
    }
    free(eboot);
    char* icon = join_path(sce_sys, "icon0.png");
    if (icon && access(icon, R_OK) != 0) {
        free(icon);
        icon = join_path(sce_sys, "ICON0.PNG");
    }
    if (icon && access(icon, R_OK) == 0)
        snprintf(game->icon_path, sizeof(game->icon_path), "%s", icon);
    free(icon);
    if (!game->title[0])
        snprintf(game->title, sizeof(game->title), "%s", base[0] ? base : "PlayStation 4 Game");
    snprintf(game->id, sizeof(game->id), "%s-%llx", game->title_id[0] ? game->title_id : "ps4", path_hash(game->path));
    game->installed = false;
    const char* update_suffixes[] = {"-patch", "-UPDATE"};
    for (size_t i = 0; i < sizeof(update_suffixes) / sizeof(update_suffixes[0]); ++i) {
        size_t candidate_size = strlen(game_root) + strlen(update_suffixes[i]) + 32;
        char* candidate = malloc(candidate_size);
        if (candidate) {
            snprintf(candidate, candidate_size, "%s%s/sce_sys/param.sfo", game_root, update_suffixes[i]);
            if (access(candidate, R_OK) == 0)
                game->has_update = true;
            free(candidate);
        }
    }
    games->count++;
}

static void scan_directory(shadps4_games* games, const char* root, unsigned depth, bool installed) {
    DIR* d;
    struct dirent* entry;
    if (depth > 6 || games->count >= SHADPS4_MAX_GAMES || games->scanned_entries >= 20000 || !(d = opendir(root)))
        return;
    while ((entry = readdir(d))) {
        char* path;
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++games->scanned_entries > 20000)
            break;
        path = join_path(root, entry->d_name);
        if (!path)
            continue;
        if (!strcasecmp(entry->d_name, "param.sfo"))
            add_sfo_game(games, path, installed);
        else if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
            scan_directory(games, path, depth + 1, installed);
        free(path);
    }
    closedir(d);
}

static size_t load_roots(const char* home, char* roots[SHADPS4_MAX_ROOTS]) {
    char *root = emulator_root(home), *library = root ? join_path(root, "library.json") : NULL;
    char* text = library ? read_file(library, 1024 * 1024, NULL) : NULL;
    size_t count = 0;
    if (text) {
        char error[128];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        const ms_json* array = ms_json_object_get(json, "roots");
        for (size_t i = 0; i < ms_json_array_length(array) && count < SHADPS4_MAX_ROOTS; ++i) {
            char* value = NULL;
            if (ms_json_as_string(ms_json_array_get(array, i), &value) && value)
                roots[count++] = value;
        }
        ms_json_free(json);
    }
    free(text);
    free(root);
    free(library);
    return count;
}

static bool save_roots(const char* home, char* roots[], size_t count) {
    char *root = emulator_root(home), *library = root ? join_path(root, "library.json") : NULL;
    ms_json_writer w;
    bool ok;
    if (!root || !library || !ensure_environment(home)) {
        free(root);
        free(library);
        return false;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schemaVersion");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "roots");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < count; ++i)
        ms_json_writer_string(&w, roots[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    ok = text && write_atomic(library, text);
    free(text);
    free(root);
    free(library);
    return ok;
}

static void collect_games(const char* home, shadps4_games* games) {
    char* roots[SHADPS4_MAX_ROOTS] = {0};
    size_t count;
    memset(games, 0, sizeof(*games));
    count = load_roots(home, roots);
    for (size_t i = 0; i < count; ++i) {
        scan_directory(games, roots[i], 0, false);
        free(roots[i]);
    }
}

static char* session_path(const char* home, const char* id) {
    char *root = emulator_root(home), *sessions = root ? join_path(root, "sessions") : NULL;
    char name[128];
    char* result;
    snprintf(name, sizeof(name), "%s.json", id);
    result = sessions ? join_path(sessions, name) : NULL;
    free(root);
    free(sessions);
    return result;
}

static char* last_session_path(const char* home, const char* id) {
    char *root = emulator_root(home), *sessions = root ? join_path(root, "sessions") : NULL;
    char name[144];
    char* result;
    snprintf(name, sizeof(name), "%s.last.json", id);
    result = sessions ? join_path(sessions, name) : NULL;
    free(root);
    free(sessions);
    return result;
}

static void save_exit_record(const char* home, const char* id, const char* executable, const char* log_path,
                             int exit_code, int exit_signal) {
    char* path = last_session_path(home, id);
    if (!path)
        return;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, id);
    ms_json_writer_key(&w, "executable");
    ms_json_writer_string(&w, executable ? executable : "");
    ms_json_writer_key(&w, "logPath");
    ms_json_writer_string(&w, log_path ? log_path : "");
    ms_json_writer_key(&w, "exitCode");
    if (exit_code >= 0)
        ms_json_writer_i64(&w, exit_code);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "exitSignal");
    if (exit_signal > 0)
        ms_json_writer_i64(&w, exit_signal);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "finishedAt");
    ms_json_writer_i64(&w, (long long)time(NULL));
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    if (text)
        (void)write_atomic(path, text);
    free(text);
    free(path);
}

static char* latest_session_log(const char* home, const char* id) {
    char *root = emulator_root(home), *logs = root ? join_path(root, "logs") : NULL;
    DIR* d = logs ? opendir(logs) : NULL;
    struct dirent* entry;
    char* result = NULL;
    time_t latest = 0;
    size_t prefix_length = strlen(id);
    if (d) {
        while ((entry = readdir(d))) {
            size_t n = strlen(entry->d_name);
            if (n <= prefix_length + 5 || strncmp(entry->d_name, id, prefix_length) ||
                entry->d_name[prefix_length] != '-' || strcmp(entry->d_name + n - 4, ".log"))
                continue;
            char* candidate = join_path(logs, entry->d_name);
            struct stat st;
            if (candidate && stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && (!result || st.st_mtime >= latest)) {
                free(result);
                result = candidate;
                latest = st.st_mtime;
            } else
                free(candidate);
        }
        closedir(d);
    }
    free(root);
    free(logs);
    return result;
}

static void last_exit_status(const char* home, const char* id, int* code, int* signal_value) {
    char* path = last_session_path(home, id);
    char* text = path ? read_file(path, 64 * 1024, NULL) : NULL;
    *code = -1;
    *signal_value = 0;
    if (text) {
        char error[128];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        long long value;
        if (ms_json_as_i64(ms_json_object_get(json, "exitCode"), &value) && value >= 0 && value <= INT32_MAX)
            *code = (int)value;
        if (ms_json_as_i64(ms_json_object_get(json, "exitSignal"), &value) && value > 0 && value <= INT32_MAX)
            *signal_value = (int)value;
        ms_json_free(json);
    }
    free(path);
    free(text);
}

static long process_elapsed_seconds(pid_t pid) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "etime=", NULL};
    char* output = run_capture(argv, 4096);
    if (!output)
        return -1;
    char* text = output;
    while (*text == ' ' || *text == '\t')
        ++text;
    char* newline = strchr(text, '\n');
    if (newline)
        *newline = '\0';
    long days = 0, hours = 0, minutes = 0, seconds = 0;
    char* dash = strchr(text, '-');
    if (dash) {
        *dash = '\0';
        days = strtol(text, NULL, 10);
        text = dash + 1;
    }
    int fields = sscanf(text, "%ld:%ld:%ld", &hours, &minutes, &seconds);
    if (fields == 2) {
        seconds = minutes;
        minutes = hours;
        hours = 0;
    } else if (fields != 3) {
        free(output);
        return -1;
    }
    free(output);
    return days * 86400L + hours * 3600L + minutes * 60L + seconds;
}

static bool process_matches_shadps4(pid_t pid, const char* executable, time_t recorded_start) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "command=", NULL};
    char* output = run_capture(argv, 64 * 1024);
    char* command = output;
    while (command && (*command == ' ' || *command == '\t'))
        ++command;
    size_t executable_length = executable ? strlen(executable) : 0;
    bool command_matches = command && executable_length > 0 && !strncmp(command, executable, executable_length) &&
                           (command[executable_length] == '\0' || command[executable_length] == ' ' ||
                            command[executable_length] == '\t' || command[executable_length] == '\n');
    free(output);
    long elapsed = process_elapsed_seconds(pid);
    time_t estimated_start = elapsed >= 0 ? time(NULL) - elapsed : 0;
    long long delta = estimated_start > recorded_start ? (long long)(estimated_start - recorded_start)
                                                       : (long long)(recorded_start - estimated_start);
    return command_matches && elapsed >= 0 && recorded_start > 0 && delta <= 10;
}

static bool process_is_shadps4(pid_t pid) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "command=", NULL};
    char* command = run_capture(argv, 64 * 1024);
    bool ok = command && (strstr(command, "/shadps4") || strstr(command, "SHADPS4.app"));
    free(command);
    return ok;
}

static pid_t session_pid(const char* home, const char* id) {
    char* path = session_path(home, id);
    char* text = path ? read_file(path, 64 * 1024, NULL) : NULL;
    pid_t pid = 0;
    if (text) {
        char error[128];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        long long value = 0, started_at = 0;
        (void)ms_json_as_i64(ms_json_object_get(json, "startedAt"), &started_at);
        char* log_path = json_string(json, "logPath");
        char* executable = json_string(json, "executable");
        if (ms_json_as_i64(ms_json_object_get(json, "pid"), &value) && value > 0 && value <= INT32_MAX) {
            int status = 0;
            pid_t candidate = (pid_t)value;
            pid_t waited = waitpid(candidate, &status, WNOHANG);
            if (waited == 0 && kill(candidate, 0) == 0 &&
                process_matches_shadps4(candidate, executable, (time_t)started_at))
                pid = candidate;
            else if (waited < 0 && errno == ECHILD && kill(candidate, 0) == 0 &&
                     process_matches_shadps4(candidate, executable, (time_t)started_at))
                pid = candidate;
            else if (waited == candidate) {
                if (log_path) {
                    FILE* log = fopen(log_path, "ab");
                    if (log) {
                        if (WIFEXITED(status))
                            fprintf(log, "\nMetalSharp: shadPS4 exited with status %d\n", WEXITSTATUS(status));
                        else if (WIFSIGNALED(status))
                            fprintf(log, "\nMetalSharp: shadPS4 exited from signal %d\n", WTERMSIG(status));
                        fclose(log);
                    }
                }
                save_exit_record(home, id, executable, log_path, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                                 WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            } else if (waited < 0 && errno == ECHILD && kill(candidate, 0) != 0) {
                save_exit_record(home, id, executable, log_path, -1, 0);
            }
        }
        free(log_path);
        free(executable);
        ms_json_free(json);
    }
    if (!pid && path)
        unlink(path);
    free(path);
    free(text);
    return pid;
}

static bool save_session(const char* home, const char* id, pid_t pid, const char* executable, const char* game_path,
                         const char* runtime_tag, const char* log) {
    char* path = session_path(home, id);
    ms_json_writer w;
    bool ok;
    if (!path)
        return false;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_i64(&w, pid);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, id);
    ms_json_writer_key(&w, "executable");
    ms_json_writer_string(&w, executable);
    ms_json_writer_key(&w, "gamePath");
    ms_json_writer_string(&w, game_path);
    ms_json_writer_key(&w, "runtimeTag");
    if (runtime_tag)
        ms_json_writer_string(&w, runtime_tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "logPath");
    ms_json_writer_string(&w, log);
    ms_json_writer_key(&w, "startedAt");
    ms_json_writer_i64(&w, (long long)time(NULL));
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static char* spawn_shadps4(const char* home, const char* id, const char* target, bool fullscreen) {
    char *exe = executable_path(home), *root = emulator_root(home),
         *isolated_home = root ? join_path(root, "home") : NULL;
    char *logs = root ? join_path(root, "logs") : NULL, *version_dir = root ? join_path(root, "current") : NULL;
    char* icd = version_dir ? join_path(version_dir, "kosmickrisp_mesa_icd.json") : NULL;
    char* log = NULL;
    char log_name[192];
    pid_t pid;
    if (!target || !exe || access(exe, X_OK) != 0 || !icd || access(icd, R_OK) != 0) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(icd);
        return error_json("shadPS4 is not installed or the game target is invalid");
    }
    if (!ensure_environment(home)) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(icd);
        return error_json("failed to prepare the isolated shadPS4 environment");
    }
    if (session_pid(home, id) > 0) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(icd);
        return error_json("this shadPS4 game is already running");
    }
    snprintf(log_name, sizeof(log_name), "%s-%lld.log", id, (long long)time(NULL));
    log = logs ? join_path(logs, log_name) : NULL;
    if (!log) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(icd);
        return error_json("failed to prepare shadPS4 logging");
    }
    pid = fork();
    if (pid < 0) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(icd);
        free(log);
        return error_json("failed to start shadPS4");
    }
    if (pid == 0) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        setpgid(0, 0);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        setenv("HOME", isolated_home, 1);
        setenv("VK_DRIVER_FILES", icd, 1);
        if (chdir(version_dir) != 0)
            _exit(126);
        execl("/usr/bin/arch", "/usr/bin/arch", "-x86_64", exe, "--fullscreen", fullscreen ? "true" : "false",
              "--config-global", "-g", target, (char*)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    char* runtime_tag = current_tag(home);
    if (!save_session(home, id, pid, exe, target, runtime_tag, log)) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(icd);
        free(log);
        free(runtime_tag);
        return error_json("failed to persist shadPS4 process supervision state");
    }
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_i64(&w, pid);
    ms_json_writer_key(&w, "logPath");
    ms_json_writer_string(&w, log);
    ms_json_writer_object_end(&w);
    char* result = ms_json_writer_take(&w);
    free(exe);
    free(root);
    free(isolated_home);
    free(logs);
    free(version_dir);
    free(icd);
    free(log);
    free(runtime_tag);
    return result;
}

static bool any_session_running(const char* home) {
    char *root = emulator_root(home), *sessions = root ? join_path(root, "sessions") : NULL;
    DIR* d = sessions ? opendir(sessions) : NULL;
    struct dirent* entry;
    bool running = false;
    if (d) {
        while ((entry = readdir(d))) {
            size_t n = strlen(entry->d_name);
            if (n <= 5 || strcmp(entry->d_name + n - 5, ".json") ||
                (n >= 10 && !strcmp(entry->d_name + n - 10, ".last.json")))
                continue;
            char id[128];
            if (n - 5 >= sizeof(id))
                continue;
            memcpy(id, entry->d_name, n - 5);
            id[n - 5] = '\0';
            if (session_pid(home, id) > 0) {
                running = true;
                break;
            }
        }
        closedir(d);
    }
    free(root);
    free(sessions);
    return running;
}

static char* stop_session(const char* home, const char* id) {
    pid_t pid = session_pid(home, id);
    char* path = session_path(home, id);
    if (!pid) {
        free(path);
        return error_json("SHADPS4 session is not running");
    }
    if (!process_is_shadps4(pid)) {
        free(path);
        return error_json("refusing to stop a process that is not SHADPS4");
    }
    (void)kill(-pid, SIGTERM);
    (void)kill(pid, SIGTERM);
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD && kill(pid, 0) != 0))
            break;
        struct timespec pause = {0, 50 * 1000 * 1000};
        nanosleep(&pause, NULL);
        if (attempt == 19) {
            (void)kill(-pid, SIGKILL);
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
        }
    }
    save_exit_record(home, id, NULL, NULL, -1, SIGTERM);
    if (path)
        unlink(path);
    free(path);
    return strdup("{\"ok\":true,\"running\":false}");
}

static size_t count_regular_files(const char* path, const char* extension, unsigned depth) {
    DIR* d;
    struct dirent* entry;
    size_t count = 0;
    if (depth > 3 || !(d = opendir(path)))
        return 0;
    while ((entry = readdir(d))) {
        struct stat st;
        char* child;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = join_path(path, entry->d_name);
        if (!child)
            continue;
        if (lstat(child, &st) == 0 && S_ISREG(st.st_mode) && (!extension || suffix_case(entry->d_name, extension)))
            count++;
        else if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
            count += count_regular_files(child, extension, depth + 1);
        free(child);
        if (count > 10000)
            break;
    }
    closedir(d);
    return count;
}

char* ms_shadps4_status_json(const char* home) {
    char *root, *exe, *tag, *data, *cache, *previous_link, *modules, *fonts;
    char previous[4096];
    ssize_t previous_len;
    ms_json_writer w;
    if (!ensure_environment(home))
        return error_json("failed to create the isolated shadPS4 environment");
    root = emulator_root(home);
    exe = executable_path(home);
    tag = current_tag(home);
    data = root ? join_path(root, "home/Library/Application Support/shadPS4") : NULL;
    cache = data ? join_path(data, "cache") : NULL;
    modules = data ? join_path(data, "sys_modules") : NULL;
    fonts = data ? join_path(data, "fonts") : NULL;
    previous_link = root ? join_path(root, "previous") : NULL;
    previous_len = previous_link ? readlink(previous_link, previous, sizeof(previous) - 1) : -1;
    if (previous_len > 0)
        previous[previous_len] = '\0';
    bool arm_host = !strcmp(machine_arch(), "arm64");
    int host_macos = host_macos_major();
    bool rosetta = arm_host && rosetta_available();
    bool supported = arm_host && host_macos >= 26 && rosetta;
    const char* reason = !arm_host         ? "intel_mac"
                         : host_macos < 26 ? "macos_too_old"
                         : !rosetta        ? "rosetta_missing"
                                           : NULL;
    bool installed = exe && access(exe, X_OK) == 0;
    if (installed && root && tag) {
        char* version_path = join_path(root, "current");
        char* capability_path = version_path ? join_path(version_path, "capabilities.json") : NULL;
        if (capability_path && access(capability_path, R_OK) != 0)
            (void)write_capability_manifest(version_path, tag);
        free(version_path);
        free(capability_path);
    }
    unsigned long long memory_bytes = host_sysctl_u64("hw.memsize");
    unsigned long long logical_cpu = host_sysctl_u64("hw.logicalcpu");
    size_t module_count = modules ? count_regular_files(modules, ".sprx", 0) : 0;
    size_t font_count = fonts ? count_regular_files(fonts, NULL, 0) : 0;
    char* roots[SHADPS4_MAX_ROOTS] = {0};
    size_t root_count = load_roots(home, roots);
    for (size_t i = 0; i < root_count; ++i)
        free(roots[i]);
    const char* state = !supported                  ? "unsupported_host"
                        : !installed                ? "missing_runtime"
                        : any_session_running(home) ? "running"
                        : root_count == 0           ? "no_game_folders"
                                                    : "ready";
    int runtime_macos = installed ? macho_minimum_macos(exe) : -1;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "shadps4");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "shadPS4");
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, "PlayStation 4");
    ms_json_writer_key(&w, "experimental");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "supported");
    ms_json_writer_bool(&w, supported);
    ms_json_writer_key(&w, "unsupportedReason");
    if (reason)
        ms_json_writer_string(&w, reason);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, installed);
    ms_json_writer_key(&w, "state");
    ms_json_writer_string(&w, state);
    ms_json_writer_key(&w, "hostArchitecture");
    ms_json_writer_string(&w, machine_arch());
    ms_json_writer_key(&w, "runtimeArchitecture");
    ms_json_writer_string(&w, "x86_64");
    ms_json_writer_key(&w, "rosettaAvailable");
    ms_json_writer_bool(&w, rosetta);
    ms_json_writer_key(&w, "hostMacosMajor");
    ms_json_writer_i64(&w, host_macos);
    ms_json_writer_key(&w, "hostMemoryBytes");
    ms_json_writer_u64(&w, memory_bytes);
    ms_json_writer_key(&w, "hostLogicalCpu");
    ms_json_writer_u64(&w, logical_cpu);
    ms_json_writer_key(&w, "warnings");
    ms_json_writer_array_begin(&w);
    if (memory_bytes > 0 && memory_bytes < 8ULL * 1024ULL * 1024ULL * 1024ULL)
        ms_json_writer_string(&w, "low_memory");
    if (logical_cpu > 0 && logical_cpu < 6)
        ms_json_writer_string(&w, "low_cpu_threads");
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "runtimeMinimumMacos");
    if (runtime_macos > 0)
        ms_json_writer_i64(&w, runtime_macos);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "currentTag");
    if (tag)
        ms_json_writer_string(&w, tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "rollbackAvailable");
    ms_json_writer_bool(&w, previous_len > 0);
    ms_json_writer_key(&w, "moduleCount");
    ms_json_writer_u64(&w, module_count);
    ms_json_writer_key(&w, "modulesReady");
    ms_json_writer_bool(&w, module_count >= SHADPS4_MIN_MODULES);
    ms_json_writer_key(&w, "fontFileCount");
    ms_json_writer_u64(&w, font_count);
    ms_json_writer_key(&w, "fontsReady");
    ms_json_writer_bool(&w, font_count > 0);
    ms_json_writer_key(&w, "gameRootCount");
    ms_json_writer_u64(&w, root_count);
    ms_json_writer_key(&w, "environmentPath");
    ms_json_writer_string(&w, root ? root : "");
    ms_json_writer_key(&w, "dataPath");
    ms_json_writer_string(&w, data ? data : "");
    ms_json_writer_key(&w, "cachePath");
    ms_json_writer_string(&w, cache ? cache : "");
    ms_json_writer_key(&w, "executablePath");
    if (installed)
        ms_json_writer_string(&w, exe);
    else
        ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    free(root);
    free(exe);
    free(tag);
    free(data);
    free(cache);
    free(modules);
    free(fonts);
    free(previous_link);
    return ms_json_writer_take(&w);
}

char* ms_shadps4_games_json(const char* home) {
    shadps4_games* games = calloc(1, sizeof(*games));
    char* roots[SHADPS4_MAX_ROOTS] = {0};
    size_t root_count = load_roots(home, roots);
    ms_json_writer w;
    if (!games)
        return error_json("failed to allocate the SHADPS4 game index");
    collect_games(home, games);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "shadps4");
    ms_json_writer_key(&w, "roots");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < root_count; ++i) {
        ms_json_writer_string(&w, roots[i]);
        free(roots[i]);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "games");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < games->count; ++i) {
        shadps4_game* game = &games->items[i];
        pid_t pid = session_pid(home, game->id);
        char* last_log = latest_session_log(home, game->id);
        int last_exit_code, last_exit_signal;
        last_exit_status(home, game->id, &last_exit_code, &last_exit_signal);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_string(&w, game->id);
        ms_json_writer_key(&w, "titleId");
        ms_json_writer_string(&w, game->title_id);
        ms_json_writer_key(&w, "title");
        ms_json_writer_string(&w, game->title);
        ms_json_writer_key(&w, "version");
        ms_json_writer_string(&w, game->version);
        ms_json_writer_key(&w, "category");
        ms_json_writer_string(&w, game->category);
        ms_json_writer_key(&w, "path");
        ms_json_writer_string(&w, game->path);
        ms_json_writer_key(&w, "installedTitle");
        ms_json_writer_bool(&w, game->installed);
        ms_json_writer_key(&w, "hasUpdate");
        ms_json_writer_bool(&w, game->has_update);
        ms_json_writer_key(&w, "hasArtwork");
        ms_json_writer_bool(&w, game->icon_path[0] != '\0');
        ms_json_writer_key(&w, "running");
        ms_json_writer_bool(&w, pid > 0);
        ms_json_writer_key(&w, "pid");
        if (pid > 0)
            ms_json_writer_i64(&w, pid);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "lastLogPath");
        if (last_log)
            ms_json_writer_string(&w, last_log);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "lastExitCode");
        if (last_exit_code >= 0)
            ms_json_writer_i64(&w, last_exit_code);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "lastExitSignal");
        if (last_exit_signal > 0)
            ms_json_writer_i64(&w, last_exit_signal);
        else
            ms_json_writer_null(&w);
        ms_json_writer_object_end(&w);
        free(last_log);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    char* result = ms_json_writer_take(&w);
    free(games);
    return result;
}

char* ms_shadps4_cover_path(const char* home, const char* id) {
    shadps4_games* games = calloc(1, sizeof(*games));
    char* result = NULL;
    if (!games)
        return NULL;
    collect_games(home, games);
    for (size_t i = 0; i < games->count; ++i)
        if (!strcmp(games->items[i].id, id) && games->items[i].icon_path[0]) {
            result = strdup(games->items[i].icon_path);
            break;
        }
    free(games);
    return result;
}

char* ms_shadps4_update_json(const char* home, const char* action) {
    if (!strcmp(action, "check"))
        return release_json(home, false);
    if (!strcmp(action, "refresh"))
        return release_json(home, true);
    if (!strcmp(action, "progress"))
        return update_progress_json();
    if (!strcmp(action, "install"))
        return start_update(home);
    if (!strcmp(action, "rollback"))
        return rollback_update(home);
    return error_json("unknown SHADPS4 update action");
}

static bool copy_regular_atomic(const char* source, const char* destination, size_t max_size) {
    struct stat st;
    int in_fd = -1, out_fd = -1;
    bool ok = false;
    char* temp = NULL;
    if (lstat(source, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || st.st_size < 0 ||
        (unsigned long long)st.st_size > max_size)
        return false;
    size_t n = strlen(destination) + 32;
    temp = malloc(n);
    if (!temp)
        return false;
    snprintf(temp, n, "%s.tmp.%ld", destination, (long)getpid());
    in_fd = open(source, O_RDONLY | O_NOFOLLOW);
    out_fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (in_fd < 0 || out_fd < 0)
        goto done;
    unsigned char buffer[64 * 1024];
    for (;;) {
        ssize_t got = read(in_fd, buffer, sizeof(buffer));
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        size_t offset = 0;
        while (offset < (size_t)got) {
            ssize_t wrote = write(out_fd, buffer + offset, (size_t)got - offset);
            if (wrote < 0) {
                if (errno == EINTR)
                    continue;
                goto done;
            }
            offset += (size_t)wrote;
        }
    }
    if (fsync(out_fd) != 0 || close(out_fd) != 0) {
        out_fd = -1;
        goto done;
    }
    out_fd = -1;
    ok = rename(temp, destination) == 0;
done:
    if (in_fd >= 0)
        close(in_fd);
    if (out_fd >= 0)
        close(out_fd);
    if (!ok && temp)
        unlink(temp);
    free(temp);
    return ok;
}

static bool supported_module_name(const char* name) {
    static const char* names[] = {
        "libSceAt9Enc.sprx",
        "libSceAudiodec.sprx",
        "libSceAudiodecCpu.sprx",
        "libSceAudiodecCpuDdp.sprx",
        "libSceAudiodecCpuDtsHdLbr.sprx",
        "libSceAudiodecCpuHevag.sprx",
        "libSceAudiodecCpuM4aac.sprx",
        "libSceAvPlayer.sprx",
        "libSceAvPlayerStreaming.sprx",
        "libSceBeisobmf.sprx",
        "libSceBemp2sys.sprx",
        "libSceCesCs.sprx",
        "libSceFont.sprx",
        "libSceFontFt.sprx",
        "libSceFreeTypeOl.sprx",
        "libSceFreeTypeOptOl.sprx",
        "libSceFreeTypeOt.sprx",
        "libSceJpegDec.sprx",
        "libSceJpegEnc.sprx",
        "libSceJson.sprx",
        "libSceJson2.sprx",
        "libSceLibcInternal.sprx",
        "libSceNgs2.sprx",
        "libScePngEnc.sprx",
        "libScePsmKitSystem.sprx",
        "libSceRtc.sprx",
        "libSceRudp.sprx",
        "libSceSystemGesture.sprx",
        "libSceUlt.sprx",
        "libSceWkFontConfig.sprx",
        "libSceXml.sprx",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(name, names[i]))
            return true;
    return false;
}

static char* import_modules(const char* home, const char* source) {
    struct stat st;
    DIR* d;
    struct dirent* entry;
    char *root = emulator_root(home),
         *target = root ? join_path(root, "home/Library/Application Support/shadPS4/sys_modules") : NULL;
    size_t imported = 0, rejected = 0;
    if (!source || lstat(source, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || !target ||
        !mkdir_p(target) || !(d = opendir(source))) {
        free(root);
        free(target);
        return error_json("select a valid directory containing console-dumped shadPS4 modules");
    }
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *from = join_path(source, entry->d_name), *to = join_path(target, entry->d_name);
        unsigned char magic[4] = {0};
        FILE* file = from ? fopen(from, "rb") : NULL;
        bool elf = file && fread(magic, 1, sizeof(magic), file) == sizeof(magic) && magic[0] == 0x7f &&
                   magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
        if (file)
            fclose(file);
        if (from && to && supported_module_name(entry->d_name) && elf &&
            copy_regular_atomic(from, to, 128ULL * 1024ULL * 1024ULL))
            imported++;
        else
            rejected++;
        free(from);
        free(to);
    }
    closedir(d);
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, imported > 0);
    ms_json_writer_key(&w, "imported");
    ms_json_writer_u64(&w, imported);
    ms_json_writer_key(&w, "rejected");
    ms_json_writer_u64(&w, rejected);
    if (!imported) {
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, "no supported decrypted PS4 firmware modules were found");
    }
    ms_json_writer_object_end(&w);
    free(root);
    free(target);
    return ms_json_writer_take(&w);
}

static bool copy_font_tree(const char* source, const char* target, unsigned depth, size_t* files,
                           unsigned long long* bytes) {
    DIR* d;
    struct dirent* entry;
    struct stat st;
    if (depth > 8 || !mkdir_p(target) || !(d = opendir(source)))
        return false;
    while ((entry = readdir(d))) {
        char *from, *to;
        bool ok;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        from = join_path(source, entry->d_name);
        to = join_path(target, entry->d_name);
        if (!from || !to || lstat(from, &st) != 0 || S_ISLNK(st.st_mode)) {
            free(from);
            free(to);
            closedir(d);
            return false;
        }
        if (S_ISDIR(st.st_mode))
            ok = copy_font_tree(from, to, depth + 1, files, bytes);
        else if (S_ISREG(st.st_mode) && st.st_size >= 0 && ++(*files) <= 10000 &&
                 (*bytes += (unsigned long long)st.st_size) <= 1024ULL * 1024ULL * 1024ULL)
            ok = copy_regular_atomic(from, to, 128ULL * 1024ULL * 1024ULL);
        else
            ok = false;
        free(from);
        free(to);
        if (!ok) {
            closedir(d);
            return false;
        }
    }
    closedir(d);
    return true;
}

static char* import_fonts(const char* home, const char* source) {
    struct stat st;
    char *root = emulator_root(home),
         *state = root ? join_path(root, "home/Library/Application Support/shadPS4") : NULL;
    char *staging = root ? join_path(root, "staging") : NULL,
         *stage = staging ? join_path(staging, "fonts-import") : NULL;
    char *target = state ? join_path(state, "fonts") : NULL,
         *backup = state ? join_path(state, "fonts.previous") : NULL;
    size_t files = 0;
    unsigned long long bytes = 0;
    bool ok = source && lstat(source, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && stage && target &&
              backup && mkdir_p(staging);
    if (ok) {
        (void)remove_tree(stage);
        ok = copy_font_tree(source, stage, 0, &files, &bytes) && files > 0;
    }
    if (ok) {
        (void)remove_tree(backup);
        if (access(target, F_OK) == 0)
            ok = rename(target, backup) == 0;
        if (ok)
            ok = rename(stage, target) == 0;
        if (!ok && access(backup, F_OK) == 0)
            (void)rename(backup, target);
        else if (ok)
            (void)remove_tree(backup);
    }
    if (!ok && stage)
        (void)remove_tree(stage);
    free(root);
    free(state);
    free(staging);
    free(stage);
    free(target);
    free(backup);
    if (!ok)
        return error_json("font import failed validation; select console-dumped font and font2 content");
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "files");
    ms_json_writer_u64(&w, files);
    ms_json_writer_key(&w, "bytes");
    ms_json_writer_u64(&w, bytes);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static char* remove_runtime(const char* home) {
    char *root = emulator_root(home), *versions, *downloads, *staging, *current, *previous;
    bool ok;
    if (!root)
        return error_json("failed to resolve SHADPS4 environment");
    if (any_session_running(home)) {
        free(root);
        return error_json("stop SHADPS4 before removing its runtime");
    }
    versions = join_path(root, "versions");
    downloads = join_path(root, "downloads");
    staging = join_path(root, "staging");
    current = join_path(root, "current");
    previous = join_path(root, "previous");
    ok = versions && downloads && staging && remove_tree(versions) && remove_tree(downloads) && remove_tree(staging);
    if (current)
        unlink(current);
    if (previous)
        unlink(previous);
    if (ok)
        ok = mkdir_p(versions) && mkdir_p(downloads) && mkdir_p(staging);
    free(root);
    free(versions);
    free(downloads);
    free(staging);
    free(current);
    free(previous);
    return ok ? strdup("{\"ok\":true,\"preservedData\":true}")
              : error_json("failed to remove the managed SHADPS4 runtime");
}

char* ms_shadps4_action_json(const char* home, const char* action, const unsigned char* body, size_t length) {
    ms_json* root = NULL;
    char *id = NULL, *path = NULL, *tag = NULL;
    char* result = NULL;
    if (!strcmp(action, "scan"))
        return ms_shadps4_games_json(home);
    root = parse_body(body, length);
    if (!root)
        return error_json("invalid shadPS4 request body");
    if (!strcmp(action, "remove-runtime")) {
        result = json_bool(root, "confirm", false) ? remove_runtime(home)
                                                   : error_json("runtime removal requires explicit confirmation");
        ms_json_free(root);
        return result;
    }
    id = json_string(root, "id");
    path = json_string(root, "path");
    tag = json_string(root, "tag");
    if (!strcmp(action, "pin-current") || !strcmp(action, "unpin") || !strcmp(action, "skip-update") ||
        !strcmp(action, "clear-skip")) {
        shadps4_update_policy policy;
        load_update_policy(home, &policy);
        bool valid = true;
        if (!strcmp(action, "pin-current")) {
            char* current = current_tag(home);
            if (!current)
                valid = false;
            else {
                free(policy.pinned_tag);
                policy.pinned_tag = current;
                free(policy.skipped_tag);
                policy.skipped_tag = NULL;
            }
        } else if (!strcmp(action, "unpin")) {
            free(policy.pinned_tag);
            policy.pinned_tag = NULL;
        } else if (!strcmp(action, "skip-update")) {
            if (!tag || !is_safe_component(tag))
                valid = false;
            else {
                free(policy.skipped_tag);
                policy.skipped_tag = strdup(tag);
            }
        } else {
            free(policy.skipped_tag);
            policy.skipped_tag = NULL;
        }
        if (!valid)
            result = error_json("a valid installed or update tag is required");
        else if (!save_update_policy(home, &policy))
            result = error_json("failed to save shadPS4 update preferences");
        else
            result = release_json(home, false);
        free_update_policy(&policy);
    } else if (!strcmp(action, "add-root")) {
        char resolved[4096];
        char* roots[SHADPS4_MAX_ROOTS] = {0};
        size_t count = load_roots(home, roots);
        struct stat st;
        char* environment = emulator_root(home);
        bool protected = false;
        if (path && realpath(path, resolved)) {
            protected = !strcmp(resolved, "/") || !strcmp(resolved, "/System") || !strncmp(resolved, "/System/", 8) ||
                        !strcmp(resolved, "/Library") || !strncmp(resolved, "/Library/", 9) ||
                        !strcmp(resolved, "/Applications") || !strncmp(resolved, "/Applications/", 14) ||
                        (environment && !strncmp(resolved, environment, strlen(environment)) &&
                         (resolved[strlen(environment)] == '\0' || resolved[strlen(environment)] == '/'));
        }
        if (!path || !realpath(path, resolved) || lstat(resolved, &st) != 0 || !S_ISDIR(st.st_mode) ||
            S_ISLNK(st.st_mode) || protected)
            result = error_json("a safe existing PS4 game folder is required");
        else {
            bool exists = false;
            for (size_t i = 0; i < count; ++i)
                if (!strcmp(roots[i], resolved))
                    exists = true;
            if (!exists && count >= SHADPS4_MAX_ROOTS)
                result = error_json("the shadPS4 game-folder limit has been reached");
            else {
                if (!exists)
                    roots[count++] = strdup(resolved);
                result = save_roots(home, roots, count) ? ms_shadps4_games_json(home)
                                                        : error_json("failed to save the shadPS4 game folder");
            }
        }
        free(environment);
        for (size_t i = 0; i < count; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "remove-root")) {
        char* roots[SHADPS4_MAX_ROOTS] = {0};
        char resolved[4096];
        const char* requested = path && realpath(path, resolved) ? resolved : path;
        size_t count = load_roots(home, roots), out = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!requested || strcmp(roots[i], requested))
                roots[out++] = roots[i];
            else
                free(roots[i]);
        }
        result = save_roots(home, roots, out) ? ms_shadps4_games_json(home)
                                              : error_json("failed to update shadPS4 game folders");
        for (size_t i = 0; i < out; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "import-modules")) {
        result = import_modules(home, path);
    } else if (!strcmp(action, "import-fonts")) {
        result = import_fonts(home, path);
    } else if (!strcmp(action, "launch")) {
        shadps4_games* games = calloc(1, sizeof(*games));
        const shadps4_game* match = NULL;
        if (games) {
            collect_games(home, games);
            for (size_t i = 0; i < games->count; ++i)
                if (id && !strcmp(games->items[i].id, id))
                    match = &games->items[i];
        }
        if (!match)
            result = error_json(games ? "shadPS4 game was not found in the indexed library"
                                      : "failed to allocate the shadPS4 game index");
        else
            result = spawn_shadps4(home, match->id, match->launch_path, json_bool(root, "fullscreen", true));
        free(games);
    } else if (!strcmp(action, "stop")) {
        result =
            id && is_safe_component(id) ? stop_session(home, id) : error_json("a valid shadPS4 game id is required");
    } else {
        result = error_json("unknown shadPS4 action");
    }
    free(id);
    free(path);
    free(tag);
    ms_json_free(root);
    return result;
}
