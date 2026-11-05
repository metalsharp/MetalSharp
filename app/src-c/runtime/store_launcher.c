#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/store_launcher.h"
#include "metalsharp_backend/game_executable.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/library_locations.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/wine_runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

/* The routes a store game can take: Ubisoft's list. */
static const char* const STORE_PIPELINE_IDS[] = {"d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9"};
static const char* const STORE_PIPELINE_NAMES[] = {"D3DMetal", "DXMT", "DXMT (32-bit)", "VKD3D", "D3D9"};

char* ms_store_path_join(const char* a, const char* b) {
    size_t x, y;
    bool slash;
    char* result;
    if (!a || !b)
        return NULL;
    x = strlen(a);
    y = strlen(b);
    slash = x > 0 && a[x - 1] != '/';
    result = malloc(x + y + (slash ? 2 : 1));
    if (result)
        snprintf(result, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return result;
}

bool ms_store_is_dir(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool ms_store_is_file(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool ms_store_mkdir_p(const char* path) {
    char* copy;
    if (!path || !path[0] || !(copy = strdup(path)))
        return false;
    for (size_t i = 1; copy[i]; i++) {
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

char* ms_store_read_text(const char* path, size_t max_size) {
    FILE* file;
    long length;
    char* text;
    size_t got;
    if (!path || !(file = fopen(path, "rb")))
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || (size_t)length > max_size ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)length + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    got = fread(text, 1, (size_t)length, file);
    fclose(file);
    text[got] = '\0';
    return text;
}

bool ms_store_contains_ci(const char* text, const char* needle) {
    size_t n = needle ? strlen(needle) : 0;
    if (!n)
        return true;
    for (; text && *text; text++)
        if (!strncasecmp(text, needle, n))
            return true;
    return false;
}

char* ms_store_error_json(const char* message) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message ? message : "");
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_store_wine(const char* home) {
    char* wine = ms_store_path_join(home, "runtime/wine/bin/metalsharp-wine");
    if (wine && access(wine, X_OK) == 0)
        return wine;
    free(wine);
    wine = ms_store_path_join(home, "runtime/wine/bin/wine");
    if (wine && access(wine, X_OK) == 0)
        return wine;
    free(wine);
    return NULL;
}

char* ms_store_wineserver(const char* home) {
    char* server = ms_store_path_join(home, "runtime/wine/bin/wineserver");
    if (server && access(server, X_OK) == 0)
        return server;
    free(server);
    return NULL;
}

void ms_store_write_state(const char* home, const char* cache_dir, const char* state, const char* error) {
    char* parent = ms_store_path_join(home, cache_dir);
    char* path = parent ? ms_store_path_join(parent, "state.json") : NULL;
    ms_json_writer writer;
    char* text;
    FILE* file;
    if (!path || !ms_store_mkdir_p(parent)) {
        free(path);
        free(parent);
        return;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "state");
    ms_json_writer_string(&writer, state ? state : "idle");
    ms_json_writer_key(&writer, "error");
    if (error && error[0])
        ms_json_writer_string(&writer, error);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "updated_at");
    ms_json_writer_u64(&writer, (unsigned long long)time(NULL));
    ms_json_writer_object_end(&writer);
    text = ms_json_writer_take(&writer);
    if (text && (file = fopen(path, "wb")) != NULL) {
        (void)fwrite(text, 1, strlen(text), file);
        fclose(file);
    }
    free(text);
    free(path);
    free(parent);
}

char* ms_store_status_json(const char* home, const char* cache_dir, bool installed, bool running, bool worker_active,
                           const char* prefix) {
    char* parent = ms_store_path_join(home, cache_dir);
    char* state_file = parent ? ms_store_path_join(parent, "state.json") : NULL;
    char* state_text = state_file ? ms_store_read_text(state_file, 8192) : NULL;
    ms_json* state_json = state_text ? ms_json_parse(state_text, strlen(state_text), NULL, 0) : NULL;
    char* error = NULL;
    const char* state = installed ? "ready" : "not_installed";
    bool installing = false;
    ms_json_writer w;
    if (state_json) {
        char* stored = NULL;
        if (ms_json_as_string(ms_json_object_get(state_json, "state"), &stored) && stored) {
            if ((!strcmp(stored, "downloading") || !strcmp(stored, "installing")) && worker_active) {
                state = !strcmp(stored, "downloading") ? "downloading" : "installing";
                installing = true;
            } else if (!installed && !strcmp(stored, "error")) {
                state = "error";
                (void)ms_json_as_string(ms_json_object_get(state_json, "error"), &error);
            }
        }
        free(stored);
    }
    /* The worker is running but has not written its first state yet. */
    if (worker_active && !installing) {
        state = "installing";
        installing = true;
        free(error);
        error = NULL;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, installed);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, running);
    ms_json_writer_key(&w, "installing");
    ms_json_writer_bool(&w, installing);
    ms_json_writer_key(&w, "state");
    ms_json_writer_string(&w, state);
    ms_json_writer_key(&w, "error");
    if (error)
        ms_json_writer_string(&w, error);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "prefix");
    ms_json_writer_string(&w, prefix ? prefix : "");
    ms_json_writer_object_end(&w);
    free(parent);
    free(state_file);
    free(state_text);
    free(error);
    ms_json_free(state_json);
    return ms_json_writer_take(&w);
}

bool ms_store_download(const char* url, const char* destination) {
    pid_t pid, waited;
    struct stat st;
    int status = 0;
    char* args[] = {"/usr/bin/curl",
                    "--fail",
                    "--silent",
                    "--show-error",
                    "--location",
                    "--proto",
                    "=https",
                    "--proto-redir",
                    "=https",
                    "--retry",
                    "3",
                    "--connect-timeout",
                    "15",
                    "--max-time",
                    "1200",
                    "--max-filesize",
                    "536870912",
                    "--output",
                    (char*)destination,
                    (char*)url,
                    NULL};
    if (!url || !destination || strncmp(url, "https://", 8))
        return false;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execv(args[0], args);
        _exit(127);
    }
    do
        waited = waitpid(pid, &status, 0);
    while (waited < 0 && errno == EINTR);
    bool valid = waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 && stat(destination, &st) == 0 &&
                 S_ISREG(st.st_mode) && st.st_size >= 1024 * 1024 && st.st_size <= 512 * 1024 * 1024;
    if (!valid)
        (void)unlink(destination);
    return valid;
}

bool ms_store_run_wine(const char* home, const char* prefix, const char* cwd, char* const argv[],
                       ms_store_apply_env apply_env, const void* env_context, bool wait, int* status, pid_t* pid_out) {
    pid_t pid, waited;
    int result = 0;
    if (!home || !prefix || !argv || !argv[0])
        return false;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        if (!wait)
            (void)setpgid(0, 0);
        if (cwd)
            (void)chdir(cwd);
        setenv("WINEPREFIX", prefix, 1);
        setenv("METALSHARP_HOME", home, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        ms_steam_apply_launch_preferences(home);
        if (apply_env)
            apply_env(home, env_context);
        execv(argv[0], argv);
        _exit(127);
    }
    if (!wait) {
        (void)setpgid(pid, pid);
        if (pid_out)
            *pid_out = pid;
        return true;
    }
    do
        waited = waitpid(pid, &result, 0);
    while (waited < 0 && errno == EINTR);
    if (waited != pid)
        return false;
    if (status)
        *status = WIFEXITED(result) ? WEXITSTATUS(result) : 128;
    return true;
}

static void apply_route_env(const char* home, const void* context) {
    ms_steam_apply_graphics_route(home, (const char*)context);
}

bool ms_store_ensure_drive_mapping(const char* prefix, const char* volume) {
    char *dosdevices = ms_store_path_join(prefix, "dosdevices"), *drive = NULL;
    struct stat st;
    bool ok = false;
    if (!dosdevices || !ms_store_mkdir_p(dosdevices))
        goto done;
    drive = ms_store_path_join(dosdevices, "y:");
    if (!drive)
        goto done;
    if (lstat(drive, &st) == 0) {
        char existing[PATH_MAX];
        ssize_t length = readlink(drive, existing, sizeof(existing) - 1);
        if (length > 0) {
            char resolved[PATH_MAX];
            existing[length] = '\0';
            if (realpath(existing, resolved))
                ok = true; /* The volume, or a user-managed Y: mapping that still works. */
            else if (volume && unlink(drive) == 0)
                ok = symlink(volume, drive) == 0;
        } else
            ok = true;
        goto done;
    }
    if (errno == ENOENT && volume)
        ok = symlink(volume, drive) == 0;
done:
    free(dosdevices);
    free(drive);
    return ok;
}

bool ms_store_initialize_prefix(const char* home, const char* prefix, const char* launcher, const char* label,
                                char* error, size_t error_size) {
    char* wine = ms_store_wine(home);
    char* reg = prefix ? ms_store_path_join(prefix, "system.reg") : NULL;
    struct stat st;
    bool initialized = reg && stat(reg, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    bool ok = true;
    if (!prefix || !wine || !ms_store_mkdir_p(prefix)) {
        snprintf(error, error_size, "MetalSharp Wine is not available to initialize %s", label);
        ok = false;
    } else if (ms_wine_is_fex()) {
        ok = ms_wine_fex_convert_prefix(home, prefix, error, error_size);
    } else if (!initialized) {
        char* argv[] = {wine, "wineboot", "-u", NULL};
        int status = 1;
        ok = ms_store_run_wine(home, prefix, NULL, argv, apply_route_env, "d3dmetal", true, &status, NULL) &&
             status == 0;
        if (!ok)
            snprintf(error, error_size, "Wine could not initialize the %s prefix", label);
    }
    if (ok) {
        char* external_volume =
            launcher && ms_library_location_apply(home, launcher) ? NULL : ms_library_best_external_volume();
        if (external_volume && !ms_store_ensure_drive_mapping(prefix, external_volume)) {
            snprintf(error, error_size, "Could not map the external volume to %s's Y: drive", label);
            ok = false;
        }
        free(external_volume);
    }
    free(wine);
    free(reg);
    return ok;
}

static bool wait_wineserver_shutdown(const char* server, const char* prefix) {
    pid_t pid = fork();
    int status = 0;
    if (pid < 0)
        return false;
    if (pid == 0) {
        char* args[] = {(char*)server, "-w", NULL};
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        execv(server, args);
        _exit(127);
    }
    for (unsigned i = 0; i < 40; i++) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (waited < 0 && errno != EINTR)
            return false;
        usleep(50000);
    }
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}

bool ms_store_stop_prefix(const char* home, const char* prefix) {
    char* server;
    pid_t pid, waited;
    int result = 0;
    if (!prefix || !ms_store_is_dir(prefix))
        return true;
    server = ms_store_wineserver(home);
    if (!server)
        return false;
    pid = fork();
    if (pid == 0) {
        char* args[] = {server, "-k", NULL};
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        execv(server, args);
        _exit(127);
    }
    if (pid < 0) {
        free(server);
        return false;
    }
    do
        waited = waitpid(pid, &result, 0);
    while (waited < 0 && errno == EINTR);
    bool stopped = waited == pid && WIFEXITED(result) && WEXITSTATUS(result) == 0;
    /* wineserver -k returns 1 when the prefix has no server: check it is stopped. */
    if (!stopped && waited == pid && WIFEXITED(result) && WEXITSTATUS(result) == 1)
        stopped = wait_wineserver_shutdown(server, prefix);
    free(server);
    return stopped;
}

bool ms_store_process_cwd_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    struct proc_vnodepathinfo info;
    int bytes = proc_pidinfo((int)pid, PROC_PIDVNODEPATHINFO, 0, &info, (int)sizeof(info));
    return root && bytes == (int)sizeof(info) && ms_resolved_path_within(info.pvi_cdir.vip_path, root);
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

static bool process_executable_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    char executable[PROC_PIDPATHINFO_MAXSIZE];
    int bytes = proc_pidpath((int)pid, executable, sizeof(executable));
    return bytes > 0 && ms_resolved_path_within(executable, root);
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

size_t ms_store_wine_processes(const char* home, ms_store_process_match match, void* context, int signal_number,
                               pid_t* first, bool* failed) {
    char runtime[PATH_MAX], line[8192];
    FILE* pipe;
    size_t count = 0;
    if (first)
        *first = 0;
    if (failed)
        *failed = false;
    if (!home || !match || !ms_wine_process_root(home, runtime, sizeof(runtime))) {
        if (failed)
            *failed = true;
        return 0;
    }
    pipe = popen("/bin/ps axo pid=,pgid=,command=", "r");
    if (!pipe) {
        if (failed)
            *failed = true;
        return 0;
    }
    while (fgets(line, sizeof(line), pipe)) {
        long raw_pid = 0, raw_group = 0;
        int offset = 0;
        char* newline;
        if (sscanf(line, "%ld %ld %n", &raw_pid, &raw_group, &offset) != 2 || raw_pid <= 1 || raw_pid > INT_MAX ||
            raw_pid == (long)getpid() || offset <= 0)
            continue;
        if ((newline = strchr(line + offset, '\n')) != NULL)
            *newline = '\0';
        if (!match((pid_t)raw_pid, (pid_t)raw_group, line + offset, context) ||
            !process_executable_within((pid_t)raw_pid, runtime))
            continue;
        if (first && !*first)
            *first = (pid_t)raw_pid;
        if (signal_number)
            (void)kill((pid_t)raw_pid, signal_number);
        count++;
    }
    int pipe_status = pclose(pipe);
    if ((pipe_status == -1 || !WIFEXITED(pipe_status) || WEXITSTATUS(pipe_status) != 0) && failed)
        *failed = true;
    return count;
}

bool ms_store_command_runs(const char* command, const char* executable) {
    const char* basename;
    size_t length;
    if (!command || !executable)
        return false;
    basename = strrchr(executable, '/');
    basename = basename ? basename + 1 : executable;
    if (strrchr(basename, '\\'))
        basename = strrchr(basename, '\\') + 1;
    length = strlen(basename);
    if (!length)
        return false;
    for (const char* match = command; *match; match++) {
        if (!strncasecmp(match, basename, length)) {
            bool start = match == command || match[-1] == '/' || match[-1] == '\\' || match[-1] == '"';
            char after = match[length];
            if (start && (!after || isspace((unsigned char)after) || after == '"' || after == '\''))
                return true;
        }
    }
    return false;
}

/* Where text holds needle[0..length), case and path separators aside; returns the end of the match. */
static const char* find_path_ci(const char* text, const char* needle, size_t length) {
    if (!text || !needle || !length)
        return NULL;
    for (const char* at = text; *at; at++) {
        size_t i = 0;
        for (; i < length && at[i]; i++) {
            char a = at[i] == '/' ? '\\' : (char)tolower((unsigned char)at[i]);
            char b = needle[i] == '/' ? '\\' : (char)tolower((unsigned char)needle[i]);
            if (a != b)
                break;
        }
        if (i == length)
            return at + length;
    }
    return NULL;
}

bool ms_store_contains_path_ci(const char* text, const char* needle) {
    return find_path_ci(text, needle, needle ? strlen(needle) : 0) != NULL;
}

bool ms_store_command_in_windows_dir(const char* command, const char* windows_dir) {
    size_t length = windows_dir ? strlen(windows_dir) : 0;
    while (length > 0 && (windows_dir[length - 1] == '\\' || windows_dir[length - 1] == '/'))
        length--;
    if (!command || length < 4)
        return false;
    for (const char* at = command; at && *at; at++) {
        const char* end = find_path_ci(at, windows_dir, length);
        if (!end)
            return false;
        if (*end == '\\' || *end == '/')
            return true;
        at = end - length;
    }
    return false;
}

char* ms_store_registry_string(const char* value) {
    const char* p = value;
    size_t n = 0;
    char* out;
    if (!p || *p != '"')
        return NULL;
    p++;
    out = malloc(strlen(p) + 1);
    if (!out)
        return NULL;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return out;
}

bool ms_store_registry_value_is(const char* line, const char* name) {
    size_t length = name ? strlen(name) : 0;
    return line && line[0] == '"' && length && !strncasecmp(line + 1, name, length) && line[1 + length] == '"' &&
           line[2 + length] == '=';
}

char* ms_store_wine_path_to_host(const char* prefix, const char* value) {
    char* base = NULL;
    char* relative;
    char drive;
    size_t length;
    if (!prefix || !value || !isalpha((unsigned char)value[0]) || value[1] != ':' ||
        (value[2] != '\\' && value[2] != '/'))
        return NULL;
    drive = (char)tolower((unsigned char)value[0]);
    relative = strdup(value + 3);
    if (!relative)
        return NULL;
    for (char* p = relative; *p; p++)
        if (*p == '\\')
            *p = '/';
    length = strlen(relative);
    while (length > 0 && relative[length - 1] == '/')
        relative[--length] = '\0';
    if (strstr(relative, "../") || !strcmp(relative, "..") ||
        (length >= 3 && !strcmp(relative + length - 3, "/.."))) {
        free(relative);
        return NULL;
    }
    if (drive == 'c')
        base = ms_store_path_join(prefix, "drive_c");
    else {
        char linkname[32], target[PATH_MAX];
        char* linkpath;
        snprintf(linkname, sizeof(linkname), "dosdevices/%c:", drive);
        linkpath = ms_store_path_join(prefix, linkname);
        if (linkpath && realpath(linkpath, target))
            base = strdup(target);
        free(linkpath);
    }
    char* joined = base ? (length ? ms_store_path_join(base, relative) : strdup(base)) : NULL;
    free(base);
    free(relative);
    return joined;
}

bool ms_store_path_in_steam_library(const char* windows_path, const char* host_path) {
    char resolved[PATH_MAX];
    if (ms_store_contains_ci(windows_path, "\\steamapps\\") || ms_store_contains_ci(windows_path, "/steamapps/") ||
        ms_store_contains_ci(host_path, "/steamapps/"))
        return true;
    if (host_path && realpath(host_path, resolved) && ms_store_contains_ci(resolved, "/steamapps/"))
        return true;
    return false;
}

bool ms_store_slug(const char* text, char* out, size_t out_size) {
    size_t n = 0;
    bool dash = false;
    if (!text || !out || out_size < 2)
        return false;
    for (const unsigned char* p = (const unsigned char*)text; *p && n + 1 < out_size; p++) {
        if (isalnum(*p)) {
            if (dash && n > 0 && n + 2 < out_size)
                out[n++] = '-';
            dash = false;
            out[n++] = (char)tolower(*p);
        } else
            dash = true;
    }
    out[n] = '\0';
    return n > 0;
}

bool ms_store_valid_id(const char* id) {
    size_t length = id ? strlen(id) : 0;
    if (!length || length > 128)
        return false;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)id[i]) && id[i] != '_' && id[i] != '-' && id[i] != '.')
            return false;
    return strcmp(id, ".") && strcmp(id, "..");
}

unsigned ms_store_fnv1a(const char* text) {
    uint32_t hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)text; p && *p; p++)
        hash = (hash ^ *p) * 16777619u;
    return hash;
}

static bool ignored_executable(const char* name) {
    static const char* const ignored[] = {
        "setup",    "install",       "uninstall", "unins",           "crash",    "reporter", "redist",
        "battleye", "easyanticheat", "vcredist",  "redistributable", "prereq",   "directx",  "dxsetup",
        "dotnet",   "errorhandler",  "cefsharp",  "subprocess",      "touchup",  "helper"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        if (ms_store_contains_ci(name, ignored[i]))
            return true;
    return false;
}

static bool ignored_directory(const char* name) {
    static const char* const ignored[] = {"support", "redist",    "redistributable", "__installer", "_commonredist",
                                          "prereq",  "installer", "directx",         "dotnet",      "redistributables"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        if (ms_store_contains_ci(name, ignored[i]))
            return true;
    return false;
}

void ms_store_largest_executable(const char* path, unsigned depth, unsigned max_depth,
                                 bool (*ignore_name)(const char* name), char** best, off_t* best_size) {
    DIR* directory;
    struct dirent* entry;
    if (depth > max_depth || !(directory = opendir(path)))
        return;
    while ((entry = readdir(directory)) != NULL) {
        char* child;
        size_t n = strlen(entry->d_name);
        struct stat st;
        if (entry->d_name[0] == '.')
            continue;
        child = ms_store_path_join(path, entry->d_name);
        if (!child)
            continue;
        if (n > 4 && !strcasecmp(entry->d_name + n - 4, ".exe") && !ignored_executable(entry->d_name) &&
            !(ignore_name && ignore_name(entry->d_name)) && stat(child, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_size > *best_size) {
            char* replacement = strdup(child);
            if (replacement) {
                free(*best);
                *best = replacement;
                *best_size = st.st_size;
            }
        } else if (depth < max_depth && !ignored_directory(entry->d_name) && ms_store_is_dir(child)) {
            ms_store_largest_executable(child, depth + 1, max_depth, ignore_name, best, best_size);
        }
        free(child);
    }
    closedir(directory);
}

int ms_store_game_executable(const char* home, const ms_store_provider* provider, const ms_store_game* game,
                             char** executable) {
    int state;
    *executable = NULL;
    state = ms_game_executable_override_load(home, provider->source, game->id, game->path, executable);
    if (state == 0) {
        free(*executable);
        *executable = provider->find_executable ? provider->find_executable(game) : NULL;
    }
    return state;
}

const char* ms_store_canonical_pipeline(const char* pipeline) {
    for (size_t i = 0; pipeline && i < sizeof(STORE_PIPELINE_IDS) / sizeof(STORE_PIPELINE_IDS[0]); i++)
        if (!strcasecmp(pipeline, STORE_PIPELINE_IDS[i]))
            return STORE_PIPELINE_IDS[i];
    return NULL;
}

static char* bottle_manifest_path(const char* home, const ms_store_provider* provider, unsigned appid) {
    char relative[128];
    snprintf(relative, sizeof(relative), "bottles/%s_%u/bottle.json", provider->source, appid);
    return ms_store_path_join(home, relative);
}

char* ms_store_pipeline_for_game(const char* home, const ms_store_provider* provider, unsigned appid) {
    char* path = bottle_manifest_path(home, provider, appid);
    char* data = path ? ms_store_read_text(path, 65536) : NULL;
    ms_json* json = data ? ms_json_parse(data, strlen(data), NULL, 0) : NULL;
    char* stored = NULL;
    char* result;
    if (json)
        (void)ms_json_as_string(ms_json_object_get(json, "preferred_pipeline"), &stored);
    result = strdup(ms_store_canonical_pipeline(stored) ? ms_store_canonical_pipeline(stored)
                                                        : provider->default_pipeline);
    free(stored);
    free(path);
    free(data);
    ms_json_free(json);
    return result;
}

/* Executable icons, extracted in the background into Ubisoft's artwork cache (keyed by appid,
 * whose namespaces keep the stores apart). */
static pthread_mutex_t icon_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    unsigned appid;
    bool pending;
} icon_jobs[512];
static size_t icon_job_count;

typedef struct {
    char* home;
    char* executable;
    unsigned appid;
} icon_job;

static void finish_icon_job(unsigned appid) {
    pthread_mutex_lock(&icon_jobs_lock);
    for (size_t i = 0; i < icon_job_count; i++)
        if (icon_jobs[i].appid == appid) {
            icon_jobs[i].pending = false;
            break;
        }
    pthread_mutex_unlock(&icon_jobs_lock);
}

static void* icon_worker(void* opaque) {
    icon_job* job = opaque;
    free(ms_steam_extract_executable_icon(job->home, job->executable, job->appid));
    finish_icon_job(job->appid);
    free(job->home);
    free(job->executable);
    free(job);
    return NULL;
}

static bool queue_icon_extraction(const char* home, const char* executable, unsigned appid) {
    icon_job* job;
    pthread_t thread;
    pthread_mutex_lock(&icon_jobs_lock);
    for (size_t i = 0; i < icon_job_count; i++)
        if (icon_jobs[i].appid == appid) {
            bool pending = icon_jobs[i].pending;
            pthread_mutex_unlock(&icon_jobs_lock);
            return pending;
        }
    if (icon_job_count >= sizeof(icon_jobs) / sizeof(icon_jobs[0])) {
        pthread_mutex_unlock(&icon_jobs_lock);
        return false;
    }
    icon_jobs[icon_job_count].appid = appid;
    icon_jobs[icon_job_count].pending = true;
    icon_job_count++;
    pthread_mutex_unlock(&icon_jobs_lock);
    job = calloc(1, sizeof(*job));
    if (job) {
        job->home = strdup(home);
        job->executable = strdup(executable);
        job->appid = appid;
    }
    if (!job || !job->home || !job->executable || pthread_create(&thread, NULL, icon_worker, job) != 0) {
        if (job) {
            free(job->home);
            free(job->executable);
            free(job);
        }
        finish_icon_job(appid);
        return false;
    }
    (void)pthread_detach(thread);
    return true;
}

void ms_store_write_game_json(ms_json_writer* w, const char* home, const ms_store_provider* provider,
                              const ms_store_game* game) {
    unsigned appid = provider->appid(game->id);
    char bottle[96], icon_relative[128];
    char* pipeline = ms_store_pipeline_for_game(home, provider, appid);
    char* executable = NULL;
    char* icon = NULL;
    bool icon_pending = false;
    (void)ms_store_game_executable(home, provider, game, &executable);
    snprintf(icon_relative, sizeof(icon_relative), "cache/ubisoft-connect/artwork/%u.png", appid);
    icon = ms_store_path_join(home, icon_relative);
    if (!ms_store_is_file(icon)) {
        free(icon);
        icon = NULL;
        icon_pending = executable && queue_icon_extraction(home, executable, appid);
    }
    snprintf(bottle, sizeof(bottle), "%s_%u", provider->source, appid);
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "source");
    ms_json_writer_string(w, provider->source);
    ms_json_writer_key(w, "appid");
    ms_json_writer_u64(w, appid);
    ms_json_writer_key(w, provider->id_key);
    ms_json_writer_string(w, game->id);
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, game->name);
    ms_json_writer_key(w, "installed");
    ms_json_writer_bool(w, true);
    ms_json_writer_key(w, "state");
    ms_json_writer_string(w, "installed");
    ms_json_writer_key(w, "game_dir");
    ms_json_writer_string(w, game->path);
    ms_json_writer_key(w, "executable_path");
    if (executable)
        ms_json_writer_string(w, executable);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "embedded_icon_path");
    if (icon)
        ms_json_writer_string(w, icon);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "icon_pending");
    ms_json_writer_bool(w, icon_pending);
    ms_json_writer_key(w, "bottle_id");
    ms_json_writer_string(w, bottle);
    ms_json_writer_key(w, "launch_method");
    ms_json_writer_string(w, provider->source);
    ms_json_writer_key(w, "preferred_pipeline");
    ms_json_writer_string(w, pipeline ? pipeline : provider->default_pipeline);
    ms_json_writer_key(w, "available_pipelines");
    ms_json_writer_array_begin(w);
    for (size_t i = 0; i < sizeof(STORE_PIPELINE_IDS) / sizeof(STORE_PIPELINE_IDS[0]); i++) {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "id");
        ms_json_writer_string(w, STORE_PIPELINE_IDS[i]);
        ms_json_writer_key(w, "name");
        ms_json_writer_string(w, STORE_PIPELINE_NAMES[i]);
        ms_json_writer_key(w, "recommended");
        ms_json_writer_bool(w, !strcmp(STORE_PIPELINE_IDS[i], provider->default_pipeline));
        ms_json_writer_object_end(w);
    }
    ms_json_writer_array_end(w);
    ms_json_writer_object_end(w);
    free(pipeline);
    free(icon);
    free(executable);
}

char* ms_store_library_json(const char* home, const ms_store_provider* provider, const ms_store_game* games,
                            size_t count) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "total");
    ms_json_writer_u64(&w, count);
    ms_json_writer_key(&w, "installed_count");
    ms_json_writer_u64(&w, count);
    ms_json_writer_key(&w, "games");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < count; i++)
        ms_store_write_game_json(&w, home, provider, &games[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

const ms_store_game* ms_store_request_game(const ms_store_provider* provider, const ms_store_game* games,
                                           size_t count, const char* body, size_t body_length, char** pipeline) {
    ms_json* json = body && body_length ? ms_json_parse(body, body_length, NULL, 0) : NULL;
    const ms_store_game* found = NULL;
    char* id = NULL;
    long long appid = -1;
    if (pipeline)
        *pipeline = NULL;
    if (!json || ms_json_type_of(json) != MS_JSON_OBJECT) {
        ms_json_free(json);
        return NULL;
    }
    (void)ms_json_as_string(ms_json_object_get(json, provider->id_key), &id);
    (void)ms_json_as_i64(ms_json_object_get(json, "appid"), &appid);
    for (size_t i = 0; !found && i < count; i++)
        if (id ? !strcmp(games[i].id, id) : (appid > 0 && provider->appid(games[i].id) == (unsigned long long)appid))
            found = &games[i];
    if (found && pipeline)
        (void)ms_json_as_string(ms_json_object_get(json, "pipeline"), pipeline);
    free(id);
    ms_json_free(json);
    return found;
}

static bool write_bottle(const char* home, const ms_store_provider* provider, const ms_store_game* game,
                         unsigned appid, const char* pipeline) {
    char bottle[96];
    char* path = bottle_manifest_path(home, provider, appid);
    char* parent = path ? strdup(path) : NULL;
    char* data;
    ms_json_writer w;
    FILE* file;
    bool ok;
    if (!parent || !strrchr(parent, '/')) {
        free(path);
        free(parent);
        return false;
    }
    *strrchr(parent, '/') = '\0';
    snprintf(bottle, sizeof(bottle), "%s_%u", provider->source, appid);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, bottle);
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, game->name);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, provider->source);
    ms_json_writer_key(&w, provider->id_key);
    ms_json_writer_string(&w, game->id);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, "preferred_pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "game_dir");
    ms_json_writer_string(&w, game->path);
    ms_json_writer_key(&w, "game_install_path");
    ms_json_writer_string(&w, game->path);
    ms_json_writer_object_end(&w);
    data = ms_json_writer_take(&w);
    ok = data && ms_store_mkdir_p(parent) && (file = fopen(path, "wb")) != NULL;
    if (ok) {
        ok = fwrite(data, 1, strlen(data), file) == strlen(data);
        ok = fclose(file) == 0 && ok;
    }
    free(data);
    free(path);
    free(parent);
    return ok;
}

char* ms_store_save_pipeline_json(const char* home, const ms_store_provider* provider, const ms_store_game* games,
                                  size_t count, const char* body, size_t body_length, int* status) {
    char *requested = NULL, *executable = NULL, *out = NULL;
    char failure[512];
    const char* pipeline;
    const ms_store_game* game = ms_store_request_game(provider, games, count, body, body_length, &requested);
    unsigned appid;
    int code = 400;
    snprintf(failure, sizeof(failure), "Could not save the %s game's graphics route", provider->label);
    pipeline = ms_store_canonical_pipeline(requested);
    if (!game) {
        code = 404;
        snprintf(failure, sizeof(failure), "Installed %s game was not found in the local registry", provider->label);
        goto done;
    }
    if (!pipeline) {
        snprintf(failure, sizeof(failure), "Unsupported graphics route for %s", game->name);
        goto done;
    }
    if (ms_store_game_executable(home, provider, game, &executable) < 0 || !executable) {
        code = 404;
        snprintf(failure, sizeof(failure), "No launchable Windows executable was found for %s", game->name);
        goto done;
    }
    code = 500;
    if (!ms_steam_stage_route_for_executable(home, pipeline, game->path, executable)) {
        snprintf(failure, sizeof(failure), "Could not stage the %s graphics route for %s", pipeline, game->name);
        goto done;
    }
    appid = provider->appid(game->id);
    /* The game takes this route when its launcher starts it too (the fex build). */
    if (!ms_steam_write_game_launch_env(home, appid, pipeline, game->path, executable)) {
        snprintf(failure, sizeof(failure), "Could not write the %s launch environment for %s", pipeline, game->name);
        goto done;
    }
    if (!write_bottle(home, provider, game, appid, pipeline))
        goto done;
    {
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pipeline");
        ms_json_writer_string(&w, pipeline);
        ms_json_writer_key(&w, "staged_executable");
        ms_json_writer_string(&w, executable);
        ms_json_writer_object_end(&w);
        out = ms_json_writer_take(&w);
        code = 200;
    }
done:
    if (status)
        *status = code;
    if (!out)
        out = ms_store_error_json(failure);
    free(requested);
    free(executable);
    return out;
}

char* ms_store_save_executable_json(const char* home, const ms_store_provider* provider, const ms_store_game* games,
                                    size_t count, const char* body, size_t body_length, int* status) {
    char failure[256];
    char* executable = NULL;
    char* out = NULL;
    const ms_store_game* game = ms_store_request_game(provider, games, count, body, body_length, NULL);
    ms_json* request = ms_json_parse(body ? body : "", body_length, NULL, 0);
    int code = 400;
    snprintf(failure, sizeof(failure), "A valid %s and executablePath are required", provider->id_key);
    if (!request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        !ms_json_as_string(ms_json_object_get(request, "executablePath"), &executable) || !executable ||
        !executable[0])
        goto done;
    if (!game) {
        code = 404;
        snprintf(failure, sizeof(failure), "Installed %s game was not found in the local registry", provider->label);
        goto done;
    }
    if (!ms_game_executable_override_save(home, provider->source, game->id, game->path, executable, failure,
                                          sizeof(failure)))
        goto done;
    {
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "executablePath");
        ms_json_writer_string(&w, executable);
        ms_json_writer_object_end(&w);
        out = ms_json_writer_take(&w);
        code = 200;
    }
done:
    if (status)
        *status = code;
    if (!out)
        out = ms_store_error_json(failure);
    free(executable);
    ms_json_free(request);
    return out;
}

/* `[Software\\Rockstar Games\\Title] 1700000000` -> `Software\Rockstar Games\Title`. */
static bool registry_section_key(const char* line, char* key, size_t key_size) {
    const char* end = strrchr(line, ']');
    size_t n = 0;
    if (line[0] != '[' || !end || end == line + 1)
        return false;
    for (const char* p = line + 1; p < end && n + 1 < key_size; p++) {
        if (*p == '\\' && p + 1 < end && p[1] == '\\')
            p++;
        key[n++] = *p;
    }
    key[n] = '\0';
    return n > 0;
}

static void registry_section_value(ms_store_registry_section* section, const char* line) {
    static const struct {
        const char* name;
        size_t offset;
        size_t size;
    } fields[] = {
        {"DisplayName", offsetof(ms_store_registry_section, display_name),
         sizeof(((ms_store_registry_section*)0)->display_name)},
        {"Publisher", offsetof(ms_store_registry_section, publisher),
         sizeof(((ms_store_registry_section*)0)->publisher)},
        {"InstallLocation", offsetof(ms_store_registry_section, install_location),
         sizeof(((ms_store_registry_section*)0)->install_location)},
        {"InstallFolder", offsetof(ms_store_registry_section, install_folder),
         sizeof(((ms_store_registry_section*)0)->install_folder)},
        {"UninstallString", offsetof(ms_store_registry_section, uninstall_string),
         sizeof(((ms_store_registry_section*)0)->uninstall_string)},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!ms_store_registry_value_is(line, fields[i].name))
            continue;
        char* value = ms_store_registry_string(strchr(line, '=') + 1);
        if (value)
            snprintf((char*)section + fields[i].offset, fields[i].size, "%s", value);
        free(value);
        return;
    }
}

bool ms_store_registry_sections(const char* registry_path, ms_store_registry_visit visit, void* context) {
    FILE* file = registry_path ? fopen(registry_path, "rb") : NULL;
    ms_store_registry_section* section = calloc(1, sizeof(*section));
    char* line = NULL;
    size_t capacity = 0;
    ssize_t length;
    bool open = false;
    if (!file || !section || !visit) {
        if (file)
            fclose(file);
        free(section);
        return false;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (line[0] == '[') {
            if (open)
                visit(section, context);
            memset(section, 0, sizeof(*section));
            open = registry_section_key(line, section->key, sizeof(section->key));
        } else if (open && line[0] == '"') {
            registry_section_value(section, line);
        }
    }
    if (open)
        visit(section, context);
    free(line);
    free(section);
    fclose(file);
    return true;
}

/* Fills game from an install folder the registry names, when it is a folder in prefix outside any
 * Steam library with a Windows executable in it (to max_depth). */
bool ms_store_game_from_folder(const char* prefix, const char* windows_folder, const char* name, unsigned max_depth,
                               ms_store_game* game) {
    char* host = ms_store_wine_path_to_host(prefix, windows_folder);
    char* executable = NULL;
    off_t size = 0;
    const char* base;
    bool ok = host && ms_store_is_dir(host) && !ms_store_path_in_steam_library(windows_folder, host);
    if (ok) {
        ms_store_largest_executable(host, 0, max_depth, NULL, &executable, &size);
        ok = executable != NULL;
    }
    if (ok) {
        memset(game, 0, sizeof(*game));
        base = strrchr(host, '/');
        base = base ? base + 1 : host;
        snprintf(game->path, sizeof(game->path), "%s", host);
        snprintf(game->windows_path, sizeof(game->windows_path), "%s", windows_folder);
        snprintf(game->name, sizeof(game->name), "%s", name && name[0] ? name : base);
        ok = ms_store_slug(base, game->id, sizeof(game->id));
    }
    free(executable);
    free(host);
    return ok;
}

/* Adds game unless a game in the same folder is listed; renames a clashing id. */
bool ms_store_add_game(ms_store_game** games, size_t* count, size_t* capacity, const ms_store_game* game) {
    char resolved[PATH_MAX];
    bool has_resolved = realpath(game->path, resolved) != NULL;
    ms_store_game copy = *game;
    for (size_t i = 0; i < *count; i++) {
        char other[PATH_MAX];
        if (!strcasecmp((*games)[i].path, game->path) ||
            (has_resolved && realpath((*games)[i].path, other) && !strcasecmp(other, resolved)))
            return false;
    }
    for (unsigned suffix = 2; suffix < 100; suffix++) {
        bool clash = false;
        for (size_t i = 0; i < *count && !clash; i++)
            clash = !strcmp((*games)[i].id, copy.id);
        if (!clash)
            break;
        char base[sizeof(copy.id)];
        snprintf(base, sizeof(base), "%s", game->id);
        base[sizeof(base) - 5] = '\0';
        snprintf(copy.id, sizeof(copy.id), "%s-%u", base, suffix);
    }
    if (*count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 8;
        ms_store_game* expanded = grown <= 1024 ? realloc(*games, grown * sizeof(**games)) : NULL;
        if (!expanded)
            return false;
        *games = expanded;
        *capacity = grown;
    }
    (*games)[(*count)++] = copy;
    return true;
}
