#include "metalsharp_backend/epic.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LEGENDARY_VERSION    "0.21.0"
#define LEGENDARY_URL        "https://github.com/legendary-gl/legendary/releases/download/0.21.0/legendary_macOS_arm64"
#define LEGENDARY_SHA256     "28f5f7d0eb8c029679d4faaa483ec85888af17a9a75977ae9170c21d8ce3428b"
#define LEGENDARY_MAX_OUTPUT (32U * 1024U * 1024U)

static char* epic_failure(const char* message) {
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "error");
    ms_json_writer_string(&writer, message ? message : "Epic operation failed");
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

static char* epic_join(const char* left, const char* right) {
    size_t left_length = strlen(left), right_length = strlen(right);
    bool slash = left_length > 0 && left[left_length - 1] != '/';
    char* result = malloc(left_length + right_length + (slash ? 2 : 1));
    if (result)
        snprintf(result, left_length + right_length + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return result;
}

static bool epic_mkdir_p(const char* path) {
    char* copy = strdup(path);
    if (!copy)
        return false;
    for (size_t index = 1; copy[index]; index++) {
        if (copy[index] != '/')
            continue;
        copy[index] = 0;
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            free(copy);
            return false;
        }
        copy[index] = '/';
    }
    bool ok = mkdir(copy, 0755) == 0 || errno == EEXIST;
    free(copy);
    return ok;
}

static char* epic_read_text(const char* path, size_t limit) {
    FILE* file = fopen(path, "rb");
    char* data = NULL;
    long length;
    size_t read_length;
    if (!file || fseek(file, 0, SEEK_END) != 0)
        goto done;
    length = ftell(file);
    if (length < 0 || (size_t)length > limit || fseek(file, 0, SEEK_SET) != 0)
        goto done;
    data = malloc((size_t)length + 1);
    if (!data)
        goto done;
    read_length = fread(data, 1, (size_t)length, file);
    data[read_length] = 0;
done:
    if (file)
        fclose(file);
    return data;
}

static bool epic_write_text_atomic(const char* path, const char* text, mode_t mode) {
    char* temporary = malloc(strlen(path) + 40);
    int fd = -1;
    bool ok = false;
    if (!temporary)
        return false;
    snprintf(temporary, strlen(path) + 40, "%s.tmp.%ld", path, (long)getpid());
    fd = open(temporary, O_CREAT | O_EXCL | O_WRONLY, mode);
    if (fd < 0)
        goto done;
    size_t remaining = strlen(text);
    const char* cursor = text;
    while (remaining > 0) {
        ssize_t count = write(fd, cursor, remaining);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (rename(temporary, path) != 0)
        goto done;
    ok = true;
done:
    if (fd >= 0)
        close(fd);
    if (!ok)
        unlink(temporary);
    free(temporary);
    return ok;
}

static char* epic_root(const char* home) {
    return epic_join(home, "epic");
}

static char* epic_config_path(const char* home) {
    char* root = epic_root(home);
    char* result = root ? epic_join(root, "legendary") : NULL;
    free(root);
    return result;
}

static char* epic_logs_path(const char* home) {
    char* root = epic_root(home);
    char* result = root ? epic_join(root, "logs") : NULL;
    free(root);
    return result;
}

static char* epic_library_cache_path(const char* home) {
    char* root = epic_root(home);
    char* result = root ? epic_join(root, "library.json") : NULL;
    free(root);
    return result;
}

static char* epic_legendary_installed_path(const char* home) {
    char* config = epic_config_path(home);
    char* result = config ? epic_join(config, "installed.json") : NULL;
    free(config);
    return result;
}

static char* epic_tool_path(const char* home) {
    const char* override = getenv("METALSHARP_EPIC_LEGENDARY_BIN");
    if (override && override[0])
        return strdup(override);
    char* tools = epic_join(home, "tools/legendary");
    char* result = tools ? epic_join(tools, "legendary-" LEGENDARY_VERSION) : NULL;
    free(tools);
    return result;
}

static bool valid_app_name(const char* app_name) {
    size_t length = app_name ? strlen(app_name) : 0;
    if (length == 0 || length > 128)
        return false;
    for (size_t index = 0; index < length; index++)
        if (!isalnum((unsigned char)app_name[index]) && app_name[index] != '_' && app_name[index] != '-' &&
            app_name[index] != '.')
            return false;
    return true;
}

static char* request_app_name(const unsigned char* body, size_t body_length) {
    char error[160];
    char* app_name = NULL;
    ms_json* request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
    if (request && ms_json_type_of(request) == MS_JSON_OBJECT)
        ms_json_as_string(ms_json_object_get(request, "appName"), &app_name);
    ms_json_free(request);
    if (!valid_app_name(app_name)) {
        free(app_name);
        return NULL;
    }
    return app_name;
}

static char* request_string(const unsigned char* body, size_t body_length, const char* key) {
    char error[160];
    char* value = NULL;
    ms_json* request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
    if (request && ms_json_type_of(request) == MS_JSON_OBJECT)
        ms_json_as_string(ms_json_object_get(request, key), &value);
    ms_json_free(request);
    return value;
}

static bool valid_pipeline(const char* pipeline) {
    static const char* const allowed[] = {"auto", "d3dmetal", "vkd3d", "m11",      "m11_32",
                                          "m10",  "m10_32",   "m9",    "fna_arm64"};
    if (!pipeline)
        return false;
    for (size_t index = 0; index < sizeof(allowed) / sizeof(allowed[0]); index++)
        if (!strcmp(pipeline, allowed[index]))
            return true;
    return false;
}

static bool valid_mouse_mode(const char* mode) {
    return mode && (!strcmp(mode, "no-recenter") || !strcmp(mode, "auto"));
}

static char* epic_game_root(const char* home);

static bool path_within(const char* path, const char* root) {
    size_t length = root ? strlen(root) : 0;
    return length > 0 && !strncmp(path, root, length) && (path[length] == '/' || path[length] == 0);
}

static char* validated_install_root(const char* home, const char* requested) {
    if (!requested || !requested[0])
        return epic_game_root(home);
    char resolved[PATH_MAX];
    struct stat metadata;
    const char* user_home = getenv("HOME");
    if (requested[0] != '/' || !realpath(requested, resolved) || stat(resolved, &metadata) != 0 ||
        !S_ISDIR(metadata.st_mode))
        return NULL;
    if (strncmp(resolved, "/Volumes/", 9) && !path_within(resolved, home) && !path_within(resolved, user_home))
        return NULL;
    return strdup(resolved);
}

static int epic_command_lock(const char* home) {
    char *root = epic_root(home), *path = NULL;
    int fd = -1;
    struct flock lock = {0};
    if (!root || !epic_mkdir_p(root))
        goto done;
    path = epic_join(root, "legendary.lock");
    fd = path ? open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600) : -1;
    if (fd < 0)
        goto done;
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno == EINTR)
            continue;
        close(fd);
        fd = -1;
        break;
    }
done:
    free(root);
    free(path);
    return fd;
}

static int epic_run_capture(const char* home, char* const argv[], const char* wine_prefix, char** output,
                            const char* log_name) {
    int pipe_fds[2] = {-1, -1};
    pid_t child;
    int status = 0, log_fd = -1, lock_fd = -1;
    char *config = NULL, *logs = NULL, *log_path = NULL, *buffer = NULL;
    size_t length = 0, capacity = 8192;
    if (output)
        *output = NULL;
    lock_fd = epic_command_lock(home);
    if (lock_fd < 0)
        goto fail;
    config = epic_config_path(home);
    logs = epic_logs_path(home);
    if (!config || !logs || !epic_mkdir_p(config) || !epic_mkdir_p(logs) || pipe(pipe_fds) != 0)
        goto fail;
    log_path = epic_join(logs, log_name ? log_name : "legendary.log");
    log_fd = log_path ? open(log_path, O_CREAT | O_APPEND | O_WRONLY, 0600) : -1;
    child = fork();
    if (child == 0) {
        close(pipe_fds[0]);
        setenv("LEGENDARY_CONFIG_PATH", config, 1);
        if (wine_prefix)
            setenv("WINEPREFIX", wine_prefix, 1);
        dup2(pipe_fds[1], STDOUT_FILENO);
        if (log_fd >= 0)
            dup2(log_fd, STDERR_FILENO);
        close(pipe_fds[1]);
        if (log_fd >= 0)
            close(log_fd);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
    if (child < 0)
        goto fail;
    buffer = malloc(capacity);
    if (!buffer) {
        kill(child, SIGTERM);
        goto wait_fail;
    }
    for (;;) {
        if (length + 4096 + 1 > capacity) {
            size_t next = capacity * 2;
            if (next > LEGENDARY_MAX_OUTPUT)
                next = LEGENDARY_MAX_OUTPUT;
            if (next <= capacity) {
                kill(child, SIGTERM);
                goto wait_fail;
            }
            char* grown = realloc(buffer, next);
            if (!grown) {
                kill(child, SIGTERM);
                goto wait_fail;
            }
            buffer = grown;
            capacity = next;
        }
        ssize_t count = read(pipe_fds[0], buffer + length, capacity - length - 1);
        if (count > 0) {
            length += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    buffer[length] = 0;
wait_fail:
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    if (output) {
        *output = buffer;
        buffer = NULL;
    }
    free(buffer);
    close(pipe_fds[0]);
    if (log_fd >= 0)
        close(log_fd);
    free(config);
    free(logs);
    free(log_path);
    close(lock_fd);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
fail:
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);
    if (pipe_fds[1] >= 0)
        close(pipe_fds[1]);
    if (log_fd >= 0)
        close(log_fd);
    free(buffer);
    free(config);
    free(logs);
    free(log_path);
    if (lock_fd >= 0)
        close(lock_fd);
    return -1;
}

static bool legendary_binary_shape(const char* path) {
    unsigned char header[8];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t count = read(fd, header, sizeof(header));
    close(fd);
    return count == (ssize_t)sizeof(header) && header[0] == 0xcf && header[1] == 0xfa && header[2] == 0xed &&
           header[3] == 0xfe && header[4] == 0x0c && header[5] == 0x00 && header[6] == 0x00 && header[7] == 0x01;
}

static bool legendary_sha256_matches(const char* path) {
    int pipe_fds[2];
    pid_t child;
    int status = 0;
    char output[256] = {0};
    if (pipe(pipe_fds) != 0)
        return false;
    child = fork();
    if (child == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        char* const argv[] = {"/usr/bin/shasum", "-a", "256", (char*)path, NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    ssize_t count = read(pipe_fds[0], output, sizeof(output) - 1);
    close(pipe_fds[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    return count > 64 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           !strncmp(output, LEGENDARY_SHA256, strlen(LEGENDARY_SHA256));
}

static bool legendary_available(const char* home) {
    char* tool = epic_tool_path(home);
    bool available = tool && access(tool, X_OK) == 0 && legendary_binary_shape(tool) && legendary_sha256_matches(tool);
    free(tool);
    return available;
}

static char* epic_game_root(const char* home) {
    char *config_dir = epic_join(home, "launcher-games/epic"), *location = NULL, *configured = NULL, *result = NULL;
    if (!config_dir)
        return NULL;
    location = epic_join(config_dir, "location.txt");
    configured = location ? epic_read_text(location, PATH_MAX) : NULL;
    if (configured) {
        char* newline = strpbrk(configured, "\r\n");
        if (newline)
            *newline = 0;
        size_t home_length = strlen(home);
        if (configured[0] == '/' && (!strncmp(configured, "/Volumes/", 9) ||
                                     (!strncmp(configured, home, home_length) &&
                                      (configured[home_length] == '/' || configured[home_length] == 0))))
            result = strdup(configured);
    }
    if (!result)
        result = epic_join(config_dir, "library");
    if (result)
        epic_mkdir_p(result);
    free(config_dir);
    free(location);
    free(configured);
    return result;
}

static char* epic_account_name(const char* home) {
    char *config = epic_config_path(home), *user_path = config ? epic_join(config, "user.json") : NULL;
    char* raw = user_path ? epic_read_text(user_path, 1024 * 1024) : NULL;
    char* display_name = NULL;
    if (raw) {
        char error[160];
        ms_json* user = ms_json_parse(raw, strlen(raw), error, sizeof(error));
        if (user && ms_json_type_of(user) == MS_JSON_OBJECT)
            ms_json_as_string(ms_json_object_get(user, "displayName"), &display_name);
        ms_json_free(user);
    }
    free(config);
    free(user_path);
    free(raw);
    return display_name;
}

static char* epic_status(const char* home, bool ok, const char* error_message) {
    char *tool = epic_tool_path(home), *config = epic_config_path(home), *root = epic_game_root(home);
    char* account = epic_account_name(home);
    bool tool_available = legendary_available(home);
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, ok);
    ms_json_writer_key(&writer, "toolAvailable");
    ms_json_writer_bool(&writer, tool_available);
    ms_json_writer_key(&writer, "toolVersion");
    ms_json_writer_string(&writer, LEGENDARY_VERSION);
    ms_json_writer_key(&writer, "toolPath");
    ms_json_writer_string(&writer, tool ? tool : "");
    ms_json_writer_key(&writer, "authenticated");
    ms_json_writer_bool(&writer, account && account[0]);
    ms_json_writer_key(&writer, "account");
    if (account && account[0])
        ms_json_writer_string(&writer, account);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "configPath");
    ms_json_writer_string(&writer, config ? config : "");
    ms_json_writer_key(&writer, "gameRoot");
    ms_json_writer_string(&writer, root ? root : "");
    if (error_message) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, error_message);
    }
    ms_json_writer_object_end(&writer);
    free(tool);
    free(config);
    free(root);
    free(account);
    return ms_json_writer_take(&writer);
}

char* ms_epic_status_json(const char* home) {
    return epic_status(home, true, NULL);
}

char* ms_epic_install_tool_json(const char* home) {
    char *tool = epic_tool_path(home), *directory = NULL, *temporary = NULL, *logs = NULL, *log_path = NULL;
    int log_fd = -1, status = -1;
    char* result = NULL;
    if (!tool)
        return epic_failure("could not resolve Legendary tool path");
    char* slash = strrchr(tool, '/');
    if (slash)
        directory = strndup(tool, (size_t)(slash - tool));
    if (!directory || !epic_mkdir_p(directory)) {
        result = epic_failure("could not create Legendary tool directory");
        goto done;
    }
    if (access(tool, X_OK) == 0 && legendary_binary_shape(tool) && legendary_sha256_matches(tool)) {
        result = epic_status(home, true, NULL);
        goto done;
    }
    temporary = malloc(strlen(tool) + 32);
    if (!temporary) {
        result = epic_failure("out of memory");
        goto done;
    }
    snprintf(temporary, strlen(tool) + 32, "%s.part.%ld", tool, (long)getpid());
    logs = epic_logs_path(home);
    if (!logs || !epic_mkdir_p(logs)) {
        result = epic_failure("could not create Epic log directory");
        goto done;
    }
    log_path = epic_join(logs, "legendary-install.log");
    log_fd = log_path ? open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600) : -1;
    pid_t child = fork();
    if (child == 0) {
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        char* const argv[] = {"/usr/bin/curl",
                              "--fail",
                              "--location",
                              "--silent",
                              "--show-error",
                              "--proto",
                              "=https",
                              "--proto-redir",
                              "=https",
                              "--tlsv1.2",
                              "--retry",
                              "2",
                              "--connect-timeout",
                              "20",
                              "--max-time",
                              "1800",
                              "--max-filesize",
                              "67108864",
                              "-A",
                              "MetalSharp/" MS_BACKEND_VERSION,
                              "-o",
                              temporary,
                              LEGENDARY_URL,
                              NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    if (child < 0) {
        result = epic_failure("could not start Legendary download");
        goto done;
    }
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !legendary_binary_shape(temporary) ||
        !legendary_sha256_matches(temporary) || chmod(temporary, 0755) != 0 || rename(temporary, tool) != 0) {
        result = epic_failure("Legendary download failed integrity validation");
        goto done;
    }
    result = epic_status(home, true, NULL);
done:
    if (log_fd >= 0)
        close(log_fd);
    if (temporary)
        unlink(temporary);
    free(tool);
    free(directory);
    free(temporary);
    free(logs);
    free(log_path);
    return result;
}

char* ms_epic_auth_json(const char* home, const unsigned char* body, size_t body_length) {
    char error[160];
    char* code = NULL;
    ms_json* request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
    if (request && ms_json_type_of(request) == MS_JSON_OBJECT)
        ms_json_as_string(ms_json_object_get(request, "code"), &code);
    ms_json_free(request);
    if (!code || strlen(code) < 8 || strlen(code) > 4096) {
        free(code);
        return epic_failure("a valid Epic authorization code is required");
    }
    char* tool = epic_tool_path(home);
    if (!tool || !legendary_available(home)) {
        free(code);
        free(tool);
        return epic_failure("Legendary is not installed");
    }
    char* const argv[] = {tool, "auth", "--code", code, NULL};
    int status = epic_run_capture(home, argv, NULL, NULL, "legendary-auth.log");
    free(code);
    free(tool);
    char* account = status == 0 ? epic_account_name(home) : NULL;
    bool authenticated = account && account[0];
    free(account);
    if (!authenticated)
        return epic_failure("Epic authentication failed; sign in again or inspect legendary-auth.log");
    return epic_status(home, true, NULL);
}

char* ms_epic_logout_json(const char* home) {
    char* tool = epic_tool_path(home);
    if (!tool || !legendary_available(home)) {
        free(tool);
        return epic_failure("Legendary is not installed");
    }
    char* const argv[] = {tool, "auth", "--delete", NULL};
    int status = epic_run_capture(home, argv, NULL, NULL, "legendary-auth.log");
    if (status == 0) {
        char* cache = epic_library_cache_path(home);
        if (cache)
            unlink(cache);
        free(cache);
    }
    free(tool);
    return status == 0 ? epic_status(home, true, NULL) : epic_failure("Epic logout failed");
}

static char* json_string_field(const ms_json* object, const char* key) {
    char* result = NULL;
    if (object)
        ms_json_as_string(ms_json_object_get(object, key), &result);
    return result;
}

static const ms_json* installed_for_app(const ms_json* installed, const char* app_name) {
    if (!installed)
        return NULL;
    if (ms_json_type_of(installed) == MS_JSON_OBJECT)
        return ms_json_object_get(installed, app_name);
    if (ms_json_type_of(installed) != MS_JSON_ARRAY)
        return NULL;
    for (size_t index = 0; index < ms_json_array_length(installed); index++) {
        const ms_json* item = ms_json_array_get(installed, index);
        char* candidate = json_string_field(item, "app_name");
        bool match = candidate && !strcmp(candidate, app_name);
        free(candidate);
        if (match)
            return item;
    }
    return NULL;
}

static char* epic_artwork_url(const ms_json* game) {
    const ms_json* metadata = ms_json_object_get(game, "metadata");
    const ms_json* images = metadata ? ms_json_object_get(metadata, "keyImages") : NULL;
    char* fallback = NULL;
    if (!images || ms_json_type_of(images) != MS_JSON_ARRAY)
        return NULL;
    for (size_t index = 0; index < ms_json_array_length(images); index++) {
        const ms_json* image = ms_json_array_get(images, index);
        char *type = json_string_field(image, "type"), *url = json_string_field(image, "url");
        if (url && !fallback)
            fallback = strdup(url);
        bool preferred = type && (!strcmp(type, "DieselGameBoxTall") || !strcmp(type, "OfferImageTall") ||
                                  !strcmp(type, "Thumbnail"));
        free(type);
        if (preferred && url) {
            free(fallback);
            return url;
        }
        free(url);
    }
    return fallback;
}

static char* epic_version(const ms_json* game) {
    const ms_json* assets = ms_json_object_get(game, "asset_infos");
    const ms_json* windows = assets ? ms_json_object_get(assets, "Windows") : NULL;
    return json_string_field(windows, "build_version");
}

static char* epic_process_path(const char* home, const char* app_name, const char* suffix);
static pid_t read_process_pid_suffix(const char* home, const char* app_name, const char* suffix);
static bool process_running(pid_t pid);

static char* epic_bottle_path(const char* home, const char* app_name) {
    char* bottles = epic_join(home, "bottles");
    char* name = malloc(strlen(app_name) + 6);
    char* result = NULL;
    if (name) {
        sprintf(name, "epic_%s", app_name);
        result = bottles ? epic_join(bottles, name) : NULL;
    }
    free(bottles);
    free(name);
    return result;
}

static char* epic_prefix_path(const char* home, const char* app_name) {
    char* bottle = epic_bottle_path(home, app_name);
    char* prefix = bottle ? epic_join(bottle, "prefix") : NULL;
    free(bottle);
    return prefix;
}

static char* epic_bottle_field(const char* home, const char* app_name, const char* key) {
    char* bottle = epic_bottle_path(home, app_name);
    char* manifest = bottle ? epic_join(bottle, "bottle.json") : NULL;
    char* raw = manifest ? epic_read_text(manifest, 1024 * 1024) : NULL;
    char* value = NULL;
    if (raw) {
        char error[160];
        ms_json* json = ms_json_parse(raw, strlen(raw), error, sizeof(error));
        if (json && ms_json_type_of(json) == MS_JSON_OBJECT)
            ms_json_as_string(ms_json_object_get(json, key), &value);
        ms_json_free(json);
    }
    free(bottle);
    free(manifest);
    free(raw);
    return value;
}

static bool epic_bottle_initialized(const char* home, const char* app_name) {
    char* prefix = epic_prefix_path(home, app_name);
    char* system_reg = prefix ? epic_join(prefix, "system.reg") : NULL;
    struct stat metadata;
    char* pipeline = epic_bottle_field(home, app_name, "preferred_pipeline");
    char* mouse_mode = epic_bottle_field(home, app_name, "mouse_mode");
    bool initialized = system_reg && stat(system_reg, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                       valid_pipeline(pipeline) && valid_mouse_mode(mouse_mode);
    free(prefix);
    free(system_reg);
    free(pipeline);
    free(mouse_mode);
    return initialized;
}

static char* epic_games_response(const char* home, const ms_json* games, const ms_json* installed) {
    ms_json_writer writer;
    char* account = epic_account_name(home);
    char* root = epic_game_root(home);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "authenticated");
    ms_json_writer_bool(&writer, account && account[0]);
    ms_json_writer_key(&writer, "account");
    if (account && account[0])
        ms_json_writer_string(&writer, account);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "gameRoot");
    ms_json_writer_string(&writer, root ? root : "");
    ms_json_writer_key(&writer, "games");
    ms_json_writer_array_begin(&writer);
    if (games && ms_json_type_of(games) == MS_JSON_ARRAY) {
        for (size_t index = 0; index < ms_json_array_length(games); index++) {
            const ms_json* game = ms_json_array_get(games, index);
            char *app_name = json_string_field(game, "app_name"), *title = json_string_field(game, "app_title");
            if (!valid_app_name(app_name) || !title) {
                free(app_name);
                free(title);
                continue;
            }
            const ms_json* install = installed_for_app(installed, app_name);
            char *version = epic_version(game), *artwork = epic_artwork_url(game);
            char* install_path = json_string_field(install, "install_path");
            char* executable = json_string_field(install, "executable");
            char* pipeline = epic_bottle_field(home, app_name, "preferred_pipeline");
            char* mouse_mode = epic_bottle_field(home, app_name, "mouse_mode");
            double install_size = 0;
            if (install)
                ms_json_as_number(ms_json_object_get(install, "install_size"), &install_size);
            ms_json_writer_object_begin(&writer);
            ms_json_writer_key(&writer, "appName");
            ms_json_writer_string(&writer, app_name);
            ms_json_writer_key(&writer, "title");
            ms_json_writer_string(&writer, title);
            ms_json_writer_key(&writer, "version");
            if (version)
                ms_json_writer_string(&writer, version);
            else
                ms_json_writer_null(&writer);
            ms_json_writer_key(&writer, "artworkUrl");
            if (artwork)
                ms_json_writer_string(&writer, artwork);
            else
                ms_json_writer_null(&writer);
            ms_json_writer_key(&writer, "installed");
            ms_json_writer_bool(&writer, install != NULL);
            ms_json_writer_key(&writer, "bottleInitialized");
            ms_json_writer_bool(&writer, epic_bottle_initialized(home, app_name));
            ms_json_writer_key(&writer, "pipeline");
            ms_json_writer_string(&writer, valid_pipeline(pipeline) ? pipeline : "auto");
            ms_json_writer_key(&writer, "mouseMode");
            ms_json_writer_string(&writer, valid_mouse_mode(mouse_mode) ? mouse_mode : "no-recenter");
            ms_json_writer_key(&writer, "running");
            ms_json_writer_bool(&writer, process_running(read_process_pid_suffix(home, app_name, "launch.pid")));
            ms_json_writer_key(&writer, "downloading");
            ms_json_writer_bool(&writer, process_running(read_process_pid_suffix(home, app_name, "pid")));
            ms_json_writer_key(&writer, "installPath");
            if (install_path)
                ms_json_writer_string(&writer, install_path);
            else
                ms_json_writer_null(&writer);
            ms_json_writer_key(&writer, "executable");
            if (executable)
                ms_json_writer_string(&writer, executable);
            else
                ms_json_writer_null(&writer);
            ms_json_writer_key(&writer, "installSize");
            ms_json_writer_u64(&writer, install_size > 0 ? (unsigned long long)install_size : 0);
            ms_json_writer_object_end(&writer);
            free(app_name);
            free(title);
            free(version);
            free(artwork);
            free(install_path);
            free(executable);
            free(pipeline);
            free(mouse_mode);
        }
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    free(account);
    free(root);
    return ms_json_writer_take(&writer);
}

static char* epic_cached_games_json(const char* home) {
    char *cache_path = epic_library_cache_path(home), *installed_path = epic_legendary_installed_path(home);
    char* games_raw = cache_path ? epic_read_text(cache_path, LEGENDARY_MAX_OUTPUT) : NULL;
    char* installed_raw = installed_path ? epic_read_text(installed_path, LEGENDARY_MAX_OUTPUT) : NULL;
    char error[160];
    ms_json *games = NULL, *installed = NULL;
    char* result = NULL;
    if (games_raw)
        games = ms_json_parse(games_raw, strlen(games_raw), error, sizeof(error));
    if (installed_raw)
        installed = ms_json_parse(installed_raw, strlen(installed_raw), error, sizeof(error));
    if (games && ms_json_type_of(games) == MS_JSON_ARRAY)
        result = epic_games_response(home, games, installed);
    free(cache_path);
    free(installed_path);
    free(games_raw);
    free(installed_raw);
    ms_json_free(games);
    ms_json_free(installed);
    return result;
}

char* ms_epic_games_json(const char* home, int force_refresh) {
    char *tool = NULL, *games_raw = NULL, *installed_raw = NULL, *cache_path = NULL;
    char error[160];
    ms_json *games = NULL, *installed = NULL;
    char* result = NULL;
    char* account = epic_account_name(home);
    if (!account || !account[0]) {
        free(account);
        return epic_failure("Epic account is not authenticated");
    }
    free(account);
    if (!force_refresh) {
        result = epic_cached_games_json(home);
        if (result)
            return result;
    }
    tool = epic_tool_path(home);
    if (!tool || !legendary_available(home)) {
        free(tool);
        return epic_failure("Legendary is not installed");
    }
    char* const games_argv[] = {
        tool, "list", "--platform", "Windows", "--json", force_refresh ? "--force-refresh" : NULL, NULL};
    int status = epic_run_capture(home, games_argv, NULL, &games_raw, "legendary-sync.log");
    if (status != 0 || !games_raw) {
        result = epic_cached_games_json(home);
        if (!result)
            result = epic_failure("Epic library sync failed; sign in again or inspect legendary-sync.log");
        goto done;
    }
    char* const installed_argv[] = {tool, "list-installed", "--json", NULL};
    if (epic_run_capture(home, installed_argv, NULL, &installed_raw, "legendary-sync.log") != 0) {
        free(installed_raw);
        installed_raw = strdup("[]");
    }
    games = ms_json_parse(games_raw, strlen(games_raw), error, sizeof(error));
    installed = installed_raw ? ms_json_parse(installed_raw, strlen(installed_raw), error, sizeof(error)) : NULL;
    if (!games || ms_json_type_of(games) != MS_JSON_ARRAY) {
        result = epic_cached_games_json(home);
        if (!result)
            result = epic_failure("Legendary returned an invalid Epic library response");
        goto done;
    }
    cache_path = epic_library_cache_path(home);
    if (cache_path) {
        char* root = epic_root(home);
        if (root && epic_mkdir_p(root))
            (void)epic_write_text_atomic(cache_path, games_raw, 0600);
        free(root);
    }
    result = epic_games_response(home, games, installed);
done:
    free(tool);
    free(cache_path);
    free(games_raw);
    free(installed_raw);
    ms_json_free(games);
    ms_json_free(installed);
    return result;
}

static void* epic_startup_sync_worker(void* opaque) {
    char* home = (char*)opaque;
    char* result = ms_epic_games_json(home, 1);
    free(result);
    free(home);
    return NULL;
}

void ms_epic_sync_on_startup(const char* home) {
    pthread_t thread;
    char* copy = home ? strdup(home) : NULL;
    if (!copy)
        return;
    if (pthread_create(&thread, NULL, epic_startup_sync_worker, copy) != 0) {
        free(copy);
        return;
    }
    (void)pthread_detach(thread);
}

static char* epic_process_path(const char* home, const char* app_name, const char* suffix) {
    char* processes = epic_join(home, "epic/processes");
    char* filename = malloc(strlen(app_name) + strlen(suffix) + 2);
    char* result = NULL;
    if (processes)
        epic_mkdir_p(processes);
    if (filename) {
        sprintf(filename, "%s.%s", app_name, suffix);
        result = processes ? epic_join(processes, filename) : NULL;
    }
    free(processes);
    free(filename);
    return result;
}

static pid_t read_process_pid_suffix(const char* home, const char* app_name, const char* suffix) {
    char* path = epic_process_path(home, app_name, suffix);
    char* raw = path ? epic_read_text(path, 64) : NULL;
    long value = raw ? strtol(raw, NULL, 10) : 0;
    free(path);
    free(raw);
    return value > 1 && value <= INT_MAX ? (pid_t)value : 0;
}

static pid_t read_process_pid(const char* home, const char* app_name) {
    return read_process_pid_suffix(home, app_name, "pid");
}

static bool process_running(pid_t pid) {
    return pid > 1 && (kill(pid, 0) == 0 || errno == EPERM);
}

static pid_t spawn_detached(const char* home, char* const argv[], const char* prefix, const char* graphics_backend,
                            const char* working_directory, const char* log_path) {
    int pid_pipe[2];
    pid_t child, launched = 0;
    if (pipe(pid_pipe) != 0)
        return 0;
    child = fork();
    if (child == 0) {
        close(pid_pipe[0]);
        if (setsid() < 0)
            _exit(1);
        pid_t grandchild = fork();
        if (grandchild < 0)
            _exit(1);
        if (grandchild > 0) {
            (void)write(pid_pipe[1], &grandchild, sizeof(grandchild));
            _exit(0);
        }
        close(pid_pipe[1]);
        char* config = epic_config_path(home);
        if (config)
            setenv("LEGENDARY_CONFIG_PATH", config, 1);
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        if (graphics_backend)
            setenv("MS_GRAPHICS_BACKEND", graphics_backend, 1);
        if (working_directory)
            chdir(working_directory);
        int log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        long max_fd = sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < (max_fd > 0 && max_fd < 65536 ? max_fd : 1024); fd++)
            close(fd);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pid_pipe[1]);
    if (child > 0) {
        ssize_t count = read(pid_pipe[0], &launched, sizeof(launched));
        if (count != (ssize_t)sizeof(launched))
            launched = 0;
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
    }
    close(pid_pipe[0]);
    return launched;
}

char* ms_epic_install_json(const char* home, const unsigned char* body, size_t body_length) {
    char* app_name = request_app_name(body, body_length);
    char* requested_root = request_string(body, body_length, "installPath");
    char *tool = NULL, *root = NULL, *logs = NULL, *log_path = NULL, *pid_path = NULL;
    char* result = NULL;
    if (!app_name)
        return epic_failure("invalid Epic app name");
    tool = epic_tool_path(home);
    root = validated_install_root(home, requested_root);
    logs = epic_logs_path(home);
    if (!tool || !root || !logs || !legendary_available(home) || !epic_mkdir_p(root) || access(root, W_OK) != 0 ||
        !epic_mkdir_p(logs)) {
        result = epic_failure(requested_root ? "the selected Epic install location is not allowed or writable"
                                             : "Epic download environment is not ready");
        goto done;
    }
    log_path = epic_process_path(home, app_name, "install.log");
    pid_path = epic_process_path(home, app_name, "pid");
    pid_t existing = read_process_pid(home, app_name);
    if (process_running(existing)) {
        result = epic_failure("an Epic operation is already running for this game");
        goto done;
    }
    char* const argv[] = {tool,          "-y", "install",    app_name,      "--platform", "Windows",
                          "--base-path", root, "--skip-sdl", "--skip-dlcs", NULL};
    pid_t pid = spawn_detached(home, argv, NULL, NULL, NULL, log_path);
    if (pid <= 1) {
        result = epic_failure("could not start Epic game download");
        goto done;
    }
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)pid);
    if (!epic_write_text_atomic(pid_path, pid_text, 0600)) {
        kill(pid, SIGTERM);
        result = epic_failure("could not record Epic download state");
        goto done;
    }
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "appName");
    ms_json_writer_string(&writer, app_name);
    ms_json_writer_key(&writer, "pid");
    ms_json_writer_u64(&writer, (unsigned long long)pid);
    ms_json_writer_key(&writer, "logPath");
    ms_json_writer_string(&writer, log_path);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
done:
    free(app_name);
    free(requested_root);
    free(tool);
    free(root);
    free(logs);
    free(log_path);
    free(pid_path);
    return result;
}

static double progress_from_log(const char* text) {
    double latest = 0;
    if (!text)
        return 0;
    for (const char* cursor = text; *cursor; cursor++) {
        if (*cursor != '%')
            continue;
        const char* start = cursor;
        while (start > text && (isdigit((unsigned char)start[-1]) || start[-1] == '.'))
            start--;
        if (start < cursor) {
            double value = strtod(start, NULL);
            if (value >= 0 && value <= 100)
                latest = value;
        }
    }
    return latest;
}

static char* last_log_line(const char* text) {
    if (!text || !text[0])
        return strdup("");
    const char* end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r'))
        end--;
    const char* start = end;
    while (start > text && start[-1] != '\n' && start[-1] != '\r')
        start--;
    return strndup(start, (size_t)(end - start));
}

char* ms_epic_progress_json(const char* home, const unsigned char* body, size_t body_length) {
    char* app_name = request_app_name(body, body_length);
    if (!app_name)
        return epic_failure("invalid Epic app name");
    char* log_path = epic_process_path(home, app_name, "install.log");
    char* log = log_path ? epic_read_text(log_path, 8U * 1024U * 1024U) : NULL;
    char* line = last_log_line(log);
    pid_t pid = read_process_pid(home, app_name);
    bool running = process_running(pid);
    double progress = progress_from_log(log);
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "appName");
    ms_json_writer_string(&writer, app_name);
    ms_json_writer_key(&writer, "active");
    ms_json_writer_bool(&writer, running);
    ms_json_writer_key(&writer, "percent");
    ms_json_writer_double(&writer, progress);
    ms_json_writer_key(&writer, "message");
    ms_json_writer_string(&writer, line ? line : "");
    ms_json_writer_key(&writer, "logPath");
    ms_json_writer_string(&writer, log_path ? log_path : "");
    ms_json_writer_object_end(&writer);
    free(app_name);
    free(log_path);
    free(log);
    free(line);
    return ms_json_writer_take(&writer);
}

char* ms_epic_cancel_json(const char* home, const unsigned char* body, size_t body_length) {
    char* app_name = request_app_name(body, body_length);
    if (!app_name)
        return epic_failure("invalid Epic app name");
    pid_t pid = read_process_pid(home, app_name);
    bool stopped = !process_running(pid);
    if (!stopped) {
        pid_t group = getpgid(pid);
        if (group > 1 && group != getpgrp())
            stopped = kill(-group, SIGTERM) == 0;
        else
            stopped = kill(pid, SIGTERM) == 0;
    }
    char* pid_path = epic_process_path(home, app_name, "pid");
    if (pid_path)
        unlink(pid_path);
    free(pid_path);
    free(app_name);
    return stopped ? strdup("{\"ok\":true}") : epic_failure("could not stop the Epic game download");
}

static int initialize_epic_prefix(const char* home, const char* prefix) {
    char* wine = epic_join(home, "runtime/wine/bin/metalsharp-wine");
    char* output = NULL;
    if (!wine || access(wine, X_OK) != 0 || !epic_mkdir_p(prefix)) {
        free(wine);
        return -1;
    }
    char* const argv[] = {wine, "wineboot", "-u", NULL};
    int status = epic_run_capture(home, argv, prefix, &output, "legendary-launch.log");
    free(output);
    free(wine);
    return status;
}

static bool write_epic_bottle_manifest(const char* home, const char* app_name, const char* pipeline,
                                       const char* mouse_mode) {
    char* bottle = epic_bottle_path(home, app_name);
    char* prefix = epic_prefix_path(home, app_name);
    char* manifest = bottle ? epic_join(bottle, "bottle.json") : NULL;
    ms_json_writer writer;
    if (!bottle || !prefix || !manifest || !epic_mkdir_p(bottle)) {
        free(bottle);
        free(prefix);
        free(manifest);
        return false;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "id");
    char* id = malloc(strlen(app_name) + 6);
    if (id)
        sprintf(id, "epic_%s", app_name);
    ms_json_writer_string(&writer, id ? id : app_name);
    ms_json_writer_key(&writer, "name");
    ms_json_writer_string(&writer, app_name);
    ms_json_writer_key(&writer, "bottle_type");
    ms_json_writer_string(&writer, "epic");
    ms_json_writer_key(&writer, "prefix_path");
    ms_json_writer_string(&writer, prefix);
    ms_json_writer_key(&writer, "preferred_pipeline");
    ms_json_writer_string(&writer, pipeline);
    ms_json_writer_key(&writer, "mouse_mode");
    ms_json_writer_string(&writer, mouse_mode);
    ms_json_writer_object_end(&writer);
    char* raw = ms_json_writer_take(&writer);
    bool ok = raw && epic_write_text_atomic(manifest, raw, 0600);
    free(id);
    free(raw);
    free(bottle);
    free(prefix);
    free(manifest);
    return ok;
}

static int configure_epic_mouse(const char* home, const char* prefix, const char* mouse_mode) {
    char* wine = epic_join(home, "runtime/wine/bin/metalsharp-wine");
    char* output = NULL;
    if (!wine)
        return -1;
    char* const disable_argv[] = {
        wine,      "reg", "add", "HKCU\\Software\\Wine\\DirectInput", "/v", "MouseWarpOverride", "/t", "REG_SZ", "/d",
        "disable", "/f",  NULL};
    char* const auto_argv[] = {wine, "reg", "delete", "HKCU\\Software\\Wine\\DirectInput", "/v", "MouseWarpOverride",
                               "/f", NULL};
    int status = epic_run_capture(home, !strcmp(mouse_mode, "no-recenter") ? disable_argv : auto_argv, prefix, &output,
                                  "legendary-launch.log");
    free(output);
    free(wine);
    return !strcmp(mouse_mode, "auto") ? 0 : status;
}

char* ms_epic_initialize_json(const char* home, const unsigned char* body, size_t body_length) {
    char* app_name = request_app_name(body, body_length);
    char* pipeline = request_string(body, body_length, "pipeline");
    char* mouse_mode = request_string(body, body_length, "mouseMode");
    char* prefix = NULL;
    char* result = NULL;
    if (!app_name || !valid_pipeline(pipeline) || !valid_mouse_mode(mouse_mode)) {
        result = epic_failure("valid Epic app, pipeline, and mouse mode are required");
        goto done;
    }
    prefix = epic_prefix_path(home, app_name);
    if (!prefix || initialize_epic_prefix(home, prefix) != 0 || configure_epic_mouse(home, prefix, mouse_mode) != 0 ||
        !write_epic_bottle_manifest(home, app_name, pipeline, mouse_mode)) {
        result = epic_failure("could not initialize the isolated Epic game bottle");
        goto done;
    }
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "appName");
    ms_json_writer_string(&writer, app_name);
    ms_json_writer_key(&writer, "prefixPath");
    ms_json_writer_string(&writer, prefix);
    ms_json_writer_key(&writer, "pipeline");
    ms_json_writer_string(&writer, pipeline);
    ms_json_writer_key(&writer, "mouseMode");
    ms_json_writer_string(&writer, mouse_mode);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
done:
    free(app_name);
    free(pipeline);
    free(mouse_mode);
    free(prefix);
    return result;
}

char* ms_epic_launch_json(const char* home, const unsigned char* body, size_t body_length) {
    char* app_name = request_app_name(body, body_length);
    char *tool = NULL, *wine = NULL, *wineserver = NULL, *prefix = NULL, *logs = NULL, *log_path = NULL;
    char *pipeline = NULL, *mouse_mode = NULL;
    char* result = NULL;
    if (!app_name)
        return epic_failure("invalid Epic app name");
    tool = epic_tool_path(home);
    wine = epic_join(home, "runtime/wine/bin/metalsharp-wine");
    wineserver = epic_join(home, "runtime/wine/bin/wineserver");
    prefix = epic_prefix_path(home, app_name);
    logs = epic_logs_path(home);
    log_path = epic_process_path(home, app_name, "launch.log");
    if (!tool || !wine || !wineserver || !prefix || !logs || !legendary_available(home) || access(wine, X_OK) != 0 ||
        access(wineserver, X_OK) != 0 || !epic_mkdir_p(logs)) {
        result = epic_failure("could not initialize the isolated Epic game bottle");
        goto done;
    }
    if (process_running(read_process_pid_suffix(home, app_name, "launch.pid"))) {
        result = epic_failure("this Epic game is already running");
        goto done;
    }
    pipeline = epic_bottle_field(home, app_name, "preferred_pipeline");
    mouse_mode = epic_bottle_field(home, app_name, "mouse_mode");
    if (!epic_bottle_initialized(home, app_name) || !valid_pipeline(pipeline) || !valid_mouse_mode(mouse_mode)) {
        result = epic_failure("initialize this Epic game bottle before launching");
        goto done;
    }
    if (configure_epic_mouse(home, prefix, mouse_mode) != 0) {
        result = epic_failure("could not apply Epic game mouse settings");
        goto done;
    }
    static const char* supervisor_script =
        "\"$1\" launch \"$2\" --skip-version-check --wine \"$3\" --wine-prefix \"$4\"; "
        "launch_status=$?; WINEPREFIX=\"$4\" \"$5\" -w; exit $launch_status";
    char* const argv[] = {
        "/bin/sh",  "-c", (char*)supervisor_script, "metalsharp-epic-supervisor", tool, app_name, wine, prefix,
        wineserver, NULL};
    pid_t pid = spawn_detached(home, argv, prefix, pipeline, NULL, log_path);
    if (pid <= 1) {
        result = epic_failure("could not launch Epic game");
        goto done;
    }
    char* launch_pid_path = epic_process_path(home, app_name, "launch.pid");
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)pid);
    if (launch_pid_path)
        epic_write_text_atomic(launch_pid_path, pid_text, 0600);
    free(launch_pid_path);
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "appName");
    ms_json_writer_string(&writer, app_name);
    ms_json_writer_key(&writer, "pid");
    ms_json_writer_u64(&writer, (unsigned long long)pid);
    ms_json_writer_key(&writer, "prefixPath");
    ms_json_writer_string(&writer, prefix);
    ms_json_writer_key(&writer, "logPath");
    ms_json_writer_string(&writer, log_path);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
done:
    free(app_name);
    free(tool);
    free(wine);
    free(wineserver);
    free(prefix);
    free(logs);
    free(log_path);
    free(pipeline);
    free(mouse_mode);
    return result;
}

char* ms_epic_stop_json(const char* home, const unsigned char* body, size_t body_length) {
    char* app_name = request_app_name(body, body_length);
    if (!app_name)
        return epic_failure("invalid Epic app name");
    char *wineserver = epic_join(home, "runtime/wine/bin/wineserver"), *prefix = epic_prefix_path(home, app_name);
    if (!wineserver || !prefix || access(wineserver, X_OK) != 0) {
        free(app_name);
        free(wineserver);
        free(prefix);
        return epic_failure("Epic game bottle is unavailable");
    }
    char* const argv[] = {wineserver, "-k", NULL};
    char* output = NULL;
    int status = epic_run_capture(home, argv, prefix, &output, "legendary-launch.log");
    free(output);
    char* pid_path = epic_process_path(home, app_name, "launch.pid");
    if (pid_path)
        unlink(pid_path);
    free(pid_path);
    free(app_name);
    free(wineserver);
    free(prefix);
    return status == 0 ? strdup("{\"ok\":true}") : epic_failure("could not stop Epic game bottle");
}

char* ms_epic_stop_all_json(const char* home) {
    char* processes = epic_join(home, "epic/processes");
    DIR* directory = processes ? opendir(processes) : NULL;
    size_t stopped = 0;
    if (directory) {
        struct dirent* entry;
        while ((entry = readdir(directory)) != NULL) {
            size_t length = strlen(entry->d_name);
            static const char suffix[] = ".launch.pid";
            if (length <= sizeof(suffix) - 1 || strcmp(entry->d_name + length - (sizeof(suffix) - 1), suffix))
                continue;
            char* app_name = strndup(entry->d_name, length - (sizeof(suffix) - 1));
            if (!valid_app_name(app_name)) {
                free(app_name);
                continue;
            }
            char* request = malloc(strlen(app_name) + 32);
            if (request) {
                sprintf(request, "{\"appName\":\"%s\"}", app_name);
                char* response = ms_epic_stop_json(home, (const unsigned char*)request, strlen(request));
                if (response && strstr(response, "\"ok\":true"))
                    stopped++;
                free(response);
            }
            free(request);
            free(app_name);
        }
        closedir(directory);
    }
    free(processes);
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "stopped");
    ms_json_writer_u64(&writer, stopped);
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

char* ms_epic_uninstall_json(const char* home, const unsigned char* body, size_t body_length) {
    char* app_name = request_app_name(body, body_length);
    if (!app_name)
        return epic_failure("invalid Epic app name");
    char *tool = epic_tool_path(home), *prefix = epic_prefix_path(home, app_name);
    if (!tool || !legendary_available(home)) {
        free(app_name);
        free(tool);
        free(prefix);
        return epic_failure("Legendary is not installed");
    }
    char* const argv[] = {tool, "-y", "uninstall", app_name, NULL};
    int status = epic_run_capture(home, argv, NULL, NULL, "legendary-uninstall.log");
    if (status == 0 && prefix) {
        char* bottle = strdup(prefix);
        char* slash = bottle ? strrchr(bottle, '/') : NULL;
        if (slash) {
            *slash = 0;
            char* rm_argv[] = {"/bin/rm", "-rf", "--", bottle, NULL};
            pid_t child = fork();
            if (child == 0) {
                execv(rm_argv[0], rm_argv);
                _exit(127);
            }
            if (child > 0)
                while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
                    ;
        }
        free(bottle);
    }
    free(app_name);
    free(tool);
    free(prefix);
    return status == 0 ? strdup("{\"ok\":true}") : epic_failure("Epic game uninstall failed");
}
