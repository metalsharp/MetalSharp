#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/steamcmd.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/logs.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Valve's macOS steamcmd (x86_64, runs under Rosetta). Forcing the Windows
 * platform makes it download the same depots the Wine Steam client would. */
#define STEAMCMD_URL          "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz"
#define LOGIN_TIMEOUT_SECONDS 300
#define MAX_FINISHED_JOBS     20

typedef struct steamcmd_job {
    unsigned appid;
    char* name;
    char* library;
    char* home;
    char state[24]; /* queued, preparing, downloading, verifying, finishing, done, failed, cancelled */
    char message[512];
    double progress;
    unsigned long long done_bytes;
    unsigned long long total_bytes;
    bool needs_login;
    bool cancel;
    pid_t pid;
    struct steamcmd_job* next;
} steamcmd_job;

typedef struct {
    char state[24]; /* idle, preparing, signing_in, needs_code, needs_confirmation, signed_in, failed, cancelled */
    char message[512];
    pid_t pid;
    int input_fd;
    bool cancel;
} steamcmd_login;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static steamcmd_job* g_jobs;
static bool g_worker_running;
static steamcmd_login g_login = {.state = "idle", .input_fd = -1};

static char* join(const char* a, const char* b) {
    size_t n = strlen(a) + strlen(b) + 2;
    char* s = malloc(n);
    if (s)
        snprintf(s, n, "%s/%s", a, b);
    return s;
}

static bool mkdir_p(const char* path) {
    char buffer[PATH_MAX];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer))
        return false;
    memcpy(buffer, path, length + 1);
    for (char* p = buffer + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

static char* read_text(const char* path, size_t limit) {
    FILE* file = fopen(path, "rb");
    char* text;
    size_t length;
    if (!file)
        return NULL;
    text = malloc(limit + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    length = fread(text, 1, limit, file);
    fclose(file);
    text[length] = '\0';
    return text;
}

static char* error_json(const char* message) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static ms_json* parse_body(const unsigned char* body, size_t length) {
    char error[96];
    return ms_json_parse(body ? (const char*)body : "{}", body ? length : 2, error, sizeof(error));
}

static char* body_string(const ms_json* body, const char* key) {
    char* value = NULL;
    if (body && !ms_json_as_string(ms_json_object_get(body, key), &value))
        value = NULL;
    return value;
}

static char* steamcmd_dir(const char* home) {
    return join(home, "tools/steamcmd");
}

static char* config_path(const char* home) {
    return join(home, "config/steamcmd.json");
}

/* Only the account name is kept; steamcmd caches its own login token. */
static char* saved_username(const char* home) {
    char* path = config_path(home);
    char* text = path ? read_text(path, 4096) : NULL;
    char error[96];
    ms_json* config = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
    char* username = config ? body_string(config, "username") : NULL;
    ms_json_free(config);
    free(text);
    free(path);
    if (username && !username[0]) {
        free(username);
        username = NULL;
    }
    return username;
}

static void save_username(const char* home, const char* username) {
    char* dir = join(home, "config");
    char* path = config_path(home);
    ms_json_writer w;
    char* raw;
    if (dir && path && mkdir_p(dir)) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "username");
        ms_json_writer_string(&w, username);
        ms_json_writer_object_end(&w);
        raw = ms_json_writer_take(&w);
        FILE* file = raw ? fopen(path, "wb") : NULL;
        if (file) {
            fputs(raw, file);
            fclose(file);
            chmod(path, 0600);
        }
        free(raw);
    }
    free(dir);
    free(path);
}

static bool run_quiet(char* const argv[], const char* directory) {
    pid_t pid = fork();
    int status = 0;
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        if (directory && chdir(directory) != 0)
            _exit(127);
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0)
        return false;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool steamcmd_installed(const char* home) {
    char* dir = steamcmd_dir(home);
    char* script = dir ? join(dir, "steamcmd.sh") : NULL;
    char* binary = dir ? join(dir, "steamcmd") : NULL;
    bool ok = script && binary && access(script, R_OK) == 0 && access(binary, X_OK) == 0;
    free(script);
    free(binary);
    free(dir);
    return ok;
}

/* Download and unpack steamcmd once; its first run then updates itself. */
static bool ensure_steamcmd(const char* home) {
    char* dir;
    char* archive;
    bool ok;
    if (steamcmd_installed(home))
        return true;
    dir = steamcmd_dir(home);
    archive = dir ? join(dir, "steamcmd_osx.tar.gz") : NULL;
    ok = dir && archive && mkdir_p(dir);
    if (ok) {
        char* const curl[] = {"/usr/bin/curl", "-fsSL", "--retry", "2", "-o", archive, STEAMCMD_URL, NULL};
        char* const tar[] = {"/usr/bin/tar", "-xzf", archive, "-C", dir, NULL};
        ok = run_quiet(curl, NULL) && run_quiet(tar, NULL);
        unlink(archive);
    }
    free(archive);
    free(dir);
    return ok && steamcmd_installed(home);
}

/* steamcmd in its own process group with stdout+stderr on a pipe. */
static pid_t spawn_steamcmd(const char* home, char* const extra[], int* output_fd, int* input_fd) {
    int output[2];
    int input[2] = {-1, -1};
    char* dir = steamcmd_dir(home);
    char* script = dir ? join(dir, "steamcmd.sh") : NULL;
    char* argv[24];
    size_t argc = 0;
    pid_t pid;
    if (!dir || !script || pipe(output) != 0) {
        free(dir);
        free(script);
        return -1;
    }
    if (input_fd && pipe(input) != 0) {
        close(output[0]);
        close(output[1]);
        free(dir);
        free(script);
        return -1;
    }
    argv[argc++] = "/bin/bash";
    argv[argc++] = script;
    for (size_t i = 0; extra && extra[i] && argc < sizeof(argv) / sizeof(argv[0]) - 1; i++)
        argv[argc++] = extra[i];
    argv[argc] = NULL;
    pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        if (input_fd) {
            dup2(input[0], STDIN_FILENO);
            close(input[0]);
            close(input[1]);
        } else {
            int null_fd = open("/dev/null", O_RDONLY);
            if (null_fd >= 0)
                dup2(null_fd, STDIN_FILENO);
        }
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        close(output[0]);
        close(output[1]);
        if (chdir(dir) != 0)
            _exit(127);
        execv(argv[0], argv);
        _exit(127);
    }
    close(output[1]);
    if (input_fd)
        close(input[0]);
    free(dir);
    free(script);
    if (pid < 0) {
        close(output[0]);
        if (input_fd)
            close(input[1]);
        return -1;
    }
    (void)setpgid(pid, pid);
    *output_fd = output[0];
    if (input_fd)
        *input_fd = input[1];
    return pid;
}

/* Wait for steamcmd (and the bash wrapper) to exit, asking it to stop
 * first when `terminate` is set; force it after five seconds. */
static void finish_process(pid_t pid, bool terminate) {
    int status;
    if (pid <= 1)
        return;
    if (terminate)
        (void)kill(-pid, SIGTERM);
    for (int i = 0; i < 50; i++) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid || (done < 0 && errno != EINTR))
            return;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    (void)kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

static bool contains_ci(const char* text, const char* needle) {
    return text && strcasestr(text, needle) != NULL;
}

/* "...ERROR (Invalid Password)" -> "Invalid Password". */
static void failure_reason(const char* line, char* out, size_t size) {
    const char* open = strrchr(line, '(');
    const char* close = open ? strchr(open, ')') : NULL;
    if (open && close && close > open + 1)
        snprintf(out, size, "%.*s", (int)(close - open - 1), open + 1);
    else
        snprintf(out, size, "%s", line);
}

/* ------------------------------- sign-in -------------------------------- */

typedef struct {
    char* home;
    char* username;
    char* password;
} login_args;

static void login_set(const char* state, const char* message) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_login.state, sizeof(g_login.state), "%s", state);
    snprintf(g_login.message, sizeof(g_login.message), "%s", message ? message : "");
    pthread_mutex_unlock(&g_lock);
}

static bool write_line(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        text += written;
        length -= (size_t)written;
    }
    return true;
}

static void wipe(char* secret) {
    if (secret) {
        volatile char* p = secret;
        while (*p)
            *p++ = '\0';
    }
}

/* Reacts to one complete or partial line of steamcmd's interactive output. */
static bool login_handle(const char* text, bool* sent, login_args* args, int input_fd, bool* done) {
    if (!*sent && strstr(text, "Steam>")) {
        /* The command line goes over stdin, so the password never shows in ps. */
        size_t size = strlen(args->username) + strlen(args->password) + 16;
        char* command = malloc(size);
        bool quoted = strchr(args->password, ' ') != NULL;
        if (!command)
            return false;
        snprintf(command, size, quoted ? "login %s \"%s\"\n" : "login %s %s\n", args->username, args->password);
        bool ok = write_line(input_fd, command);
        wipe(command);
        free(command);
        wipe(args->password);
        *sent = true;
        login_set("signing_in", "Signing in to Steam... check Steam Guard to approve the login.");
        return ok;
    }
    if (contains_ci(text, "Logged in OK") || contains_ci(text, "Waiting for user info...OK")) {
        save_username(args->home, args->username);
        login_set("signed_in", "Signed in to Steam");
        (void)write_line(input_fd, "quit\n");
        *done = true;
        return true;
    }
    if (contains_ci(text, "confirm the login in the Steam Mobile app")) {
        login_set("needs_confirmation", "Approve the sign-in in the Steam Mobile app, or enter a Steam Guard code.");
        return true;
    }
    if (contains_ci(text, "Two-factor code:")) {
        login_set("needs_code", "Enter the code from the Steam Mobile app.");
        return true;
    }
    if (contains_ci(text, "Steam Guard code:")) {
        login_set("needs_code", "Enter the Steam Guard code Steam emailed you.");
        return true;
    }
    if (*sent && (strstr(text, "ERROR (") || strstr(text, "FAILED (") || contains_ci(text, "Login Failure"))) {
        char reason[256];
        failure_reason(text, reason, sizeof(reason));
        login_set("failed", reason);
        (void)write_line(input_fd, "quit\n");
        *done = true;
        return true;
    }
    return true;
}

static void* login_thread(void* raw) {
    login_args* args = raw;
    int output_fd = -1, input_fd = -1;
    char buffer[4096];
    char line[2048];
    size_t line_length = 0;
    bool sent = false, done = false;
    time_t started = time(NULL);
    pid_t pid;

    login_set("preparing", "Preparing steamcmd...");
    if (!ensure_steamcmd(args->home)) {
        login_set("failed", "Could not download steamcmd");
        goto out;
    }
    pid = spawn_steamcmd(args->home, NULL, &output_fd, &input_fd);
    if (pid < 0) {
        login_set("failed", "Could not start steamcmd");
        goto out;
    }
    pthread_mutex_lock(&g_lock);
    g_login.pid = pid;
    g_login.input_fd = input_fd;
    pthread_mutex_unlock(&g_lock);
    while (!done) {
        struct pollfd poll_fd = {.fd = output_fd, .events = POLLIN};
        bool cancel;
        pthread_mutex_lock(&g_lock);
        cancel = g_login.cancel;
        pthread_mutex_unlock(&g_lock);
        if (cancel || time(NULL) - started > LOGIN_TIMEOUT_SECONDS) {
            login_set(cancel ? "cancelled" : "failed", cancel ? "Sign-in cancelled" : "Steam sign-in timed out");
            break;
        }
        int ready = poll(&poll_fd, 1, 500);
        if (ready <= 0)
            continue;
        ssize_t n = read(output_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            pthread_mutex_lock(&g_lock);
            bool finished = !strcmp(g_login.state, "signed_in");
            pthread_mutex_unlock(&g_lock);
            if (!finished)
                login_set("failed", "steamcmd exited before signing in");
            break;
        }
        for (ssize_t i = 0; i < n && !done; i++) {
            char c = buffer[i];
            if (c == '\n' || c == '\r') {
                line[line_length] = '\0';
                if (line_length && !login_handle(line, &sent, args, input_fd, &done))
                    done = true;
                line_length = 0;
            } else if (line_length < sizeof(line) - 1) {
                line[line_length++] = c;
            }
        }
        /* Prompts ("Steam>", "Two-factor code:") arrive without a newline. */
        if (!done && line_length) {
            line[line_length] = '\0';
            if (strstr(line, "Steam>") || strstr(line, "code:")) {
                if (!login_handle(line, &sent, args, input_fd, &done))
                    done = true;
                line_length = 0;
            }
        }
    }
    pthread_mutex_lock(&g_lock);
    g_login.pid = 0;
    g_login.input_fd = -1;
    pthread_mutex_unlock(&g_lock);
    close(input_fd);
    close(output_fd);
    pthread_mutex_lock(&g_lock);
    bool signed_in = !strcmp(g_login.state, "signed_in");
    pthread_mutex_unlock(&g_lock);
    finish_process(pid, !signed_in);
out:
    wipe(args->password);
    free(args->password);
    free(args->username);
    free(args->home);
    free(args);
    return NULL;
}

static bool any_job_active_locked(void) {
    for (steamcmd_job* job = g_jobs; job; job = job->next)
        if (strcmp(job->state, "done") && strcmp(job->state, "failed") && strcmp(job->state, "cancelled"))
            return true;
    return false;
}

static bool login_active_locked(void) {
    return !strcmp(g_login.state, "preparing") || !strcmp(g_login.state, "signing_in") ||
           !strcmp(g_login.state, "needs_code") || !strcmp(g_login.state, "needs_confirmation");
}

char* ms_steamcmd_login_json(const char* home, const unsigned char* body, size_t length) {
    ms_json* request = parse_body(body, length);
    char* username = body_string(request, "username");
    char* password = body_string(request, "password");
    login_args* args;
    pthread_t thread;
    ms_json_free(request);
    if (!username || !password || !username[0] || !password[0] || strpbrk(username, " \t\r\n\"") ||
        strpbrk(password, "\r\n\"")) {
        wipe(password);
        free(password);
        free(username);
        return error_json("Enter a Steam username and password");
    }
    pthread_mutex_lock(&g_lock);
    if (login_active_locked() || any_job_active_locked()) {
        pthread_mutex_unlock(&g_lock);
        wipe(password);
        free(password);
        free(username);
        return error_json("steamcmd is busy; wait for the current sign-in or install to finish");
    }
    g_login.cancel = false;
    snprintf(g_login.state, sizeof(g_login.state), "preparing");
    snprintf(g_login.message, sizeof(g_login.message), "Preparing steamcmd...");
    pthread_mutex_unlock(&g_lock);
    args = calloc(1, sizeof(*args));
    if (!args || !(args->home = strdup(home))) {
        free(args);
        wipe(password);
        free(password);
        free(username);
        login_set("failed", "Out of memory");
        return error_json("Out of memory");
    }
    args->username = username;
    args->password = password;
    if (pthread_create(&thread, NULL, login_thread, args) != 0) {
        login_set("failed", "Could not start sign-in");
        wipe(password);
        free(password);
        free(username);
        free(args->home);
        free(args);
        return error_json("Could not start sign-in");
    }
    pthread_detach(thread);
    return strdup("{\"ok\":true}");
}

char* ms_steamcmd_login_code_json(const unsigned char* body, size_t length) {
    ms_json* request = parse_body(body, length);
    char* code = body_string(request, "code");
    bool ok = false;
    ms_json_free(request);
    if (!code || !code[0] || strpbrk(code, " \t\r\n")) {
        free(code);
        return error_json("Enter the Steam Guard code");
    }
    pthread_mutex_lock(&g_lock);
    if ((!strcmp(g_login.state, "needs_code") || !strcmp(g_login.state, "needs_confirmation")) &&
        g_login.input_fd >= 0) {
        size_t size = strlen(code) + 2;
        char* line = malloc(size);
        if (line) {
            snprintf(line, size, "%s\n", code);
            ok = write_line(g_login.input_fd, line);
            free(line);
        }
        if (ok) {
            snprintf(g_login.state, sizeof(g_login.state), "signing_in");
            snprintf(g_login.message, sizeof(g_login.message), "Checking the code...");
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(code);
    return ok ? strdup("{\"ok\":true}") : error_json("steamcmd is not waiting for a code");
}

char* ms_steamcmd_login_cancel_json(void) {
    pthread_mutex_lock(&g_lock);
    g_login.cancel = true;
    pthread_mutex_unlock(&g_lock);
    return strdup("{\"ok\":true}");
}

char* ms_steamcmd_status_json(const char* home) {
    char* username = saved_username(home);
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, steamcmd_installed(home));
    ms_json_writer_key(&w, "username");
    if (username)
        ms_json_writer_string(&w, username);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "signedIn");
    ms_json_writer_bool(&w, username != NULL);
    pthread_mutex_lock(&g_lock);
    ms_json_writer_key(&w, "login");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "state");
    ms_json_writer_string(&w, g_login.state);
    ms_json_writer_key(&w, "message");
    ms_json_writer_string(&w, g_login.message);
    ms_json_writer_object_end(&w);
    pthread_mutex_unlock(&g_lock);
    ms_json_writer_object_end(&w);
    free(username);
    return ms_json_writer_take(&w);
}

/* ------------------------------ libraries ------------------------------- */

static char* steam_root(const char* home) {
    return join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
}

/* "Z:\\Volumes\\SSD\\SteamLibrary" (vdf-escaped) -> /Volumes/SSD/SteamLibrary. */
static char* windows_library_path(const char* home, const char* raw) {
    char windows[PATH_MAX];
    size_t out = 0;
    for (size_t i = 0; raw[i] && out < sizeof(windows) - 1; i++) {
        if (raw[i] == '\\' && raw[i + 1] == '\\')
            i++;
        windows[out++] = raw[i] == '\\' ? '/' : raw[i];
    }
    windows[out] = '\0';
    if (out < 3 || windows[1] != ':' || windows[2] != '/')
        return NULL;
    char drive[PATH_MAX], resolved[PATH_MAX];
    snprintf(drive, sizeof(drive), "%s/prefix-steam/dosdevices/%c:", home, (char)tolower((unsigned char)windows[0]));
    if (!realpath(drive, resolved))
        return NULL;
    size_t length = strlen(resolved);
    if (length > 1 && resolved[length - 1] == '/')
        resolved[length - 1] = '\0';
    return join(strcmp(resolved, "/") ? resolved : "", windows + 3);
}

typedef struct {
    char* paths[16];
    size_t count;
} library_list;

static void add_library(library_list* list, char* path) {
    char resolved[PATH_MAX];
    if (!path)
        return;
    if (realpath(path, resolved)) {
        free(path);
        path = strdup(resolved);
    }
    for (size_t i = 0; path && i < list->count; i++)
        if (!strcmp(list->paths[i], path)) {
            free(path);
            return;
        }
    if (path && list->count < sizeof(list->paths) / sizeof(list->paths[0]))
        list->paths[list->count++] = path;
    else
        free(path);
}

static void collect_libraries(const char* home, library_list* list) {
    char* root = steam_root(home);
    char* vdf_path = root ? join(root, "steamapps/libraryfolders.vdf") : NULL;
    char* text = vdf_path ? read_text(vdf_path, 1 << 20) : NULL;
    if (root)
        add_library(list, strdup(root));
    for (char* line = text; line && *line;) {
        char* next = strchr(line, '\n');
        if (next)
            *next = '\0';
        char* key = strstr(line, "\"path\"");
        if (key) {
            char* open = strchr(key + 6, '"');
            char* close = open ? strrchr(open + 1, '"') : NULL;
            if (open && close && close > open + 1) {
                *close = '\0';
                add_library(list, windows_library_path(home, open + 1));
            }
        }
        line = next ? next + 1 : NULL;
    }
    free(text);
    free(vdf_path);
    free(root);
}

static bool is_library(const char* home, const char* path) {
    library_list list = {0};
    char resolved[PATH_MAX];
    bool found = false;
    collect_libraries(home, &list);
    for (size_t i = 0; i < list.count; i++) {
        if (!found && path && realpath(path, resolved) && !strcmp(resolved, list.paths[i]))
            found = true;
        free(list.paths[i]);
    }
    return found;
}

char* ms_steamcmd_libraries_json(const char* home) {
    library_list list = {0};
    char* root = steam_root(home);
    char resolved_root[PATH_MAX] = "";
    ms_json_writer w;
    if (root && !realpath(root, resolved_root))
        snprintf(resolved_root, sizeof(resolved_root), "%s", root);
    collect_libraries(home, &list);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "libraries");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < list.count; i++) {
        struct statfs fs;
        struct stat info;
        bool internal = !strcmp(list.paths[i], resolved_root);
        bool available = stat(list.paths[i], &info) == 0 && S_ISDIR(info.st_mode);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "path");
        ms_json_writer_string(&w, list.paths[i]);
        ms_json_writer_key(&w, "internal");
        ms_json_writer_bool(&w, internal);
        ms_json_writer_key(&w, "external");
        ms_json_writer_bool(&w, !strncmp(list.paths[i], "/Volumes/", 9));
        ms_json_writer_key(&w, "available");
        ms_json_writer_bool(&w, available);
        ms_json_writer_key(&w, "freeBytes");
        if (available && statfs(list.paths[i], &fs) == 0)
            ms_json_writer_u64(&w, (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_bsize);
        else
            ms_json_writer_null(&w);
        ms_json_writer_object_end(&w);
        free(list.paths[i]);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    free(root);
    return ms_json_writer_take(&w);
}

/* ------------------------------- installs ------------------------------- */

static void job_set(steamcmd_job* job, const char* state, const char* message) {
    pthread_mutex_lock(&g_lock);
    if (state)
        snprintf(job->state, sizeof(job->state), "%s", state);
    if (message)
        snprintf(job->message, sizeof(job->message), "%s", message);
    pthread_mutex_unlock(&g_lock);
}

static void remove_tree(const char* path) {
    char* const argv[] = {"/bin/rm", "-rf", (char*)path, NULL};
    (void)run_quiet(argv, NULL);
}

static char* manifest_value(const char* text, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* at = text ? strstr(text, pattern) : NULL;
    const char* open = at ? strchr(at + strlen(pattern), '"') : NULL;
    const char* close = open ? strchr(open + 1, '"') : NULL;
    return open && close ? strndup(open + 1, (size_t)(close - open - 1)) : NULL;
}

/* steamcmd wrote the game into the staging folder with its manifest under
 * staging/steamapps; give it Steam's normal layout in the chosen library. */
static bool finish_install(steamcmd_job* job, const char* staging, char* error, size_t error_size) {
    char manifest_name[64];
    snprintf(manifest_name, sizeof(manifest_name), "appmanifest_%u.acf", job->appid);
    char* staged_apps = join(staging, "steamapps");
    char* staged_manifest = staged_apps ? join(staged_apps, manifest_name) : NULL;
    char* text = staged_manifest ? read_text(staged_manifest, 1 << 20) : NULL;
    char* installdir = manifest_value(text, "installdir");
    char* apps = join(job->library, "steamapps");
    char* common = apps ? join(apps, "common") : NULL;
    char* target = common && installdir ? join(common, installdir) : NULL;
    char* manifest = apps ? join(apps, manifest_name) : NULL;
    bool ok = false;
    if (!text || !installdir || !installdir[0] || strchr(installdir, '/') || !strcmp(installdir, "..")) {
        snprintf(error, error_size, "steamcmd did not write an app manifest");
        goto done;
    }
    if (target && access(target, F_OK) == 0 && rmdir(target) != 0) {
        snprintf(error, error_size, "%s already exists in this library", installdir);
        goto done;
    }
    if (!target || !manifest || rename(staging, target) != 0) {
        snprintf(error, error_size, "Could not move the game into %s", common ? common : "the library");
        goto done;
    }
    {
        char* moved_apps = join(target, "steamapps");
        char* moved_manifest = moved_apps ? join(moved_apps, manifest_name) : NULL;
        if (!moved_manifest || rename(moved_manifest, manifest) != 0) {
            snprintf(error, error_size, "Could not register the game with Steam");
        } else {
            char* temp = join(moved_apps, "temp");
            char* downloading = join(moved_apps, "downloading");
            if (temp)
                remove_tree(temp);
            if (downloading)
                remove_tree(downloading);
            (void)rmdir(moved_apps);
            free(temp);
            free(downloading);
            ok = true;
        }
        free(moved_manifest);
        free(moved_apps);
    }
done:
    free(manifest);
    free(target);
    free(common);
    free(apps);
    free(installdir);
    free(text);
    free(staged_manifest);
    free(staged_apps);
    return ok;
}

static bool auth_problem(const char* line) {
    return contains_ci(line, "password:") || contains_ci(line, "Cached credentials not found") ||
           contains_ci(line, "Two-factor code:") || contains_ci(line, "Steam Guard code:") ||
           contains_ci(line, "Invalid Password") || contains_ci(line, "Login Failure") ||
           contains_ci(line, "confirm the login in the Steam Mobile app");
}

/* " Update state (0x61) downloading, progress: 4.64 (3145728 / 67863136)" */
static void parse_progress(steamcmd_job* job, const char* line) {
    const char* state = strstr(line, "Update state (");
    const char* progress = strstr(line, "progress: ");
    if (!state || !progress)
        return;
    const char* word = strchr(state, ')');
    char label[24] = "";
    double percent = 0;
    unsigned long long done = 0, total = 0;
    if (word) {
        word += 2;
        size_t length = strcspn(word, ",");
        snprintf(label, sizeof(label), "%.*s", (int)(length < sizeof(label) - 1 ? length : sizeof(label) - 1), word);
    }
    if (sscanf(progress, "progress: %lf (%llu / %llu)", &percent, &done, &total) < 1 || total == 0)
        return;
    pthread_mutex_lock(&g_lock);
    snprintf(job->state, sizeof(job->state), "%s", strstr(label, "verif") ? "verifying" : "downloading");
    job->progress = percent;
    job->done_bytes = done;
    job->total_bytes = total;
    snprintf(job->message, sizeof(job->message), "%s", label);
    pthread_mutex_unlock(&g_lock);
}

static void run_job(steamcmd_job* job) {
    char* username = saved_username(job->home);
    char staging[PATH_MAX], install_dir_arg[PATH_MAX + 32], app_arg[32];
    char buffer[4096], line[2048], error[512] = "";
    size_t line_length = 0;
    bool success = false, needs_login = false;
    int output_fd = -1;
    pid_t pid;

    if (!username) {
        pthread_mutex_lock(&g_lock);
        job->needs_login = true;
        pthread_mutex_unlock(&g_lock);
        job_set(job, "failed", "Sign in to Steam first");
        return;
    }
    job_set(job, "preparing", "Preparing steamcmd...");
    if (!ensure_steamcmd(job->home)) {
        free(username);
        job_set(job, "failed", "Could not download steamcmd");
        return;
    }
    snprintf(staging, sizeof(staging), "%s/steamapps/common/.metalsharp-install-%u", job->library, job->appid);
    if (!mkdir_p(staging)) {
        free(username);
        job_set(job, "failed", "Could not create the install folder in this library");
        return;
    }
    snprintf(install_dir_arg, sizeof(install_dir_arg), "%s", staging);
    snprintf(app_arg, sizeof(app_arg), "%u", job->appid);
    char* const extra[] = {"+@sSteamCmdForcePlatformType",
                           "windows",
                           "+force_install_dir",
                           install_dir_arg,
                           "+login",
                           username,
                           "+app_update",
                           app_arg,
                           "validate",
                           "+quit",
                           NULL};
    pid = spawn_steamcmd(job->home, extra, &output_fd, NULL);
    free(username);
    if (pid < 0) {
        job_set(job, "failed", "Could not start steamcmd");
        return;
    }
    pthread_mutex_lock(&g_lock);
    job->pid = pid;
    snprintf(job->state, sizeof(job->state), "downloading");
    snprintf(job->message, sizeof(job->message), "Signing in...");
    pthread_mutex_unlock(&g_lock);
    for (;;) {
        struct pollfd poll_fd = {.fd = output_fd, .events = POLLIN};
        bool cancel;
        pthread_mutex_lock(&g_lock);
        cancel = job->cancel;
        pthread_mutex_unlock(&g_lock);
        if (cancel || needs_login)
            break;
        int ready = poll(&poll_fd, 1, 500);
        if (ready <= 0)
            continue;
        ssize_t n = read(output_fd, buffer, sizeof(buffer));
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            char c = buffer[i];
            if (c != '\n' && c != '\r') {
                if (line_length < sizeof(line) - 1)
                    line[line_length++] = c;
                continue;
            }
            line[line_length] = '\0';
            line_length = 0;
            if (strstr(line, "Success! App '"))
                success = true;
            else if (auth_problem(line))
                needs_login = true;
            else if (contains_ci(line, "No subscription"))
                snprintf(error, sizeof(error), "This Steam account doesn't own this game");
            else if (strstr(line, "Error! App '") || strstr(line, "ERROR! "))
                snprintf(error, sizeof(error), "%s", line);
            else
                parse_progress(job, line);
        }
        /* An expired cached login leaves steamcmd waiting at a prompt. */
        if (line_length) {
            line[line_length] = '\0';
            if (auth_problem(line))
                needs_login = true;
        }
    }
    close(output_fd);
    pthread_mutex_lock(&g_lock);
    bool cancelled = job->cancel;
    pthread_mutex_unlock(&g_lock);
    finish_process(pid, cancelled || needs_login || !success);
    pthread_mutex_lock(&g_lock);
    job->pid = 0;
    cancelled = job->cancel;
    pthread_mutex_unlock(&g_lock);
    if (cancelled) {
        remove_tree(staging);
        job_set(job, "cancelled", "Install cancelled");
        return;
    }
    if (needs_login) {
        pthread_mutex_lock(&g_lock);
        job->needs_login = true;
        pthread_mutex_unlock(&g_lock);
        job_set(job, "failed", "Steam sign-in expired; sign in again");
        return;
    }
    if (!success) {
        job_set(job, "failed", error[0] ? error : "steamcmd did not finish the download");
        return;
    }
    job_set(job, "finishing", "Adding the game to Steam...");
    if (!finish_install(job, staging, error, sizeof(error))) {
        job_set(job, "failed", error);
        return;
    }
    pthread_mutex_lock(&g_lock);
    job->progress = 100;
    pthread_mutex_unlock(&g_lock);
    job_set(job, "done", "Installed");
    {
        char message[PATH_MAX + 128];
        snprintf(message, sizeof(message), "steamcmd installed %s (%u) into %s", job->name ? job->name : "game",
                 job->appid, job->library);
        ms_log_event(job->home, message);
    }
}

static void prune_finished_locked(void) {
    size_t finished = 0;
    steamcmd_job** link = &g_jobs;
    while (*link) {
        steamcmd_job* job = *link;
        bool over = !strcmp(job->state, "done") || !strcmp(job->state, "failed") || !strcmp(job->state, "cancelled");
        if (over && ++finished > MAX_FINISHED_JOBS) {
            *link = job->next;
            free(job->name);
            free(job->library);
            free(job->home);
            free(job);
            continue;
        }
        link = &job->next;
    }
}

static void* install_worker(void* unused) {
    (void)unused;
    for (;;) {
        steamcmd_job* next = NULL;
        pthread_mutex_lock(&g_lock);
        for (steamcmd_job* job = g_jobs; job; job = job->next)
            if (!strcmp(job->state, "queued"))
                next = job;
        if (!next) {
            g_worker_running = false;
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        if (next->cancel) {
            snprintf(next->state, sizeof(next->state), "cancelled");
            snprintf(next->message, sizeof(next->message), "Install cancelled");
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        pthread_mutex_unlock(&g_lock);
        run_job(next);
    }
}

char* ms_steamcmd_install_json(const char* home, const unsigned char* body, size_t length) {
    ms_json* request = parse_body(body, length);
    long long appid = 0;
    char* library = body_string(request, "library");
    char* name = body_string(request, "name");
    steamcmd_job* job;
    bool start_worker;
    (void)ms_json_as_i64(ms_json_object_get(request, "appid"), &appid);
    ms_json_free(request);
    if (appid <= 0 || appid > 0xffffffffLL || !library || !is_library(home, library)) {
        free(library);
        free(name);
        return error_json("Choose one of the Steam libraries to install into");
    }
    pthread_mutex_lock(&g_lock);
    if (login_active_locked()) {
        pthread_mutex_unlock(&g_lock);
        free(library);
        free(name);
        return error_json("Finish signing in to Steam first");
    }
    for (job = g_jobs; job; job = job->next)
        if (job->appid == (unsigned)appid && strcmp(job->state, "done") && strcmp(job->state, "failed") &&
            strcmp(job->state, "cancelled")) {
            pthread_mutex_unlock(&g_lock);
            free(library);
            free(name);
            return error_json("This game is already being installed");
        }
    job = calloc(1, sizeof(*job));
    if (!job || !(job->home = strdup(home))) {
        pthread_mutex_unlock(&g_lock);
        free(job);
        free(library);
        free(name);
        return error_json("Out of memory");
    }
    char resolved[PATH_MAX];
    job->appid = (unsigned)appid;
    job->library = realpath(library, resolved) ? strdup(resolved) : library;
    if (job->library != library)
        free(library);
    job->name = name;
    snprintf(job->state, sizeof(job->state), "queued");
    snprintf(job->message, sizeof(job->message), "Waiting to install");
    job->next = g_jobs;
    g_jobs = job;
    prune_finished_locked();
    start_worker = !g_worker_running;
    g_worker_running = true;
    pthread_mutex_unlock(&g_lock);
    if (start_worker) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, install_worker, NULL) == 0)
            pthread_detach(thread);
        else {
            pthread_mutex_lock(&g_lock);
            g_worker_running = false;
            snprintf(job->state, sizeof(job->state), "failed");
            snprintf(job->message, sizeof(job->message), "Could not start the installer");
            pthread_mutex_unlock(&g_lock);
            return error_json("Could not start the installer");
        }
    }
    return strdup("{\"ok\":true}");
}

char* ms_steamcmd_installs_json(void) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "installs");
    ms_json_writer_array_begin(&w);
    pthread_mutex_lock(&g_lock);
    for (steamcmd_job* job = g_jobs; job; job = job->next) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, job->appid);
        ms_json_writer_key(&w, "name");
        ms_json_writer_string(&w, job->name ? job->name : "");
        ms_json_writer_key(&w, "library");
        ms_json_writer_string(&w, job->library ? job->library : "");
        ms_json_writer_key(&w, "state");
        ms_json_writer_string(&w, job->state);
        ms_json_writer_key(&w, "message");
        ms_json_writer_string(&w, job->message);
        ms_json_writer_key(&w, "progress");
        ms_json_writer_double(&w, job->progress);
        ms_json_writer_key(&w, "downloadedBytes");
        ms_json_writer_u64(&w, job->done_bytes);
        ms_json_writer_key(&w, "totalBytes");
        ms_json_writer_u64(&w, job->total_bytes);
        ms_json_writer_key(&w, "needsLogin");
        ms_json_writer_bool(&w, job->needs_login);
        ms_json_writer_object_end(&w);
    }
    pthread_mutex_unlock(&g_lock);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_steamcmd_cancel_json(const unsigned char* body, size_t length) {
    ms_json* request = parse_body(body, length);
    long long appid = 0;
    bool found = false;
    pid_t pid = 0;
    (void)ms_json_as_i64(ms_json_object_get(request, "appid"), &appid);
    ms_json_free(request);
    pthread_mutex_lock(&g_lock);
    for (steamcmd_job* job = g_jobs; job; job = job->next)
        if (job->appid == (unsigned)appid && strcmp(job->state, "done") && strcmp(job->state, "failed") &&
            strcmp(job->state, "cancelled")) {
            job->cancel = true;
            pid = job->pid;
            found = true;
        }
    pthread_mutex_unlock(&g_lock);
    if (pid > 1)
        (void)kill(-pid, SIGTERM);
    return found ? strdup("{\"ok\":true}") : error_json("This game is not being installed");
}

char* ms_steamcmd_stop_all_json(void) {
    pid_t pids[64];
    size_t count = 0;
    pthread_mutex_lock(&g_lock);
    for (steamcmd_job* job = g_jobs; job; job = job->next) {
        job->cancel = true;
        if (job->pid > 1 && count < sizeof(pids) / sizeof(pids[0]))
            pids[count++] = job->pid;
    }
    g_login.cancel = true;
    if (g_login.pid > 1 && count < sizeof(pids) / sizeof(pids[0]))
        pids[count++] = g_login.pid;
    pthread_mutex_unlock(&g_lock);
    for (size_t i = 0; i < count; i++)
        (void)kill(-pids[i], SIGTERM);
    return strdup("{\"ok\":true}");
}
