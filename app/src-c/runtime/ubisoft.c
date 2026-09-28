#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/ubisoft.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UBISOFT_INSTALLER_URL    "https://ubi.li/4vxt9"
#define UBISOFT_PIPELINE_DEFAULT "d3dmetal"

static pthread_mutex_t ubisoft_install_lock = PTHREAD_MUTEX_INITIALIZER;
static bool ubisoft_install_active;
static pthread_mutex_t ubisoft_icon_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    unsigned appid;
    bool pending;
} ubisoft_icon_jobs[256];
static size_t ubisoft_icon_job_count;

typedef struct {
    char id[32];
    char name[512];
    char path[PATH_MAX];
} ubisoft_game;

typedef struct {
    char* home;
    char* game_path;
    unsigned appid;
} ubisoft_icon_job;

static char* path_join(const char* a, const char* b) {
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

static bool path_is_dir(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_is_file(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool ubisoft_process_cwd_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    struct proc_vnodepathinfo info;
    int bytes = proc_pidinfo((int)pid, PROC_PIDVNODEPATHINFO, 0, &info, (int)sizeof(info));
    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/')
        length--;
    return bytes == (int)sizeof(info) && !strncmp(info.pvi_cdir.vip_path, root, length) &&
           (info.pvi_cdir.vip_path[length] == '\0' || info.pvi_cdir.vip_path[length] == '/');
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

static bool ubisoft_process_executable_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    char executable[PROC_PIDPATHINFO_MAXSIZE];
    int bytes = proc_pidpath((int)pid, executable, sizeof(executable));
    size_t length = strlen(root);
    return bytes > 0 && !strncmp(executable, root, length) && (executable[length] == '\0' || executable[length] == '/');
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

static bool mkdir_p(const char* path) {
    char* copy;
    size_t i;
    if (!path || !path[0] || !(copy = strdup(path)))
        return false;
    for (i = 1; copy[i]; i++) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            copy[i] = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static char* ubisoft_prefix(const char* home) {
    return path_join(home, "prefix-ubisoft");
}

static char* ubisoft_launcher(const char* home) {
    char* prefix = ubisoft_prefix(home);
    char* base = prefix ? path_join(prefix, "drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher") : NULL;
    static const char* const names[] = {"UbisoftConnect.exe", "upc.exe", "Uplay.exe"};
    char* result = NULL;
    if (base) {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            char* candidate = path_join(base, names[i]);
            if (candidate && path_is_file(candidate)) {
                result = candidate;
                break;
            }
            free(candidate);
        }
    }
    free(base);
    free(prefix);
    return result;
}

static char* ubisoft_wine(const char* home) {
    char* wine = path_join(home, "runtime/wine/bin/metalsharp-wine");
    if (wine && access(wine, X_OK) == 0)
        return wine;
    free(wine);
    wine = path_join(home, "runtime/wine/bin/wine");
    if (wine && access(wine, X_OK) == 0)
        return wine;
    free(wine);
    return NULL;
}

static char* ubisoft_wineserver(const char* home) {
    char* server = path_join(home, "runtime/wine/bin/wineserver");
    if (server && access(server, X_OK) == 0)
        return server;
    free(server);
    return NULL;
}

static char* state_path(const char* home) {
    return path_join(home, "cache/ubisoft-connect/state.json");
}

static char* pid_path(const char* home) {
    return path_join(home, "cache/ubisoft-connect/client.pid");
}

static void write_state(const char* home, const char* state, const char* error) {
    char* path = state_path(home);
    char* parent = path_join(home, "cache/ubisoft-connect");
    ms_json_writer writer;
    char* text;
    FILE* file;
    if (!path || !parent || !mkdir_p(parent)) {
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

static bool contains_case_insensitive(const char* text, const char* needle) {
    size_t n = strlen(needle);
    if (!n)
        return true;
    for (; text && *text; text++)
        if (!strncasecmp(text, needle, n))
            return true;
    return false;
}

static char* read_text(const char* path, size_t max_size) {
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

static bool ubisoft_client_command(const char* command) {
    return contains_case_insensitive(command, "UbisoftConnect.exe") || contains_case_insensitive(command, "upc.exe") ||
           contains_case_insensitive(command, "Uplay.exe");
}

static bool ubisoft_group_has_client(const char* home, pid_t group) {
    char runtime[PATH_MAX], prefix[PATH_MAX], line[4096];
    FILE* pipe;
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    snprintf(prefix, sizeof(prefix), "%s/prefix-ubisoft", home);
    pipe = popen("/bin/ps axo pid=,pgid=,command=", "r");
    if (!pipe)
        return false;
    while (fgets(line, sizeof(line), pipe)) {
        long raw_pid = 0, raw_group = 0;
        int command_offset = 0;
        if (sscanf(line, "%ld %ld %n", &raw_pid, &raw_group, &command_offset) != 2 || raw_pid <= 1 ||
            raw_pid > INT_MAX || command_offset <= 0)
            continue;
        if (ubisoft_client_command(line + command_offset) &&
            ubisoft_process_executable_within((pid_t)raw_pid, runtime) &&
            (raw_group == (long)group || ubisoft_process_cwd_within((pid_t)raw_pid, prefix))) {
            pclose(pipe);
            return true;
        }
    }
    pclose(pipe);
    return false;
}

static bool ubisoft_pid_matches(const char* home, pid_t target) {
    char runtime[PATH_MAX], prefix[PATH_MAX], command[4096] = {0};
    int output[2], status = 0;
    size_t used = 0;
    pid_t child, waited;
    if (target <= 1 || pipe(output) != 0)
        return false;
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    snprintf(prefix, sizeof(prefix), "%s/prefix-ubisoft", home);
    child = fork();
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(output[1]);
        char pid_text[32];
        snprintf(pid_text, sizeof(pid_text), "%ld", (long)target);
        char* args[] = {"/bin/ps", "-p", pid_text, "-o", "command=", NULL};
        execv(args[0], args);
        _exit(127);
    }
    close(output[1]);
    if (child < 0) {
        close(output[0]);
        return false;
    }
    while (used + 1 < sizeof(command)) {
        ssize_t n = read(output[0], command + used, sizeof(command) - used - 1);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    close(output[0]);
    do
        waited = waitpid(child, &status, 0);
    while (waited < 0 && errno == EINTR);
    command[used] = '\0';
    if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && ubisoft_client_command(command) &&
        ubisoft_process_executable_within(target, runtime) && ubisoft_process_cwd_within(target, prefix))
        return true;
    return ubisoft_group_has_client(home, target);
}

static pid_t read_client_pid(const char* home) {
    char* path = pid_path(home);
    char* text = path ? read_text(path, 64) : NULL;
    char* end = NULL;
    long value = text ? strtol(text, &end, 10) : -1;
    pid_t pid = value > 1 && value <= INT_MAX && end != text ? (pid_t)value : 0;
    if (pid > 0 && !ubisoft_pid_matches(home, pid))
        pid = 0;
    if (!pid && path)
        unlink(path);
    free(path);
    free(text);
    return pid;
}

static bool run_child_wait(const char* executable, char* const argv[], const char* home, const char* prefix,
                           const char* pipeline) {
    pid_t pid, waited;
    int status = 0;
    if (!executable || !argv || !prefix)
        return false;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        if (pipeline) {
            ms_steam_apply_launch_preferences(home);
            ms_steam_apply_graphics_route(home, pipeline);
        }
        execv(executable, argv);
        _exit(127);
    }
    do
        waited = waitpid(pid, &status, 0);
    while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static char* find_external_volume(void) {
    DIR* directory = opendir("/Volumes");
    struct dirent* entry;
    char* best = NULL;
    uint64_t best_free = 0;
    if (!directory)
        return NULL;
    while ((entry = readdir(directory)) != NULL) {
        char *candidate, resolved[PATH_MAX];
        struct statfs volume;
        uint64_t available;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !strcasecmp(entry->d_name, "Recovery") ||
            !strcasecmp(entry->d_name, "Macintosh HD") || entry->d_name[0] == '.')
            continue;
        candidate = path_join("/Volumes", entry->d_name);
        if (!candidate)
            continue;
        if (realpath(candidate, resolved) && statfs(resolved, &volume) == 0 &&
            !strncmp(volume.f_mntonname, "/Volumes/", 9) && strcmp(volume.f_mntonname, "/Volumes/Recovery") &&
            !(volume.f_flags & MNT_RDONLY) && access(resolved, W_OK | X_OK) == 0) {
            uint64_t block_size = volume.f_bsize > 0 ? (uint64_t)volume.f_bsize : 0;
            uint64_t blocks = volume.f_bavail > 0 ? (uint64_t)volume.f_bavail : 0;
            available = block_size && blocks > UINT64_MAX / block_size ? UINT64_MAX : blocks * block_size;
            if (!best || available > best_free) {
                char* replacement = strdup(resolved);
                if (replacement) {
                    free(best);
                    best = replacement;
                    best_free = available;
                }
            }
        }
        free(candidate);
    }
    closedir(directory);
    return best;
}

static bool ensure_ubisoft_drive_mapping(const char* prefix, const char* volume) {
    char *dosdevices = path_join(prefix, "dosdevices"), *drive = NULL;
    struct stat st;
    bool ok = false;
    if (!dosdevices || !mkdir_p(dosdevices))
        goto done;
    drive = path_join(dosdevices, "y:");
    if (!drive)
        goto done;
    if (lstat(drive, &st) == 0) {
        char existing[PATH_MAX];
        ssize_t length = readlink(drive, existing, sizeof(existing) - 1);
        if (length > 0) {
            existing[length] = '\0';
            char resolved[PATH_MAX];
            if (realpath(existing, resolved) && volume && !strcmp(resolved, volume))
                ok = true;
            else if (realpath(existing, resolved))
                ok = true; /* Preserve an existing user-managed Y: mapping. */
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

static bool initialize_prefix(const char* home, char* error, size_t error_size) {
    char* prefix = ubisoft_prefix(home);
    char* wine = ubisoft_wine(home);
    char* reg = prefix ? path_join(prefix, "system.reg") : NULL;
    struct stat st;
    bool initialized = reg && stat(reg, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    if (!prefix || !wine || !mkdir_p(prefix)) {
        snprintf(error, error_size, "MetalSharp Wine is not available to initialize Ubisoft Connect");
        free(prefix);
        free(wine);
        free(reg);
        return false;
    }
    if (!initialized) {
        char* argv[] = {wine, "wineboot", "-u", NULL};
        if (!run_child_wait(wine, argv, home, prefix, "d3dmetal")) {
            snprintf(error, error_size, "Wine could not initialize the Ubisoft Connect prefix");
            free(prefix);
            free(wine);
            free(reg);
            return false;
        }
    }
    char* external_volume = find_external_volume();
    if (external_volume && !ensure_ubisoft_drive_mapping(prefix, external_volume)) {
        snprintf(error, error_size, "Could not map the external volume to Ubisoft Connect's Y: drive");
        free(external_volume);
        free(prefix);
        free(wine);
        free(reg);
        return false;
    }
    free(external_volume);
    free(prefix);
    free(wine);
    free(reg);
    return true;
}

static bool spawn_launcher(const char* home, const char* launcher, const char* pipeline, unsigned game_appid) {
    char* wine = ubisoft_wine(home);
    char* prefix = ubisoft_prefix(home);
    char* pidfile = pid_path(home);
    pid_t pid;
    FILE* file;
    if (!wine || !prefix || !launcher || !pidfile) {
        free(wine);
        free(prefix);
        free(pidfile);
        return false;
    }
    pid = fork();
    if (pid == 0) {
        char* args[] = {wine, (char*)launcher, "-no-cef-sandbox", "-cef-single-process", "-noverifyfiles", "-no-dwrite",
                        NULL};
        (void)setpgid(0, 0);
        (void)chdir(prefix);
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        ms_steam_apply_launch_preferences(home);
        ms_steam_apply_graphics_route(home, pipeline ? pipeline : UBISOFT_PIPELINE_DEFAULT);
        execv(wine, args);
        _exit(127);
    }
    if (pid < 0) {
        free(wine);
        free(prefix);
        free(pidfile);
        return false;
    }
    (void)setpgid(pid, pid);
    file = fopen(pidfile, "wb");
    if (file) {
        fprintf(file, "%ld\n", (long)pid);
        fclose(file);
    }
    if (game_appid)
        ms_process_register_game(game_appid, pid);
    free(wine);
    free(prefix);
    free(pidfile);
    return true;
}

static bool launch_connect(const char* home) {
    char* launcher = ubisoft_launcher(home);
    bool ok = launcher && spawn_launcher(home, launcher, "d3dmetal", 0);
    free(launcher);
    return ok;
}

static bool spawn_game_process(const char* home, const char* executable, const char* game_dir, const char* pipeline,
                               unsigned appid) {
    char* wine = ubisoft_wine(home);
    char* prefix = ubisoft_prefix(home);
    pid_t pid;
    if (!wine || !prefix || !executable || !game_dir) {
        free(wine);
        free(prefix);
        return false;
    }
    pid = fork();
    if (pid == 0) {
        char* args[] = {wine, (char*)executable, NULL};
        (void)setpgid(0, 0);
        (void)chdir(game_dir);
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        ms_steam_apply_launch_preferences(home);
        ms_steam_apply_graphics_route(home, pipeline);
        execv(wine, args);
        _exit(127);
    }
    if (pid < 0) {
        free(wine);
        free(prefix);
        return false;
    }
    (void)setpgid(pid, pid);
    ms_process_register_game(appid, pid);
    free(wine);
    free(prefix);
    return true;
}

static bool run_curl(const char* installer) {
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
                    "600",
                    "--max-filesize",
                    "536870912",
                    "--output",
                    (char*)installer,
                    (char*)UBISOFT_INSTALLER_URL,
                    NULL};
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
    bool valid = waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 && stat(installer, &st) == 0 &&
                 S_ISREG(st.st_mode) && st.st_size >= 1024 * 1024 && st.st_size <= 512 * 1024 * 1024;
    if (!valid)
        (void)unlink(installer);
    return valid;
}

static void* install_worker(void* opaque) {
    char* home = opaque;
    char error[256] = {0};
    char* installer_dir = path_join(home, "cache/ubisoft-connect");
    char* installer = installer_dir ? path_join(installer_dir, "UbisoftConnectInstaller.exe") : NULL;
    char* wine = ubisoft_wine(home);
    char* prefix = ubisoft_prefix(home);
    bool ok = installer_dir && installer && wine && prefix && mkdir_p(installer_dir);
    if (ok)
        ok = initialize_prefix(home, error, sizeof(error));
    if (!ok && !error[0])
        snprintf(error, sizeof(error), "Could not prepare the Ubisoft Connect installer");
    if (ok) {
        write_state(home, "downloading", NULL);
        ok = run_curl(installer);
        if (!ok)
            snprintf(error, sizeof(error), "Could not download the Ubisoft Connect installer from Ubisoft");
    }
    if (ok) {
        char* args[] = {wine, installer, NULL};
        write_state(home, "installing", NULL);
        ok = run_child_wait(wine, args, home, prefix, "d3dmetal");
        if (!ok)
            snprintf(error, sizeof(error), "The Ubisoft Connect installer did not complete successfully");
    }
    if (ok) {
        char* installed_launcher = ubisoft_launcher(home);
        if (!installed_launcher) {
            ok = false;
            snprintf(error, sizeof(error), "Installer finished, but Ubisoft Connect was not found in its prefix");
        }
        free(installed_launcher);
    }
    if (ok) {
        if (!launch_connect(home)) {
            ok = false;
            snprintf(error, sizeof(error), "Ubisoft Connect installed, but could not be launched");
        }
    }
    write_state(home, ok ? "ready" : "error", ok ? NULL : error);
    free(installer_dir);
    free(installer);
    free(wine);
    free(prefix);
    pthread_mutex_lock(&ubisoft_install_lock);
    ubisoft_install_active = false;
    pthread_mutex_unlock(&ubisoft_install_lock);
    free(home);
    return NULL;
}

static bool start_install_worker(const char* home) {
    pthread_t thread;
    pthread_mutex_lock(&ubisoft_install_lock);
    if (ubisoft_install_active) {
        pthread_mutex_unlock(&ubisoft_install_lock);
        return true;
    }
    ubisoft_install_active = true;
    pthread_mutex_unlock(&ubisoft_install_lock);
    char* copy = strdup(home);
    if (!copy || pthread_create(&thread, NULL, install_worker, copy) != 0) {
        free(copy);
        pthread_mutex_lock(&ubisoft_install_lock);
        ubisoft_install_active = false;
        pthread_mutex_unlock(&ubisoft_install_lock);
        return false;
    }
    pthread_detach(thread);
    return true;
}

char* ms_ubisoft_status_json(const char* home) {
    bool worker_active;
    pthread_mutex_lock(&ubisoft_install_lock);
    worker_active = ubisoft_install_active;
    pthread_mutex_unlock(&ubisoft_install_lock);
    char* launcher = ubisoft_launcher(home);
    char* state_file = state_path(home);
    char* state_text = state_file ? read_text(state_file, 8192) : NULL;
    char* error = NULL;
    ms_json* state_json = state_text ? ms_json_parse(state_text, strlen(state_text), NULL, 0) : NULL;
    ms_json_writer w;
    char* out;
    const char* state = launcher ? "ready" : "not_installed";
    bool installing = false;
    if (state_json) {
        char* stored = NULL;
        if (ms_json_as_string(ms_json_object_get(state_json, "state"), &stored) && stored) {
            if ((!strcmp(stored, "downloading") || !strcmp(stored, "installing")) && worker_active) {
                state = !strcmp(stored, "downloading") ? "downloading" : "installing";
                installing = true;
            } else if (!launcher && !strcmp(stored, "error")) {
                state = "error";
                (void)ms_json_as_string(ms_json_object_get(state_json, "error"), &error);
            }
        }
        free(stored);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, launcher != NULL);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, read_client_pid(home) > 0);
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
    {
        char* prefix = ubisoft_prefix(home);
        ms_json_writer_string(&w, prefix ? prefix : "");
        free(prefix);
    }
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(launcher);
    free(state_file);
    free(state_text);
    free(error);
    ms_json_free(state_json);
    return out;
}

char* ms_ubisoft_launch_json(const char* home, int* status) {
    char* launcher;
    ms_json_writer w;
    char* out;
    if (status)
        *status = 500;
    launcher = ubisoft_launcher(home);
    if (!launcher) {
        if (!start_install_worker(home)) {
            if (status)
                *status = 500;
            return strdup("{\"ok\":false,\"error\":\"Could not start Ubisoft Connect installation\"}");
        }
        if (status)
            *status = 202;
        return strdup("{\"ok\":true,\"installing\":true}");
    }
    free(launcher);
    if (read_client_pid(home) > 0) {
        if (status)
            *status = 200;
        return strdup("{\"ok\":true,\"running\":true}");
    }
    if (!initialize_prefix(home, (char[256]){0}, 256) || !launch_connect(home))
        return strdup("{\"ok\":false,\"error\":\"Could not start Ubisoft Connect with D3DMetal\"}");
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, true);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    if (status)
        *status = 200;
    return out;
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

static bool stop_prefix(const char* home) {
    char* server = ubisoft_wineserver(home);
    char* prefix = ubisoft_prefix(home);
    pid_t pid, waited;
    int result = 0;
    if (!server || !prefix || !path_is_dir(prefix)) {
        bool stopped = !path_is_dir(prefix);
        free(server);
        free(prefix);
        return stopped;
    }
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
        free(prefix);
        return false;
    }
    do
        waited = waitpid(pid, &result, 0);
    while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(result)) {
        free(server);
        free(prefix);
        return false;
    }
    bool stopped = WEXITSTATUS(result) == 0;
    /* wineserver -k returns 1 when the prefix has no server. Verify that the
     * prefix is already stopped instead of failing an otherwise idle migration. */
    if (!stopped && WEXITSTATUS(result) == 1)
        stopped = wait_wineserver_shutdown(server, prefix);
    free(server);
    free(prefix);
    return stopped;
}

char* ms_ubisoft_stop_json(const char* home, int* status) {
    char* pidfile = pid_path(home);
    bool ok = stop_prefix(home);
    if (pidfile)
        unlink(pidfile);
    free(pidfile);
    if (status)
        *status = ok ? 200 : 500;
    return ok ? strdup("{\"ok\":true,\"running\":false}")
              : strdup("{\"ok\":false,\"error\":\"Could not stop Ubisoft Connect processes\"}");
}

static char* registry_unescape(const char* value) {
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

static bool valid_ubisoft_id(const char* id) {
    if (!id || !*id)
        return false;
    for (const unsigned char* p = (const unsigned char*)id; *p; p++)
        if (!isdigit(*p))
            return false;
    return true;
}

static char* wine_path_to_host(const char* prefix, const char* value) {
    char* base = NULL;
    char* relative;
    char drive;
    if (!value || !isalpha((unsigned char)value[0]) || value[1] != ':' || (value[2] != '\\' && value[2] != '/'))
        return NULL;
    drive = (char)tolower((unsigned char)value[0]);
    relative = strdup(value + 3);
    if (!relative)
        return NULL;
    for (char* p = relative; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (drive == 'c')
        base = path_join(prefix, "drive_c");
    else {
        char linkname[32];
        char* linkpath;
        char target[PATH_MAX];
        snprintf(linkname, sizeof(linkname), "dosdevices/%c:", drive);
        linkpath = path_join(prefix, linkname);
        if (linkpath && realpath(linkpath, target))
            base = strdup(target);
        free(linkpath);
    }
    char* joined = base ? path_join(base, relative) : NULL;
    free(base);
    free(relative);
    return joined;
}

static bool directory_has_executable(const char* path, unsigned depth) {
    DIR* directory;
    struct dirent* entry;
    bool found = false;
    if (depth > 3 || !(directory = opendir(path)))
        return false;
    while (!found && (entry = readdir(directory)) != NULL) {
        char* child;
        size_t n = strlen(entry->d_name);
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = path_join(path, entry->d_name);
        if (!child)
            continue;
        if (n > 4 && !strcasecmp(entry->d_name + n - 4, ".exe") && path_is_file(child))
            found = true;
        else if (path_is_dir(child) && depth < 3)
            found = directory_has_executable(child, depth + 1);
        free(child);
    }
    closedir(directory);
    return found;
}

static bool parse_registry_section(const char* line, char* id, size_t id_size) {
    const char* end;
    const char* slash;
    const char* begin;
    size_t n;
    if (*line != '[' || !contains_case_insensitive(line, "ubisoft") || !contains_case_insensitive(line, "installs"))
        return false;
    end = strchr(line, ']');
    if (!end)
        return false;
    slash = end;
    while (slash > line && slash[-1] != '\\')
        slash--;
    begin = slash;
    n = (size_t)(end - begin);
    if (!n || n >= id_size)
        return false;
    memcpy(id, begin, n);
    id[n] = '\0';
    return valid_ubisoft_id(id);
}

static bool parse_uninstall_section(const char* line, char* id, size_t id_size) {
    const char* end;
    const char* begin;
    size_t length;
    if (*line != '[' || !contains_case_insensitive(line, "Uninstall") ||
        !contains_case_insensitive(line, "Uplay Install"))
        return false;
    end = strchr(line, ']');
    if (!end)
        return false;
    begin = end;
    while (begin > line && begin[-1] != ' ')
        begin--;
    length = (size_t)(end - begin);
    if (!length || length >= id_size)
        return false;
    memcpy(id, begin, length);
    id[length] = '\0';
    return valid_ubisoft_id(id);
}

static char* registry_display_name(const char* prefix, const char* id) {
    char* path = path_join(prefix, "system.reg");
    FILE* file = path ? fopen(path, "rb") : NULL;
    char* line = NULL;
    size_t capacity = 0;
    char section_id[32] = {0};
    bool match = false;
    char* result = NULL;
    ssize_t length;
    while (file && (length = getline(&line, &capacity, file)) >= 0) {
        if (length > 0 && line[0] == '[')
            match = parse_uninstall_section(line, section_id, sizeof(section_id)) && !strcmp(section_id, id);
        else if (match && !strncasecmp(line, "\"DisplayName\"=", strlen("\"DisplayName\"="))) {
            result = registry_unescape(strchr(line, '=') + 1);
            if (result && result[0])
                break;
            free(result);
            result = NULL;
        }
    }
    free(line);
    if (file)
        fclose(file);
    free(path);
    return result;
}

static size_t scan_registry_games(const char* home, ubisoft_game* games, size_t cap) {
    char* prefix = ubisoft_prefix(home);
    char* registry = prefix ? path_join(prefix, "system.reg") : NULL;
    FILE* file = registry ? fopen(registry, "rb") : NULL;
    char* line = NULL;
    size_t line_cap = 0, count = 0;
    ssize_t len;
    char id[32] = {0}, title[512] = {0}, install_dir[PATH_MAX] = {0};
    if (!file) {
        free(prefix);
        free(registry);
        return 0;
    }
    while ((len = getline(&line, &line_cap, file)) >= 0) {
        if (len > 0 && line[0] == '[') {
            if (id[0] && install_dir[0] && count < cap) {
                char* path = wine_path_to_host(prefix, install_dir);
                if (path && path_is_dir(path) && directory_has_executable(path, 0)) {
                    ubisoft_game* game = &games[count++];
                    char* display_name = title[0] ? strdup(title) : registry_display_name(prefix, id);
                    snprintf(game->id, sizeof(game->id), "%s", id);
                    snprintf(game->name, sizeof(game->name), "%s",
                             display_name         ? display_name
                             : strrchr(path, '/') ? strrchr(path, '/') + 1
                                                  : path);
                    snprintf(game->path, sizeof(game->path), "%s", path);
                    free(display_name);
                }
                free(path);
            }
            id[0] = title[0] = install_dir[0] = '\0';
            (void)parse_registry_section(line, id, sizeof(id));
        } else if (id[0]) {
            char* eq = strchr(line, '=');
            if (eq && line[0] == '"') {
                char* key_end = strchr(line + 1, '"');
                if (key_end) {
                    size_t key_len = (size_t)(key_end - line - 1);
                    char* value = registry_unescape(eq + 1);
                    if (value && key_len == strlen("InstallDir") && !strncasecmp(line + 1, "InstallDir", key_len))
                        snprintf(install_dir, sizeof(install_dir), "%s", value);
                    else if (value && key_len == strlen("DisplayName") &&
                             !strncasecmp(line + 1, "DisplayName", key_len))
                        snprintf(title, sizeof(title), "%s", value);
                    free(value);
                }
            }
        }
    }
    if (id[0] && install_dir[0] && count < cap) {
        char* path = wine_path_to_host(prefix, install_dir);
        if (path && path_is_dir(path) && directory_has_executable(path, 0)) {
            ubisoft_game* game = &games[count++];
            char* display_name = title[0] ? strdup(title) : registry_display_name(prefix, id);
            snprintf(game->id, sizeof(game->id), "%s", id);
            snprintf(game->name, sizeof(game->name), "%s",
                     display_name         ? display_name
                     : strrchr(path, '/') ? strrchr(path, '/') + 1
                                          : path);
            snprintf(game->path, sizeof(game->path), "%s", path);
            free(display_name);
        }
        free(path);
    }
    free(line);
    fclose(file);
    free(prefix);
    free(registry);
    return count;
}

static unsigned ubisoft_appid(const char* id) {
    uint32_t hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)id; *p; p++)
        hash = (hash ^ *p) * 16777619u;
    return 0x80000000u | (hash & 0x7fffffffu);
}

static bool ignored_game_executable(const char* name) {
    static const char* const ignored[] = {
        "setup",           "install",  "uninstall",      "unins",    "crash",         "reporter",
        "launcher",        "upc",      "ubisoftconnect", "battleye", "easyanticheat", "redist",
        "redistributable", "vcredist", "prereq",         "directx",  "dxsetup",       "dotnet"};
    char lower[PATH_MAX];
    size_t n = strlen(name);
    if (n >= sizeof(lower))
        n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[n] = '\0';
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        if (strstr(lower, ignored[i]))
            return true;
    return false;
}

static bool ignored_game_directory(const char* name) {
    static const char* const ignored[] = {"support", "redist",    "redistributable", "__installer", "_commonredist",
                                          "prereq",  "installer", "directx",         "dotnet"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        if (contains_case_insensitive(name, ignored[i]))
            return true;
    return false;
}

static void ubisoft_find_game_executable(const char* path, unsigned depth, char** best, off_t* best_size) {
    DIR* directory;
    struct dirent* entry;
    if (depth > 3 || !(directory = opendir(path)))
        return;
    while ((entry = readdir(directory)) != NULL) {
        char* child;
        size_t n = strlen(entry->d_name);
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = path_join(path, entry->d_name);
        if (!child)
            continue;
        if (n > 4 && !strcasecmp(entry->d_name + n - 4, ".exe") && !ignored_game_executable(entry->d_name) &&
            stat(child, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > *best_size) {
            char* replacement = strdup(child);
            if (replacement) {
                free(*best);
                *best = replacement;
                *best_size = st.st_size;
            }
        } else if (path_is_dir(child) && depth < 3 && !ignored_game_directory(entry->d_name)) {
            ubisoft_find_game_executable(child, depth + 1, best, best_size);
        }
        free(child);
    }
    closedir(directory);
}

static void* ubisoft_extract_icon_worker(void* opaque) {
    ubisoft_icon_job* job = opaque;
    char* executable = NULL;
    off_t executable_size = 0;
    char* icon = NULL;
    ubisoft_find_game_executable(job->game_path, 0, &executable, &executable_size);
    if (executable)
        icon = ms_steam_extract_executable_icon(job->home, executable, job->appid);
    free(icon);
    free(executable);
    pthread_mutex_lock(&ubisoft_icon_jobs_lock);
    for (size_t i = 0; i < ubisoft_icon_job_count; i++)
        if (ubisoft_icon_jobs[i].appid == job->appid) {
            ubisoft_icon_jobs[i].pending = false;
            break;
        }
    pthread_mutex_unlock(&ubisoft_icon_jobs_lock);
    free(job->home);
    free(job->game_path);
    free(job);
    return NULL;
}

static bool ubisoft_queue_icon_extraction(const char* home, const ubisoft_game* game, unsigned appid) {
    ubisoft_icon_job* job;
    pthread_t thread;
    bool already_pending = false;
    pthread_mutex_lock(&ubisoft_icon_jobs_lock);
    for (size_t i = 0; i < ubisoft_icon_job_count; i++) {
        if (ubisoft_icon_jobs[i].appid == appid) {
            already_pending = ubisoft_icon_jobs[i].pending;
            pthread_mutex_unlock(&ubisoft_icon_jobs_lock);
            return already_pending;
        }
    }
    if (ubisoft_icon_job_count >= sizeof(ubisoft_icon_jobs) / sizeof(ubisoft_icon_jobs[0])) {
        pthread_mutex_unlock(&ubisoft_icon_jobs_lock);
        return false;
    }
    ubisoft_icon_jobs[ubisoft_icon_job_count].appid = appid;
    ubisoft_icon_jobs[ubisoft_icon_job_count].pending = true;
    ubisoft_icon_job_count++;
    pthread_mutex_unlock(&ubisoft_icon_jobs_lock);

    job = calloc(1, sizeof(*job));
    if (job) {
        job->home = strdup(home);
        job->game_path = strdup(game->path);
        job->appid = appid;
    }
    if (!job || !job->home || !job->game_path || pthread_create(&thread, NULL, ubisoft_extract_icon_worker, job) != 0) {
        if (job) {
            free(job->home);
            free(job->game_path);
            free(job);
        }
        pthread_mutex_lock(&ubisoft_icon_jobs_lock);
        for (size_t i = 0; i < ubisoft_icon_job_count; i++)
            if (ubisoft_icon_jobs[i].appid == appid) {
                ubisoft_icon_jobs[i].pending = false;
                break;
            }
        pthread_mutex_unlock(&ubisoft_icon_jobs_lock);
        return false;
    }
    (void)pthread_detach(thread);
    return true;
}

static bool game_process_command_matches(const char* command, const char* executable) {
    const char* basename;
    size_t length;
    if (!command || !executable)
        return false;
    basename = strrchr(executable, '/');
    basename = basename ? basename + 1 : executable;
    length = strlen(basename);
    for (const char* match = command; *match; match++) {
        if (!strncasecmp(match, basename, length)) {
            bool start = match == command || match[-1] == '/' || match[-1] == '\\';
            char after = match[length];
            if (start && (!after || isspace((unsigned char)after) || after == '"' || after == '\''))
                return true;
        }
    }
    return false;
}

static pid_t ubisoft_detect_installed_game_pid(const char* home, const ubisoft_game* game, const char* executable) {
    char runtime[PATH_MAX], line[4096];
    FILE* pipe;
    pid_t result = 0;
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe)
        return 0;
    while (fgets(line, sizeof(line), pipe)) {
        long raw_pid = 0;
        int command_offset = 0;
        if (sscanf(line, "%ld %n", &raw_pid, &command_offset) != 1 || raw_pid <= 1 || raw_pid > INT_MAX ||
            command_offset <= 0)
            continue;
        if (game_process_command_matches(line + command_offset, executable) &&
            ubisoft_process_executable_within((pid_t)raw_pid, runtime) &&
            ubisoft_process_cwd_within((pid_t)raw_pid, game->path)) {
            result = (pid_t)raw_pid;
            break;
        }
    }
    pclose(pipe);
    return result;
}

void ms_ubisoft_register_running_games(const char* home) {
    ubisoft_game games[256];
    size_t count = scan_registry_games(home, games, sizeof(games) / sizeof(games[0]));
    for (size_t i = 0; i < count; i++) {
        char* executable = NULL;
        off_t executable_size = 0;
        ubisoft_find_game_executable(games[i].path, 0, &executable, &executable_size);
        pid_t pid = executable ? ubisoft_detect_installed_game_pid(home, &games[i], executable) : 0;
        if (pid > 1)
            ms_process_register_game(ubisoft_appid(games[i].id), pid);
        free(executable);
    }
}

static char* pipeline_for_game(const char* home, unsigned appid) {
    char dir[64];
    snprintf(dir, sizeof(dir), "bottles/ubisoft_%u/bottle.json", appid);
    char* path = path_join(home, dir);
    char* data = path ? read_text(path, 65536) : NULL;
    ms_json* json = data ? ms_json_parse(data, strlen(data), NULL, 0) : NULL;
    char* result = NULL;
    if (json)
        (void)ms_json_as_string(ms_json_object_get(json, "preferred_pipeline"), &result);
    if (!result)
        result = strdup(UBISOFT_PIPELINE_DEFAULT);
    free(path);
    free(data);
    ms_json_free(json);
    return result;
}

/* Official Ubisoft game-page og:image assets, with the installed executable icon as renderer fallback. */
static const char* ubisoft_official_artwork_url(const ubisoft_game* game) {
    if (!strcmp(game->id, "5266") || !strcasecmp(game->name, "Far Cry 6"))
        return "https://staticctf.ubisoft.com/J3yJr34U2pZ2Ieem48Dwy9uqj5PNUQTn/Bn213V7aySLmjGwfQkSMy/"
               "bffebc6e9a19f3524a306d89cbc3b0d4/fc6-page_meta-thumbnail.jpg";
    if (!strcasecmp(game->name, "Assassin's Creed Odyssey"))
        return "https://staticctf.ubisoft.com/J3yJr34U2pZ2Ieem48Dwy9uqj5PNUQTn/7KyI2BUBqcUIqCj086Zrm1/"
               "8136be553b82b6eafe010bb04c57f01c/acod-header-background-desktop-1920x1080-v2.jpg";
    if (!strcasecmp(game->name, "Assassin's Creed Unity"))
        return "https://staticctf.ubisoft.com/J3yJr34U2pZ2Ieem48Dwy9uqj5PNUQTn/xAgERoN33SYzSIi4YGOSy/"
               "7e39368e53fe9a5b0b3f804cf858a4fc/acu-ubicom-keyart-thumbnail.jpg";
    return NULL;
}

static void write_game_json(ms_json_writer* w, const char* home, const ubisoft_game* game) {
    unsigned appid = ubisoft_appid(game->id);
    char bottle[64], icon_relative[128];
    char* pipeline = pipeline_for_game(home, appid);
    char* icon_cache = NULL;
    char* icon = NULL;
    bool icon_pending = false;
    snprintf(icon_relative, sizeof(icon_relative), "cache/ubisoft-connect/artwork/%u.png", appid);
    icon_cache = path_join(home, icon_relative);
    if (path_is_file(icon_cache)) {
        icon = icon_cache;
        icon_cache = NULL;
    } else {
        icon_pending = ubisoft_queue_icon_extraction(home, game, appid);
    }
    free(icon_cache);
    snprintf(bottle, sizeof(bottle), "ubisoft_%u", appid);
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "source");
    ms_json_writer_string(w, "ubisoft");
    ms_json_writer_key(w, "appid");
    ms_json_writer_u64(w, appid);
    ms_json_writer_key(w, "ubisoft_id");
    ms_json_writer_string(w, game->id);
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, game->name);
    ms_json_writer_key(w, "installed");
    ms_json_writer_bool(w, true);
    ms_json_writer_key(w, "state");
    ms_json_writer_string(w, "installed");
    ms_json_writer_key(w, "game_dir");
    ms_json_writer_string(w, game->path);
    ms_json_writer_key(w, "embedded_icon_path");
    if (icon)
        ms_json_writer_string(w, icon);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "icon_pending");
    ms_json_writer_bool(w, icon_pending);
    ms_json_writer_key(w, "ubisoft_artwork_url");
    {
        const char* artwork = ubisoft_official_artwork_url(game);
        if (artwork)
            ms_json_writer_string(w, artwork);
        else
            ms_json_writer_null(w);
    }
    ms_json_writer_key(w, "bottle_id");
    ms_json_writer_string(w, bottle);
    ms_json_writer_key(w, "launch_method");
    ms_json_writer_string(w, "ubisoft");
    ms_json_writer_key(w, "preferred_pipeline");
    ms_json_writer_string(w, pipeline ? pipeline : UBISOFT_PIPELINE_DEFAULT);
    ms_json_writer_key(w, "available_pipelines");
    ms_json_writer_array_begin(w);
    const char* ids[] = {"d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9"};
    const char* names[] = {"D3DMetal", "DXMT", "DXMT (32-bit)", "VKD3D", "D3D9"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "id");
        ms_json_writer_string(w, ids[i]);
        ms_json_writer_key(w, "name");
        ms_json_writer_string(w, names[i]);
        ms_json_writer_object_end(w);
    }
    ms_json_writer_array_end(w);
    ms_json_writer_object_end(w);
    free(pipeline);
    free(icon);
}

char* ms_ubisoft_library_json(const char* home) {
    ubisoft_game games[256];
    size_t count = scan_registry_games(home, games, sizeof(games) / sizeof(games[0]));
    ms_json_writer w;
    char* out;
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
        write_game_json(&w, home, &games[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}

static bool parse_request(const char* body, size_t length, char** id, char** pipeline) {
    ms_json* json;
    bool ok = false;
    if (!body || !length)
        return false;
    json = ms_json_parse(body, length, NULL, 0);
    if (!json)
        return false;
    if (ms_json_as_string(ms_json_object_get(json, "ubisoft_id"), id) && valid_ubisoft_id(*id)) {
        (void)ms_json_as_string(ms_json_object_get(json, "pipeline"), pipeline);
        ok = true;
    }
    ms_json_free(json);
    return ok;
}

static const ubisoft_game* find_game(ubisoft_game* games, size_t count, const char* id) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(games[i].id, id))
            return &games[i];
    return NULL;
}

static const char* ubisoft_canonical_pipeline(const char* pipeline) {
    const char* const ids[] = {"d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9"};
    for (size_t i = 0; pipeline && i < sizeof(ids) / sizeof(ids[0]); i++)
        if (!strcasecmp(pipeline, ids[i]))
            return ids[i];
    return NULL;
}

static bool supported_pipeline(const char* pipeline) {
    return ubisoft_canonical_pipeline(pipeline) != NULL;
}

char* ms_ubisoft_save_pipeline_json(const char* home, const char* body, size_t body_length, int* status) {
    char *id = NULL, *pipeline = NULL, *dir = NULL, *path = NULL, *data = NULL, *executable = NULL;
    ubisoft_game games[256];
    size_t count = scan_registry_games(home, games, sizeof(games) / sizeof(games[0]));
    const ubisoft_game* game;
    unsigned appid;
    off_t executable_size = 0;
    ms_json_writer w;
    char* out = NULL;
    char failure[512] = "Could not save Ubisoft pipeline";
    if (status)
        *status = 400;
    if (!parse_request(body, body_length, &id, &pipeline) || !pipeline || !supported_pipeline(pipeline))
        goto done;
    {
        char* normalized = strdup(ubisoft_canonical_pipeline(pipeline));
        if (!normalized)
            goto done;
        free(pipeline);
        pipeline = normalized;
    }
    game = find_game(games, count, id);
    if (!game) {
        if (status)
            *status = 404;
        snprintf(failure, sizeof(failure), "Installed Ubisoft game was not found in the local registry");
        goto done;
    }
    ubisoft_find_game_executable(game->path, 0, &executable, &executable_size);
    if (!executable) {
        if (status)
            *status = 404;
        snprintf(failure, sizeof(failure), "No launchable Windows executable was found for %s", game->name);
        goto done;
    }
    if (!ms_steam_stage_route_for_executable(home, pipeline, game->path, executable)) {
        if (status)
            *status = 500;
        snprintf(failure, sizeof(failure), "Could not stage the %s graphics route for %s", pipeline, game->name);
        goto done;
    }
    appid = ubisoft_appid(id);
    char bottle[64];
    snprintf(bottle, sizeof(bottle), "ubisoft_%u", appid);
    dir = path_join(home, "bottles");
    if (!dir || !mkdir_p(dir))
        goto done;
    char bottle_dir[80];
    snprintf(bottle_dir, sizeof(bottle_dir), "ubisoft_%u", appid);
    char* bottle_path = path_join(dir, bottle_dir);
    if (!bottle_path || !mkdir_p(bottle_path)) {
        free(bottle_path);
        goto done;
    }
    path = path_join(bottle_path, "bottle.json");
    free(bottle_path);
    if (!path)
        goto done;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_i64(&w, 1);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, bottle);
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, game->name);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "ubisoft");
    ms_json_writer_key(&w, "ubisoft_id");
    ms_json_writer_string(&w, id);
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
    if (!data)
        goto done;
    FILE* file = fopen(path, "wb");
    if (!file || fwrite(data, 1, strlen(data), file) != strlen(data)) {
        if (file)
            fclose(file);
        goto done;
    }
    fclose(file);
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
    if (status)
        *status = 200;
done:
    if (!out) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, failure);
        ms_json_writer_object_end(&w);
        out = ms_json_writer_take(&w);
    }
    free(id);
    free(pipeline);
    free(executable);
    free(dir);
    free(path);
    free(data);
    return out;
}

char* ms_ubisoft_launch_game_json(const char* home, const char* body, size_t body_length, int* status) {
    char *id = NULL, *pipeline = NULL;
    ubisoft_game games[256];
    size_t count = scan_registry_games(home, games, sizeof(games) / sizeof(games[0]));
    const ubisoft_game* game;
    char *launcher = NULL, *executable = NULL;
    char failure[512] = "Could not launch the installed Ubisoft game";
    unsigned appid = 0;
    off_t executable_size = 0;
    int failure_status = 400;
    bool ok = false;
    ms_json_writer writer;
    char* out;
    if (status)
        *status = failure_status;
    if (!parse_request(body, body_length, &id, &pipeline)) {
        snprintf(failure, sizeof(failure), "Request is missing a valid Ubisoft game ID");
        goto done;
    }
    const char* normalized_pipeline = ubisoft_canonical_pipeline(pipeline);
    free(pipeline);
    pipeline = strdup(normalized_pipeline ? normalized_pipeline : UBISOFT_PIPELINE_DEFAULT);
    if (!pipeline) {
        failure_status = 500;
        snprintf(failure, sizeof(failure), "Could not allocate the graphics route for the Ubisoft game");
        goto done;
    }
    failure_status = 500;
    game = find_game(games, count, id);
    if (!game) {
        failure_status = 404;
        snprintf(failure, sizeof(failure), "Installed Ubisoft game was not found in the local registry");
        goto done;
    }
    launcher = ubisoft_launcher(home);
    if (!launcher) {
        failure_status = 409;
        snprintf(failure, sizeof(failure), "Ubisoft Connect is not installed in its MetalSharp prefix");
        goto done;
    }
    {
        char init_error[256] = {0};
        if (!initialize_prefix(home, init_error, sizeof(init_error))) {
            snprintf(failure, sizeof(failure), "%s",
                     init_error[0] ? init_error : "Could not initialize the Ubisoft prefix");
            goto done;
        }
    }
    appid = ubisoft_appid(id);
    ubisoft_find_game_executable(game->path, 0, &executable, &executable_size);
    if (!executable) {
        failure_status = 404;
        snprintf(failure, sizeof(failure), "No launchable Windows executable was found for %s", game->name);
        goto done;
    }
    if (!ms_steam_stage_route_for_executable(home, pipeline, game->path, executable)) {
        snprintf(failure, sizeof(failure), "Could not stage the %s graphics route for %s", pipeline, game->name);
        goto done;
    }
    ms_steam_deploy_controller_input_shims(home, game->path);
    if (read_client_pid(home) <= 0 && !launch_connect(home)) {
        snprintf(failure, sizeof(failure), "Could not start Ubisoft Connect for %s", game->name);
        goto done;
    }
    if (read_client_pid(home) <= 0) {
        struct timespec delay = {2, 0};
        (void)nanosleep(&delay, NULL);
    }
    ok = spawn_game_process(home, executable, game->path, pipeline, appid);
    if (!ok)
        snprintf(failure, sizeof(failure), "Could not start the MetalSharp Wine process for %s", game->name);
done:
    if (status)
        *status = ok ? 200 : failure_status;
    if (ok)
        out = strdup("{\"ok\":true}");
    else {
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, false);
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, failure);
        ms_json_writer_object_end(&writer);
        out = ms_json_writer_take(&writer);
    }
    free(id);
    free(pipeline);
    free(launcher);
    free(executable);
    return out;
}
