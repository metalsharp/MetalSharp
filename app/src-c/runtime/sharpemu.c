#include "metalsharp_backend/sharpemu.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x00000100
#endif

#define SHARPEMU_MAX_GAMES           512
#define SHARPEMU_MAX_ROOTS           32
#define SHARPEMU_MAX_CAPTURE         (8 * 1024 * 1024)
#define SHARPEMU_MAX_SCAN_ENTRIES    20000
#define SHARPEMU_STABLE_REPO         "sharpemu/sharpemu"
#define SHARPEMU_EFFECTIVE_MIN_MACOS 26

typedef struct {
    char id[96];
    char title_id[32];
    char title[256];
    char content_version[48];
    char master_version[48];
    char path[4096];
    char launch_path[4096];
    char metadata_path[4096];
    char icon_path[4096];
    unsigned long long executable_size;
    unsigned long long executable_device;
    unsigned long long executable_inode;
    long long executable_mtime;
} sharpemu_game;

typedef struct {
    sharpemu_game items[SHARPEMU_MAX_GAMES];
    size_t count;
    size_t scanned_entries;
    bool truncated;
} sharpemu_games;

typedef struct {
    char* tag;
    char* version;
    char* asset_name;
    char* url;
    char* digest;
    char* source_commit;
    unsigned long long size;
    long long release_id;
    long long asset_id;
    char* published_at;
} sharpemu_release;

typedef struct {
    pthread_mutex_t mutex;
    bool running;
    int percent;
    char status[32];
    char message[256];
    char error[256];
    char target[160];
    pid_t worker_pid;
} sharpemu_update_state;

static sharpemu_update_state g_update = {PTHREAD_MUTEX_INITIALIZER, false, 0, "idle", "", "", "", 0};

static bool update_running(void) {
    bool running;
    pthread_mutex_lock(&g_update.mutex);
    running = g_update.running;
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
            if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            copy[i] = '/';
        }
    }
    bool ok = mkdir(copy, 0700) == 0 || errno == EEXIST;
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
    size_t n = strlen(path) + 32;
    char* tmp = malloc(n);
    FILE* f = NULL;
    bool ok;
    if (!tmp)
        return false;
    snprintf(tmp, n, "%s.tmp.%ld", path, (long)getpid());
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
    ms_json_writer_string(&w, message ? message : "SHARPEMU operation failed");
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

static bool request_keys_allowed(const ms_json* root, const char* const allowed[], size_t allowed_count) {
    for (size_t i = 0; i < ms_json_object_length(root); ++i) {
        const char* key = ms_json_object_key_at(root, i);
        bool found = false;
        for (size_t j = 0; key && j < allowed_count; ++j)
            if (!strcmp(key, allowed[j])) {
                found = true;
                break;
            }
        if (!found)
            return false;
    }
    return true;
}

static bool remove_tree(const char* path);

static char* emulator_root(const char* home) {
    return join_path(home, "emulators/sharpemu");
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
    static const char* directories[] = {"versions",
                                        "downloads",
                                        "staging",
                                        "sessions",
                                        "logs",
                                        "home",
                                        "state",
                                        "state/saves",
                                        "state/custom-configs",
                                        "cache",
                                        "cache/dotnet-bundle",
                                        "cache/ampr-index",
                                        "cache/vulkan",
                                        "writable",
                                        "writable/tmp",
                                        "writable/temp0",
                                        "writable/download0",
                                        "writable/devlog",
                                        "writable/hostapp"};
    char* root = emulator_root(home);
    bool ok = root != NULL;
    for (size_t i = 0; ok && i < sizeof(directories) / sizeof(directories[0]); ++i) {
        char* path = join_path(root, directories[i]);
        ok = path && mkdir_p(path);
        if (ok)
            (void)chmod(path, 0700);
        free(path);
    }
    char* manifest = root ? join_path(root, "environment.json") : NULL;
    if (ok)
        cleanup_interrupted_updates(root);
    if (ok && manifest && access(manifest, F_OK) != 0)
        ok = write_atomic(manifest, "{\"schemaVersion\":1,\"provider\":\"sharpemu\",\"managedRuntime\":true,"
                                    "\"isolatedState\":true,\"channel\":\"stable\"}\n");
    free(manifest);
    free(root);
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

static bool contains_case(const char* value, const char* needle) {
    size_t length = strlen(needle);
    for (const char* p = value; *p; ++p)
        if (!strncasecmp(p, needle, length))
            return true;
    return false;
}

static bool valid_stable_tag(const char* tag) {
    if (!tag || tag[0] != 'v' || tag[1] < '0' || tag[1] > '9' || !is_safe_component(tag))
        return false;
    bool dot = false;
    for (const char* p = tag + 1; *p; ++p) {
        if (*p == '.')
            dot = true;
        else if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-'))
            return false;
    }
    return dot && !contains_case(tag, "alpha") && !contains_case(tag, "beta") && !contains_case(tag, "-rc");
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
    const char* override = getenv("METALSHARP_SHARPEMU_BIN");
    char *root, *path;
    if (override && override[0])
        return strdup(override);
    root = emulator_root(home);
    path = root ? join_path(root, "current/SharpEmu") : NULL;
    free(root);
    return path;
}

static const char* machine_arch(void) {
    const char* override = getenv("METALSHARP_SHARPEMU_HOST_ARCH");
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

static void release_free(sharpemu_release* release) {
    if (!release)
        return;
    free(release->tag);
    free(release->version);
    free(release->asset_name);
    free(release->url);
    free(release->digest);
    free(release->source_commit);
    free(release->published_at);
    memset(release, 0, sizeof(*release));
}

static char* release_field(const ms_json* object, const char* key) {
    char* value = NULL;
    (void)ms_json_as_string(ms_json_object_get(object, key), &value);
    return value;
}

static bool valid_commit_sha(const char* value) {
    if (!value || strlen(value) != 40)
        return false;
    for (const char* p = value; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            return false;
    return true;
}

static char* resolve_tag_commit(const char* tag) {
    char url[512];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/git/ref/tags/%s", SHARPEMU_STABLE_REPO, tag);
    const char* argv[] = {"/usr/bin/curl",
                          "--fail",
                          "--silent",
                          "--show-error",
                          "--location",
                          "--max-redirs",
                          "3",
                          "--proto",
                          "=https",
                          "--proto-redir",
                          "=https",
                          "--max-time",
                          "20",
                          "-A",
                          "MetalSharp-SharpEmu",
                          url,
                          NULL};
    char* text = run_capture(argv, 1024 * 1024);
    char error[160];
    ms_json* root = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
    const ms_json* object = root ? ms_json_object_get(root, "object") : NULL;
    char *sha = json_string(object, "sha"), *type = json_string(object, "type");
    free(text);
    ms_json_free(root);
    if (type && !strcmp(type, "tag") && valid_commit_sha(sha)) {
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/git/tags/%s", SHARPEMU_STABLE_REPO, sha);
        const char* tag_argv[] = {"/usr/bin/curl",
                                  "--fail",
                                  "--silent",
                                  "--show-error",
                                  "--location",
                                  "--max-redirs",
                                  "3",
                                  "--proto",
                                  "=https",
                                  "--proto-redir",
                                  "=https",
                                  "--max-time",
                                  "20",
                                  "-A",
                                  "MetalSharp-SharpEmu",
                                  url,
                                  NULL};
        text = run_capture(tag_argv, 1024 * 1024);
        root = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
        object = root ? ms_json_object_get(root, "object") : NULL;
        char *commit = json_string(object, "sha"), *object_type = json_string(object, "type");
        free(text);
        ms_json_free(root);
        free(sha);
        free(type);
        if (object_type && !strcmp(object_type, "commit") && valid_commit_sha(commit)) {
            free(object_type);
            return commit;
        }
        free(object_type);
        free(commit);
        return NULL;
    }
    free(type);
    if (valid_commit_sha(sha))
        return sha;
    free(sha);
    return NULL;
}

static bool load_release(const char* home, sharpemu_release* out, char* error, size_t error_size, bool force) {
    const char* fixture = getenv("METALSHARP_SHARPEMU_RELEASE_JSON");
    char *text = NULL, parse_error[160], *environment = NULL, *cache = NULL;
    ms_json* root;
    const ms_json* assets;
    const ms_json* asset = NULL;
    if (fixture && fixture[0])
        text = read_file(fixture, SHARPEMU_MAX_CAPTURE, NULL);
    else {
        struct stat cache_stat;
        environment = home ? emulator_root(home) : NULL;
        cache = environment ? join_path(environment, "release-cache.json") : NULL;
        if (!force && cache && stat(cache, &cache_stat) == 0 && S_ISREG(cache_stat.st_mode) &&
            time(NULL) - cache_stat.st_mtime >= 0 && time(NULL) - cache_stat.st_mtime < 12 * 60 * 60)
            text = read_file(cache, SHARPEMU_MAX_CAPTURE, NULL);
        const char* argv[] = {"/usr/bin/curl",
                              "--fail",
                              "--silent",
                              "--show-error",
                              "--location",
                              "--max-redirs",
                              "5",
                              "--proto",
                              "=https",
                              "--proto-redir",
                              "=https",
                              "--max-time",
                              "20",
                              "-A",
                              "MetalSharp-SharpEmu",
                              "https://api.github.com/repos/sharpemu/sharpemu/releases/latest",
                              NULL};
        if (!text) {
            text = run_capture(argv, SHARPEMU_MAX_CAPTURE);
            if (text && cache) {
                (void)ensure_environment(home);
                (void)write_atomic(cache, text);
            }
        }
    }
    free(environment);
    free(cache);
    if (!text) {
        snprintf(error, error_size, "failed to fetch the official SharpEmu release");
        return false;
    }
    root = ms_json_parse(text, strlen(text), parse_error, sizeof(parse_error));
    free(text);
    bool draft = false, prerelease = false;
    if (!root || ms_json_type_of(root) != MS_JSON_OBJECT ||
        (ms_json_as_bool(ms_json_object_get(root, "draft"), &draft) && draft) ||
        (ms_json_as_bool(ms_json_object_get(root, "prerelease"), &prerelease) && prerelease)) {
        ms_json_free(root);
        snprintf(error, error_size, "failed to parse a stable SharpEmu release response");
        return false;
    }
    out->tag = release_field(root, "tag_name");
    out->published_at = release_field(root, "published_at");
    out->source_commit = release_field(root, "metalsharp_source_commit");
    (void)ms_json_as_i64(ms_json_object_get(root, "id"), &out->release_id);
    if (!valid_stable_tag(out->tag)) {
        ms_json_free(root);
        release_free(out);
        snprintf(error, error_size, "the latest SharpEmu release tag is not a supported stable tag");
        return false;
    }
    char expected[256];
    snprintf(expected, sizeof(expected), "sharpemu-%s-osx-x64.tar.gz", out->tag + 1);
    assets = ms_json_object_get(root, "assets");
    for (size_t i = 0; i < ms_json_array_length(assets); ++i) {
        const ms_json* candidate = ms_json_array_get(assets, i);
        char* name = release_field(candidate, "name");
        bool match = name && !strcmp(name, expected);
        free(name);
        if (match) {
            if (asset) {
                ms_json_free(root);
                release_free(out);
                snprintf(error, error_size, "the SharpEmu release has duplicate macOS assets");
                return false;
            }
            asset = candidate;
        }
    }
    if (!asset) {
        ms_json_free(root);
        release_free(out);
        snprintf(error, error_size, "the official release has no exact macOS x64 asset");
        return false;
    }
    out->asset_name = release_field(asset, "name");
    out->url = release_field(asset, "browser_download_url");
    out->digest = release_field(asset, "digest");
    (void)ms_json_as_i64(ms_json_object_get(asset, "id"), &out->asset_id);
    long long size = 0;
    if (ms_json_as_i64(ms_json_object_get(asset, "size"), &size) && size > 0)
        out->size = (unsigned long long)size;
    out->version = strdup(out->tag + 1);
    ms_json_free(root);
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "https://github.com/%s/releases/download/%s/", SHARPEMU_STABLE_REPO, out->tag);
    if (!out->asset_name || strcmp(out->asset_name, expected) || !out->url ||
        strncmp(out->url, prefix, strlen(prefix)) || strcmp(out->url + strlen(prefix), expected) || !out->digest ||
        strncmp(out->digest, "sha256:", 7) || strlen(out->digest + 7) != 64 || out->size == 0 ||
        out->size > 512ULL * 1024ULL * 1024ULL || out->release_id <= 0 || out->asset_id <= 0) {
        release_free(out);
        snprintf(error, error_size, "the SharpEmu release metadata is incomplete or untrusted");
        return false;
    }
    for (const char* p = out->digest + 7; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
            release_free(out);
            snprintf(error, error_size, "the SharpEmu release digest is invalid");
            return false;
        }
    if (!out->source_commit)
        out->source_commit = resolve_tag_commit(out->tag);
    if (!valid_commit_sha(out->source_commit)) {
        release_free(out);
        snprintf(error, error_size, "the SharpEmu release tag could not be bound to a source commit");
        return false;
    }
    char *managed_root = home ? emulator_root(home) : NULL,
         *versions = managed_root ? join_path(managed_root, "versions") : NULL;
    char* version = versions ? join_path(versions, out->tag) : NULL;
    char* observed_path = version ? join_path(version, "source-manifest.json") : NULL;
    char* observed_text = observed_path ? read_file(observed_path, 1024 * 1024, NULL) : NULL;
    bool unchanged = true;
    if (observed_text) {
        char observed_error[160];
        ms_json* observed = ms_json_parse(observed_text, strlen(observed_text), observed_error, sizeof(observed_error));
        long long asset_id = 0, asset_size = 0;
        char *digest = json_string(observed, "assetDigest"), *url = json_string(observed, "assetUrl");
        char* source_commit = json_string(observed, "sourceCommit");
        unchanged = observed && ms_json_as_i64(ms_json_object_get(observed, "assetId"), &asset_id) &&
                    ms_json_as_i64(ms_json_object_get(observed, "assetSize"), &asset_size) &&
                    asset_id == out->asset_id && asset_size >= 0 && (unsigned long long)asset_size == out->size &&
                    digest && !strcmp(digest, out->digest) && url && !strcmp(url, out->url) && source_commit &&
                    !strcmp(source_commit, out->source_commit);
        free(digest);
        free(url);
        free(source_commit);
        ms_json_free(observed);
    }
    free(observed_text);
    free(observed_path);
    free(version);
    free(versions);
    free(managed_root);
    if (!unchanged) {
        release_free(out);
        snprintf(error, error_size, "the observed SharpEmu release asset changed upstream and was quarantined");
        return false;
    }
    return true;
}

typedef struct {
    char* pinned_tag;
    char* skipped_tag;
} sharpemu_update_policy;

static void load_update_policy(const char* home, sharpemu_update_policy* policy) {
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

static void free_update_policy(sharpemu_update_policy* policy) {
    free(policy->pinned_tag);
    free(policy->skipped_tag);
    memset(policy, 0, sizeof(*policy));
}

static bool save_update_policy(const char* home, const sharpemu_update_policy* policy) {
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
    sharpemu_release release = {0};
    sharpemu_update_policy policy;
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
    ms_json_writer_string(&w, "sharpemu");
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
    ms_json_writer_key(&w, "releaseId");
    ms_json_writer_i64(&w, release.release_id);
    ms_json_writer_key(&w, "assetId");
    ms_json_writer_i64(&w, release.asset_id);
    ms_json_writer_key(&w, "downloadSize");
    ms_json_writer_u64(&w, release.size);
    ms_json_writer_key(&w, "digest");
    ms_json_writer_string(&w, release.digest);
    ms_json_writer_key(&w, "upstreamNotarized");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "assetMutable");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "publishedAt");
    ms_json_writer_string(&w, release.published_at ? release.published_at : "");
    ms_json_writer_key(&w, "sourceCommit");
    ms_json_writer_string(&w, release.source_commit);
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

static const char* archive_tool(const char* name) {
    const char* candidates[] = {"/opt/homebrew/bin/lsar", "/usr/local/bin/lsar", "/usr/bin/lsar",
                                "/opt/homebrew/bin/unar", "/usr/local/bin/unar", "/usr/bin/unar"};
    size_t first = !strcmp(name, "lsar") ? 0 : 3;
    for (size_t i = first; i < first + 3; ++i)
        if (access(candidates[i], X_OK) == 0)
            return candidates[i];
    return NULL;
}

static bool archive_entry_safe(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n > 1024 || name[0] == '/' || strchr(name, '\\'))
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

static bool archive_entries_safe(const char* archive) {
    const char* lsar = archive_tool("lsar");
    if (!lsar)
        return false;
    const char* argv[] = {lsar, "-json", archive, NULL};
    char* listing = run_capture(argv, SHARPEMU_MAX_CAPTURE);
    char parse_error[160];
    ms_json* root = listing ? ms_json_parse(listing, strlen(listing), parse_error, sizeof(parse_error)) : NULL;
    const ms_json* entries = root ? ms_json_object_get(root, "lsarContents") : NULL;
    size_t count = ms_json_array_length(entries);
    char** names = count > 0 && count <= 512 ? calloc(count, sizeof(*names)) : NULL;
    bool ok = names != NULL;
    unsigned long long total_size = 0;
    for (size_t i = 0; ok && i < count; ++i) {
        const ms_json* entry = ms_json_array_get(entries, i);
        char* name = json_string(entry, "XADFileName");
        long long size = 0;
        bool root_directory = name && !strcmp(name, ".") && json_bool(entry, "XADIsDirectory", false);
        if (name && !strncmp(name, "./", 2))
            memmove(name, name + 2, strlen(name + 2) + 1);
        bool special = json_bool(entry, "XADIsLink", false) || json_bool(entry, "XADIsHardLink", false) ||
                       json_bool(entry, "XADIsCharacterDevice", false) || json_bool(entry, "XADIsBlockDevice", false) ||
                       json_bool(entry, "XADIsFIFO", false) || json_bool(entry, "TARIsSparseFile", false) ||
                       ms_json_object_get(entry, "XADLinkDestination") != NULL;
        if (!name || (!root_directory && !archive_entry_safe(name)) || special ||
            !ms_json_as_i64(ms_json_object_get(entry, "XADFileSize"), &size) || size < 0 ||
            (unsigned long long)size > 256ULL * 1024ULL * 1024ULL)
            ok = false;
        else {
            total_size += (unsigned long long)size;
            if (total_size > 512ULL * 1024ULL * 1024ULL)
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

static unsigned long long available_disk_bytes(const char* path) {
    struct statvfs info;
    return path && statvfs(path, &info) == 0 ? (unsigned long long)info.f_bavail * info.f_frsize : 0;
}

static int host_macos_major(void) {
    const char* override = getenv("METALSHARP_SHARPEMU_HOST_MACOS");
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
    size_t dependency_count = 0;
    for (char* line = ok ? strtok_r(output, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        if (*line != ' ' && *line != '\t')
            continue;
        while (*line == ' ' || *line == '\t')
            ++line;
        char* end = strchr(line, ' ');
        if (end)
            *end = '\0';
        dependency_count++;
        if (!strncmp(line, "/usr/lib/", 9) || !strncmp(line, "/System/Library/", 16) || !strncmp(line, "@rpath/", 7) ||
            !strncmp(line, "@loader_path/", 13) || !strncmp(line, "@executable_path/", 17))
            continue;
        ok = false;
        break;
    }
    free(output);
    return ok && dependency_count > 0;
}

static bool validate_macho_x86_64(const char* path) {
    const char* argv[] = {"/usr/bin/lipo", "-archs", path, NULL};
    char* output = run_capture(argv, 4096);
    bool ok = output && strstr(output, "x86_64");
    free(output);
    return ok;
}

static bool is_macho_file(const char* path) {
    const char* argv[] = {"/usr/bin/file", "-b", path, NULL};
    char* output = run_capture(argv, 4096);
    bool result = output && strstr(output, "Mach-O");
    free(output);
    return result;
}

static bool validate_runtime_tree(const char* path, unsigned depth, size_t* macho_count, int* minimum_macos) {
    if (depth > 8)
        return false;
    DIR* d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    struct dirent* entry;
    while (ok && (entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        struct stat st;
        if (!child || lstat(child, &st) != 0 || S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
            ok = false;
        else if (S_ISDIR(st.st_mode))
            ok = validate_runtime_tree(child, depth + 1, macho_count, minimum_macos);
        else if (is_macho_file(child)) {
            int component_min = macho_minimum_macos(child);
            (*macho_count)++;
            if (component_min > *minimum_macos)
                *minimum_macos = component_min;
            ok = validate_macho_x86_64(child) && validate_macho_dependencies(child);
        }
        free(child);
    }
    closedir(d);
    return ok;
}

static bool sign_runtime_tree(const char* path, unsigned depth, const char* main_executable) {
    if (depth > 8)
        return false;
    DIR* d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    struct dirent* entry;
    while (ok && (entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        struct stat st;
        if (!child || lstat(child, &st) != 0)
            ok = false;
        else if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
            ok = sign_runtime_tree(child, depth + 1, main_executable);
        else if (S_ISREG(st.st_mode) && strcmp(child, main_executable) && is_macho_file(child)) {
            const char* sign_argv[] = {"/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", child, NULL};
            const char* verify_argv[] = {"/usr/bin/codesign", "--verify", "--strict", child, NULL};
            ok = run_wait(sign_argv, NULL, NULL) == 0 && run_wait(verify_argv, NULL, NULL) == 0;
        }
        free(child);
    }
    closedir(d);
    return ok;
}

static bool make_tree_read_only(const char* path) {
    DIR* d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        struct stat st;
        if (!child || lstat(child, &st) != 0 || S_ISLNK(st.st_mode))
            ok = false;
        else if (S_ISDIR(st.st_mode)) {
            if (!make_tree_read_only(child) || chmod(child, 0500) != 0)
                ok = false;
        } else if (!S_ISREG(st.st_mode) || chmod(child, (st.st_mode & 0111) ? 0500 : 0400) != 0)
            ok = false;
        free(child);
    }
    closedir(d);
    return chmod(path, 0500) == 0 && ok;
}

static void make_tree_writable(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode))
        return;
    if (!S_ISDIR(st.st_mode)) {
        (void)chmod(path, 0600);
        return;
    }
    (void)chmod(path, 0700);
    DIR* d = opendir(path);
    if (!d)
        return;
    struct dirent* entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        if (child)
            make_tree_writable(child);
        free(child);
    }
    closedir(d);
}

static bool rosetta_available(void) {
    const char* forced = getenv("METALSHARP_SHARPEMU_ROSETTA");
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
        snprintf(error, error_size, "failed to stage the active SHARPEMU version");
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
    if (getenv("METALSHARP_SHARPEMU_FAIL_ACTIVATION")) {
        unlink(temp);
        snprintf(error, error_size, "SharpEmu activation was interrupted by the validation hook");
        goto done;
    }
    if (rename(temp, current) != 0) {
        unlink(temp);
        snprintf(error, error_size, "failed to activate the SHARPEMU version");
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
    sharpemu_release release;
} update_job;

static bool any_session_running(const char* home);
static bool active_runtime_valid(const char* home);
static bool verify_runtime_signatures(const char* path, unsigned depth);

static bool append_manifest_files(ms_json_writer* w, const char* root, const char* path, unsigned depth,
                                  size_t* count) {
    if (depth > 8 || *count > 512)
        return false;
    DIR* d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    struct dirent* entry;
    while (ok && (entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        struct stat st;
        if (!child || lstat(child, &st) != 0 || S_ISLNK(st.st_mode))
            ok = false;
        else if (S_ISDIR(st.st_mode))
            ok = append_manifest_files(w, root, child, depth + 1, count);
        else if (S_ISREG(st.st_mode)) {
            char digest[65];
            const char* relative = child + strlen(root);
            if (*relative == '/')
                ++relative;
            if (++(*count) > 512 || !file_sha256(child, digest))
                ok = false;
            else {
                ms_json_writer_object_begin(w);
                ms_json_writer_key(w, "path");
                ms_json_writer_string(w, relative);
                ms_json_writer_key(w, "size");
                ms_json_writer_u64(w, (unsigned long long)st.st_size);
                ms_json_writer_key(w, "sha256");
                ms_json_writer_string(w, digest);
                ms_json_writer_object_end(w);
            }
        } else
            ok = false;
        free(child);
    }
    closedir(d);
    return ok;
}

static bool write_file_manifest(const char* version_dir, const char* name, const sharpemu_release* release,
                                bool locally_signed, int minimum_macos) {
    char* path = join_path(version_dir, name);
    if (!path)
        return false;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schemaVersion");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "sharpemu");
    ms_json_writer_key(&w, "repository");
    ms_json_writer_string(&w, SHARPEMU_STABLE_REPO);
    ms_json_writer_key(&w, "tag");
    ms_json_writer_string(&w, release->tag);
    ms_json_writer_key(&w, "sourceCommit");
    ms_json_writer_string(&w, release->source_commit);
    ms_json_writer_key(&w, "releaseId");
    ms_json_writer_i64(&w, release->release_id);
    ms_json_writer_key(&w, "assetId");
    ms_json_writer_i64(&w, release->asset_id);
    ms_json_writer_key(&w, "assetName");
    ms_json_writer_string(&w, release->asset_name);
    ms_json_writer_key(&w, "assetUrl");
    ms_json_writer_string(&w, release->url);
    ms_json_writer_key(&w, "assetSize");
    ms_json_writer_u64(&w, release->size);
    ms_json_writer_key(&w, "assetDigest");
    ms_json_writer_string(&w, release->digest);
    ms_json_writer_key(&w, "runtimeArchitecture");
    ms_json_writer_string(&w, "x86_64");
    ms_json_writer_key(&w, "effectiveMinimumMacos");
    ms_json_writer_i64(&w, minimum_macos);
    ms_json_writer_key(&w, "upstreamNotarized");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "locallyAdHocSigned");
    ms_json_writer_bool(&w, locally_signed);
    ms_json_writer_key(&w, "files");
    ms_json_writer_array_begin(&w);
    size_t count = 0;
    bool ok = append_manifest_files(&w, version_dir, version_dir, 0, &count);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    ok = ok && text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static bool write_capability_manifest(const char* version_dir, const char* tag, int minimum_macos) {
    char* path = join_path(version_dir, "capabilities.json");
    if (!path)
        return false;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schemaVersion");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "sharpemu");
    ms_json_writer_key(&w, "runtimeTag");
    ms_json_writer_string(&w, tag);
    ms_json_writer_key(&w, "runtimeArchitecture");
    ms_json_writer_string(&w, "x86_64");
    ms_json_writer_key(&w, "effectiveMinimumMacos");
    ms_json_writer_i64(&w, minimum_macos);
    ms_json_writer_key(&w, "cliOnly");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "graphicsBackend");
    ms_json_writer_string(&w, "vulkan-moltenvk");
    ms_json_writer_key(&w, "networkDefault");
    ms_json_writer_string(&w, "denied");
    ms_json_writer_key(&w, "networkOptInAvailable");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "forbiddenFeatures");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "upstream-gui");
    ms_json_writer_string(&w, "upstream-updater");
    ms_json_writer_string(&w, "debug-server");
    ms_json_writer_string(&w, "native-metal");
    ms_json_writer_string(&w, "firmware-import");
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    bool ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static bool network_sandbox_available(void) {
    const char* forced = getenv("METALSHARP_SHARPEMU_SANDBOX");
    if (forced)
        return !strcmp(forced, "1") || !strcasecmp(forced, "true");
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static bool checked = false, available = false;
    pthread_mutex_lock(&mutex);
    if (checked) {
        bool cached = available;
        pthread_mutex_unlock(&mutex);
        return cached;
    }
    if (access("/usr/bin/sandbox-exec", X_OK) == 0 && access("/usr/bin/nc", X_OK) == 0) {
        const char* allow[] = {"/usr/bin/sandbox-exec", "-p", "(version 1)(allow default)", "/usr/bin/true", NULL};
        if (run_wait(allow, NULL, NULL) == 0) {
            int listener = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in address;
            socklen_t address_size = sizeof(address);
            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_port = 0;
            if (listener >= 0 && inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1 &&
                bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0 && listen(listener, 1) == 0 &&
                getsockname(listener, (struct sockaddr*)&address, &address_size) == 0) {
                char port[16];
                snprintf(port, sizeof(port), "%u", (unsigned)ntohs(address.sin_port));
                const char* deny[] = {"/usr/bin/sandbox-exec",
                                      "-p",
                                      "(version 1)(allow default)(deny network*)",
                                      "/usr/bin/nc",
                                      "-z",
                                      "-w",
                                      "1",
                                      "127.0.0.1",
                                      port,
                                      NULL};
                available = run_wait(deny, NULL, NULL) != 0;
            }
            if (listener >= 0)
                close(listener);
        }
    }
    checked = true;
    bool result = available;
    pthread_mutex_unlock(&mutex);
    return result;
}

static bool probe_test_hook_enabled(void) {
    const char* requested = getenv("METALSHARP_SHARPEMU_SKIP_PROBE_FOR_TESTS");
    uint32_t size = 0;
    (void)_NSGetExecutablePath(NULL, &size);
    char* executable = size > 0 && size < 64 * 1024 ? malloc(size) : NULL;
    bool development_binary = executable && _NSGetExecutablePath(executable, &size) == 0 &&
                              (strstr(executable, "/src-c/build/") || strstr(executable, "/src-c/build-asan/"));
    free(executable);
    return requested && (!strcmp(requested, "1") || !strcasecmp(requested, "true")) && development_binary &&
           getenv("METALSHARP_SHARPEMU_RELEASE_JSON") && getenv("METALSHARP_SHARPEMU_DOWNLOAD_FILE");
}

static bool moltenvk_loader_probe(const char* version_dir) {
    char* loader = join_path(version_dir, "libMoltenVK.dylib");
    if (!loader)
        return false;
    void* handle = dlopen(loader, RTLD_NOW | RTLD_LOCAL);
    typedef int (*enumerate_version_fn)(uint32_t*);
    enumerate_version_fn enumerate = NULL;
    if (handle)
        *(void**)(&enumerate) = dlsym(handle, "vkEnumerateInstanceVersion");
    uint32_t version = 0;
    bool ok = enumerate && enumerate(&version) == 0 && version != 0;
    if (handle)
        dlclose(handle);
    free(loader);
    return ok;
}

static void configure_isolated_environment(const char* root, const char* session_id, const char* runtime_digest,
                                           const char* log_path) {
    static const char* forbidden[] = {"SHARPEMU_WRITABLE_APP0",
                                      "SHARPEMU_DEBUG_SERVER",
                                      "SHARPEMU_NET_REDIRECT",
                                      "SHARPEMU_RENDERDOC",
                                      "SHARPEMU_GPU_BACKEND",
                                      "DYLD_INSERT_LIBRARIES",
                                      "DYLD_LIBRARY_PATH",
                                      "LD_PRELOAD",
                                      "DOTNET_STARTUP_HOOKS",
                                      "CORECLR_PROFILER",
                                      "HTTP_PROXY",
                                      "HTTPS_PROXY",
                                      "ALL_PROXY"};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i)
        unsetenv(forbidden[i]);
    char *isolated_home = join_path(root, "home"), *state = join_path(root, "state/saves");
    char *cache = join_path(root, "cache"), *writable = join_path(root, "writable");
    char* dotnet_base = cache ? join_path(cache, "dotnet-bundle") : NULL;
    char* dotnet = dotnet_base ? join_path(dotnet_base, runtime_digest ? runtime_digest : "runtime") : NULL;
    char* ampr = cache ? join_path(cache, "ampr-index") : NULL;
    char* vulkan_base = cache ? join_path(cache, "vulkan") : NULL;
    char* vulkan = vulkan_base ? join_path(vulkan_base, session_id) : NULL;
    char* tmp_base = writable ? join_path(writable, "tmp") : NULL;
    char* tmp = tmp_base ? join_path(tmp_base, session_id) : NULL;
    const char* names[] = {"temp0", "download0", "devlog", "hostapp"};
    char* session_paths[4] = {0};
    for (size_t i = 0; i < 4; ++i) {
        char* base = writable ? join_path(writable, names[i]) : NULL;
        session_paths[i] = base ? join_path(base, session_id) : NULL;
        free(base);
    }
    char* pipeline = vulkan ? join_path(vulkan, "pipeline.bin") : NULL;
    if (isolated_home)
        setenv("HOME", isolated_home, 1);
    if (dotnet) {
        (void)mkdir_p(dotnet);
        setenv("DOTNET_BUNDLE_EXTRACT_BASE_DIR", dotnet, 1);
    }
    if (tmp) {
        (void)mkdir_p(tmp);
        setenv("TMPDIR", tmp, 1);
    }
    if (state) {
        (void)mkdir_p(state);
        setenv("SHARPEMU_SAVEDATA_DIR", state, 1);
    }
    if (ampr) {
        (void)mkdir_p(ampr);
        setenv("SHARPEMU_AMPR_INDEX_CACHE", ampr, 1);
    }
    if (vulkan)
        (void)mkdir_p(vulkan);
    if (pipeline)
        setenv("SHARPEMU_VK_PIPELINE_CACHE_PATH", pipeline, 1);
    const char* variables[] = {"SHARPEMU_TEMP0_DIR", "SHARPEMU_DOWNLOAD0_DIR", "SHARPEMU_DEVLOG_APP_DIR",
                               "SHARPEMU_HOSTAPP_DIR"};
    for (size_t i = 0; i < 4; ++i)
        if (session_paths[i]) {
            (void)mkdir_p(session_paths[i]);
            setenv(variables[i], session_paths[i], 1);
        }
    if (log_path)
        setenv("SHARPEMU_LOG_FILE", log_path, 1);
    setenv("SHARPEMU_LOG_NO_COLOR", "1", 1);
    free(isolated_home);
    free(state);
    free(cache);
    free(writable);
    free(dotnet_base);
    free(dotnet);
    free(ampr);
    free(vulkan_base);
    free(vulkan);
    free(tmp_base);
    free(tmp);
    free(pipeline);
    for (size_t i = 0; i < 4; ++i)
        free(session_paths[i]);
}

static bool probe_runtime(const char* executable, const char* version_dir, const char* environment, const char* tag) {
    if (probe_test_hook_enabled())
        return true;
    int fds[2], status = 0;
    pid_t pid;
    char output[128 * 1024];
    size_t used = 0;
    char* probe_log = join_path(environment, "logs/probe.log");
    if (!probe_log || pipe(fds) != 0) {
        free(probe_log);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        free(probe_log);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (chdir(version_dir) != 0)
            _exit(126);
        configure_isolated_environment(environment, "probe", tag, probe_log);
        alarm(20);
        execl("/usr/bin/arch", "/usr/bin/arch", "-x86_64", executable, "--log-level=info", "--log-file", probe_log,
              "/nonexistent/metalsharp-sharpemu-probe/eboot.bin", (char*)NULL);
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
    free(probe_log);
    return WIFEXITED(status) && WEXITSTATUS(status) == 2 &&
           (strstr(output, "EBOOT file was not found") || strstr(output, "SharpEmu"));
}

static void* update_worker(void* raw) {
    update_job* job = raw;
    sharpemu_release* release = &job->release;
    char *root = emulator_root(job->home), *downloads = root ? join_path(root, "downloads") : NULL;
    char *staging = root ? join_path(root, "staging") : NULL, *versions = root ? join_path(root, "versions") : NULL;
    char *archive = NULL, *download = NULL, *stage = NULL, *version_dir = NULL, *source_exe = NULL;
    char *source_root = NULL, *dest_exe = NULL;
    char error[256] = "", sha[65];
    struct stat st;
    bool ok = false;
    const char* unar = archive_tool("unar");
    if (!root || !downloads || !staging || !versions || !unar || !archive_tool("lsar")) {
        snprintf(error, sizeof(error), "missing_archive_tools: SharpEmu requires lsar and unar");
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
    if (!archive || !download || !stage || !version_dir || !mkdir_p(stage)) {
        snprintf(error, sizeof(error), "failed to create SharpEmu update staging");
        goto done;
    }
    update_set("downloading", 15, "Downloading the official stable SharpEmu runtime", NULL);
    unlink(download);
    const char* fixture = getenv("METALSHARP_SHARPEMU_DOWNLOAD_FILE");
    char maximum_download[32];
    snprintf(maximum_download, sizeof(maximum_download), "%llu", release->size);
    const char* copy_argv[] = {"/bin/cp", fixture, download, NULL};
    const char* curl_argv[] = {"/usr/bin/curl",
                               "--fail",
                               "--location",
                               "--max-redirs",
                               "5",
                               "--proto",
                               "=https",
                               "--proto-redir",
                               "=https",
                               "--silent",
                               "--show-error",
                               "--max-filesize",
                               maximum_download,
                               "--output",
                               download,
                               "--write-out",
                               "%{url_effective}",
                               release->url,
                               NULL};
    char* final_url = NULL;
    int download_status;
    if (fixture && fixture[0])
        download_status = run_wait(copy_argv, NULL, NULL);
    else {
        final_url = run_capture(curl_argv, 4096);
        download_status = approved_download_url(final_url) ? 0 : -1;
    }
    free(final_url);
    if (download_status != 0 || lstat(download, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) ||
        (unsigned long long)st.st_size != release->size) {
        snprintf(error, sizeof(error), "SharpEmu download failed or had an unexpected size");
        goto done;
    }
    update_set("verifying", 35, "Verifying SharpEmu SHA-256 and archive structure", NULL);
    if (!file_sha256(download, sha) || strcasecmp(sha, release->digest + 7)) {
        snprintf(error, sizeof(error), "SharpEmu download digest did not match the official release");
        goto done;
    }
    if (rename(download, archive) != 0 || !archive_entries_safe(archive)) {
        snprintf(error, sizeof(error), "SharpEmu archive failed path-safety validation");
        goto done;
    }
    update_set("extracting", 50, "Extracting SharpEmu into isolated staging", NULL);
    const char* extract_argv[] = {unar, "-q", "-o", stage, archive, NULL};
    if (run_wait(extract_argv, NULL, NULL) != 0 || !symlinks_stay_inside(stage, stage)) {
        snprintf(error, sizeof(error), "SharpEmu archive extraction failed validation");
        goto done;
    }
    source_exe = find_stage_file(stage, "SharpEmu", 0);
    if (!source_exe || access(source_exe, X_OK) != 0) {
        snprintf(error, sizeof(error), "SharpEmu release is missing its executable");
        goto done;
    }
    source_root = strdup(source_exe);
    char* slash = source_root ? strrchr(source_root, '/') : NULL;
    if (!slash) {
        snprintf(error, sizeof(error), "SharpEmu release layout is invalid");
        goto done;
    }
    *slash = '\0';
    char *molten = join_path(source_root, "libMoltenVK.dylib"), *vulkan = join_path(source_root, "libvulkan.1.dylib");
    char *plugins = join_path(source_root, "plugins"), *license = join_path(source_root, "LICENSE.txt");
    size_t macho_count = 0;
    int minimum_macos = -1;
    bool layout_ok = molten && vulkan && plugins && license && access(molten, R_OK) == 0 && access(vulkan, R_OK) == 0 &&
                     access(plugins, R_OK) == 0 && access(license, R_OK) == 0;
    if (!layout_ok || !validate_runtime_tree(source_root, 0, &macho_count, &minimum_macos) || macho_count < 3) {
        snprintf(error, sizeof(error), "SharpEmu runtime architecture or dependencies failed validation");
        free(molten);
        free(vulkan);
        free(plugins);
        free(license);
        goto done;
    }
    free(molten);
    free(vulkan);
    free(plugins);
    free(license);
    if (minimum_macos < SHARPEMU_EFFECTIVE_MIN_MACOS || host_macos_major() < minimum_macos) {
        snprintf(error, sizeof(error), "SharpEmu requires macOS %d or newer", minimum_macos);
        goto done;
    }
    if (access(version_dir, F_OK) == 0) {
        make_tree_writable(version_dir);
        if (!remove_tree(version_dir)) {
            snprintf(error, sizeof(error), "failed to replace an incomplete SharpEmu version");
            goto done;
        }
    }
    update_set("validating", 68, "Recording provenance and locally signing SharpEmu", NULL);
    if (!write_file_manifest(source_root, "source-manifest.json", release, false, minimum_macos) ||
        rename(source_root, version_dir) != 0) {
        snprintf(error, sizeof(error), "failed to record or move SharpEmu provenance");
        goto done;
    }
    free(source_root);
    source_root = NULL;
    dest_exe = join_path(version_dir, "SharpEmu");
    if (!dest_exe || !sign_runtime_tree(version_dir, 0, dest_exe)) {
        snprintf(error, sizeof(error), "failed to locally sign SharpEmu native dependencies");
        goto done;
    }
    const char* sign_main[] = {"/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", dest_exe, NULL};
    const char* verify_main[] = {"/usr/bin/codesign", "--verify", "--strict", dest_exe, NULL};
    if (run_wait(sign_main, NULL, NULL) != 0 || run_wait(verify_main, NULL, NULL) != 0 ||
        (!probe_test_hook_enabled() && !moltenvk_loader_probe(version_dir)) ||
        !probe_runtime(dest_exe, version_dir, root, release->tag) ||
        !write_capability_manifest(version_dir, release->tag, minimum_macos) ||
        !write_file_manifest(version_dir, "activation-manifest.json", release, true, minimum_macos)) {
        snprintf(error, sizeof(error), "SharpEmu signature, CLI probe, or activation manifest failed");
        goto done;
    }
    while (any_session_running(job->home)) {
        update_set("waiting_for_exit", 85, "SharpEmu update will activate after active games exit", NULL);
        sleep(1);
    }
    if (!make_tree_read_only(version_dir)) {
        snprintf(error, sizeof(error), "failed to make the SharpEmu version immutable");
        goto done;
    }
    update_set("activating", 94, "Atomically activating SharpEmu", NULL);
    if (!switch_version(root, release->tag, error, sizeof(error)))
        goto done;
    ok = true;
done:
    if (stage)
        (void)remove_tree(stage);
    if (!ok && version_dir) {
        make_tree_writable(version_dir);
        (void)remove_tree(version_dir);
    }
    if (!ok && archive)
        unlink(archive);
    if (download)
        unlink(download);
    update_set(ok ? "completed" : "failed", ok ? 100 : 0, ok ? "SharpEmu is ready" : "SharpEmu update failed",
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
    free(source_root);
    free(dest_exe);
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
    if (strcmp(machine_arch(), "arm64") && strcmp(machine_arch(), "x86_64"))
        return error_json("SharpEmu requires an x86-64 or Apple Silicon Mac");
    if (host_macos_major() < SHARPEMU_EFFECTIVE_MIN_MACOS)
        return error_json("the current SharpEmu runtime requires macOS 26 or newer");
    if (!strcmp(machine_arch(), "arm64") && !rosetta_available())
        return error_json("Rosetta 2 is required to run SharpEmu");
    if (!ensure_environment(home))
        return error_json("failed to create the isolated SharpEmu environment");
    char* environment = emulator_root(home);
    unsigned long long free_bytes = available_disk_bytes(environment);
    free(environment);
    if (free_bytes > 0 && free_bytes < 1024ULL * 1024ULL * 1024ULL)
        return error_json("at least 1 GiB of available disk space is required to install and stage SharpEmu");
    pthread_mutex_lock(&g_update.mutex);
    if (g_update.running) {
        pthread_mutex_unlock(&g_update.mutex);
        return error_json("a SharpEmu update is already running");
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
        update_set("failed", 0, "SharpEmu update failed", error[0] ? error : "failed to prepare update");
        return error_json(error[0] ? error : "failed to prepare update");
    }
    installed_tag = current_tag(home);
    bool installed_ready = active_runtime_valid(home);
    if (installed_ready && installed_tag && !strcmp(installed_tag, job->release.tag)) {
        free(installed_tag);
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("idle", 0, "SharpEmu is already up to date", NULL);
        return error_json("SharpEmu is already up to date");
    }
    free(installed_tag);
    pthread_mutex_lock(&g_update.mutex);
    snprintf(g_update.target, sizeof(g_update.target), "%s", job->release.tag);
    pthread_mutex_unlock(&g_update.mutex);
    if (pthread_create(&thread, NULL, update_worker, job) != 0) {
        release_free(&job->release);
        free(job->home);
        free(job);
        update_set("failed", 0, "SharpEmu update failed", "failed to start update worker");
        return error_json("failed to start SharpEmu update worker");
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
        return error_json("failed to resolve SHARPEMU environment");
    if (update_running()) {
        free(root);
        return error_json("wait for the SharpEmu update transaction before rolling back");
    }
    if (any_session_running(home)) {
        free(root);
        return error_json("stop SHARPEMU before rolling back");
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
        char* executable = candidate ? join_path(candidate, "SharpEmu") : NULL;
        char* manifest = candidate ? join_path(candidate, "activation-manifest.json") : NULL;
        if (executable && manifest && access(executable, X_OK) == 0 && access(manifest, R_OK) == 0 &&
            verify_runtime_signatures(candidate, 0))
            ok = switch_version(root, tag, error, sizeof(error));
        free(candidate);
        free(executable);
        free(manifest);
    }
    free(root);
    free(previous);
    if (!ok)
        return error_json("no valid previous SHARPEMU version is available");
    return ms_sharpemu_status_json(home);
}

static unsigned long long path_hash(const char* value) {
    unsigned long long h = 1469598103934665603ULL;
    while (*value) {
        h ^= (unsigned char)*value++;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool game_exists(const sharpemu_games* games, const char* executable) {
    for (size_t i = 0; i < games->count; ++i)
        if (!strcmp(games->items[i].launch_path, executable))
            return true;
    return false;
}

static bool valid_ppsa_id(const char* value) {
    if (!value || strncmp(value, "PPSA", 4) || strlen(value) != 9)
        return false;
    for (size_t i = 4; i < 9; ++i)
        if (value[i] < '0' || value[i] > '9')
            return false;
    return true;
}

static void sanitize_metadata(char* value, size_t limit) {
    if (!value)
        return;
    size_t out = 0;
    for (size_t i = 0; value[i] && out + 1 < limit; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c >= 32 && c != 127)
            value[out++] = value[i];
        else if (out > 0 && value[out - 1] != ' ')
            value[out++] = ' ';
    }
    while (out > 0 && value[out - 1] == ' ')
        --out;
    value[out] = '\0';
}

static bool valid_eboot(const char* path, unsigned long long* size_out, unsigned long long* device_out,
                        unsigned long long* inode_out, long long* mtime_out) {
    struct stat st;
    unsigned char header[32] = {0};
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 64 ||
        (unsigned long long)st.st_size > 32ULL * 1024ULL * 1024ULL * 1024ULL || read(fd, header, sizeof(header)) < 8) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    close(fd);
    bool elf = header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F';
    bool fself = ((header[0] == 0x54 && header[1] == 0x14 && header[2] == 0xf5 && header[3] == 0xee) ||
                  (header[0] == 0x4f && header[1] == 0x15 && header[2] == 0x3d && header[3] == 0x1d)) &&
                 header[5] == 0x01 && header[6] == 0x01 && header[7] == 0x12;
    if (size_out)
        *size_out = (unsigned long long)st.st_size;
    if (device_out)
        *device_out = (unsigned long long)st.st_dev;
    if (inode_out)
        *inode_out = (unsigned long long)st.st_ino;
    if (mtime_out)
        *mtime_out = (long long)st.st_mtime;
    return elf || fself;
}

static bool valid_png(const char* path) {
    struct stat st;
    unsigned char h[24];
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 24 || st.st_size > 32 * 1024 * 1024 ||
        read(fd, h, sizeof(h)) != (ssize_t)sizeof(h)) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    close(fd);
    static const unsigned char magic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    unsigned long width =
        ((unsigned long)h[16] << 24) | ((unsigned long)h[17] << 16) | ((unsigned long)h[18] << 8) | h[19];
    unsigned long height =
        ((unsigned long)h[20] << 24) | ((unsigned long)h[21] << 16) | ((unsigned long)h[22] << 8) | h[23];
    return !memcmp(h, magic, sizeof(magic)) && !memcmp(h + 12, "IHDR", 4) && width > 0 && height > 0 && width <= 8192 &&
           height <= 8192 && width * height <= 33554432UL;
}

static char* localized_title(const ms_json* root) {
    const ms_json* localized = ms_json_object_get(root, "localizedParameters");
    if (ms_json_type_of(localized) != MS_JSON_OBJECT)
        return NULL;
    char* language = json_string(localized, "defaultLanguage");
    const ms_json* block = language ? ms_json_object_get(localized, language) : NULL;
    char* title = json_string(block, "titleName");
    free(language);
    if (title)
        return title;
    for (size_t i = 0; i < ms_json_object_length(localized); ++i) {
        const ms_json* candidate = ms_json_object_value_at(localized, i);
        title = json_string(candidate, "titleName");
        if (title)
            return title;
    }
    return NULL;
}

static void add_eboot_game(sharpemu_games* games, const char* eboot) {
    if (games->count >= SHARPEMU_MAX_GAMES || game_exists(games, eboot))
        return;
    char canonical[4096];
    unsigned long long executable_size = 0, executable_device = 0, executable_inode = 0;
    long long executable_mtime = 0;
    if (!realpath(eboot, canonical) ||
        !valid_eboot(canonical, &executable_size, &executable_device, &executable_inode, &executable_mtime))
        return;
    char game_root[4096];
    const char* slash = strrchr(canonical, '/');
    if (!slash || (size_t)(slash - canonical) == 0 || (size_t)(slash - canonical) >= sizeof(game_root))
        return;
    memcpy(game_root, canonical, (size_t)(slash - canonical));
    game_root[slash - canonical] = '\0';
    char *sce = join_path(game_root, "sce_sys"), *metadata = sce ? join_path(sce, "param.json") : NULL;
    if (!metadata || access(metadata, R_OK) != 0) {
        free(metadata);
        metadata = join_path(game_root, "param.json");
    }
    char* text = metadata ? read_file(metadata, 1024 * 1024, NULL) : NULL;
    char parse_error[160];
    ms_json* root = text ? ms_json_parse(text, strlen(text), parse_error, sizeof(parse_error)) : NULL;
    sharpemu_game* game = &games->items[games->count];
    memset(game, 0, sizeof(*game));
    snprintf(game->path, sizeof(game->path), "%s", game_root);
    snprintf(game->launch_path, sizeof(game->launch_path), "%s", canonical);
    game->executable_size = executable_size;
    game->executable_device = executable_device;
    game->executable_inode = executable_inode;
    game->executable_mtime = executable_mtime;
    if (root && ms_json_type_of(root) == MS_JSON_OBJECT) {
        char *title_id = json_string(root, "titleId"), *content = json_string(root, "contentVersion");
        char *master = json_string(root, "masterVersion"), *title = localized_title(root);
        if (title_id && valid_ppsa_id(title_id))
            snprintf(game->title_id, sizeof(game->title_id), "%s", title_id);
        if (content)
            snprintf(game->content_version, sizeof(game->content_version), "%s", content);
        if (master)
            snprintf(game->master_version, sizeof(game->master_version), "%s", master);
        if (title)
            snprintf(game->title, sizeof(game->title), "%s", title);
        free(title_id);
        free(content);
        free(master);
        free(title);
        snprintf(game->metadata_path, sizeof(game->metadata_path), "%s", metadata);
    }
    ms_json_free(root);
    free(text);
    sanitize_metadata(game->title, sizeof(game->title));
    sanitize_metadata(game->content_version, sizeof(game->content_version));
    sanitize_metadata(game->master_version, sizeof(game->master_version));
    if (!game->title[0]) {
        const char* base = strrchr(game_root, '/');
        snprintf(game->title, sizeof(game->title), "%s", base && base[1] ? base + 1 : "PlayStation 5 Game");
    }
    const char* art_names[] = {"icon0.png", "pic0.png", "pic1.png"};
    for (size_t i = 0; sce && i < sizeof(art_names) / sizeof(art_names[0]); ++i) {
        char* image = join_path(sce, art_names[i]);
        if (image && valid_png(image)) {
            snprintf(game->icon_path, sizeof(game->icon_path), "%s", image);
            free(image);
            break;
        }
        free(image);
    }
    snprintf(game->id, sizeof(game->id), "%s-%llx", game->title_id[0] ? game->title_id : "ps5",
             path_hash(game->launch_path));
    games->count++;
    free(sce);
    free(metadata);
}

static void scan_directory(sharpemu_games* games, const char* root, unsigned depth) {
    DIR* d;
    struct dirent* entry;
    if (depth > 8 || games->count >= SHARPEMU_MAX_GAMES || games->scanned_entries >= SHARPEMU_MAX_SCAN_ENTRIES ||
        !(d = opendir(root)))
        return;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++games->scanned_entries > SHARPEMU_MAX_SCAN_ENTRIES) {
            games->truncated = true;
            break;
        }
        char* path = join_path(root, entry->d_name);
        struct stat st;
        if (!path)
            continue;
        if (!strcmp(entry->d_name, "eboot.bin"))
            add_eboot_game(games, path);
        else if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
            scan_directory(games, path, depth + 1);
        free(path);
        if (games->count >= SHARPEMU_MAX_GAMES) {
            games->truncated = true;
            break;
        }
    }
    closedir(d);
}

static size_t load_roots(const char* home, char* roots[SHARPEMU_MAX_ROOTS]) {
    char *root = emulator_root(home), *library = root ? join_path(root, "state/roots.json") : NULL;
    char* text = library ? read_file(library, 1024 * 1024, NULL) : NULL;
    size_t count = 0;
    if (text) {
        char error[128];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        const ms_json* array = ms_json_object_get(json, "roots");
        for (size_t i = 0; i < ms_json_array_length(array) && count < SHARPEMU_MAX_ROOTS; ++i) {
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
    char *root = emulator_root(home), *library = root ? join_path(root, "state/roots.json") : NULL;
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

static void collect_games(const char* home, sharpemu_games* games) {
    char* roots[SHARPEMU_MAX_ROOTS] = {0};
    size_t count;
    memset(games, 0, sizeof(*games));
    count = load_roots(home, roots);
    for (size_t i = 0; i < count; ++i) {
        scan_directory(games, roots[i], 0);
        free(roots[i]);
    }
}

static bool save_game_cache(const char* home, const sharpemu_games* games) {
    char *root = emulator_root(home), *path = root ? join_path(root, "library-cache.json") : NULL;
    if (!path) {
        free(root);
        return false;
    }
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schemaVersion");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "games");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < games->count; ++i) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_string(&w, games->items[i].id);
        ms_json_writer_key(&w, "launchPath");
        ms_json_writer_string(&w, games->items[i].launch_path);
        ms_json_writer_key(&w, "executableSize");
        ms_json_writer_u64(&w, games->items[i].executable_size);
        ms_json_writer_key(&w, "executableDevice");
        ms_json_writer_u64(&w, games->items[i].executable_device);
        ms_json_writer_key(&w, "executableInode");
        ms_json_writer_u64(&w, games->items[i].executable_inode);
        ms_json_writer_key(&w, "executableMtime");
        ms_json_writer_i64(&w, games->items[i].executable_mtime);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    bool ok = text && write_atomic(path, text);
    free(text);
    free(path);
    free(root);
    return ok;
}

static bool cached_launch_target(const char* home, const char* id, char path_out[4096],
                                 unsigned long long* expected_size, unsigned long long* expected_device,
                                 unsigned long long* expected_inode, long long* expected_mtime) {
    char *root = emulator_root(home), *path = root ? join_path(root, "library-cache.json") : NULL;
    char* text = path ? read_file(path, 2 * 1024 * 1024, NULL) : NULL;
    bool found = false;
    if (text) {
        char error[160];
        ms_json* json = ms_json_parse(text, strlen(text), error, sizeof(error));
        const ms_json* games = ms_json_object_get(json, "games");
        for (size_t i = 0; i < ms_json_array_length(games); ++i) {
            const ms_json* game = ms_json_array_get(games, i);
            char *cached_id = json_string(game, "id"), *launch_path = json_string(game, "launchPath");
            long long size = 0, device = 0, inode = 0, mtime = 0;
            if (cached_id && launch_path && !strcmp(cached_id, id) && strlen(launch_path) < 4096 &&
                ms_json_as_i64(ms_json_object_get(game, "executableSize"), &size) && size > 0 &&
                ms_json_as_i64(ms_json_object_get(game, "executableDevice"), &device) && device >= 0 &&
                ms_json_as_i64(ms_json_object_get(game, "executableInode"), &inode) && inode > 0 &&
                ms_json_as_i64(ms_json_object_get(game, "executableMtime"), &mtime)) {
                snprintf(path_out, 4096, "%s", launch_path);
                *expected_size = (unsigned long long)size;
                *expected_device = (unsigned long long)device;
                *expected_inode = (unsigned long long)inode;
                *expected_mtime = mtime;
                found = true;
            }
            free(cached_id);
            free(launch_path);
            if (found)
                break;
        }
        ms_json_free(json);
    }
    free(text);
    free(path);
    free(root);
    return found;
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

static bool process_matches_sharpemu(pid_t pid, const char* executable, time_t recorded_start) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "command=", NULL};
    char* output = run_capture(argv, 64 * 1024);
    char* command = output;
    while (command && (*command == ' ' || *command == '\t'))
        ++command;
    size_t executable_length = executable ? strlen(executable) : 0;
    char* executable_at = command && executable_length > 0 ? strstr(command, executable) : NULL;
    bool command_matches = executable_at &&
                           (executable_at == command || executable_at[-1] == ' ' || executable_at[-1] == '\t') &&
                           (executable_at[executable_length] == '\0' || executable_at[executable_length] == ' ' ||
                            executable_at[executable_length] == '\t' || executable_at[executable_length] == '\n');
    free(output);
    long elapsed = process_elapsed_seconds(pid);
    time_t estimated_start = elapsed >= 0 ? time(NULL) - elapsed : 0;
    long long delta = estimated_start > recorded_start ? (long long)(estimated_start - recorded_start)
                                                       : (long long)(recorded_start - estimated_start);
    return command_matches && elapsed >= 0 && recorded_start > 0 && delta <= 10;
}

static bool process_is_sharpemu(pid_t pid) {
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)pid);
    const char* argv[] = {"/bin/ps", "-p", value, "-o", "command=", NULL};
    char* command = run_capture(argv, 64 * 1024);
    bool ok = command && strstr(command, "/SharpEmu");
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
                process_matches_sharpemu(candidate, executable, (time_t)started_at))
                pid = candidate;
            else if (waited < 0 && errno == ECHILD && kill(candidate, 0) == 0 &&
                     process_matches_sharpemu(candidate, executable, (time_t)started_at))
                pid = candidate;
            else if (waited == candidate) {
                if (log_path) {
                    FILE* log = fopen(log_path, "ab");
                    if (log) {
                        if (WIFEXITED(status))
                            fprintf(log, "\nMetalSharp: SharpEmu exited with status %d\n", WEXITSTATUS(status));
                        else if (WIFSIGNALED(status))
                            fprintf(log, "\nMetalSharp: SharpEmu exited from signal %d\n", WTERMSIG(status));
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
                         const char* runtime_tag, const char* log, bool network_enabled) {
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
    ms_json_writer_key(&w, "networkEnabled");
    ms_json_writer_bool(&w, network_enabled);
    ms_json_writer_key(&w, "startedAt");
    ms_json_writer_i64(&w, (long long)time(NULL));
    ms_json_writer_object_end(&w);
    char* text = ms_json_writer_take(&w);
    ok = text && write_atomic(path, text);
    free(text);
    free(path);
    return ok;
}

static char* spawn_sharpemu(const char* home, const char* id, const char* target, unsigned long long expected_size,
                            unsigned long long expected_device, unsigned long long expected_inode,
                            long long expected_mtime, bool fullscreen, bool network_enabled) {
    char *exe = executable_path(home), *root = emulator_root(home), *logs = root ? join_path(root, "logs") : NULL;
    char *version_dir = root ? join_path(root, "current") : NULL, *log = NULL, *runtime_tag = NULL;
    char log_name[192];
    unsigned long long actual_size = 0, actual_device = 0, actual_inode = 0;
    long long actual_mtime = 0;
    if (!target || !exe || !active_runtime_valid(home) || access(exe, X_OK) != 0 ||
        !valid_eboot(target, &actual_size, &actual_device, &actual_inode, &actual_mtime) ||
        actual_size != expected_size || actual_device != expected_device || actual_inode != expected_inode ||
        actual_mtime != expected_mtime)
        goto invalid;
    if (!ensure_environment(home))
        goto environment_error;
    if (!network_enabled && !network_sandbox_available())
        goto sandbox_error;
    if (session_pid(home, id) > 0)
        goto running_error;
    struct timespec log_time = {0};
    (void)clock_gettime(CLOCK_REALTIME, &log_time);
    snprintf(log_name, sizeof(log_name), "%s-%lld-%09ld.log", id, (long long)log_time.tv_sec, log_time.tv_nsec);
    log = logs ? join_path(logs, log_name) : NULL;
    if (!log)
        goto logging_error;
    pid_t pid = fork();
    if (pid < 0)
        goto launch_error;
    if (pid == 0) {
        int fd = open(log, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        setpgid(0, 0);
        if (fd < 0)
            _exit(126);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        if (chdir(version_dir) != 0)
            _exit(126);
        runtime_tag = current_tag(home);
        configure_isolated_environment(root, id, runtime_tag, log);
        const char* window = fullscreen ? "--window-mode=exclusive" : "--window-mode=windowed";
        if (network_enabled)
            execl("/usr/bin/arch", "/usr/bin/arch", "-x86_64", exe, "--cpu-engine=native", "--log-level=info",
                  "--log-file", log, window, "--scaling=fit", "--vsync=on", target, (char*)NULL);
        else
            execl("/usr/bin/sandbox-exec", "/usr/bin/sandbox-exec", "-p", "(version 1)(allow default)(deny network*)",
                  "/usr/bin/arch", "-x86_64", exe, "--cpu-engine=native", "--log-level=info", "--log-file", log, window,
                  "--scaling=fit", "--vsync=on", target, (char*)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    runtime_tag = current_tag(home);
    if (!save_session(home, id, pid, exe, target, runtime_tag, log, network_enabled)) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        free(exe);
        free(root);
        free(logs);
        free(version_dir);
        free(log);
        free(runtime_tag);
        return error_json("failed to persist SharpEmu process supervision state");
    }
    bool started = false;
    time_t recorded_start = time(NULL);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (process_matches_sharpemu(pid, exe, recorded_start)) {
            started = true;
            break;
        }
        if (kill(pid, 0) != 0)
            break;
        struct timespec pause = {0, 10 * 1000 * 1000};
        (void)nanosleep(&pause, NULL);
    }
    if (!started) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        char* persisted = session_path(home, id);
        if (persisted)
            unlink(persisted);
        free(persisted);
        free(exe);
        free(root);
        free(logs);
        free(version_dir);
        free(log);
        free(runtime_tag);
        return error_json("SharpEmu exited before process supervision became ready");
    }
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "sharpemu");
    ms_json_writer_key(&w, "pid");
    ms_json_writer_i64(&w, pid);
    ms_json_writer_key(&w, "networkEnabled");
    ms_json_writer_bool(&w, network_enabled);
    ms_json_writer_key(&w, "logPath");
    ms_json_writer_string(&w, log);
    ms_json_writer_object_end(&w);
    char* result = ms_json_writer_take(&w);
    free(exe);
    free(root);
    free(logs);
    free(version_dir);
    free(log);
    free(runtime_tag);
    return result;
invalid:
    free(exe);
    free(root);
    free(logs);
    free(version_dir);
    return error_json("SharpEmu is not installed or the indexed eboot.bin changed");
environment_error:
    free(exe);
    free(root);
    free(logs);
    free(version_dir);
    return error_json("failed to prepare the isolated SharpEmu environment");
sandbox_error:
    free(exe);
    free(root);
    free(logs);
    free(version_dir);
    return error_json("network containment is unavailable; explicit network opt-in is required to launch");
running_error:
    free(exe);
    free(root);
    free(logs);
    free(version_dir);
    return error_json("this SharpEmu game is already running");
logging_error:
    free(exe);
    free(root);
    free(logs);
    free(version_dir);
    return error_json("failed to prepare SharpEmu logging");
launch_error:
    free(exe);
    free(root);
    free(logs);
    free(version_dir);
    free(log);
    return error_json("failed to start SharpEmu");
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
        return error_json("SHARPEMU session is not running");
    }
    if (!process_is_sharpemu(pid)) {
        free(path);
        return error_json("refusing to stop a process that is not SHARPEMU");
    }
    (void)kill(-pid, SIGINT);
    (void)kill(pid, SIGINT);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD && kill(pid, 0) != 0))
            break;
        struct timespec pause = {0, 50 * 1000 * 1000};
        nanosleep(&pause, NULL);
        if (attempt == 59) {
            (void)kill(-pid, SIGTERM);
            (void)kill(pid, SIGTERM);
        }
        if (attempt == 99) {
            (void)kill(-pid, SIGKILL);
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
        }
    }
    save_exit_record(home, id, NULL, NULL, -1, SIGINT);
    if (path)
        unlink(path);
    free(path);
    return strdup("{\"ok\":true,\"running\":false}");
}

static bool verify_runtime_signatures(const char* path, unsigned depth) {
    if (depth > 8)
        return false;
    DIR* directory = opendir(path);
    if (!directory)
        return false;
    bool ok = true;
    struct dirent* entry;
    while (ok && (entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        struct stat st;
        if (!child || lstat(child, &st) != 0 || S_ISLNK(st.st_mode))
            ok = false;
        else if (S_ISDIR(st.st_mode))
            ok = verify_runtime_signatures(child, depth + 1);
        else if (S_ISREG(st.st_mode) && is_macho_file(child)) {
            const char* argv[] = {"/usr/bin/codesign", "--verify", "--strict", child, NULL};
            ok = run_wait(argv, NULL, NULL) == 0;
        }
        free(child);
    }
    closedir(directory);
    return ok;
}

typedef struct {
    pthread_mutex_t mutex;
    char path[4096];
    uint64_t fingerprint;
    bool valid;
} runtime_validation_cache;

static runtime_validation_cache g_runtime_validation = {PTHREAD_MUTEX_INITIALIZER, "", 0, false};

static bool runtime_tree_fingerprint(const char* path, unsigned depth, uint64_t* hash, size_t* regular_files) {
    if (depth > 8)
        return false;
    DIR* directory = opendir(path);
    if (!directory)
        return false;
    bool ok = true;
    struct dirent* entry;
    while (ok && (entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join_path(path, entry->d_name);
        struct stat st;
        if (!child || lstat(child, &st) != 0 || S_ISLNK(st.st_mode))
            ok = false;
        else {
            for (const unsigned char* p = (const unsigned char*)child; *p; ++p) {
                *hash ^= *p;
                *hash *= 1099511628211ULL;
            }
            *hash ^= (uint64_t)st.st_ino ^ (uint64_t)st.st_size ^ (uint64_t)st.st_mtime;
            *hash *= 1099511628211ULL;
            if (S_ISDIR(st.st_mode))
                ok = runtime_tree_fingerprint(child, depth + 1, hash, regular_files);
            else if (S_ISREG(st.st_mode))
                (*regular_files)++;
            else
                ok = false;
        }
        free(child);
    }
    closedir(directory);
    return ok;
}

static bool active_runtime_valid(const char* home) {
    char *exe = executable_path(home), *environment = emulator_root(home);
    char* version = environment ? join_path(environment, "current") : NULL;
    char* manifest = version ? join_path(version, "activation-manifest.json") : NULL;
    uint64_t fingerprint = 1469598103934665603ULL;
    size_t regular_files = 0;
    bool fingerprint_ok = version && runtime_tree_fingerprint(version, 0, &fingerprint, &regular_files);
    pthread_mutex_lock(&g_runtime_validation.mutex);
    if (fingerprint_ok && !strcmp(g_runtime_validation.path, version) &&
        g_runtime_validation.fingerprint == fingerprint) {
        bool cached = g_runtime_validation.valid;
        pthread_mutex_unlock(&g_runtime_validation.mutex);
        free(exe);
        free(environment);
        free(version);
        free(manifest);
        return cached;
    }
    pthread_mutex_unlock(&g_runtime_validation.mutex);
    char* text = manifest ? read_file(manifest, 2 * 1024 * 1024, NULL) : NULL;
    struct stat st;
    bool valid = exe && version && text && lstat(exe, &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode);
    char parse_error[160];
    ms_json* json = valid ? ms_json_parse(text, strlen(text), parse_error, sizeof(parse_error)) : NULL;
    const ms_json* files = json ? ms_json_object_get(json, "files") : NULL;
    size_t count = ms_json_array_length(files);
    valid = valid && json && json_bool(json, "locallyAdHocSigned", false) && count > 0 && count <= 512 &&
            regular_files == count + 1;
    for (size_t i = 0; valid && i < count; ++i) {
        const ms_json* item = ms_json_array_get(files, i);
        char *relative = json_string(item, "path"), *expected = json_string(item, "sha256");
        long long expected_size = -1;
        char* path = relative && archive_entry_safe(relative) ? join_path(version, relative) : NULL;
        char actual[65];
        valid = path && expected && strlen(expected) == 64 &&
                ms_json_as_i64(ms_json_object_get(item, "size"), &expected_size) && expected_size >= 0 &&
                lstat(path, &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_size == expected_size &&
                file_sha256(path, actual) && !strcasecmp(actual, expected);
        free(relative);
        free(expected);
        free(path);
    }
    if (valid)
        valid = verify_runtime_signatures(version, 0);
    pthread_mutex_lock(&g_runtime_validation.mutex);
    snprintf(g_runtime_validation.path, sizeof(g_runtime_validation.path), "%s", version ? version : "");
    g_runtime_validation.fingerprint = fingerprint;
    g_runtime_validation.valid = valid && fingerprint_ok;
    pthread_mutex_unlock(&g_runtime_validation.mutex);
    valid = valid && fingerprint_ok;
    ms_json_free(json);
    free(text);
    free(exe);
    free(environment);
    free(version);
    free(manifest);
    return valid;
}

char* ms_sharpemu_status_json(const char* home) {
    if (!ensure_environment(home))
        return error_json("failed to create the isolated SharpEmu environment");
    char *root = emulator_root(home), *exe = executable_path(home), *tag = current_tag(home);
    char *data = root ? join_path(root, "state") : NULL, *cache = root ? join_path(root, "cache") : NULL;
    char *logs = root ? join_path(root, "logs") : NULL, *previous_link = root ? join_path(root, "previous") : NULL;
    char previous[4096];
    ssize_t previous_len = previous_link ? readlink(previous_link, previous, sizeof(previous) - 1) : -1;
    bool arm_host = !strcmp(machine_arch(), "arm64"), intel_host = !strcmp(machine_arch(), "x86_64");
    int host_macos = host_macos_major();
    bool rosetta = !arm_host || rosetta_available();
    bool supported = (arm_host || intel_host) && host_macos >= SHARPEMU_EFFECTIVE_MIN_MACOS && rosetta;
    const char* reason = !(arm_host || intel_host)                   ? "unsupported_architecture"
                         : host_macos < SHARPEMU_EFFECTIVE_MIN_MACOS ? "macos_too_old"
                         : !rosetta                                  ? "rosetta_missing"
                                                                     : NULL;
    struct stat executable_stat;
    bool installed = exe && lstat(exe, &executable_stat) == 0 && S_ISREG(executable_stat.st_mode) &&
                     !S_ISLNK(executable_stat.st_mode) && access(exe, X_OK) == 0;
    bool runtime_valid = installed && active_runtime_valid(home);
    bool sandbox = network_sandbox_available();
    bool archive_tools = archive_tool("lsar") && archive_tool("unar");
    bool gpu_probe = runtime_valid && probe_test_hook_enabled();
    if (runtime_valid && root && !gpu_probe) {
        char* current = join_path(root, "current");
        gpu_probe = current && moltenvk_loader_probe(current);
        free(current);
    }
    unsigned long long disk_bytes = available_disk_bytes(root);
    char* roots[SHARPEMU_MAX_ROOTS] = {0};
    size_t root_count = load_roots(home, roots);
    for (size_t i = 0; i < root_count; ++i)
        free(roots[i]);
    sharpemu_games* games = calloc(1, sizeof(*games));
    if (games && root_count > 0)
        collect_games(home, games);
    size_t game_count = games ? games->count : 0;
    bool running = any_session_running(home);
    const char* state = !supported                     ? "unsupported_host"
                        : !installed                   ? "missing_runtime"
                        : !runtime_valid || !gpu_probe ? "runtime_probe_failed"
                        : running                      ? "running"
                        : root_count == 0              ? "no_game_roots"
                        : game_count == 0              ? "no_games"
                                                       : "ready";
    unsigned long long memory_bytes = host_sysctl_u64("hw.memsize");
    unsigned long long logical_cpu = host_sysctl_u64("hw.logicalcpu");
    bool updating;
    pthread_mutex_lock(&g_update.mutex);
    updating = g_update.running;
    pthread_mutex_unlock(&g_update.mutex);
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "sharpemu");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "SharpEmu");
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, "PlayStation 5");
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
    ms_json_writer_key(&w, "hostMacosMajor");
    ms_json_writer_i64(&w, host_macos);
    ms_json_writer_key(&w, "runtimeMinimumMacos");
    ms_json_writer_i64(&w, SHARPEMU_EFFECTIVE_MIN_MACOS);
    ms_json_writer_key(&w, "hostMemoryBytes");
    ms_json_writer_u64(&w, memory_bytes);
    ms_json_writer_key(&w, "hostLogicalCpu");
    ms_json_writer_u64(&w, logical_cpu);
    ms_json_writer_key(&w, "availableDiskBytes");
    ms_json_writer_u64(&w, disk_bytes);
    ms_json_writer_key(&w, "archiveToolsAvailable");
    ms_json_writer_bool(&w, archive_tools);
    ms_json_writer_key(&w, "gpuProbeReady");
    ms_json_writer_bool(&w, gpu_probe);
    ms_json_writer_key(&w, "networkIsolationAvailable");
    ms_json_writer_bool(&w, sandbox);
    ms_json_writer_key(&w, "networkDefault");
    ms_json_writer_string(&w, "denied");
    ms_json_writer_key(&w, "networkOptInAvailable");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "upstreamNotarized");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "locallyAdHocSigned");
    ms_json_writer_bool(&w, runtime_valid);
    ms_json_writer_key(&w, "cliOnly");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "graphicsBackend");
    ms_json_writer_string(&w, "Vulkan · MoltenVK");
    ms_json_writer_key(&w, "updateRunning");
    ms_json_writer_bool(&w, updating);
    ms_json_writer_key(&w, "warnings");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "experimental_emulator");
    ms_json_writer_string(&w, "most_games_unsupported");
    ms_json_writer_string(&w, "upstream_not_notarized");
    if (!sandbox)
        ms_json_writer_string(&w, "network_isolation_unavailable");
    if (!archive_tools)
        ms_json_writer_string(&w, "missing_archive_tools");
    if (installed && !gpu_probe)
        ms_json_writer_string(&w, "moltenvk_probe_failed");
    if (memory_bytes > 0 && memory_bytes < 16ULL * 1024ULL * 1024ULL * 1024ULL)
        ms_json_writer_string(&w, "low_memory");
    if (logical_cpu > 0 && logical_cpu < 8)
        ms_json_writer_string(&w, "low_cpu_threads");
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "currentTag");
    if (tag)
        ms_json_writer_string(&w, tag);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "rollbackAvailable");
    ms_json_writer_bool(&w, previous_len > 0);
    ms_json_writer_key(&w, "gameRootCount");
    ms_json_writer_u64(&w, root_count);
    ms_json_writer_key(&w, "gameCount");
    ms_json_writer_u64(&w, game_count);
    ms_json_writer_key(&w, "environmentPath");
    ms_json_writer_string(&w, root ? root : "");
    ms_json_writer_key(&w, "dataPath");
    ms_json_writer_string(&w, data ? data : "");
    ms_json_writer_key(&w, "cachePath");
    ms_json_writer_string(&w, cache ? cache : "");
    ms_json_writer_key(&w, "logsPath");
    ms_json_writer_string(&w, logs ? logs : "");
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
    free(logs);
    free(previous_link);
    free(games);
    return ms_json_writer_take(&w);
}

char* ms_sharpemu_games_json(const char* home) {
    sharpemu_games* games = calloc(1, sizeof(*games));
    char* roots[SHARPEMU_MAX_ROOTS] = {0};
    size_t root_count = load_roots(home, roots);
    ms_json_writer w;
    if (!games)
        return error_json("failed to allocate the SHARPEMU game index");
    collect_games(home, games);
    if (!save_game_cache(home, games)) {
        free(games);
        for (size_t i = 0; i < root_count; ++i)
            free(roots[i]);
        return error_json("failed to persist the SharpEmu launch index");
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "sharpemu");
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
        sharpemu_game* game = &games->items[i];
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
        ms_json_writer_key(&w, "contentVersion");
        ms_json_writer_string(&w, game->content_version);
        ms_json_writer_key(&w, "masterVersion");
        ms_json_writer_string(&w, game->master_version);
        ms_json_writer_key(&w, "path");
        ms_json_writer_string(&w, game->path);
        ms_json_writer_key(&w, "executableSize");
        ms_json_writer_u64(&w, game->executable_size);
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

char* ms_sharpemu_cover_path(const char* home, const char* id) {
    sharpemu_games* games = calloc(1, sizeof(*games));
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

char* ms_sharpemu_sessions_json(const char* home) {
    char *root = emulator_root(home), *sessions = root ? join_path(root, "sessions") : NULL;
    DIR* directory = sessions ? opendir(sessions) : NULL;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "sharpemu");
    ms_json_writer_key(&w, "sessions");
    ms_json_writer_array_begin(&w);
    struct dirent* entry;
    size_t emitted = 0;
    while (directory && emitted < 64 && (entry = readdir(directory))) {
        size_t n = strlen(entry->d_name);
        if (n <= 5 || strcmp(entry->d_name + n - 5, ".json") ||
            (n >= 10 && !strcmp(entry->d_name + n - 10, ".last.json")))
            continue;
        char id[128];
        if (n - 5 >= sizeof(id))
            continue;
        memcpy(id, entry->d_name, n - 5);
        id[n - 5] = '\0';
        pid_t pid = session_pid(home, id);
        if (pid <= 0)
            continue;
        char* path = session_path(home, id);
        char* text = path ? read_file(path, 64 * 1024, NULL) : NULL;
        char error[128];
        ms_json* session = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
        char *game_path = json_string(session, "gamePath"), *runtime_tag = json_string(session, "runtimeTag");
        char* log_path = json_string(session, "logPath");
        long long started_at = 0;
        (void)ms_json_as_i64(ms_json_object_get(session, "startedAt"), &started_at);
        bool network = json_bool(session, "networkEnabled", false);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_string(&w, id);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_i64(&w, pid);
        ms_json_writer_key(&w, "gamePath");
        ms_json_writer_string(&w, game_path ? game_path : "");
        ms_json_writer_key(&w, "runtimeTag");
        if (runtime_tag)
            ms_json_writer_string(&w, runtime_tag);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "logPath");
        ms_json_writer_string(&w, log_path ? log_path : "");
        ms_json_writer_key(&w, "networkEnabled");
        ms_json_writer_bool(&w, network);
        ms_json_writer_key(&w, "startedAt");
        ms_json_writer_i64(&w, started_at);
        ms_json_writer_object_end(&w);
        emitted++;
        free(game_path);
        free(runtime_tag);
        free(log_path);
        ms_json_free(session);
        free(text);
        free(path);
    }
    if (directory)
        closedir(directory);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    free(root);
    free(sessions);
    return ms_json_writer_take(&w);
}

char* ms_sharpemu_update_json(const char* home, const char* action) {
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
    return error_json("unknown SHARPEMU update action");
}

static char* remove_runtime(const char* home) {
    char *root = emulator_root(home), *versions, *downloads, *staging, *current, *previous;
    bool ok;
    if (!root)
        return error_json("failed to resolve SHARPEMU environment");
    if (any_session_running(home)) {
        free(root);
        return error_json("stop SHARPEMU before removing its runtime");
    }
    if (update_running()) {
        free(root);
        return error_json("wait for the SharpEmu update transaction before removing its runtime");
    }
    versions = join_path(root, "versions");
    downloads = join_path(root, "downloads");
    staging = join_path(root, "staging");
    current = join_path(root, "current");
    previous = join_path(root, "previous");
    if (versions)
        make_tree_writable(versions);
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
              : error_json("failed to remove the managed SHARPEMU runtime");
}

char* ms_sharpemu_action_json(const char* home, const char* action, const unsigned char* body, size_t length) {
    ms_json* root = NULL;
    char *id = NULL, *path = NULL, *tag = NULL;
    char* result = NULL;
    if (!strcmp(action, "scan"))
        return ms_sharpemu_games_json(home);
    root = parse_body(body, length);
    if (!root)
        return error_json("invalid SharpEmu request body");
    const char* const confirm_keys[] = {"confirm"};
    const char* const tag_keys[] = {"tag"};
    const char* const path_keys[] = {"path"};
    const char* const launch_keys[] = {"id", "fullscreen", "allowNetwork"};
    const char* const id_keys[] = {"id"};
    const char* const* allowed = NULL;
    size_t allowed_count = 0;
    if (!strcmp(action, "remove-runtime")) {
        allowed = confirm_keys;
        allowed_count = 1;
    } else if (!strcmp(action, "pin-current") || !strcmp(action, "unpin") || !strcmp(action, "skip-update") ||
               !strcmp(action, "clear-skip")) {
        allowed = tag_keys;
        allowed_count = 1;
    } else if (!strcmp(action, "add-root") || !strcmp(action, "remove-root")) {
        allowed = path_keys;
        allowed_count = 1;
    } else if (!strcmp(action, "launch")) {
        allowed = launch_keys;
        allowed_count = 3;
    } else if (!strcmp(action, "stop")) {
        allowed = id_keys;
        allowed_count = 1;
    }
    if (!allowed || !request_keys_allowed(root, allowed, allowed_count)) {
        ms_json_free(root);
        return error_json("SharpEmu request contains unknown fields");
    }
    bool typed_bool;
    if ((!strcmp(action, "remove-runtime") && !ms_json_as_bool(ms_json_object_get(root, "confirm"), &typed_bool)) ||
        ((!strcmp(action, "add-root") || !strcmp(action, "remove-root")) &&
         ms_json_type_of(ms_json_object_get(root, "path")) != MS_JSON_STRING) ||
        (!strcmp(action, "stop") && ms_json_type_of(ms_json_object_get(root, "id")) != MS_JSON_STRING) ||
        (!strcmp(action, "launch") && (ms_json_type_of(ms_json_object_get(root, "id")) != MS_JSON_STRING ||
                                       (ms_json_object_get(root, "fullscreen") &&
                                        !ms_json_as_bool(ms_json_object_get(root, "fullscreen"), &typed_bool)) ||
                                       (ms_json_object_get(root, "allowNetwork") &&
                                        !ms_json_as_bool(ms_json_object_get(root, "allowNetwork"), &typed_bool))))) {
        ms_json_free(root);
        return error_json("SharpEmu request fields have invalid types");
    }
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
        sharpemu_update_policy policy;
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
            result = error_json("failed to save SharpEmu update preferences");
        else
            result = release_json(home, false);
        free_update_policy(&policy);
    } else if (!strcmp(action, "add-root")) {
        char resolved[4096], resolved_home[4096];
        char* roots[SHARPEMU_MAX_ROOTS] = {0};
        size_t count = load_roots(home, roots);
        struct stat input_st, resolved_st;
        char* environment = emulator_root(home);
        const char* user_home = getenv("HOME");
        bool has_home = user_home && realpath(user_home, resolved_home);
        bool safe = path && realpath(path, resolved) && lstat(path, &input_st) == 0 && !S_ISLNK(input_st.st_mode) &&
                    lstat(resolved, &resolved_st) == 0 && S_ISDIR(resolved_st.st_mode) && !S_ISLNK(resolved_st.st_mode);
        bool protected = !safe || !strcmp(resolved, "/") || !strcmp(resolved, "/System") ||
                         !strncmp(resolved, "/System/", 8) || !strcmp(resolved, "/Library") ||
                         !strncmp(resolved, "/Library/", 9) || !strcmp(resolved, "/Applications") ||
                         !strncmp(resolved, "/Applications/", 14) || (has_home && !strcmp(resolved, resolved_home)) ||
                         (environment && !strncmp(resolved, environment, strlen(environment)) &&
                          (resolved[strlen(environment)] == '\0' || resolved[strlen(environment)] == '/'));
        bool exists = false, overlaps = false;
        for (size_t i = 0; i < count; ++i) {
            if (!strcmp(roots[i], resolved))
                exists = true;
            size_t old_len = strlen(roots[i]), new_len = strlen(resolved);
            if ((!strncmp(roots[i], resolved, new_len) && roots[i][new_len] == '/') ||
                (!strncmp(resolved, roots[i], old_len) && resolved[old_len] == '/'))
                overlaps = true;
        }
        if (protected)
            result = error_json("a safe existing PlayStation 5 game folder is required");
        else if (overlaps)
            result = error_json("nested SharpEmu game folders are not allowed");
        else if (!exists && count >= SHARPEMU_MAX_ROOTS)
            result = error_json("the SharpEmu game-folder limit has been reached");
        else {
            if (!exists)
                roots[count++] = strdup(resolved);
            result = save_roots(home, roots, count) ? ms_sharpemu_games_json(home)
                                                    : error_json("failed to save the SharpEmu game folder");
        }
        free(environment);
        for (size_t i = 0; i < count; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "remove-root")) {
        char* roots[SHARPEMU_MAX_ROOTS] = {0};
        char resolved[4096];
        const char* requested = path && realpath(path, resolved) ? resolved : path;
        size_t count = load_roots(home, roots), out = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!requested || strcmp(roots[i], requested))
                roots[out++] = roots[i];
            else
                free(roots[i]);
        }
        result = save_roots(home, roots, out) ? ms_sharpemu_games_json(home)
                                              : error_json("failed to update SharpEmu game folders");
        for (size_t i = 0; i < out; ++i)
            free(roots[i]);
    } else if (!strcmp(action, "launch")) {
        char launch_path[4096];
        unsigned long long expected_size = 0, expected_device = 0, expected_inode = 0;
        long long expected_mtime = 0;
        if (!id || !is_safe_component(id) ||
            !cached_launch_target(home, id, launch_path, &expected_size, &expected_device, &expected_inode,
                                  &expected_mtime))
            result = error_json("SharpEmu game was not found in the indexed library; scan again");
        else
            result =
                spawn_sharpemu(home, id, launch_path, expected_size, expected_device, expected_inode, expected_mtime,
                               json_bool(root, "fullscreen", false), json_bool(root, "allowNetwork", false));
    } else if (!strcmp(action, "stop")) {
        result =
            id && is_safe_component(id) ? stop_session(home, id) : error_json("a valid SharpEmu game id is required");
    } else {
        result = error_json("unknown SharpEmu action");
    }
    free(id);
    free(path);
    free(tag);
    ms_json_free(root);
    return result;
}
