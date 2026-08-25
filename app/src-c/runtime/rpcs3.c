#include "metalsharp_backend/rpcs3.h"
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

#define RPCS3_MAX_GAMES   512
#define RPCS3_MAX_ROOTS   32
#define RPCS3_MAX_CAPTURE (8 * 1024 * 1024)
#define RPCS3_ARM_REPO    "RPCS3/rpcs3-binaries-mac-arm64"
#define RPCS3_INTEL_REPO  "RPCS3/rpcs3-binaries-mac"

typedef struct {
    char id[96];
    char title_id[32];
    char title[256];
    char version[48];
    char category[24];
    char path[4096];
    char sfo_path[4096];
    char icon_path[4096];
    bool installed;
} rpcs3_game;

typedef struct {
    rpcs3_game items[RPCS3_MAX_GAMES];
    size_t count;
} rpcs3_games;

typedef struct {
    char* tag;
    char* version;
    char* asset_name;
    char* url;
    char* digest;
    unsigned long long size;
    char* published_at;
} rpcs3_release;

typedef struct {
    pthread_mutex_t mutex;
    bool running;
    int percent;
    char status[32];
    char message[256];
    char error[256];
    char target[160];
    pid_t worker_pid;
} rpcs3_update_state;

static rpcs3_update_state g_update = {PTHREAD_MUTEX_INITIALIZER, false, 0, "idle", "", "", "", 0};

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
    ms_json_writer_string(&w, message ? message : "RPCS3 operation failed");
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

static char* emulator_root(const char* home) {
    return join_path(home, "emulators/rpcs3");
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
    state_home = join_path(root, "home/Library/Application Support/rpcs3");
    manifest = join_path(root, "environment.json");
    ok = versions && downloads && staging && logs && sessions && state_home && mkdir_p(versions) &&
         mkdir_p(downloads) && mkdir_p(staging) && mkdir_p(logs) && mkdir_p(sessions) && mkdir_p(state_home);
    if (ok && manifest && access(manifest, F_OK) != 0)
        ok = write_atomic(
            manifest, "{\"schemaVersion\":1,\"provider\":\"rpcs3\",\"managedRuntime\":true,\"isolatedHome\":true}\n");
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
    const char* override = getenv("METALSHARP_RPCS3_APP");
    char *root, *path;
    if (override && override[0]) {
        struct stat st;
        if (stat(override, &st) == 0 && S_ISDIR(st.st_mode))
            return join_path(override, "Contents/MacOS/rpcs3");
        return strdup(override);
    }
    root = emulator_root(home);
    path = root ? join_path(root, "current/RPCS3.app/Contents/MacOS/rpcs3") : NULL;
    free(root);
    return path;
}

static const char* machine_arch(void) {
    static char result[32];
    struct utsname info;
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

static void release_free(rpcs3_release* release) {
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

static bool load_release(const char* home, rpcs3_release* out, char* error, size_t error_size, bool force) {
    const char* fixture = getenv("METALSHARP_RPCS3_RELEASE_JSON");
    char *text = NULL, parse_error[160];
    char *environment = NULL, *cache = NULL;
    ms_json* root;
    const ms_json* assets;
    const ms_json* asset = NULL;
    if (fixture && fixture[0])
        text = read_file(fixture, RPCS3_MAX_CAPTURE, NULL);
    else {
        struct stat cache_stat;
        environment = home ? emulator_root(home) : NULL;
        cache = environment ? join_path(environment, "release-cache.json") : NULL;
        if (!force && cache && stat(cache, &cache_stat) == 0 && S_ISREG(cache_stat.st_mode) &&
            time(NULL) - cache_stat.st_mtime >= 0 && time(NULL) - cache_stat.st_mtime < 12 * 60 * 60)
            text = read_file(cache, RPCS3_MAX_CAPTURE, NULL);
        const char* repo = !strcmp(machine_arch(), "arm64") ? RPCS3_ARM_REPO : RPCS3_INTEL_REPO;
        char url[256];
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", repo);
        const char* argv[] = {"/usr/bin/curl",    "--fail",     "--silent", "--show-error",
                              "--location",       "--max-time", "20",       "-A",
                              "MetalSharp-RPCS3", url,          NULL};
        if (!text) {
            text = run_capture(argv, RPCS3_MAX_CAPTURE);
            if (text && cache) {
                (void)ensure_environment(home);
                (void)write_atomic(cache, text);
            }
        }
    }
    free(environment);
    free(cache);
    if (!text) {
        snprintf(error, error_size, "failed to fetch the official RPCS3 release");
        return false;
    }
    root = ms_json_parse(text, strlen(text), parse_error, sizeof(parse_error));
    free(text);
    if (!root || ms_json_type_of(root) != MS_JSON_OBJECT) {
        ms_json_free(root);
        snprintf(error, error_size, "failed to parse the RPCS3 release response");
        return false;
    }
    out->tag = release_field(root, "tag_name");
    out->published_at = release_field(root, "published_at");
    assets = ms_json_object_get(root, "assets");
    for (size_t i = 0; i < ms_json_array_length(assets); ++i) {
        const ms_json* candidate = ms_json_array_get(assets, i);
        char* name = release_field(candidate, "name");
        bool match = name && strstr(name, ".7z") &&
                     ((!strcmp(machine_arch(), "arm64") && strstr(name, "macos_aarch64")) ||
                      (strcmp(machine_arch(), "arm64") && strstr(name, "macos") && !strstr(name, "aarch64")));
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
    if (!out->digest) {
        char* body = release_field(root, "body");
        if (body && strlen(body) >= 64) {
            out->digest = strndup(body, 64);
            if (out->digest) {
                char* prefixed = malloc(strlen(out->digest) + 8);
                if (prefixed) {
                    sprintf(prefixed, "sha256:%s", out->digest);
                    free(out->digest);
                    out->digest = prefixed;
                }
            }
        }
        free(body);
    }
    if (out->asset_name) {
        const char* marker = strstr(out->asset_name, "rpcs3-v");
        const char* end = marker ? strstr(marker + 7, "_macos") : NULL;
        if (marker && end && end > marker + 7)
            out->version = strndup(marker + 7, (size_t)(end - (marker + 7)));
    }
    if (!out->version)
        out->version = strdup(out->tag);
    ms_json_free(root);
    if (!out->asset_name || !is_safe_component(out->asset_name) || !out->url || !out->digest || out->size == 0 ||
        strncmp(out->url, "https://github.com/RPCS3/", 25) != 0) {
        release_free(out);
        snprintf(error, error_size, "the RPCS3 release metadata is incomplete or untrusted");
        return false;
    }
    return true;
}

typedef struct {
    char* pinned_tag;
    char* skipped_tag;
} rpcs3_update_policy;

static void load_update_policy(const char* home, rpcs3_update_policy* policy) {
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

static void free_update_policy(rpcs3_update_policy* policy) {
    free(policy->pinned_tag);
    free(policy->skipped_tag);
    memset(policy, 0, sizeof(*policy));
}

static bool save_update_policy(const char* home, const rpcs3_update_policy* policy) {
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
    rpcs3_release release = {0};
    rpcs3_update_policy policy;
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
    ms_json_writer_string(&w, "rpcs3");
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

static char* find_app(const char* root, unsigned depth) {
    DIR* d;
    struct dirent* entry;
    if (depth > 4 || !(d = opendir(root)))
        return NULL;
    while ((entry = readdir(d))) {
        struct stat st;
        char *path, *found;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = join_path(root, entry->d_name);
        if (!path)
            continue;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !strcmp(entry->d_name, "RPCS3.app")) {
            closedir(d);
            return path;
        }
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            found = find_app(path, depth + 1);
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
        if (S_ISLNK(st.st_mode)) {
            char resolved[4096];
            size_t prefix = strlen(resolved_root);
            if (!realpath(child, resolved) || strncmp(resolved, resolved_root, prefix) ||
                (resolved[prefix] && resolved[prefix] != '/')) {
                free(child);
                closedir(d);
                return false;
            }
        } else if (S_ISDIR(st.st_mode) && !symlinks_stay_inside(root, child)) {
            free(child);
            closedir(d);
            return false;
        }
        free(child);
    }
    closedir(d);
    return true;
}

static const char* unar_path(void) {
    const char* override = getenv("METALSHARP_UNAR_BIN");
    if (override && access(override, X_OK) == 0)
        return override;
    const char* paths[] = {"/opt/homebrew/bin/unar", "/usr/local/bin/unar", "/usr/bin/unar"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        if (access(paths[i], X_OK) == 0)
            return paths[i];
    return NULL;
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
        snprintf(error, error_size, "failed to stage the active RPCS3 version");
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
    if (rename(temp, current) != 0) {
        unlink(temp);
        snprintf(error, error_size, "failed to activate the RPCS3 version");
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
    rpcs3_release release;
} update_job;

static bool any_session_running(const char* home);

static void* update_worker(void* raw) {
    update_job* job = raw;
    rpcs3_release* release = &job->release;
    char *root = emulator_root(job->home), *downloads = root ? join_path(root, "downloads") : NULL;
    char *staging = root ? join_path(root, "staging") : NULL, *versions = root ? join_path(root, "versions") : NULL;
    char *archive = NULL, *download = NULL, *stage = NULL, *version_dir = NULL, *app = NULL, *dest_app = NULL,
         *home_dir = NULL;
    char error[256] = "", sha[65];
    struct stat st;
    const char* unar = unar_path();
    bool ok = false;
    if (!root || !downloads || !staging || !versions || !unar) {
        snprintf(error, sizeof(error), "%s",
                 unar ? "failed to prepare RPCS3 paths" : "unar is required to install RPCS3");
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
        snprintf(error, sizeof(error), "failed to create RPCS3 update staging");
        goto done;
    }
    update_set("downloading", 15, "Downloading the official RPCS3 build", NULL);
    unlink(download);
    const char* download_fixture = getenv("METALSHARP_RPCS3_DOWNLOAD_FILE");
    const char* curl_argv[] = {"/usr/bin/curl", "--fail", "--location", "--silent", "--show-error",
                               "--output",      download, release->url, NULL};
    const char* copy_argv[] = {"/bin/cp", download_fixture, download, NULL};
    int download_status =
        download_fixture && download_fixture[0] ? run_wait(copy_argv, NULL, NULL) : run_wait(curl_argv, NULL, NULL);
    if (download_status != 0 || stat(download, &st) != 0 || !S_ISREG(st.st_mode) ||
        (unsigned long long)st.st_size != release->size) {
        snprintf(error, sizeof(error), "RPCS3 download failed or had an unexpected size");
        goto done;
    }
    update_set("verifying", 45, "Verifying RPCS3 SHA-256", NULL);
    if (!file_sha256(download, sha) ||
        strcasecmp(sha, !strncasecmp(release->digest, "sha256:", 7) ? release->digest + 7 : release->digest)) {
        snprintf(error, sizeof(error), "RPCS3 download digest did not match the official release");
        goto done;
    }
    if (rename(download, archive) != 0) {
        snprintf(error, sizeof(error), "failed to finalize the verified RPCS3 download");
        goto done;
    }
    update_set("extracting", 60, "Extracting RPCS3 into isolated staging", NULL);
    const char* extract_argv[] = {unar, "-quiet", "-output-directory", stage, archive, NULL};
    if (run_wait(extract_argv, NULL, NULL) != 0 || !(app = find_app(stage, 0)) || !symlinks_stay_inside(stage, app)) {
        snprintf(error, sizeof(error), "RPCS3 archive extraction failed validation");
        goto done;
    }
    char* exe = join_path(app, "Contents/MacOS/rpcs3");
    if (!exe || access(exe, X_OK) != 0) {
        free(exe);
        snprintf(error, sizeof(error), "RPCS3.app is missing its executable");
        goto done;
    }
    if (access("/usr/bin/lipo", X_OK) == 0) {
        const char* lipo_argv[] = {"/usr/bin/lipo", "-archs", exe, NULL};
        char* archs = run_capture(lipo_argv, 4096);
        const char* required = !strcmp(machine_arch(), "arm64") ? "arm64" : "x86_64";
        if (!archs || !strstr(archs, required)) {
            free(archs);
            free(exe);
            snprintf(error, sizeof(error), "RPCS3.app has the wrong executable architecture");
            goto done;
        }
        free(archs);
    }
    free(exe);
    update_set("validating", 75, "Validating the RPCS3 application signature", NULL);
    const char* codesign_argv[] = {"/usr/bin/codesign", "--verify", "--deep", "--strict", app, NULL};
    if (access("/usr/bin/codesign", X_OK) == 0 && run_wait(codesign_argv, NULL, NULL) != 0) {
        snprintf(error, sizeof(error), "RPCS3.app failed code-signature validation");
        goto done;
    }
    if (access(version_dir, F_OK) == 0 && !remove_tree(version_dir)) {
        snprintf(error, sizeof(error), "failed to replace an incomplete RPCS3 version");
        goto done;
    }
    if (!mkdir_p(version_dir)) {
        snprintf(error, sizeof(error), "failed to create the version directory");
        goto done;
    }
    dest_app = join_path(version_dir, "RPCS3.app");
    if (!dest_app || rename(app, dest_app) != 0) {
        snprintf(error, sizeof(error), "failed to move RPCS3 into the version store");
        goto done;
    }
    while (any_session_running(job->home)) {
        update_set("waiting_for_exit", 85, "RPCS3 update is ready and will activate after RPCS3 exits", NULL);
        sleep(1);
    }
    update_set("activating", 90, "Atomically activating RPCS3", NULL);
    if (!switch_version(root, release->tag, error, sizeof(error)))
        goto done;
    home_dir = join_path(root, "home");
    (void)home_dir;
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
    update_set(ok ? "completed" : "failed", ok ? 100 : 0, ok ? "RPCS3 is ready" : "RPCS3 update failed",
               ok ? NULL : error);
    free(root);
    free(downloads);
    free(staging);
    free(versions);
    free(archive);
    free(download);
    free(stage);
    free(version_dir);
    free(app);
    free(dest_app);
    free(home_dir);
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
    if (!ensure_environment(home))
        return error_json("failed to create the isolated RPCS3 environment");
    pthread_mutex_lock(&g_update.mutex);
    if (g_update.running) {
        pthread_mutex_unlock(&g_update.mutex);
        return error_json("an RPCS3 update is already running");
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
        update_set("failed", 0, "RPCS3 update failed", error[0] ? error : "failed to prepare update");
        return error_json(error[0] ? error : "failed to prepare update");
    }
    installed_tag = current_tag(home);
    if (installed_tag && !strcmp(installed_tag, job->release.tag)) {
        free(installed_tag);
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("idle", 0, "RPCS3 is already up to date", NULL);
        return error_json("RPCS3 is already up to date");
    }
    free(installed_tag);
    pthread_mutex_lock(&g_update.mutex);
    snprintf(g_update.target, sizeof(g_update.target), "%s", job->release.tag);
    pthread_mutex_unlock(&g_update.mutex);
    if (pthread_create(&thread, NULL, update_worker, job) != 0) {
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("failed", 0, "RPCS3 update failed", "failed to start update worker");
        return error_json("failed to start RPCS3 update worker");
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
        return error_json("failed to resolve RPCS3 environment");
    if (any_session_running(home)) {
        free(root);
        return error_json("stop RPCS3 before rolling back");
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
        char* executable = candidate ? join_path(candidate, "RPCS3.app/Contents/MacOS/rpcs3") : NULL;
        if (executable && access(executable, X_OK) == 0)
            ok = switch_version(root, tag, error, sizeof(error));
        free(candidate);
        free(executable);
    }
    free(root);
    free(previous);
    if (!ok)
        return error_json("no valid previous RPCS3 version is available");
    return ms_rpcs3_status_json(home);
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

static bool game_exists(const rpcs3_games* games, const char* sfo) {
    for (size_t i = 0; i < games->count; ++i)
        if (!strcmp(games->items[i].sfo_path, sfo))
            return true;
    return false;
}

static void add_sfo_game(rpcs3_games* games, const char* sfo, bool installed) {
    size_t size = 0;
    unsigned char* data;
    char dir[4096], parent[4096];
    const char* slash;
    rpcs3_game* game;
    if (games->count >= RPCS3_MAX_GAMES || game_exists(games, sfo))
        return;
    data = (unsigned char*)read_file(sfo, 1024 * 1024, &size);
    if (!data)
        return;
    game = &games->items[games->count];
    memset(game, 0, sizeof(*game));
    snprintf(game->sfo_path, sizeof(game->sfo_path), "%s", sfo);
    (void)sfo_value(data, size, "TITLE_ID", game->title_id, sizeof(game->title_id));
    (void)sfo_value(data, size, "TITLE", game->title, sizeof(game->title));
    (void)sfo_value(data, size, "APP_VER", game->version, sizeof(game->version));
    (void)sfo_value(data, size, "CATEGORY", game->category, sizeof(game->category));
    free(data);
    slash = strrchr(sfo, '/');
    if (!slash)
        return;
    size_t dir_len = (size_t)(slash - sfo);
    if (dir_len >= sizeof(dir))
        return;
    memcpy(dir, sfo, dir_len);
    dir[dir_len] = '\0';
    const char* base = strrchr(dir, '/');
    if (base && !strcmp(base + 1, "PS3_GAME")) {
        size_t parent_len = (size_t)(base - dir);
        if (parent_len >= sizeof(parent))
            return;
        memcpy(parent, dir, parent_len);
        parent[parent_len] = '\0';
        snprintf(game->path, sizeof(game->path), "%s", parent);
    } else
        snprintf(game->path, sizeof(game->path), "%s", dir);
    char* icon = join_path(dir, "ICON0.PNG");
    if (icon && access(icon, R_OK) == 0)
        snprintf(game->icon_path, sizeof(game->icon_path), "%s", icon);
    free(icon);
    if (!game->title[0]) {
        const char* name = strrchr(game->path, '/');
        snprintf(game->title, sizeof(game->title), "%s", name ? name + 1 : "PlayStation 3 Game");
    }
    snprintf(game->id, sizeof(game->id), "%s%s%llx", game->title_id[0] ? game->title_id : "ps3",
             game->title_id[0] ? "-" : "-", path_hash(game->path));
    game->installed = installed;
    games->count++;
}

static void scan_directory(rpcs3_games* games, const char* root, unsigned depth, bool installed) {
    DIR* d;
    struct dirent* entry;
    if (depth > 6 || games->count >= RPCS3_MAX_GAMES || !(d = opendir(root)))
        return;
    while ((entry = readdir(d))) {
        char* path;
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = join_path(root, entry->d_name);
        if (!path)
            continue;
        if (!strcmp(entry->d_name, "PARAM.SFO"))
            add_sfo_game(games, path, installed);
        else if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
            scan_directory(games, path, depth + 1, installed);
        free(path);
    }
    closedir(d);
}

static size_t load_roots(const char* home, char* roots[RPCS3_MAX_ROOTS]) {
    char *root = emulator_root(home), *library = root ? join_path(root, "library.json") : NULL;
    char* text = library ? read_file(library, 1024 * 1024, NULL) : NULL;
    size_t count = 0;
    if (text) {
        char error[128];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        const ms_json* array = ms_json_object_get(json, "roots");
        for (size_t i = 0; i < ms_json_array_length(array) && count < RPCS3_MAX_ROOTS; ++i) {
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

static void collect_games(const char* home, rpcs3_games* games) {
    char *root = emulator_root(home),
         *installed = root ? join_path(root, "home/Library/Application Support/rpcs3/dev_hdd0/game") : NULL;
    char* roots[RPCS3_MAX_ROOTS] = {0};
    size_t count;
    memset(games, 0, sizeof(*games));
    if (installed)
        scan_directory(games, installed, 0, true);
    count = load_roots(home, roots);
    for (size_t i = 0; i < count; ++i) {
        scan_directory(games, roots[i], 0, false);
        free(roots[i]);
    }
    free(root);
    free(installed);
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

static bool process_matches_rpcs3(pid_t pid, const char* executable) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "command=", NULL};
    char* command = run_capture(argv, 64 * 1024);
    bool ok = command && executable && executable[0] && strstr(command, executable);
    free(command);
    return ok;
}

static bool process_is_rpcs3(pid_t pid) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "command=", NULL};
    char* command = run_capture(argv, 64 * 1024);
    bool ok = command && (strstr(command, "/rpcs3") || strstr(command, "RPCS3.app"));
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
        long long value = 0;
        char* log_path = json_string(json, "logPath");
        char* executable = json_string(json, "executable");
        if (ms_json_as_i64(ms_json_object_get(json, "pid"), &value) && value > 0 && value <= INT32_MAX) {
            int status = 0;
            pid_t candidate = (pid_t)value;
            pid_t waited = waitpid(candidate, &status, WNOHANG);
            if (waited == 0 && kill(candidate, 0) == 0 && process_matches_rpcs3(candidate, executable))
                pid = candidate;
            else if (waited < 0 && errno == ECHILD && kill(candidate, 0) == 0 &&
                     process_matches_rpcs3(candidate, executable))
                pid = candidate;
            else if (waited == candidate && log_path) {
                FILE* log = fopen(log_path, "ab");
                if (log) {
                    if (WIFEXITED(status))
                        fprintf(log, "\nMetalSharp: RPCS3 exited with status %d\n", WEXITSTATUS(status));
                    else if (WIFSIGNALED(status))
                        fprintf(log, "\nMetalSharp: RPCS3 exited from signal %d\n", WTERMSIG(status));
                    fclose(log);
                }
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

static bool save_session(const char* home, const char* id, pid_t pid, const char* executable, const char* log) {
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

static char* spawn_rpcs3(const char* home, const char* id, const char* target, bool fullscreen,
                         const char* install_option) {
    char *exe = executable_path(home), *root = emulator_root(home),
         *isolated_home = root ? join_path(root, "home") : NULL;
    char *logs = root ? join_path(root, "logs") : NULL, *log = NULL;
    char log_name[192];
    pid_t pid;
    if (!exe || access(exe, X_OK) != 0) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        return error_json("RPCS3 is not installed");
    }
    if (!ensure_environment(home)) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        return error_json("failed to prepare the isolated RPCS3 environment");
    }
    snprintf(log_name, sizeof(log_name), "%s-%lld.log", id, (long long)time(NULL));
    log = logs ? join_path(logs, log_name) : NULL;
    if (!log) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        return error_json("failed to prepare RPCS3 logging");
    }
    pid = fork();
    if (pid < 0) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(log);
        return error_json("failed to start RPCS3");
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
        if (install_option)
            execl(exe, exe, "--headless", install_option, target, (char*)NULL);
        else if (!target)
            execl(exe, exe, (char*)NULL);
        else if (fullscreen)
            execl(exe, exe, "--no-gui", "--fullscreen", target, (char*)NULL);
        else
            execl(exe, exe, "--no-gui", target, (char*)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    if (!save_session(home, id, pid, exe, log)) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(log);
        return error_json("failed to persist RPCS3 process supervision state");
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
    free(log);
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
            if (n <= 5 || strcmp(entry->d_name + n - 5, ".json"))
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
        return error_json("RPCS3 session is not running");
    }
    if (!process_is_rpcs3(pid)) {
        free(path);
        return error_json("refusing to stop a process that is not RPCS3");
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
    if (path)
        unlink(path);
    free(path);
    return strdup("{\"ok\":true,\"running\":false}");
}

char* ms_rpcs3_status_json(const char* home) {
    char *root, *exe, *tag, *data, *cache, *previous_link;
    bool installed, firmware;
    char previous[4096];
    ssize_t previous_len;
    ms_json_writer w;
    if (!ensure_environment(home))
        return error_json("failed to create the isolated RPCS3 environment");
    root = emulator_root(home);
    exe = executable_path(home);
    tag = current_tag(home);
    data = root ? join_path(root, "home/Library/Application Support/rpcs3") : NULL;
    cache = root ? join_path(root, "home/Library/Caches/rpcs3") : NULL;
    previous_link = root ? join_path(root, "previous") : NULL;
    previous_len = previous_link ? readlink(previous_link, previous, sizeof(previous) - 1) : -1;
    if (previous_len > 0)
        previous[previous_len] = '\0';
    installed = exe && access(exe, X_OK) == 0;
    char* flash = data ? join_path(data, "dev_flash/vsh/module/vsh.self") : NULL;
    firmware = flash && access(flash, R_OK) == 0;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "rpcs3");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "RPCS3");
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, "PlayStation 3");
    ms_json_writer_key(&w, "supported");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, installed);
    ms_json_writer_key(&w, "state");
    ms_json_writer_string(&w, installed ? (firmware ? "ready" : "missing_firmware") : "not_installed");
    ms_json_writer_key(&w, "architecture");
    ms_json_writer_string(&w, machine_arch());
    ms_json_writer_key(&w, "currentTag");
    if (tag)
        ms_json_writer_string(&w, tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "rollbackAvailable");
    ms_json_writer_bool(&w, previous_len > 0);
    ms_json_writer_key(&w, "firmwareInstalled");
    ms_json_writer_bool(&w, firmware);
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
    free(previous_link);
    free(flash);
    return ms_json_writer_take(&w);
}

char* ms_rpcs3_games_json(const char* home) {
    rpcs3_games* games = calloc(1, sizeof(*games));
    char* roots[RPCS3_MAX_ROOTS] = {0};
    size_t root_count = load_roots(home, roots);
    ms_json_writer w;
    if (!games)
        return error_json("failed to allocate the RPCS3 game index");
    collect_games(home, games);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "rpcs3");
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
        rpcs3_game* game = &games->items[i];
        pid_t pid = session_pid(home, game->id);
        char* last_log = latest_session_log(home, game->id);
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
        ms_json_writer_object_end(&w);
        free(last_log);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    char* result = ms_json_writer_take(&w);
    free(games);
    return result;
}

char* ms_rpcs3_cover_path(const char* home, const char* id) {
    rpcs3_games* games = calloc(1, sizeof(*games));
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

char* ms_rpcs3_update_json(const char* home, const char* action) {
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
    return error_json("unknown RPCS3 update action");
}

static char* remove_runtime(const char* home) {
    char *root = emulator_root(home), *versions, *downloads, *staging, *current, *previous;
    bool ok;
    if (!root)
        return error_json("failed to resolve RPCS3 environment");
    if (any_session_running(home)) {
        free(root);
        return error_json("stop RPCS3 before removing its runtime");
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
              : error_json("failed to remove the managed RPCS3 runtime");
}

char* ms_rpcs3_action_json(const char* home, const char* action, const unsigned char* body, size_t length) {
    ms_json* root = NULL;
    char *id = NULL, *path = NULL, *tag = NULL;
    char* result = NULL;
    if (!strcmp(action, "scan"))
        return ms_rpcs3_games_json(home);
    if (!strcmp(action, "open-ui"))
        return spawn_rpcs3(home, "ui", NULL, false, NULL);
    if (!strcmp(action, "remove-runtime")) {
        root = parse_body(body, length);
        if (!root || !json_bool(root, "confirm", false))
            result = error_json("runtime removal requires explicit confirmation");
        else
            result = remove_runtime(home);
        ms_json_free(root);
        return result;
    }
    root = parse_body(body, length);
    if (!root)
        return error_json("invalid RPCS3 request body");
    id = json_string(root, "id");
    path = json_string(root, "path");
    tag = json_string(root, "tag");
    if (!strcmp(action, "pin-current") || !strcmp(action, "unpin") || !strcmp(action, "skip-update") ||
        !strcmp(action, "clear-skip")) {
        rpcs3_update_policy policy;
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
            result = error_json("failed to save RPCS3 update preferences");
        else
            result = release_json(home, false);
        free_update_policy(&policy);
    } else if (!strcmp(action, "add-root")) {
        char resolved[4096];
        char* roots[RPCS3_MAX_ROOTS] = {0};
        size_t count = load_roots(home, roots);
        struct stat st;
        if (!path || !realpath(path, resolved) || stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
            result = error_json("a valid game library folder is required");
        else {
            bool exists = false;
            for (size_t i = 0; i < count; ++i)
                if (!strcmp(roots[i], resolved))
                    exists = true;
            if (!exists && count < RPCS3_MAX_ROOTS)
                roots[count++] = strdup(resolved);
            result = save_roots(home, roots, count) ? ms_rpcs3_games_json(home)
                                                    : error_json("failed to save the RPCS3 game folder");
        }
        for (size_t i = 0; i < count; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "remove-root")) {
        char* roots[RPCS3_MAX_ROOTS] = {0};
        char resolved[4096];
        const char* requested = path && realpath(path, resolved) ? resolved : path;
        size_t count = load_roots(home, roots), out = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!requested || strcmp(roots[i], requested))
                roots[out++] = roots[i];
            else
                free(roots[i]);
        }
        result = save_roots(home, roots, out) ? ms_rpcs3_games_json(home)
                                              : error_json("failed to update RPCS3 game folders");
        for (size_t i = 0; i < out; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "launch")) {
        rpcs3_games* games = calloc(1, sizeof(*games));
        const rpcs3_game* match = NULL;
        if (games) {
            collect_games(home, games);
            for (size_t i = 0; i < games->count; ++i)
                if (id && !strcmp(games->items[i].id, id))
                    match = &games->items[i];
        }
        if (!match)
            result = error_json(games ? "RPCS3 game was not found in the indexed library"
                                      : "failed to allocate the RPCS3 game index");
        else
            result = spawn_rpcs3(home, match->id, match->path, json_bool(root, "fullscreen", true), NULL);
        free(games);
    } else if (!strcmp(action, "stop")) {
        result = id && is_safe_component(id) ? stop_session(home, id) : error_json("a valid RPCS3 game id is required");
    } else if (!strcmp(action, "install-firmware") || !strcmp(action, "install-package")) {
        const char* extension = !strcmp(action, "install-firmware") ? ".PUP" : ".pkg";
        const char* option = !strcmp(action, "install-firmware") ? "--installfw" : "--installpkg";
        struct stat st;
        size_t n = path ? strlen(path) : 0, e = strlen(extension);
        bool valid_extension = n >= e && !strcasecmp(path + n - e, extension);
        if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || !valid_extension)
            result = error_json(!strcmp(action, "install-firmware") ? "select a valid PS3UPDAT.PUP file"
                                                                    : "select a valid PS3 .pkg file");
        else
            result = spawn_rpcs3(home, !strcmp(action, "install-firmware") ? "firmware-install" : "package-install",
                                 path, false, option);
    } else
        result = error_json("unknown RPCS3 action");
    free(id);
    free(path);
    free(tag);
    ms_json_free(root);
    return result;
}
