#include "metalsharp_backend/pcsx2.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
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

#define PCSX2_MAX_GAMES        512
#define PCSX2_MAX_ROOTS        32
#define PCSX2_MAX_CAPTURE      (8 * 1024 * 1024)
#define PCSX2_MAX_SCAN_ENTRIES 20000
#define PCSX2_STABLE_REPO      "PCSX2/pcsx2"
#define PCSX2_TEAM_ID          "PTMR35SWS3"

typedef struct {
    char id[96];
    char serial[32];
    char title[256];
    char region[48];
    char format[16];
    char path[4096];
    char icon_path[4096];
    unsigned long long size;
} pcsx2_game;

typedef struct {
    pcsx2_game items[PCSX2_MAX_GAMES];
    size_t count;
    size_t scanned_entries;
    time_t started_at;
    bool truncated;
} pcsx2_games;

typedef struct {
    char* tag;
    char* version;
    char* asset_name;
    char* url;
    char* digest;
    unsigned long long size;
    char* published_at;
} pcsx2_release;

typedef struct {
    pthread_mutex_t mutex;
    bool running;
    int percent;
    char status[32];
    char message[256];
    char error[256];
    char target[160];
    pid_t worker_pid;
} pcsx2_update_state;

static pcsx2_update_state g_update = {PTHREAD_MUTEX_INITIALIZER, false, 0, "idle", "", "", "", 0};

static bool update_running(void) {
    pthread_mutex_lock(&g_update.mutex);
    bool running = g_update.running;
    pthread_mutex_unlock(&g_update.mutex);
    return running;
}

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
    struct stat st;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > limit) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    char* data = malloc((size_t)st.st_size + 1);
    if (!data) {
        close(fd);
        return NULL;
    }
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t got = read(fd, data + used, (size_t)st.st_size - used);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;
        used += (size_t)got;
    }
    close(fd);
    if (used != (size_t)st.st_size) {
        free(data);
        return NULL;
    }
    data[used] = '\0';
    if (length_out)
        *length_out = used;
    return data;
}

static bool write_atomic(const char* path, const char* data) {
    size_t n = strlen(path) + 64;
    char* tmp = malloc(n);
    FILE* f = NULL;
    bool ok;
    struct timespec timestamp = {0};
    if (!tmp)
        return false;
    (void)clock_gettime(CLOCK_REALTIME, &timestamp);
    snprintf(tmp, n, "%s.tmp.%ld.%ld", path, (long)getpid(), timestamp.tv_nsec);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0)
        f = fdopen(fd, "wb");
    if (fd >= 0 && !f)
        close(fd);
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
    ms_json_writer_string(&w, message ? message : "PCSX2 operation failed");
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
    return join_path(home, "emulators/pcsx2");
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
    char *root = emulator_root(home), *versions, *downloads, *staging, *logs, *sessions, *backups;
    char *app_support, *state_home, *manifest;
    bool ok = false;
    if (!root)
        return false;
    versions = join_path(root, "versions");
    downloads = join_path(root, "downloads");
    staging = join_path(root, "staging");
    logs = join_path(root, "logs");
    sessions = join_path(root, "sessions");
    backups = join_path(root, "backups");
    app_support = join_path(root, "home/Library/Application Support");
    state_home = app_support ? join_path(app_support, "PCSX2") : NULL;
    manifest = join_path(root, "environment.json");
    ok = versions && downloads && staging && logs && sessions && backups && app_support && state_home &&
         mkdir_p(versions) && mkdir_p(downloads) && mkdir_p(staging) && mkdir_p(logs) && mkdir_p(sessions) &&
         mkdir_p(backups) && mkdir_p(app_support) && mkdir_p(state_home);
    if (ok) {
        const char* private_paths[] = {root, downloads, staging, logs, sessions, backups, app_support, state_home};
        for (size_t i = 0; i < sizeof(private_paths) / sizeof(private_paths[0]); ++i)
            if (chmod(private_paths[i], 0700) != 0)
                ok = false;
    }
    if (ok)
        cleanup_interrupted_updates(root);
    if (ok && manifest && access(manifest, F_OK) != 0)
        ok = write_atomic(manifest, "{\"schemaVersion\":1,\"provider\":\"pcsx2\",\"managedRuntime\":true,"
                                    "\"isolatedHome\":true,\"channel\":\"stable\"}\n");
    free(root);
    free(versions);
    free(downloads);
    free(staging);
    free(logs);
    free(sessions);
    free(backups);
    free(app_support);
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

static bool valid_stable_tag(const char* tag) {
    if (!tag || tag[0] != 'v')
        return false;
    const char* p = tag + 1;
    for (int component = 0; component < 3; ++component) {
        if (*p < '0' || *p > '9')
            return false;
        while (*p >= '0' && *p <= '9')
            p++;
        if (component < 2) {
            if (*p++ != '.')
                return false;
        }
    }
    return *p == '\0';
}

static char* current_tag(const char* home) {
    char *root = emulator_root(home), *current = root ? join_path(root, "current") : NULL;
    char target[4096];
    ssize_t n = current ? readlink(current, target, sizeof(target) - 1) : -1;
    char* result = NULL;
    if (n > 0) {
        target[n] = '\0';
        const char* prefix = "versions/";
        const char* tag = target + strlen(prefix);
        if (!strncmp(target, prefix, strlen(prefix)) && is_safe_component(tag))
            result = strdup(tag);
    }
    free(root);
    free(current);
    return result;
}

static char* current_version_path(const char* home) {
    char *root = emulator_root(home), *tag = current_tag(home), *versions = root ? join_path(root, "versions") : NULL;
    char* path = versions && tag ? join_path(versions, tag) : NULL;
    free(root);
    free(tag);
    free(versions);
    return path;
}

static char* executable_path(const char* home) {
    char* version = current_version_path(home);
    char* path = version ? join_path(version, "PCSX2.app/Contents/MacOS/PCSX2") : NULL;
    free(version);
    return path;
}

static const char* machine_arch(void) {
    const char* override = getenv("METALSHARP_PCSX2_HOST_ARCH");
    static char result[32];
    struct utsname info;
    if (override && override[0])
        return override;
    if (!result[0]) {
        if (uname(&info) == 0)
            snprintf(result, sizeof(result), "%s", info.machine);
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
        alarm(600);
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static char* run_capture_timeout(const char* const argv[], size_t limit, unsigned timeout_seconds) {
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
        alarm(timeout_seconds);
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

static char* run_capture(const char* const argv[], size_t limit) {
    return run_capture_timeout(argv, limit, 60);
}

static char* run_capture_both(const char* const argv[], size_t limit) {
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
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        alarm(60);
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
        ssize_t got = read(fds[0], data + used, capacity - used - 1);
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

static void release_free(pcsx2_release* release) {
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

static bool valid_sha256_digest(const char* digest) {
    if (!digest || strncmp(digest, "sha256:", 7) || strlen(digest + 7) != 64)
        return false;
    for (const char* p = digest + 7; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            return false;
    return true;
}

static bool load_release(const char* home, pcsx2_release* out, char* error, size_t error_size, bool force) {
    const char* fixture = getenv("METALSHARP_PCSX2_RELEASE_JSON");
    char *text = NULL, parse_error[160];
    char *environment = NULL, *cache = NULL;
    ms_json* root;
    const ms_json* assets;
    const ms_json* asset = NULL;
    if (fixture && fixture[0])
        text = read_file(fixture, PCSX2_MAX_CAPTURE, NULL);
    else {
        struct stat cache_stat;
        environment = home ? emulator_root(home) : NULL;
        cache = environment ? join_path(environment, "release-cache.json") : NULL;
        if (!force && cache && stat(cache, &cache_stat) == 0 && S_ISREG(cache_stat.st_mode) &&
            time(NULL) - cache_stat.st_mtime >= 0 && time(NULL) - cache_stat.st_mtime < 12 * 60 * 60)
            text = read_file(cache, PCSX2_MAX_CAPTURE, NULL);
        char url[256];
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", PCSX2_STABLE_REPO);
        const char* argv[] = {"/usr/bin/curl",    "--fail",     "--silent", "--show-error",
                              "--location",       "--max-time", "20",       "-A",
                              "MetalSharp-PCSX2", url,          NULL};
        if (!text) {
            text = run_capture(argv, PCSX2_MAX_CAPTURE);
            if (text && cache) {
                (void)ensure_environment(home);
                (void)write_atomic(cache, text);
            }
        }
    }
    free(environment);
    free(cache);
    if (!text) {
        snprintf(error, error_size, "failed to fetch the official PCSX2 release");
        return false;
    }
    root = ms_json_parse(text, strlen(text), parse_error, sizeof(parse_error));
    free(text);
    if (!root || ms_json_type_of(root) != MS_JSON_OBJECT) {
        ms_json_free(root);
        snprintf(error, error_size, "failed to parse the PCSX2 release response");
        return false;
    }
    out->tag = release_field(root, "tag_name");
    out->published_at = release_field(root, "published_at");
    bool draft = json_bool(root, "draft", false), prerelease = json_bool(root, "prerelease", false);
    char expected_asset[256];
    snprintf(expected_asset, sizeof(expected_asset), "pcsx2-%s-macos-Qt.tar.xz", out->tag ? out->tag : "");
    assets = ms_json_object_get(root, "assets");
    size_t matches = 0;
    for (size_t i = 0; i < ms_json_array_length(assets); ++i) {
        const ms_json* candidate = ms_json_array_get(assets, i);
        char* name = release_field(candidate, "name");
        if (name && !strcmp(name, expected_asset)) {
            asset = candidate;
            matches++;
        }
        free(name);
    }
    if (!out->tag || !asset || matches != 1 || draft || prerelease || !valid_stable_tag(out->tag)) {
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
    char expected_url[1024];
    snprintf(expected_url, sizeof(expected_url), "https://github.com/PCSX2/pcsx2/releases/download/%s/%s",
             out->tag ? out->tag : "", out->asset_name ? out->asset_name : "");
    if (!out->asset_name || !is_safe_component(out->asset_name) || !out->url || !valid_sha256_digest(out->digest) ||
        out->size == 0 || out->size > 2ULL * 1024ULL * 1024ULL * 1024ULL || strcmp(out->url, expected_url)) {
        release_free(out);
        snprintf(error, error_size, "the PCSX2 release metadata is incomplete or untrusted");
        return false;
    }
    return true;
}

typedef struct {
    char* pinned_tag;
    char* skipped_tag;
} pcsx2_update_policy;

static void load_update_policy(const char* home, pcsx2_update_policy* policy) {
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

static void free_update_policy(pcsx2_update_policy* policy) {
    free(policy->pinned_tag);
    free(policy->skipped_tag);
    memset(policy, 0, sizeof(*policy));
}

static bool save_update_policy(const char* home, const pcsx2_update_policy* policy) {
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
    pcsx2_release release = {0};
    pcsx2_update_policy policy;
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
    ms_json_writer_string(&w, "pcsx2");
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
        (void)chmod(path, 0700);
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

static bool make_tree_read_only(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode))
        return false;
    if (S_ISDIR(st.st_mode)) {
        size_t path_length = strlen(path);
        if (path_length >= 4 && !strcmp(path + path_length - 4, ".app"))
            return true;
        DIR* d = opendir(path);
        struct dirent* entry;
        if (!d)
            return false;
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char* child = join_path(path, entry->d_name);
            bool ok = child && make_tree_read_only(child);
            if (!ok) {
                fprintf(stderr, "PCSX2 failed to freeze runtime entry: %s\n", child ? child : path);
                free(child);
                closedir(d);
                return false;
            }
            free(child);
        }
        closedir(d);
        if (chmod(path, 0555) != 0) {
            fprintf(stderr, "PCSX2 failed to freeze directory %s: %s\n", path, strerror(errno));
            return false;
        }
        return true;
    }
    if (!S_ISREG(st.st_mode))
        return false;
    if (chmod(path, (st.st_mode & 0111) ? 0555 : 0444) != 0) {
        fprintf(stderr, "PCSX2 failed to freeze file %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
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
        if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) ||
            (S_ISREG(st.st_mode) && st.st_nlink != 1)) {
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

static const char* tool_path(const char* name) {
    const char* candidates[3] = {NULL, NULL, NULL};
    if (!strcmp(name, "lsar")) {
        candidates[0] = "/opt/homebrew/bin/lsar";
        candidates[1] = "/usr/local/bin/lsar";
        candidates[2] = "/usr/bin/lsar";
    } else if (!strcmp(name, "unar")) {
        candidates[0] = "/opt/homebrew/bin/unar";
        candidates[1] = "/usr/local/bin/unar";
        candidates[2] = "/usr/bin/unar";
    }
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        if (candidates[i] && access(candidates[i], X_OK) == 0)
            return candidates[i];
    return NULL;
}

static bool archive_entry_safe(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= 4096 || name[0] == '/' || strchr(name, '\\'))
        return false;
    const char* part = name;
    for (size_t i = 0; i <= n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if ((c > 0 && c < 32) || c >= 127)
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

static bool archive_entries_safe(const char* archive, const char* expected_top) {
    const char* lsar = tool_path("lsar");
    if (!lsar)
        return false;
    const char* argv[] = {lsar, "-json", archive, NULL};
    char* listing = run_capture(argv, PCSX2_MAX_CAPTURE);
    char parse_error[160];
    ms_json* root = listing ? ms_json_parse(listing, strlen(listing), parse_error, sizeof(parse_error)) : NULL;
    const ms_json* entries = root ? ms_json_object_get(root, "lsarContents") : NULL;
    size_t count = ms_json_array_length(entries);
    char** names = count > 0 && count <= 20000 ? calloc(count, sizeof(*names)) : NULL;
    bool ok = names != NULL;
    unsigned long long total_size = 0;
    for (size_t i = 0; ok && i < count; ++i) {
        const ms_json* entry = ms_json_array_get(entries, i);
        char* name = json_string(entry, "XADFileName");
        long long size = 0;
        bool special = json_bool(entry, "XADIsLink", false) || json_bool(entry, "XADIsHardLink", false) ||
                       json_bool(entry, "XADIsCharacterDevice", false) || json_bool(entry, "XADIsBlockDevice", false) ||
                       json_bool(entry, "XADIsFIFO", false) || ms_json_object_get(entry, "XADLinkDestination") != NULL;
        const char* slash = name ? strchr(name, '/') : NULL;
        size_t top_length = slash ? (size_t)(slash - name) : name ? strlen(name) : 0;
        if (!name || !archive_entry_safe(name) || special || top_length != strlen(expected_top) ||
            strncmp(name, expected_top, top_length))
            ok = false;
        if (!ms_json_as_i64(ms_json_object_get(entry, "XADFileSize"), &size) || size < 0)
            ok = false;
        else if (size > 0) {
            total_size += (unsigned long long)size;
            if (total_size > 2ULL * 1024ULL * 1024ULL * 1024ULL)
                ok = false;
        }
        for (size_t j = 0; ok && j < i; ++j)
            if (!strcasecmp(names[j], name))
                ok = false;
        if (ok)
            names[i] = name;
        else
            free(name);
    }
    for (size_t i = 0; i < count; ++i)
        free(names ? names[i] : NULL);
    free(names);
    ms_json_free(root);
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
    const char* override = getenv("METALSHARP_PCSX2_HOST_MACOS");
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

static int plist_minimum_macos(const char* app) {
    char* plist = join_path(app, "Contents/Info.plist");
    const char* argv[] = {"/usr/libexec/PlistBuddy", "-c", "Print:LSMinimumSystemVersion", plist, NULL};
    char* output = plist ? run_capture(argv, 4096) : NULL;
    int major = version_major(output);
    free(plist);
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
        const char* dependency_name = strrchr(line, '/');
        const char* binary_name = strrchr(path, '/');
        if (!strncmp(line, "/usr/lib/", 9) || !strncmp(line, "/System/Library/", 16) || !strncmp(line, "@rpath/", 7) ||
            !strncmp(line, "@loader_path/", 13) || !strncmp(line, "@executable_path/", 17) ||
            (line_number == 2 && dependency_name && binary_name && !strcmp(dependency_name + 1, binary_name + 1)))
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

static bool is_macho_file(const char* path) {
    unsigned char bytes[4];
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    ssize_t n = fd >= 0 ? read(fd, bytes, sizeof(bytes)) : -1;
    if (fd >= 0)
        close(fd);
    if (n != 4)
        return false;
    uint32_t value =
        ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
    return value == 0xfeedfaceU || value == 0xfeedfacfU || value == 0xcefaedfeU || value == 0xcffaedfeU ||
           value == 0xcafebabeU || value == 0xbebafecaU;
}

static bool validate_bundle_machos(const char* path, unsigned depth, size_t* count, int* minimum_macos) {
    DIR* directory;
    struct dirent* entry;
    if (depth > 16 || !(directory = opendir(path)))
        return false;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        struct stat st;
        bool ok = child && lstat(child, &st) == 0 && !S_ISLNK(st.st_mode);
        if (ok && S_ISDIR(st.st_mode))
            ok = validate_bundle_machos(child, depth + 1, count, minimum_macos);
        else if (ok && S_ISREG(st.st_mode) && is_macho_file(child)) {
            (*count)++;
            int candidate_macos = macho_minimum_macos(child);
            if (candidate_macos > *minimum_macos)
                *minimum_macos = candidate_macos;
            ok = *count <= 4096 && candidate_macos > 0 && validate_macho_x86_64(child) &&
                 validate_macho_dependencies(child);
        } else if (ok && !S_ISREG(st.st_mode))
            ok = false;
        if (!ok) {
            fprintf(stderr, "PCSX2 validation rejected bundle entry: %s\n", child ? child : path);
            free(child);
            closedir(directory);
            return false;
        }
        free(child);
    }
    closedir(directory);
    return true;
}

static bool signature_test_hook_enabled(void) {
    const char* requested = getenv("METALSHARP_PCSX2_SKIP_SIGNATURE_FOR_TESTS");
    uint32_t size = 0;
    (void)_NSGetExecutablePath(NULL, &size);
    char* executable = size > 0 && size < 64 * 1024 ? malloc(size) : NULL;
    bool development_binary = executable && _NSGetExecutablePath(executable, &size) == 0 &&
                              (strstr(executable, "/src-c/build/") || strstr(executable, "/src-c/build-asan/"));
    free(executable);
    return requested && (!strcmp(requested, "1") || !strcasecmp(requested, "true")) && development_binary &&
           getenv("METALSHARP_PCSX2_RELEASE_JSON") && getenv("METALSHARP_PCSX2_DOWNLOAD_FILE");
}

static bool validate_bundle_identity(const char* app, const char* executable, const char* tag) {
    if (signature_test_hook_enabled())
        return validate_macho_x86_64(executable);
    const char* verify[] = {"/usr/bin/codesign", "--verify", "--deep", "--strict", app, NULL};
    const char* details[] = {"/usr/bin/codesign", "-dv", "--verbose=4", app, NULL};
    const char* assess[] = {"/usr/sbin/spctl", "-a", "--type", "execute", "-vv", app, NULL};
    const char* bundle_id_argv[] = {"/usr/libexec/PlistBuddy", "-c", "Print:CFBundleIdentifier", NULL, NULL};
    const char* version_argv[] = {"/usr/libexec/PlistBuddy", "-c", "Print:CFBundleShortVersionString", NULL, NULL};
    char* plist = join_path(app, "Contents/Info.plist");
    bundle_id_argv[3] = plist;
    version_argv[3] = plist;
    char* signature = run_capture_both(details, 256 * 1024);
    char* bundle_id = plist ? run_capture(bundle_id_argv, 4096) : NULL;
    char* version = plist ? run_capture(version_argv, 4096) : NULL;
    const char* expected_version = tag && tag[0] == 'v' ? tag + 1 : tag;
    if (bundle_id)
        bundle_id[strcspn(bundle_id, "\r\n")] = '\0';
    if (version)
        version[strcspn(version, "\r\n")] = '\0';
    bool version_matches =
        version && tag && expected_version && (!strcmp(version, tag) || !strcmp(version, expected_version));
    bool ok = run_wait(verify, NULL, NULL) == 0 && run_wait(assess, NULL, NULL) == 0 && signature &&
              strstr(signature, "TeamIdentifier=" PCSX2_TEAM_ID) && strstr(signature, "Runtime Version=") &&
              bundle_id && !strcmp(bundle_id, "net.pcsx2.pcsx2") && version_matches;
    free(plist);
    free(signature);
    free(bundle_id);
    free(version);
    return ok;
}

static bool rosetta_available(void) {
    const char* forced = getenv("METALSHARP_PCSX2_ROSETTA");
    if (forced)
        return !strcmp(forced, "1") || !strcasecmp(forced, "true");
    const char* argv[] = {"/usr/bin/arch", "-x86_64", "/usr/bin/true", NULL};
    return run_wait(argv, NULL, NULL) == 0;
}

static bool intel_sse41_available(void) {
    const char* forced = getenv("METALSHARP_PCSX2_SSE41");
    if (forced)
        return !strcmp(forced, "1") || !strcasecmp(forced, "true");
    const char* argv[] = {"/usr/sbin/sysctl", "-n", "machdep.cpu.features", NULL};
    char* features = run_capture(argv, 16 * 1024);
    bool available = features && (strstr(features, "SSE4.1") || strstr(features, "SSE4_1"));
    free(features);
    return available;
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
        snprintf(error, error_size, "failed to stage the active PCSX2 version");
        goto done;
    }
    if (old_len > 0) {
        old[old_len] = '\0';
    }
    if (old_len > 0 && strcmp(old, target)) {
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
    if (getenv("METALSHARP_PCSX2_FAIL_ACTIVATION")) {
        unlink(temp);
        snprintf(error, error_size, "PCSX2 activation was interrupted by the validation hook");
        goto done;
    }
    if (rename(temp, current) != 0) {
        unlink(temp);
        snprintf(error, error_size, "failed to activate the PCSX2 version");
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
    pcsx2_release release;
} update_job;

static bool any_session_running(const char* home);
static bool active_runtime_valid(const char* home);
static bool disable_upstream_updater(const char* home);
static bool sync_game_list_roots(const char* home);

static bool write_source_manifest(const char* version_dir, const pcsx2_release* release) {
    char* path = join_path(version_dir, "source.json");
    ms_json_writer w;
    if (!path)
        return false;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "pcsx2");
    ms_json_writer_key(&w, "repository");
    ms_json_writer_string(&w, PCSX2_STABLE_REPO);
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
    ms_json_writer_key(&w, "teamIdentifier");
    ms_json_writer_string(&w, PCSX2_TEAM_ID);
    ms_json_writer_key(&w, "upstreamSignaturePreserved");
    ms_json_writer_bool(&w, true);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    bool ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static bool write_capability_manifest(const char* version_dir, const char* tag, bool data_path, int minimum_macos) {
    char* path = join_path(version_dir, "capabilities.json");
    if (!path)
        return false;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schemaVersion");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "pcsx2");
    ms_json_writer_key(&w, "runtimeTag");
    ms_json_writer_string(&w, tag);
    ms_json_writer_key(&w, "runtimeArchitecture");
    ms_json_writer_string(&w, "x86_64");
    ms_json_writer_key(&w, "minimumMacOS");
    char minimum[32];
    snprintf(minimum, sizeof(minimum), "%d.0", minimum_macos > 0 ? minimum_macos : 11);
    ms_json_writer_string(&w, minimum);
    ms_json_writer_key(&w, "bundleIdentifier");
    ms_json_writer_string(&w, "net.pcsx2.pcsx2");
    ms_json_writer_key(&w, "teamIdentifier");
    ms_json_writer_string(&w, PCSX2_TEAM_ID);
    ms_json_writer_key(&w, "dataIsolation");
    ms_json_writer_string(&w, "home");
    ms_json_writer_key(&w, "dataPathFlag");
    ms_json_writer_bool(&w, data_path);
    ms_json_writer_key(&w, "cli");
    ms_json_writer_array_begin(&w);
    const char* cli[] = {"-batch", "-nogui", "-logfile", "-testconfig", "-setupwizard", "--"};
    for (size_t i = 0; i < sizeof(cli) / sizeof(cli[0]); ++i)
        ms_json_writer_string(&w, cli[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "content");
    ms_json_writer_array_begin(&w);
    const char* formats[] = {"iso", "bin", "img", "mdf", "gz", "cso", "zso", "chd", "elf"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
        ms_json_writer_string(&w, formats[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    bool ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static bool run_runtime_probe(const char* executable, const char* working_dir, const char* isolated_home,
                              const char* argument, char* output, size_t output_size) {
    int fds[2], status = 0;
    pid_t pid;
    size_t used = 0;
    if (!output || output_size < 2 || pipe(fds) != 0)
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
        if (chdir(working_dir) != 0)
            _exit(126);
        setenv("HOME", isolated_home, 1);
        alarm(15);
        if (!strcmp(machine_arch(), "arm64"))
            execl("/usr/bin/arch", "/usr/bin/arch", "-x86_64", executable, argument, (char*)NULL);
        else
            execl(executable, executable, argument, (char*)NULL);
        _exit(127);
    }
    close(fds[1]);
    while (used + 1 < output_size) {
        ssize_t got = read(fds[0], output + used, output_size - used - 1);
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
    if (!WIFEXITED(status))
        return false;
    int exit_code = WEXITSTATUS(status);
    return exit_code == 0 || (exit_code == 1 && (!strcmp(argument, "-version") || !strcmp(argument, "-help")));
}

static bool probe_runtime(const char* executable, const char* version_dir, const char* isolated_home, bool* data_path) {
    char version[64 * 1024], help[256 * 1024], config[64 * 1024];
    bool ok = run_runtime_probe(executable, version_dir, isolated_home, "-version", version, sizeof(version)) &&
              run_runtime_probe(executable, version_dir, isolated_home, "-help", help, sizeof(help)) &&
              run_runtime_probe(executable, version_dir, isolated_home, "-testconfig", config, sizeof(config));
    if (!ok || !strstr(version, "PCSX2") || !strstr(help, "-batch") || !strstr(help, "-nogui") ||
        !strstr(help, "-logfile") || !strstr(help, "-testconfig") || !strstr(help, "--"))
        return false;
    *data_path = strstr(help, "-datapath") != NULL;
    return true;
}

static bool copy_backup_file(const char* source, const char* destination, size_t* files, unsigned long long* bytes) {
    struct stat st;
    if (lstat(source, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || st.st_size < 0 ||
        ++(*files) > 20000 || (*bytes += (unsigned long long)st.st_size) > 2ULL * 1024ULL * 1024ULL * 1024ULL)
        return false;
    int input = open(source, O_RDONLY | O_NOFOLLOW);
    int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    bool ok = input >= 0 && output >= 0;
    unsigned char buffer[64 * 1024];
    while (ok) {
        ssize_t got = read(input, buffer, sizeof(buffer));
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        size_t offset = 0;
        while (offset < (size_t)got) {
            ssize_t wrote = write(output, buffer + offset, (size_t)got - offset);
            if (wrote < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            offset += (size_t)wrote;
        }
    }
    if (ok)
        ok = fsync(output) == 0;
    if (input >= 0)
        close(input);
    if (output >= 0 && close(output) != 0)
        ok = false;
    if (!ok)
        unlink(destination);
    return ok;
}

static bool copy_backup_tree(const char* source, const char* destination, unsigned depth, size_t* files,
                             unsigned long long* bytes) {
    struct stat st;
    if (depth > 12 || lstat(source, &st) != 0 || S_ISLNK(st.st_mode))
        return false;
    if (S_ISREG(st.st_mode))
        return copy_backup_file(source, destination, files, bytes);
    if (!S_ISDIR(st.st_mode) || !mkdir_p(destination))
        return false;
    DIR* d = opendir(source);
    struct dirent* entry;
    if (!d)
        return false;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *from = join_path(source, entry->d_name), *to = join_path(destination, entry->d_name);
        bool ok = from && to && copy_backup_tree(from, to, depth + 1, files, bytes);
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

static bool backup_configuration(const char* home, const char* from_tag) {
    if (!from_tag)
        return true;
    char *root = emulator_root(home), *backups = root ? join_path(root, "backups") : NULL;
    char name[256];
    struct timespec timestamp = {0};
    (void)clock_gettime(CLOCK_REALTIME, &timestamp);
    snprintf(name, sizeof(name), "config-%s-%lld-%ld", from_tag, (long long)timestamp.tv_sec, timestamp.tv_nsec);
    char* backup = backups ? join_path(backups, name) : NULL;
    char* data = root ? join_path(root, "home/Library/Application Support/PCSX2") : NULL;
    bool ok = root && backups && backup && data && mkdir_p(backup);
    size_t files = 0;
    unsigned long long bytes = 0;
    const char* directories[] = {"inis", "gamesettings", "inputprofiles"};
    for (size_t i = 0; ok && i < sizeof(directories) / sizeof(directories[0]); ++i) {
        char *source = join_path(data, directories[i]), *destination = join_path(backup, directories[i]);
        if (!source || !destination)
            ok = false;
        else if (access(source, F_OK) == 0)
            ok = copy_backup_tree(source, destination, 0, &files, &bytes);
        free(source);
        free(destination);
    }
    if (ok) {
        char* manifest = join_path(backup, "backup.json");
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "schemaVersion");
        ms_json_writer_i64(&w, 1);
        ms_json_writer_key(&w, "provider");
        ms_json_writer_string(&w, "pcsx2");
        ms_json_writer_key(&w, "fromTag");
        ms_json_writer_string(&w, from_tag);
        ms_json_writer_key(&w, "files");
        ms_json_writer_u64(&w, files);
        ms_json_writer_key(&w, "bytes");
        ms_json_writer_u64(&w, bytes);
        ms_json_writer_key(&w, "createdAt");
        ms_json_writer_i64(&w, (long long)time(NULL));
        ms_json_writer_object_end(&w);
        char* text = ms_json_writer_take(&w);
        ok = manifest && text && write_atomic(manifest, text);
        free(text);
        free(manifest);
    }
    if (!ok && backup)
        (void)remove_tree(backup);
    free(root);
    free(backups);
    free(backup);
    free(data);
    return ok;
}

static void* update_worker(void* raw) {
    update_job* job = raw;
    pcsx2_release* release = &job->release;
    char *root = emulator_root(job->home), *downloads = root ? join_path(root, "downloads") : NULL;
    char *staging = root ? join_path(root, "staging") : NULL, *versions = root ? join_path(root, "versions") : NULL;
    char* isolated_home = root ? join_path(root, "home") : NULL;
    char *archive = NULL, *download = NULL, *stage = NULL, *version_dir = NULL, *candidate_dir = NULL;
    char *replaced_dir = NULL, *source_app = NULL, *dest_app = NULL, *source_exe = NULL, *dest_exe = NULL;
    char *license = NULL, *notices = NULL, *source_license = NULL, *source_notices = NULL;
    char error[256] = "", sha[65], expected_app[192];
    struct stat st;
    bool ok = false, data_path = false, had_existing = false;
    const char* unar = tool_path("unar");
    snprintf(expected_app, sizeof(expected_app), "PCSX2-%s.app", release->tag);
    if (!root || !downloads || !staging || !versions || !isolated_home || !unar || !tool_path("lsar")) {
        snprintf(error, sizeof(error), "failed to prepare the PCSX2 update environment");
        goto done;
    }
    archive = join_path(downloads, release->asset_name);
    if (archive) {
        download = malloc(strlen(archive) + 6);
        if (download)
            sprintf(download, "%s.part", archive);
    }
    char stage_name[96];
    snprintf(stage_name, sizeof(stage_name), "update-%ld-%lld", (long)getpid(), (long long)time(NULL));
    stage = join_path(staging, stage_name);
    version_dir = join_path(versions, release->tag);
    candidate_dir = stage ? join_path(stage, "candidate") : NULL;
    if (!archive || !download || !stage || !version_dir || !candidate_dir || !mkdir_p(stage)) {
        snprintf(error, sizeof(error), "failed to create PCSX2 update staging");
        goto done;
    }

    update_set("downloading", 15, "Downloading the official stable PCSX2 app", NULL);
    unlink(download);
    const char* fixture = getenv("METALSHARP_PCSX2_DOWNLOAD_FILE");
    const char* curl_argv[] = {"/usr/bin/curl",
                               "--fail",
                               "--location",
                               "--max-redirs",
                               "5",
                               "--proto",
                               "=https",
                               "--proto-redir",
                               "=https",
                               "--connect-timeout",
                               "20",
                               "--max-time",
                               "1800",
                               "--speed-time",
                               "60",
                               "--speed-limit",
                               "1024",
                               "--silent",
                               "--show-error",
                               "--output",
                               download,
                               "--write-out",
                               "%{url_effective}",
                               release->url,
                               NULL};
    const char* copy_argv[] = {"/bin/cp", fixture, download, NULL};
    char* final_url = NULL;
    int download_status;
    if (fixture && fixture[0])
        download_status = run_wait(copy_argv, NULL, NULL);
    else {
        final_url = run_capture_timeout(curl_argv, 4096, 1800);
        download_status = approved_download_url(final_url) ? 0 : -1;
    }
    free(final_url);
    if (download_status != 0 || lstat(download, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) ||
        (unsigned long long)st.st_size != release->size) {
        snprintf(error, sizeof(error), "PCSX2 download failed or had an unexpected size");
        goto done;
    }
    update_set("verifying", 38, "Verifying PCSX2 integrity and archive paths", NULL);
    if (!file_sha256(download, sha) || strcasecmp(sha, release->digest + 7)) {
        snprintf(error, sizeof(error), "PCSX2 download digest did not match the official release");
        goto done;
    }
    if (rename(download, archive) != 0 || !archive_entries_safe(archive, expected_app)) {
        snprintf(error, sizeof(error), "PCSX2 archive failed path-safety validation");
        goto done;
    }

    update_set("extracting", 52, "Extracting PCSX2 into isolated staging", NULL);
    const char* extract_argv[] = {unar, "-quiet", "-force-overwrite", "-output-directory", stage, archive, NULL};
    if (run_wait(extract_argv, NULL, NULL) != 0 || !symlinks_stay_inside(stage, stage)) {
        snprintf(error, sizeof(error), "PCSX2 archive extraction failed validation");
        goto done;
    }
    source_app = join_path(stage, expected_app);
    source_exe = source_app ? join_path(source_app, "Contents/MacOS/PCSX2") : NULL;
    if (!source_app || !source_exe || access(source_exe, X_OK) != 0 || !validate_macho_x86_64(source_exe)) {
        snprintf(error, sizeof(error), "PCSX2 release is missing its validated app executable");
        goto done;
    }
    int min_macos = macho_minimum_macos(source_exe);
    int plist_macos = plist_minimum_macos(source_app);
    if (plist_macos > min_macos)
        min_macos = plist_macos;
    int host_macos = host_macos_major();
    size_t macho_count = 0;
    if (min_macos < 11 || host_macos < min_macos) {
        snprintf(error, sizeof(error), "PCSX2 requires macOS %d or newer", min_macos > 0 ? min_macos : 11);
        goto done;
    }
    if (!validate_bundle_machos(source_app, 0, &macho_count, &min_macos) || macho_count == 0) {
        snprintf(error, sizeof(error), "PCSX2 bundle architecture or dependency validation failed");
        goto done;
    }
    if (host_macos < min_macos) {
        snprintf(error, sizeof(error), "PCSX2 bundled components require macOS %d or newer", min_macos);
        goto done;
    }
    if (!validate_bundle_identity(source_app, source_exe, release->tag)) {
        snprintf(error, sizeof(error), "PCSX2 bundle identity, signature, or notarization failed validation");
        goto done;
    }
    if (!mkdir_p(candidate_dir)) {
        snprintf(error, sizeof(error), "failed to create the PCSX2 candidate directory");
        goto done;
    }
    dest_app = join_path(candidate_dir, "PCSX2.app");
    if (!dest_app || rename(source_app, dest_app) != 0) {
        snprintf(error, sizeof(error), "failed to move PCSX2 into the version store");
        goto done;
    }
    dest_exe = join_path(dest_app, "Contents/MacOS/PCSX2");
    if (!dest_exe || !validate_bundle_identity(dest_app, dest_exe, release->tag)) {
        snprintf(error, sizeof(error), "the preserved PCSX2 signature failed after staging");
        goto done;
    }

    while (any_session_running(job->home)) {
        update_set("waiting_for_exit", 68, "PCSX2 update will continue after active processes exit", NULL);
        sleep(1);
    }
    char* before_tag = current_tag(job->home);
    if (!backup_configuration(job->home, before_tag)) {
        free(before_tag);
        snprintf(error, sizeof(error), "failed to preserve PCSX2 configuration before update");
        goto done;
    }
    free(before_tag);
    update_set("validating", 72, "Initializing and probing the isolated PCSX2 environment", NULL);
    source_license = join_path(dest_app, "Contents/Resources/docs/GPL.html");
    source_notices = join_path(dest_app, "Contents/Resources/docs/ThirdPartyLicenses.html");
    license = join_path(candidate_dir, "LICENSE");
    notices = join_path(candidate_dir, "THIRD_PARTY_LICENSES.html");
    const char* license_copy[] = {"/bin/cp", source_license, license, NULL};
    const char* notices_copy[] = {"/bin/cp", source_notices, notices, NULL};
    if (!source_license || !source_notices || !license || !notices || run_wait(license_copy, NULL, NULL) != 0 ||
        run_wait(notices_copy, NULL, NULL) != 0 || !write_source_manifest(candidate_dir, release) ||
        !probe_runtime(dest_exe, candidate_dir, isolated_home, &data_path) ||
        !write_capability_manifest(candidate_dir, release->tag, data_path, min_macos) ||
        !disable_upstream_updater(job->home) || !sync_game_list_roots(job->home)) {
        snprintf(error, sizeof(error), "PCSX2 provenance or CLI capability validation failed");
        goto done;
    }

    while (any_session_running(job->home)) {
        update_set("waiting_for_exit", 85, "PCSX2 update will activate after active processes exit", NULL);
        sleep(1);
    }
    update_set("activating", 92, "Atomically activating PCSX2", NULL);
    if (!validate_bundle_identity(dest_app, dest_exe, release->tag)) {
        snprintf(error, sizeof(error), "the PCSX2 signature changed while freezing the runtime");
        goto done;
    }
    char replaced_name[256];
    snprintf(replaced_name, sizeof(replaced_name), "%s.replaced.%ld", release->tag, (long)getpid());
    replaced_dir = join_path(versions, replaced_name);
    had_existing = access(version_dir, F_OK) == 0;
    if (!replaced_dir || (had_existing && rename(version_dir, replaced_dir) != 0) ||
        rename(candidate_dir, version_dir) != 0) {
        if (had_existing && replaced_dir && access(version_dir, F_OK) != 0)
            (void)rename(replaced_dir, version_dir);
        snprintf(error, sizeof(error), "failed to commit the verified PCSX2 candidate: %s", strerror(errno));
        goto done;
    }
    free(dest_app);
    free(dest_exe);
    dest_app = join_path(version_dir, "PCSX2.app");
    dest_exe = dest_app ? join_path(dest_app, "Contents/MacOS/PCSX2") : NULL;
    if (!dest_app || !dest_exe || !make_tree_read_only(version_dir) ||
        !validate_bundle_identity(dest_app, dest_exe, release->tag)) {
        (void)remove_tree(version_dir);
        if (had_existing)
            (void)rename(replaced_dir, version_dir);
        snprintf(error, sizeof(error), "failed to freeze the committed PCSX2 runtime");
        goto done;
    }
    if (!switch_version(root, release->tag, error, sizeof(error))) {
        (void)remove_tree(version_dir);
        if (had_existing)
            (void)rename(replaced_dir, version_dir);
        goto done;
    }
    if (had_existing)
        (void)remove_tree(replaced_dir);
    ok = true;

done:
    if (stage)
        (void)remove_tree(stage);
    if (!ok && candidate_dir)
        (void)remove_tree(candidate_dir);
    if (!ok && archive)
        unlink(archive);
    if (download)
        unlink(download);
    update_set(ok ? "completed" : "failed", ok ? 100 : 0, ok ? "PCSX2 is ready" : "PCSX2 update failed",
               ok ? NULL : error);
    free(root);
    free(downloads);
    free(staging);
    free(versions);
    free(isolated_home);
    free(archive);
    free(download);
    free(stage);
    free(version_dir);
    free(candidate_dir);
    free(replaced_dir);
    free(source_app);
    free(dest_app);
    free(source_exe);
    free(dest_exe);
    free(license);
    free(notices);
    free(source_license);
    free(source_notices);
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
    bool arm = !strcmp(machine_arch(), "arm64");
    bool intel = !strcmp(machine_arch(), "x86_64");
    if (!arm && !intel)
        return error_json("PCSX2 requires an Intel or Apple Silicon Mac");
    if (host_macos_major() < 11)
        return error_json("PCSX2 requires macOS 11 or newer");
    if (arm && !rosetta_available())
        return error_json("Rosetta 2 is required to run PCSX2 on Apple Silicon");
    if (intel && !intel_sse41_available())
        return error_json("PCSX2 requires SSE4.1 on Intel Macs");
    if (!ensure_environment(home))
        return error_json("failed to create the isolated PCSX2 environment");
    pthread_mutex_lock(&g_update.mutex);
    if (g_update.running) {
        pthread_mutex_unlock(&g_update.mutex);
        return error_json("a PCSX2 update is already running");
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
        update_set("failed", 0, "PCSX2 update failed", error[0] ? error : "failed to prepare update");
        return error_json(error[0] ? error : "failed to prepare update");
    }
    installed_tag = current_tag(home);
    char* installed_executable = executable_path(home);
    bool installed_ready =
        installed_executable && access(installed_executable, X_OK) == 0 && active_runtime_valid(home);
    free(installed_executable);
    if (installed_ready && installed_tag && !strcmp(installed_tag, job->release.tag)) {
        free(installed_tag);
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("idle", 0, "PCSX2 is already up to date", NULL);
        return error_json("PCSX2 is already up to date");
    }
    free(installed_tag);
    pthread_mutex_lock(&g_update.mutex);
    snprintf(g_update.target, sizeof(g_update.target), "%s", job->release.tag);
    pthread_mutex_unlock(&g_update.mutex);
    if (pthread_create(&thread, NULL, update_worker, job) != 0) {
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("failed", 0, "PCSX2 update failed", "failed to start update worker");
        return error_json("failed to start PCSX2 update worker");
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
        return error_json("failed to resolve PCSX2 environment");
    if (update_running()) {
        free(root);
        return error_json("wait for the PCSX2 runtime transaction before rolling back");
    }
    if (any_session_running(home)) {
        free(root);
        return error_json("stop PCSX2 before rolling back");
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
        char* app = candidate ? join_path(candidate, "PCSX2.app") : NULL;
        char* executable = app ? join_path(app, "Contents/MacOS/PCSX2") : NULL;
        if (app && executable && access(executable, X_OK) == 0 && validate_bundle_identity(app, executable, tag))
            ok = switch_version(root, tag, error, sizeof(error));
        free(candidate);
        free(app);
        free(executable);
    }
    free(root);
    free(previous);
    if (!ok)
        return error_json("no valid previous PCSX2 version is available");
    return ms_pcsx2_status_json(home);
}

static unsigned long long path_hash(const char* value) {
    unsigned long long h = 1469598103934665603ULL;
    while (*value) {
        h ^= (unsigned char)*value++;
        h *= 1099511628211ULL;
    }
    return h;
}

static const char* supported_extension(const char* name) {
    static const char* extensions[] = {"iso", "bin", "img", "mdf", "gz", "cso", "zso", "chd", "elf"};
    const char* dot = strrchr(name, '.');
    if (!dot || !dot[1])
        return NULL;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (!strcasecmp(dot + 1, extensions[i]))
            return extensions[i];
    return NULL;
}

static bool game_path_exists(const pcsx2_games* games, const char* path) {
    for (size_t i = 0; i < games->count; ++i)
        if (!strcmp(games->items[i].path, path))
            return true;
    return false;
}

static void sanitize_title(char* title) {
    const unsigned char* source = (const unsigned char*)title;
    unsigned char* destination = (unsigned char*)title;
    while (*source) {
        if (*source < 0x80) {
            *destination++ = (*source < 32 || *source == 127) ? ' ' : *source;
            source++;
            continue;
        }
        size_t width = (*source >= 0xc2 && *source <= 0xdf)   ? 2
                       : (*source >= 0xe0 && *source <= 0xef) ? 3
                       : (*source >= 0xf0 && *source <= 0xf4) ? 4
                                                              : 0;
        bool valid = width > 0;
        for (size_t i = 1; valid && i < width; ++i)
            valid = (source[i] & 0xc0) == 0x80;
        if (valid && width == 3)
            valid = !(*source == 0xe0 && source[1] < 0xa0) && !(*source == 0xed && source[1] >= 0xa0);
        if (valid && width == 4)
            valid = !(*source == 0xf0 && source[1] < 0x90) && !(*source == 0xf4 && source[1] >= 0x90);
        if (valid) {
            for (size_t i = 0; i < width; ++i)
                *destination++ = *source++;
        } else {
            *destination++ = '?';
            source++;
        }
    }
    *destination = '\0';
    size_t n = strlen(title);
    while (n && (title[n - 1] == ' ' || title[n - 1] == '.'))
        title[--n] = '\0';
}

static bool valid_utf8_name(const char* name) {
    char* copy = name ? strdup(name) : NULL;
    if (!copy)
        return false;
    sanitize_title(copy);
    bool valid = !strcmp(copy, name);
    free(copy);
    return valid;
}

static bool serial_prefix(const unsigned char* p) {
    static const char* prefixes[] = {"SLUS", "SCUS", "SLES", "SCES", "SLPS", "SCPS", "SLPM", "SCAJ",
                                     "SCKA", "SLAJ", "PBPX", "PAPX", "TCES", "TCUS", "TLPS"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (!memcmp(p, prefixes[i], 4))
            return true;
    return false;
}

static void extract_disc_serial(const char* path, char output[32]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return;
    unsigned char* data = malloc(32 * 1024 * 1024);
    if (!data) {
        close(fd);
        return;
    }
    ssize_t got = read(fd, data, 32 * 1024 * 1024);
    close(fd);
    if (got > 12) {
        for (size_t i = 0; i + 10 < (size_t)got; ++i) {
            if (!serial_prefix(data + i))
                continue;
            size_t cursor = i + 4;
            while (cursor < (size_t)got && cursor < i + 8 &&
                   (data[cursor] == '_' || data[cursor] == '-' || data[cursor] == '.' || data[cursor] == ' '))
                cursor++;
            char digits[6] = {0};
            size_t found = 0;
            while (cursor < (size_t)got && cursor < i + 16 && found < 5) {
                if (data[cursor] >= '0' && data[cursor] <= '9')
                    digits[found++] = (char)data[cursor];
                else if (data[cursor] != '.' && data[cursor] != '_' && data[cursor] != '-')
                    break;
                cursor++;
            }
            if (found == 5) {
                snprintf(output, 32, "%c%c%c%c-%s", data[i], data[i + 1], data[i + 2], data[i + 3], digits);
                break;
            }
        }
    }
    free(data);
}

static bool valid_cover_file(const char* path, const char* extension) {
    unsigned char bytes[12] = {0};
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    ssize_t got = fd >= 0 ? read(fd, bytes, sizeof(bytes)) : -1;
    if (fd >= 0)
        close(fd);
    if (got < 4)
        return false;
    if (!strcasecmp(extension, "png"))
        return got >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8);
    if (!strcasecmp(extension, "jpg") || !strcasecmp(extension, "jpeg"))
        return bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
    return !strcasecmp(extension, "webp") && got >= 12 && !memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WEBP", 4);
}

static void find_local_cover(const char* home, pcsx2_game* game) {
    char *root = emulator_root(home),
         *covers = root ? join_path(root, "home/Library/Application Support/PCSX2/covers") : NULL;
    const char* stems[2] = {game->serial[0] ? game->serial : NULL, game->id};
    const char* extensions[] = {"png", "jpg", "jpeg", "webp"};
    for (size_t s = 0; covers && s < 2 && !game->icon_path[0]; ++s) {
        if (!stems[s])
            continue;
        for (size_t e = 0; e < sizeof(extensions) / sizeof(extensions[0]); ++e) {
            char name[160];
            snprintf(name, sizeof(name), "%s.%s", stems[s], extensions[e]);
            char* candidate = join_path(covers, name);
            struct stat st;
            if (candidate && lstat(candidate, &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) &&
                st.st_size > 0 && st.st_size <= 10 * 1024 * 1024 && valid_cover_file(candidate, extensions[e]))
                snprintf(game->icon_path, sizeof(game->icon_path), "%s", candidate);
            free(candidate);
            if (game->icon_path[0])
                break;
        }
    }
    free(root);
    free(covers);
}

static void add_game_file(const char* home, pcsx2_games* games, const char* path, const char* name,
                          const struct stat* st) {
    const char* extension = supported_extension(name);
    if (!extension || games->count >= PCSX2_MAX_GAMES || game_path_exists(games, path) || st->st_size <= 0)
        return;
    if (!strcasecmp(extension, "elf")) {
        unsigned char magic[4] = {0};
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        ssize_t n = fd >= 0 ? read(fd, magic, sizeof(magic)) : -1;
        if (fd >= 0)
            close(fd);
        if (n != 4 || memcmp(magic, "\177ELF", 4))
            return;
    }
    pcsx2_game* game = &games->items[games->count];
    memset(game, 0, sizeof(*game));
    snprintf(game->path, sizeof(game->path), "%s", path);
    snprintf(game->format, sizeof(game->format), "%s", extension);
    game->size = (unsigned long long)st->st_size;
    const char* dot = strrchr(name, '.');
    size_t title_length = dot && dot > name ? (size_t)(dot - name) : strlen(name);
    if (title_length >= sizeof(game->title))
        title_length = sizeof(game->title) - 1;
    memcpy(game->title, name, title_length);
    game->title[title_length] = '\0';
    sanitize_title(game->title);
    if (!game->title[0])
        snprintf(game->title, sizeof(game->title), "PlayStation 2 Game");
    if (strcasecmp(extension, "elf"))
        extract_disc_serial(path, game->serial);
    if (!strncmp(game->serial, "SLUS", 4) || !strncmp(game->serial, "SCUS", 4) || !strncmp(game->serial, "TCUS", 4))
        snprintf(game->region, sizeof(game->region), "USA");
    else if (!strncmp(game->serial, "SLES", 4) || !strncmp(game->serial, "SCES", 4) ||
             !strncmp(game->serial, "TCES", 4))
        snprintf(game->region, sizeof(game->region), "Europe");
    else if (!strncmp(game->serial, "SLPS", 4) || !strncmp(game->serial, "SCPS", 4) ||
             !strncmp(game->serial, "SLPM", 4) || !strncmp(game->serial, "PBPX", 4) ||
             !strncmp(game->serial, "PAPX", 4) || !strncmp(game->serial, "TLPS", 4))
        snprintf(game->region, sizeof(game->region), "Japan");
    else if (game->serial[0])
        snprintf(game->region, sizeof(game->region), "Asia");
    snprintf(game->id, sizeof(game->id), "ps2-%llx", path_hash(path));
    find_local_cover(home, game);
    games->count++;
}

static void scan_directory(const char* home, pcsx2_games* games, const char* directory, unsigned depth) {
    DIR* d;
    struct dirent* entry;
    if (depth > 8 || games->count >= PCSX2_MAX_GAMES || games->scanned_entries >= PCSX2_MAX_SCAN_ENTRIES ||
        (games->started_at > 0 && time(NULL) - games->started_at >= 3)) {
        games->truncated = true;
        return;
    }
    if (!(d = opendir(directory)))
        return;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++games->scanned_entries > PCSX2_MAX_SCAN_ENTRIES ||
            (games->started_at > 0 && time(NULL) - games->started_at >= 3)) {
            games->truncated = true;
            break;
        }
        char* path = valid_utf8_name(entry->d_name) ? join_path(directory, entry->d_name) : NULL;
        struct stat st;
        if (path && strlen(path) < 4096 && lstat(path, &st) == 0 && !S_ISLNK(st.st_mode)) {
            if (S_ISDIR(st.st_mode))
                scan_directory(home, games, path, depth + 1);
            else if (S_ISREG(st.st_mode))
                add_game_file(home, games, path, entry->d_name, &st);
        }
        free(path);
    }
    closedir(d);
}

static size_t load_roots(const char* home, char* roots[PCSX2_MAX_ROOTS]) {
    char *root = emulator_root(home), *library = root ? join_path(root, "library.json") : NULL;
    char* text = library ? read_file(library, 1024 * 1024, NULL) : NULL;
    size_t count = 0;
    if (text) {
        char error[128];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        const ms_json* array = ms_json_object_get(json, "roots");
        for (size_t i = 0; i < ms_json_array_length(array) && count < PCSX2_MAX_ROOTS; ++i) {
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
    if (!root || !library || !ensure_environment(home)) {
        free(root);
        free(library);
        return false;
    }
    ms_json_writer w;
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
    bool ok = text && write_atomic(library, text);
    free(text);
    free(root);
    free(library);
    return ok;
}

typedef struct {
    pthread_mutex_t mutex;
    char home[4096];
    pcsx2_games* games;
    time_t scanned_at;
} pcsx2_library_cache;

static pcsx2_library_cache g_library = {PTHREAD_MUTEX_INITIALIZER, "", NULL, 0};

static void invalidate_game_cache(void) {
    pthread_mutex_lock(&g_library.mutex);
    g_library.scanned_at = 0;
    pthread_mutex_unlock(&g_library.mutex);
}

static void collect_games_uncached(const char* home, pcsx2_games* games) {
    char* roots[PCSX2_MAX_ROOTS] = {0};
    memset(games, 0, sizeof(*games));
    games->started_at = time(NULL);
    size_t count = load_roots(home, roots);
    for (size_t i = 0; i < count; ++i) {
        char resolved[4096];
        struct stat st;
        if (lstat(roots[i], &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && realpath(roots[i], resolved) &&
            !strcmp(resolved, roots[i]))
            scan_directory(home, games, roots[i], 0);
        else
            games->truncated = true;
        free(roots[i]);
    }
}

static void collect_games(const char* home, pcsx2_games* games) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_library.mutex);
    if (g_library.games && !strcmp(g_library.home, home) && now >= g_library.scanned_at &&
        now - g_library.scanned_at < 30) {
        memcpy(games, g_library.games, sizeof(*games));
        pthread_mutex_unlock(&g_library.mutex);
        return;
    }
    pthread_mutex_unlock(&g_library.mutex);
    collect_games_uncached(home, games);
    pthread_mutex_lock(&g_library.mutex);
    if (!g_library.games)
        g_library.games = malloc(sizeof(*g_library.games));
    if (g_library.games) {
        memcpy(g_library.games, games, sizeof(*games));
        snprintf(g_library.home, sizeof(g_library.home), "%s", home);
        g_library.scanned_at = now;
    }
    pthread_mutex_unlock(&g_library.mutex);
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

static bool process_matches_pcsx2(pid_t pid, const char* executable, time_t recorded_start) {
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

static bool process_is_pcsx2(pid_t pid) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "command=", NULL};
    char* command = run_capture(argv, 64 * 1024);
    bool ok = command && (strstr(command, "/pcsx2") || strstr(command, "PCSX2.app"));
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
                process_matches_pcsx2(candidate, executable, (time_t)started_at))
                pid = candidate;
            else if (waited < 0 && errno == ECHILD && kill(candidate, 0) == 0 &&
                     process_matches_pcsx2(candidate, executable, (time_t)started_at))
                pid = candidate;
            else if (waited == candidate) {
                if (log_path) {
                    FILE* log = fopen(log_path, "ab");
                    if (log) {
                        if (WIFEXITED(status))
                            fprintf(log, "\nMetalSharp: PCSX2 exited with status %d\n", WEXITSTATUS(status));
                        else if (WIFSIGNALED(status))
                            fprintf(log, "\nMetalSharp: PCSX2 exited from signal %d\n", WTERMSIG(status));
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
                         const char* runtime_tag, const char* log, const char* pcsx2_log) {
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
    ms_json_writer_key(&w, "pcsx2LogPath");
    if (pcsx2_log)
        ms_json_writer_string(&w, pcsx2_log);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "startedAt");
    ms_json_writer_i64(&w, (long long)time(NULL));
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static uint32_t read_le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool validate_bios_file(const char* path, char region[32], char description[96]) {
    struct stat st;
    if (!path || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || st.st_size < 4 * 1024 * 1024 ||
        st.st_size > 8 * 1024 * 1024)
        return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    unsigned char entry[16];
    bool found_reset = false, found_version = false;
    unsigned long long file_offset = 0;
    if (fd < 0)
        return false;
    for (size_t i = 0; i < 512 * 1024; ++i) {
        if (read(fd, entry, sizeof(entry)) != (ssize_t)sizeof(entry))
            break;
        if (!memcmp(entry, "RESET", 5) && entry[5] == '\0') {
            found_reset = true;
            break;
        }
    }
    while (found_reset && entry[0] && memchr(entry, '\0', 10)) {
        uint32_t file_size = read_le32(entry + 12);
        if (!memcmp(entry, "ROMVER", 6) && entry[6] == '\0') {
            unsigned char romver[15] = {0};
            if (pread(fd, romver, 14, (off_t)file_offset) == 14) {
                bool digits = true;
                const size_t positions[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13};
                for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i)
                    if (romver[positions[i]] < '0' || romver[positions[i]] > '9')
                        digits = false;
                const char* zone = romver[4] == 'J'   ? "Japan"
                                   : romver[4] == 'A' ? "USA"
                                   : romver[4] == 'E' ? "Europe"
                                   : romver[4] == 'H' ? "Asia"
                                   : romver[4] == 'C' ? "China"
                                   : romver[4] == 'T' ? "Development"
                                   : romver[4] == 'X' ? "Test"
                                   : romver[4] == 'P' ? "Free"
                                                      : NULL;
                if (digits && zone) {
                    snprintf(region, 32, "%s", zone);
                    snprintf(description, 96, "%s v%c%c.%c%c (%c%c/%c%c/%c%c%c%c)", zone, romver[0], romver[1],
                             romver[2], romver[3], romver[12], romver[13], romver[10], romver[11], romver[6], romver[7],
                             romver[8], romver[9]);
                    found_version = true;
                }
            }
        }
        file_offset += (file_size + 15U) & ~15U;
        if (read(fd, entry, sizeof(entry)) != (ssize_t)sizeof(entry))
            break;
    }
    close(fd);
    return found_version && file_offset <= (unsigned long long)st.st_size;
}

static bool bios_status(const char* home, char region[32], char description[96], size_t* count) {
    char *root = emulator_root(home),
         *directory = root ? join_path(root, "home/Library/Application Support/PCSX2/bios") : NULL;
    DIR* d = directory ? opendir(directory) : NULL;
    struct dirent* entry;
    bool ready = false;
    *count = 0;
    if (d) {
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char* path = join_path(directory, entry->d_name);
            char candidate_region[32] = "", candidate_description[96] = "";
            if (path && validate_bios_file(path, candidate_region, candidate_description)) {
                (*count)++;
                if (!ready) {
                    snprintf(region, 32, "%s", candidate_region);
                    snprintf(description, 96, "%s", candidate_description);
                    ready = true;
                }
            }
            free(path);
        }
        closedir(d);
    }
    free(root);
    free(directory);
    return ready;
}

static bool ini_has_value(const char* home, const char* key, const char* value) {
    char *root = emulator_root(home),
         *path = root ? join_path(root, "home/Library/Application Support/PCSX2/inis/PCSX2.ini") : NULL;
    char* text = path ? read_file(path, 2 * 1024 * 1024, NULL) : NULL;
    bool found = false;
    if (text) {
        char pattern[160];
        snprintf(pattern, sizeof(pattern), "%s = %s", key, value);
        found = strstr(text, pattern) != NULL;
    }
    free(root);
    free(path);
    free(text);
    return found;
}

static bool setup_complete(const char* home) {
    return ini_has_value(home, "SetupWizardIncomplete", "false");
}

static bool disable_upstream_updater(const char* home) {
    char *root = emulator_root(home),
         *path = root ? join_path(root, "home/Library/Application Support/PCSX2/inis/PCSX2.ini") : NULL;
    char* text = path ? read_file(path, 2 * 1024 * 1024, NULL) : NULL;
    if (!path || !text) {
        free(root);
        free(path);
        free(text);
        return false;
    }
    const char* section = strstr(text, "[AutoUpdater]");
    char* replacement = NULL;
    if (!section) {
        size_t n = strlen(text) + 64;
        replacement = malloc(n);
        if (replacement)
            snprintf(replacement, n, "%s\n[AutoUpdater]\nCheckAtStartup = false\n", text);
    } else {
        const char* end = strstr(section + 1, "\n[");
        const char* key = strstr(section, "CheckAtStartup =");
        if (key && (!end || key < end)) {
            const char* line_end = strchr(key, '\n');
            if (!line_end)
                line_end = text + strlen(text);
            size_t prefix = (size_t)(key - text), suffix = strlen(line_end);
            replacement = malloc(prefix + strlen("CheckAtStartup = false") + suffix + 1);
            if (replacement) {
                memcpy(replacement, text, prefix);
                strcpy(replacement + prefix, "CheckAtStartup = false");
                strcpy(replacement + prefix + strlen("CheckAtStartup = false"), line_end);
            }
        } else {
            const char* insert = end ? end + 1 : text + strlen(text);
            size_t prefix = (size_t)(insert - text), suffix = strlen(insert);
            bool newline = prefix > 0 && text[prefix - 1] != '\n';
            replacement = malloc(prefix + (newline ? 1 : 0) + strlen("CheckAtStartup = false\n") + suffix + 1);
            if (replacement) {
                memcpy(replacement, text, prefix);
                size_t offset = prefix;
                if (newline)
                    replacement[offset++] = '\n';
                strcpy(replacement + offset, "CheckAtStartup = false\n");
                strcpy(replacement + offset + strlen("CheckAtStartup = false\n"), insert);
            }
        }
    }
    bool ok = replacement && write_atomic(path, replacement);
    free(root);
    free(path);
    free(text);
    free(replacement);
    return ok;
}

static bool update_game_list_path(const char* home, const char* value, bool add) {
    if (!value || strchr(value, '\n') || strchr(value, '\r'))
        return false;
    char *root = emulator_root(home),
         *path = root ? join_path(root, "home/Library/Application Support/PCSX2/inis/PCSX2.ini") : NULL;
    char* text = path ? read_file(path, 2 * 1024 * 1024, NULL) : NULL;
    if (!path || !text) {
        free(root);
        free(path);
        free(text);
        return true;
    }
    char line[4250];
    snprintf(line, sizeof(line), "RecursivePaths = %s", value);
    const char* section = strstr(text, "[GameList]");
    const char* section_end = section ? strstr(section + 1, "\n[") : NULL;
    const char* match = NULL;
    const char* search = section;
    size_t line_length = strlen(line);
    while (search && (search = strstr(search, line)) != NULL) {
        bool within = !section_end || search < section_end;
        bool at_line_start = search == text || search[-1] == '\n';
        char after = search[line_length];
        if (within && at_line_start && (after == '\0' || after == '\n' || after == '\r')) {
            match = search;
            break;
        }
        search++;
    }
    char* replacement = NULL;
    bool needs_change = (add && !match) || (!add && match);
    if (add && !match) {
        if (!section) {
            size_t n = strlen(text) + strlen(line) + 32;
            replacement = malloc(n);
            if (replacement)
                snprintf(replacement, n, "%s\n[GameList]\n%s\n", text, line);
        } else {
            const char* insert = section_end ? section_end + 1 : text + strlen(text);
            size_t prefix = (size_t)(insert - text), suffix = strlen(insert);
            bool newline = prefix > 0 && text[prefix - 1] != '\n';
            replacement = malloc(prefix + (newline ? 1 : 0) + strlen(line) + suffix + 2);
            if (replacement) {
                memcpy(replacement, text, prefix);
                size_t offset = prefix;
                if (newline)
                    replacement[offset++] = '\n';
                strcpy(replacement + offset, line);
                replacement[offset + strlen(line)] = '\n';
                strcpy(replacement + offset + strlen(line) + 1, insert);
            }
        }
    } else if (!add && match) {
        const char* start = match;
        if (start > text && start[-1] == '\n')
            start--;
        const char* end = strchr(match, '\n');
        end = end ? end + 1 : text + strlen(text);
        size_t prefix = (size_t)(start - text), suffix = strlen(end);
        replacement = malloc(prefix + suffix + 1);
        if (replacement) {
            memcpy(replacement, text, prefix);
            strcpy(replacement + prefix, end);
        }
    }
    bool ok = !needs_change || (replacement && write_atomic(path, replacement));
    free(root);
    free(path);
    free(text);
    free(replacement);
    return ok;
}

static bool sync_game_list_roots(const char* home) {
    char* roots[PCSX2_MAX_ROOTS] = {0};
    size_t count = load_roots(home, roots);
    bool ok = true;
    for (size_t i = 0; i < count; ++i) {
        if (!update_game_list_path(home, roots[i], true))
            ok = false;
        free(roots[i]);
    }
    return ok;
}

static bool capability_data_path(const char* home) {
    char* version = current_version_path(home);
    char* path = version ? join_path(version, "capabilities.json") : NULL;
    char* text = path ? read_file(path, 256 * 1024, NULL) : NULL;
    bool supported = false;
    if (text) {
        char error[128];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        supported = json_bool(json, "dataPathFlag", false);
        ms_json_free(json);
    }
    free(version);
    free(path);
    free(text);
    return supported;
}

static bool active_runtime_valid(const char* home) {
    char *version = current_version_path(home), *tag = current_tag(home),
         *app = version ? join_path(version, "PCSX2.app") : NULL;
    char* exe = app ? join_path(app, "Contents/MacOS/PCSX2") : NULL;
    bool ok = tag && app && exe && access(exe, X_OK) == 0 && validate_bundle_identity(app, exe, tag);
    free(version);
    free(tag);
    free(app);
    free(exe);
    return ok;
}

static bool active_runtime_valid_cached(const char* home) {
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static char cached_tag[160];
    static time_t checked_at;
    static bool cached_result;
    char* tag = current_tag(home);
    time_t now = time(NULL);
    pthread_mutex_lock(&mutex);
    if (tag && !strcmp(tag, cached_tag) && now >= checked_at && now - checked_at < 30) {
        bool result = cached_result;
        pthread_mutex_unlock(&mutex);
        free(tag);
        return result;
    }
    pthread_mutex_unlock(&mutex);
    bool result = active_runtime_valid(home);
    pthread_mutex_lock(&mutex);
    snprintf(cached_tag, sizeof(cached_tag), "%s", tag ? tag : "");
    checked_at = now;
    cached_result = result;
    pthread_mutex_unlock(&mutex);
    free(tag);
    return result;
}

static char* spawn_pcsx2(const char* home, const char* id, const char* target, bool fullscreen, bool setup) {
    char *exe = executable_path(home), *root = emulator_root(home),
         *isolated_home = root ? join_path(root, "home") : NULL;
    char* logs = root ? join_path(root, "logs") : NULL;
    char* version_dir = current_version_path(home);
    char* data_path = root ? join_path(root, "home/Library/Application Support/PCSX2") : NULL;
    char *log = NULL, *pcsx2_log = NULL, log_name[192], pcsx2_log_name[208];
    pid_t pid;
    if (update_running()) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(data_path);
        return error_json("wait for the PCSX2 runtime transaction to finish");
    }
    if ((!target && strcmp(id, "ui") && strcmp(id, "setup")) || !exe || access(exe, X_OK) != 0 ||
        !active_runtime_valid(home)) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(data_path);
        return error_json("PCSX2 is not installed, trusted, or the game target is invalid");
    }
    if (!ensure_environment(home) || !disable_upstream_updater(home)) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(data_path);
        return error_json("failed to prepare the isolated PCSX2 environment");
    }
    if (any_session_running(home)) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(data_path);
        return error_json("another managed PCSX2 process is already running");
    }
    snprintf(log_name, sizeof(log_name), "%s-%lld.log", id, (long long)time(NULL));
    log = logs ? join_path(logs, log_name) : NULL;
    if (target) {
        snprintf(pcsx2_log_name, sizeof(pcsx2_log_name), "%s-%lld.pcsx2.log", id, (long long)time(NULL));
        pcsx2_log = logs ? join_path(logs, pcsx2_log_name) : NULL;
    }
    if (!log || (target && !pcsx2_log)) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(data_path);
        free(log);
        free(pcsx2_log);
        return error_json("failed to prepare PCSX2 logging");
    }
    pid = fork();
    if (pid < 0) {
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(data_path);
        free(log);
        free(pcsx2_log);
        return error_json("failed to start PCSX2");
    }
    if (pid == 0) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        setpgid(0, 0);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        setenv("HOME", isolated_home, 1);
        if (chdir(version_dir) != 0)
            _exit(126);
        bool use_data_path = capability_data_path(home);
        char* args[16];
        size_t index = 0;
        args[index++] = exe;
        if (use_data_path) {
            args[index++] = "-datapath";
            args[index++] = data_path;
        }
        if (setup)
            args[index++] = "-setupwizard";
        else if (target) {
            args[index++] = "-nogui";
            args[index++] = "-batch";
            if (fullscreen)
                args[index++] = "-fullscreen";
            args[index++] = "-logfile";
            args[index++] = pcsx2_log;
            args[index++] = "--";
            args[index++] = (char*)target;
        }
        args[index] = NULL;
        if (!strcmp(machine_arch(), "arm64")) {
            char* arch_args[19] = {"/usr/bin/arch", "-x86_64"};
            for (size_t i = 0; i <= index; ++i)
                arch_args[i + 2] = args[i];
            execv(arch_args[0], arch_args);
        } else
            execv(exe, args);
        _exit(127);
    }
    setpgid(pid, pid);
    char* runtime_tag = current_tag(home);
    if (!save_session(home, id, pid, exe, target ? target : "", runtime_tag, log, pcsx2_log)) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        free(exe);
        free(root);
        free(isolated_home);
        free(logs);
        free(version_dir);
        free(data_path);
        free(log);
        free(pcsx2_log);
        free(runtime_tag);
        return error_json("failed to persist PCSX2 process supervision state");
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
    ms_json_writer_key(&w, "pcsx2LogPath");
    if (pcsx2_log)
        ms_json_writer_string(&w, pcsx2_log);
    else
        ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    char* result = ms_json_writer_take(&w);
    free(exe);
    free(root);
    free(isolated_home);
    free(logs);
    free(version_dir);
    free(data_path);
    free(log);
    free(pcsx2_log);
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
    char* session_text = path ? read_file(path, 64 * 1024, NULL) : NULL;
    char *session_executable = NULL, *session_log = NULL;
    if (session_text) {
        char parse_error[128];
        ms_json* session = ms_json_parse(session_text, strlen(session_text), parse_error, sizeof(parse_error));
        session_executable = json_string(session, "executable");
        session_log = json_string(session, "logPath");
        ms_json_free(session);
    }
    free(session_text);
    if (!pid) {
        free(path);
        free(session_executable);
        free(session_log);
        return error_json("PCSX2 session is not running");
    }
    if (!process_is_pcsx2(pid)) {
        free(path);
        free(session_executable);
        free(session_log);
        return error_json("refusing to stop a process that is not PCSX2");
    }
    (void)kill(-pid, SIGTERM);
    (void)kill(pid, SIGTERM);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD && kill(pid, 0) != 0))
            break;
        struct timespec pause = {0, 100 * 1000 * 1000};
        nanosleep(&pause, NULL);
        if (attempt == 99) {
            (void)kill(-pid, SIGKILL);
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
        }
    }
    save_exit_record(home, id, session_executable, session_log, -1, SIGTERM);
    if (path)
        unlink(path);
    free(path);
    free(session_executable);
    free(session_log);
    return strdup("{\"ok\":true,\"running\":false}");
}

static char* stop_all_sessions(const char* home) {
    char *root = emulator_root(home), *sessions = root ? join_path(root, "sessions") : NULL;
    DIR* d = sessions ? opendir(sessions) : NULL;
    struct dirent* entry;
    size_t stopped = 0;
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
                char* result = stop_session(home, id);
                if (result && strstr(result, "\"ok\":true"))
                    stopped++;
                free(result);
            }
        }
        closedir(d);
    }
    free(root);
    free(sessions);
    if (!stopped)
        return error_json("no managed PCSX2 process is running");
    return strdup("{\"ok\":true,\"running\":false}");
}

char* ms_pcsx2_status_json(const char* home) {
    if (!ensure_environment(home))
        return error_json("failed to create the isolated PCSX2 environment");
    char *root = emulator_root(home), *exe = executable_path(home), *tag = current_tag(home);
    char* data = root ? join_path(root, "home/Library/Application Support/PCSX2") : NULL;
    char *cache = data ? join_path(data, "cache") : NULL, *previous_link = root ? join_path(root, "previous") : NULL;
    char previous[4096];
    ssize_t previous_len = previous_link ? readlink(previous_link, previous, sizeof(previous) - 1) : -1;
    bool arm = !strcmp(machine_arch(), "arm64"), intel = !strcmp(machine_arch(), "x86_64");
    bool rosetta = arm && rosetta_available(), sse41 = intel && intel_sse41_available();
    int host_macos = host_macos_major();
    bool supported = (arm || intel) && host_macos >= 11 && (!arm || rosetta) && (!intel || sse41);
    const char* reason = (!arm && !intel)  ? "unsupported_architecture"
                         : host_macos < 11 ? "macos_too_old"
                         : arm && !rosetta ? "rosetta_missing"
                         : intel && !sse41 ? "sse41_missing"
                                           : NULL;
    bool installed = exe && access(exe, X_OK) == 0;
    bool runtime_valid = installed && active_runtime_valid_cached(home);
    char bios_region[32] = "", bios_description[96] = "";
    size_t bios_count = 0;
    bool bios_ready = bios_status(home, bios_region, bios_description, &bios_count);
    bool configured = setup_complete(home);
    char* roots[PCSX2_MAX_ROOTS] = {0};
    size_t root_count = load_roots(home, roots);
    for (size_t i = 0; i < root_count; ++i)
        free(roots[i]);
    bool running = any_session_running(home);
    const char* state = !supported       ? "unsupported_host"
                        : !installed     ? "missing_runtime"
                        : !runtime_valid ? "runtime_probe_failed"
                        : running        ? "running"
                        : !bios_ready    ? "missing_bios"
                        : !configured    ? "setup_required"
                        : !root_count    ? "no_game_folders"
                                         : "ready";
    unsigned long long memory_bytes = host_sysctl_u64("hw.memsize");
    unsigned long long logical_cpu = host_sysctl_u64("hw.logicalcpu");
    int runtime_macos = installed ? macho_minimum_macos(exe) : -1;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "pcsx2");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "PCSX2");
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, "PlayStation 2");
    ms_json_writer_key(&w, "supported");
    ms_json_writer_bool(&w, supported);
    ms_json_writer_key(&w, "unsupportedReason");
    if (reason)
        ms_json_writer_string(&w, reason);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, installed);
    ms_json_writer_key(&w, "runtimeValid");
    ms_json_writer_bool(&w, runtime_valid);
    ms_json_writer_key(&w, "state");
    ms_json_writer_string(&w, state);
    ms_json_writer_key(&w, "hostArchitecture");
    ms_json_writer_string(&w, machine_arch());
    ms_json_writer_key(&w, "runtimeArchitecture");
    ms_json_writer_string(&w, "x86_64");
    ms_json_writer_key(&w, "rosettaAvailable");
    ms_json_writer_bool(&w, rosetta);
    ms_json_writer_key(&w, "sse41Available");
    ms_json_writer_bool(&w, sse41);
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
    if (logical_cpu > 0 && logical_cpu < 4)
        ms_json_writer_string(&w, "low_cpu_threads");
    if (!tool_path("lsar") || !tool_path("unar"))
        ms_json_writer_string(&w, "missing_archive_tools");
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
    ms_json_writer_key(&w, "setupComplete");
    ms_json_writer_bool(&w, configured);
    ms_json_writer_key(&w, "biosInstalled");
    ms_json_writer_bool(&w, bios_ready);
    ms_json_writer_key(&w, "biosCount");
    ms_json_writer_u64(&w, bios_count);
    ms_json_writer_key(&w, "biosRegion");
    if (bios_ready)
        ms_json_writer_string(&w, bios_region);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "biosDescription");
    if (bios_ready)
        ms_json_writer_string(&w, bios_description);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "gameRootCount");
    ms_json_writer_u64(&w, root_count);
    ms_json_writer_key(&w, "activeSessionCount");
    ms_json_writer_u64(&w, running ? 1 : 0);
    ms_json_writer_key(&w, "dataPathFlag");
    ms_json_writer_bool(&w, capability_data_path(home));
    ms_json_writer_key(&w, "upstreamUpdaterDisabled");
    ms_json_writer_bool(&w, ini_has_value(home, "CheckAtStartup", "false"));
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
    return ms_json_writer_take(&w);
}

char* ms_pcsx2_games_json(const char* home) {
    pcsx2_games* games = calloc(1, sizeof(*games));
    char* roots[PCSX2_MAX_ROOTS] = {0};
    size_t root_count = load_roots(home, roots);
    ms_json_writer w;
    if (!games)
        return error_json("failed to allocate the PCSX2 game index");
    collect_games(home, games);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "pcsx2");
    ms_json_writer_key(&w, "roots");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < root_count; ++i) {
        ms_json_writer_string(&w, roots[i]);
        free(roots[i]);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "scannedEntries");
    ms_json_writer_u64(&w, games->scanned_entries);
    ms_json_writer_key(&w, "truncated");
    ms_json_writer_bool(&w, games->truncated);
    ms_json_writer_key(&w, "games");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < games->count; ++i) {
        pcsx2_game* game = &games->items[i];
        pid_t pid = session_pid(home, game->id);
        char* last_log = latest_session_log(home, game->id);
        int last_exit_code, last_exit_signal;
        last_exit_status(home, game->id, &last_exit_code, &last_exit_signal);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_string(&w, game->id);
        ms_json_writer_key(&w, "serial");
        if (game->serial[0])
            ms_json_writer_string(&w, game->serial);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "title");
        ms_json_writer_string(&w, game->title);
        ms_json_writer_key(&w, "region");
        if (game->region[0])
            ms_json_writer_string(&w, game->region);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "format");
        ms_json_writer_string(&w, game->format);
        ms_json_writer_key(&w, "size");
        ms_json_writer_u64(&w, game->size);
        ms_json_writer_key(&w, "path");
        ms_json_writer_string(&w, game->path);
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

char* ms_pcsx2_cover_path(const char* home, const char* id) {
    pcsx2_games* games = calloc(1, sizeof(*games));
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

char* ms_pcsx2_update_json(const char* home, const char* action) {
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
    return error_json("unknown PCSX2 update action");
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

static bool safe_import_filename(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n > 255 || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    for (size_t i = 0; i < n; ++i)
        if (name[i] == '/' || name[i] == '\\' || (unsigned char)name[i] < 32 || (unsigned char)name[i] == 127)
            return false;
    return true;
}

static bool known_bios_companion(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot)
        return false;
    const char* extensions[] = {".rom1", ".rom2", ".erom", ".nvm", ".mec"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (!strcasecmp(dot, extensions[i]))
            return true;
    return false;
}

static char* import_bios(const char* home, const char* source) {
    struct stat source_st;
    if (update_running())
        return error_json("wait for the PCSX2 runtime transaction before importing a BIOS");
    if (!source || lstat(source, &source_st) != 0 || S_ISLNK(source_st.st_mode) ||
        (!S_ISREG(source_st.st_mode) && !S_ISDIR(source_st.st_mode)))
        return error_json("select a regular BIOS dump file or directory");
    if (any_session_running(home))
        return error_json("stop PCSX2 before importing a BIOS");
    char *root = emulator_root(home), *state = root ? join_path(root, "home/Library/Application Support/PCSX2") : NULL;
    char* staging = root ? join_path(root, "staging") : NULL;
    char stage_name[96];
    snprintf(stage_name, sizeof(stage_name), "bios-import-%ld-%lld", (long)getpid(), (long long)time(NULL));
    char *stage = staging ? join_path(staging, stage_name) : NULL, *target = state ? join_path(state, "bios") : NULL;
    char* backup = state ? join_path(state, "bios.previous") : NULL;
    bool ok = root && state && staging && stage && target && backup && ensure_environment(home) && mkdir_p(stage);
    size_t imported = 0, bios_count = 0, examined = 0;
    unsigned long long bytes = 0;
    char region[32] = "", description[96] = "";
    if (ok && S_ISREG(source_st.st_mode)) {
        const char* name = strrchr(source, '/');
        name = name ? name + 1 : source;
        char candidate_region[32] = "", candidate_description[96] = "";
        char* destination = safe_import_filename(name) ? join_path(stage, name) : NULL;
        ok = destination && validate_bios_file(source, candidate_region, candidate_description) &&
             copy_regular_atomic(source, destination, 8ULL * 1024ULL * 1024ULL);
        if (ok) {
            imported = bios_count = 1;
            bytes = (unsigned long long)source_st.st_size;
            snprintf(region, sizeof(region), "%s", candidate_region);
            snprintf(description, sizeof(description), "%s", candidate_description);
        }
        free(destination);
    } else if (ok) {
        DIR* d = opendir(source);
        struct dirent* entry;
        ok = d != NULL;
        while (ok && (entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            if (++examined > 128 || !safe_import_filename(entry->d_name)) {
                ok = false;
                break;
            }
            char *from = join_path(source, entry->d_name), *to = join_path(stage, entry->d_name);
            struct stat st;
            char candidate_region[32] = "", candidate_description[96] = "";
            bool main_bios = from && validate_bios_file(from, candidate_region, candidate_description);
            bool companion = !main_bios && known_bios_companion(entry->d_name);
            if (!from || !to || lstat(from, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) ||
                (!main_bios && !companion) || st.st_size < 0 ||
                (bytes += (unsigned long long)st.st_size) > 64ULL * 1024ULL * 1024ULL ||
                !copy_regular_atomic(from, to, companion ? 16ULL * 1024ULL * 1024ULL : 8ULL * 1024ULL * 1024ULL))
                ok = false;
            else {
                imported++;
                if (main_bios) {
                    bios_count++;
                    if (!region[0]) {
                        snprintf(region, sizeof(region), "%s", candidate_region);
                        snprintf(description, sizeof(description), "%s", candidate_description);
                    }
                }
            }
            free(from);
            free(to);
        }
        if (d)
            closedir(d);
        ok = ok && bios_count > 0;
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
        return error_json("BIOS import failed ROMDIR/ROMVER, path, type, count, or size validation");
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "imported");
    ms_json_writer_u64(&w, imported);
    ms_json_writer_key(&w, "biosCount");
    ms_json_writer_u64(&w, bios_count);
    ms_json_writer_key(&w, "bytes");
    ms_json_writer_u64(&w, bytes);
    ms_json_writer_key(&w, "region");
    ms_json_writer_string(&w, region);
    ms_json_writer_key(&w, "description");
    ms_json_writer_string(&w, description);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static char* remove_runtime(const char* home) {
    char *root = emulator_root(home), *versions, *downloads, *staging, *current, *previous;
    bool ok;
    if (!root)
        return error_json("failed to resolve PCSX2 environment");
    if (update_running()) {
        free(root);
        return error_json("wait for the PCSX2 runtime transaction before removing it");
    }
    if (any_session_running(home)) {
        free(root);
        return error_json("stop PCSX2 before removing its runtime");
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
              : error_json("failed to remove the managed PCSX2 runtime");
}

static bool protected_game_root(const char* resolved, const char* environment) {
    const char* user_home = getenv("HOME");
    const char* exact[] = {"/",    "/Applications", "/Library", "/System",  "/Users", "/Volumes", "/bin", "/cores",
                           "/dev", "/etc",          "/opt",     "/private", "/sbin",  "/tmp",     "/usr", "/var"};
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); ++i)
        if (!strcmp(resolved, exact[i]))
            return true;
    if (!strncmp(resolved, "/System/", 8) || !strncmp(resolved, "/Library/", 9) ||
        !strncmp(resolved, "/Applications/", 14) || !strncmp(resolved, "/dev/", 5) || !strncmp(resolved, "/usr/", 5) ||
        !strncmp(resolved, "/bin/", 5) || !strncmp(resolved, "/sbin/", 6) || !strncmp(resolved, "/etc/", 5) ||
        !strncmp(resolved, "/private/etc/", 13) || !strncmp(resolved, "/private/var/db/", 16) ||
        !strncmp(resolved, "/private/var/root/", 18))
        return true;
    if (user_home && !strcmp(resolved, user_home))
        return true;
    return environment && !strncmp(resolved, environment, strlen(environment)) &&
           (resolved[strlen(environment)] == '\0' || resolved[strlen(environment)] == '/');
}

static bool valid_launch_target(const char* home, const char* path) {
    char resolved[4096];
    struct stat st;
    if (!path || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || st.st_size <= 0 ||
        !realpath(path, resolved) || strcmp(path, resolved) || !supported_extension(path))
        return false;
    char* roots[PCSX2_MAX_ROOTS] = {0};
    size_t count = load_roots(home, roots);
    bool within = false;
    for (size_t i = 0; i < count; ++i) {
        size_t n = strlen(roots[i]);
        if (!strncmp(path, roots[i], n) && path[n] == '/')
            within = true;
        free(roots[i]);
    }
    return within;
}

static char* initialize_pcsx2(const char* home) {
    if (update_running())
        return error_json("wait for the PCSX2 runtime transaction before initializing");
    if (any_session_running(home))
        return error_json("stop PCSX2 before initializing its managed state");
    if (!active_runtime_valid(home))
        return error_json("install or repair the trusted PCSX2 runtime first");
    char *root = emulator_root(home), *exe = executable_path(home), *tag = current_tag(home);
    char* version_dir = current_version_path(home);
    char* isolated_home = root ? join_path(root, "home") : NULL;
    bool data_path = false;
    bool ok = root && exe && tag && version_dir && isolated_home &&
              probe_runtime(exe, version_dir, isolated_home, &data_path) &&
              write_capability_manifest(version_dir, tag, data_path, macho_minimum_macos(exe)) &&
              disable_upstream_updater(home) && sync_game_list_roots(home);
    free(root);
    free(exe);
    free(tag);
    free(version_dir);
    free(isolated_home);
    return ok ? ms_pcsx2_status_json(home) : error_json("PCSX2 isolated initialization failed validation");
}

char* ms_pcsx2_action_json(const char* home, const char* action, const unsigned char* body, size_t length) {
    ms_json* root = NULL;
    char *id = NULL, *path = NULL, *tag = NULL;
    char* result = NULL;
    if (!strcmp(action, "scan")) {
        invalidate_game_cache();
        return ms_pcsx2_games_json(home);
    }
    root = parse_body(body, length);
    if (!root)
        return error_json("invalid PCSX2 request body");
    if (!strcmp(action, "remove-runtime")) {
        result = json_bool(root, "confirm", false) ? remove_runtime(home)
                                                   : error_json("runtime removal requires explicit confirmation");
        ms_json_free(root);
        return result;
    }
    if (!strcmp(action, "initialize")) {
        result = initialize_pcsx2(home);
        ms_json_free(root);
        return result;
    }
    id = json_string(root, "id");
    path = json_string(root, "path");
    tag = json_string(root, "tag");
    if (!strcmp(action, "pin-current") || !strcmp(action, "unpin") || !strcmp(action, "skip-update") ||
        !strcmp(action, "clear-skip")) {
        pcsx2_update_policy policy;
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
            result = error_json("failed to save PCSX2 update preferences");
        else
            result = release_json(home, false);
        free_update_policy(&policy);
    } else if (!strcmp(action, "add-root")) {
        char resolved[4096];
        char* roots[PCSX2_MAX_ROOTS] = {0};
        size_t count = load_roots(home, roots);
        struct stat st;
        char* environment = emulator_root(home);
        bool protected = path && realpath(path, resolved) && protected_game_root(resolved, environment);
        if (update_running())
            result = error_json("wait for the PCSX2 runtime transaction before changing game folders");
        else if (any_session_running(home))
            result = error_json("stop PCSX2 before changing game folders");
        else if (!path || !realpath(path, resolved) || strchr(resolved, '\n') || strchr(resolved, '\r') ||
                 lstat(resolved, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || protected)
            result = error_json("a safe existing PlayStation 2 game folder is required");
        else {
            bool exists = false;
            for (size_t i = 0; i < count; ++i)
                if (!strcmp(roots[i], resolved))
                    exists = true;
            if (!exists && count >= PCSX2_MAX_ROOTS)
                result = error_json("the PCSX2 game-folder limit has been reached");
            else if (exists)
                result = ms_pcsx2_games_json(home);
            else {
                char* added = strdup(resolved);
                if (!added)
                    result = error_json("failed to allocate the PCSX2 game folder");
                else {
                    roots[count++] = added;
                    bool config_saved = update_game_list_path(home, resolved, true);
                    bool library_saved = config_saved && save_roots(home, roots, count);
                    if (config_saved && !library_saved)
                        (void)update_game_list_path(home, resolved, false);
                    if (library_saved)
                        invalidate_game_cache();
                    result =
                        library_saved ? ms_pcsx2_games_json(home) : error_json("failed to save the PCSX2 game folder");
                }
            }
        }
        free(environment);
        for (size_t i = 0; i < count; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "remove-root")) {
        char* roots[PCSX2_MAX_ROOTS] = {0};
        char resolved[4096];
        const char* requested = path && realpath(path, resolved) ? resolved : path;
        size_t count = load_roots(home, roots), out = 0;
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (!requested || strcmp(roots[i], requested))
                roots[out++] = roots[i];
            else {
                found = true;
                free(roots[i]);
            }
        }
        if (update_running())
            result = error_json("wait for the PCSX2 runtime transaction before changing game folders");
        else if (any_session_running(home))
            result = error_json("stop PCSX2 before changing game folders");
        else if (!requested)
            result = error_json("a PCSX2 game folder is required");
        else if (!found)
            result = ms_pcsx2_games_json(home);
        else {
            bool config_saved = update_game_list_path(home, requested, false);
            bool library_saved = config_saved && save_roots(home, roots, out);
            if (config_saved && !library_saved)
                (void)update_game_list_path(home, requested, true);
            if (library_saved)
                invalidate_game_cache();
            result = library_saved ? ms_pcsx2_games_json(home) : error_json("failed to update PCSX2 game folders");
        }
        for (size_t i = 0; i < out; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "import-bios")) {
        result = import_bios(home, path);
    } else if (!strcmp(action, "open-ui")) {
        result = spawn_pcsx2(home, "ui", NULL, false, false);
    } else if (!strcmp(action, "open-setup")) {
        result = spawn_pcsx2(home, "setup", NULL, false, true);
    } else if (!strcmp(action, "launch")) {
        pcsx2_games* games = calloc(1, sizeof(*games));
        const pcsx2_game* match = NULL;
        if (games) {
            collect_games(home, games);
            for (size_t i = 0; i < games->count; ++i)
                if (id && !strcmp(games->items[i].id, id))
                    match = &games->items[i];
        }
        char bios_region[32] = "", bios_description[96] = "";
        size_t bios_count = 0;
        if (!match)
            result = error_json(games ? "PCSX2 game was not found in the indexed library"
                                      : "failed to allocate the PCSX2 game index");
        else if (!setup_complete(home))
            result = error_json("finish the isolated PCSX2 setup before launching a game");
        else if (!bios_status(home, bios_region, bios_description, &bios_count))
            result = error_json("import a valid console-dumped PlayStation 2 BIOS before launching");
        else if (!valid_launch_target(home, match->path))
            result = error_json("the indexed PCSX2 game path changed; scan the library again");
        else
            result = spawn_pcsx2(home, match->id, match->path, json_bool(root, "fullscreen", true), false);
        free(games);
    } else if (!strcmp(action, "stop")) {
        result = id && is_safe_component(id) ? stop_session(home, id) : stop_all_sessions(home);
    } else {
        result = error_json("unknown PCSX2 action");
    }
    free(id);
    free(path);
    free(tag);
    ms_json_free(root);
    return result;
}
