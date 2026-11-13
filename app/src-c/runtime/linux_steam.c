#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "metalsharp_backend/linux_steam.h"

#include "metalsharp_backend/config.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/wine_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STATUS_CACHE_MS 2000ULL
/* status --json answers at once (no wait on the virtual machine); this bounds a wedged one */
#define STATUS_TIMEOUT_SECONDS 3U
#define START_TIMEOUT_SECONDS  300U
#define SHOW_TIMEOUT_SECONDS   60U
#define URL_TIMEOUT_SECONDS    30U

/* run_command's results besides an exit status */
#define RUN_FAILED    (-1)
#define RUN_TIMED_OUT (-2)
/* reaped by a process-wide SIGCHLD reaper (bottle actions): it ended, with an unknown status */
#define RUN_LOST     (-3)
#define RUN_SIGNALED (-4)

typedef struct {
    char* home;
    bool install;
    bool start;
} linux_steam_job;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ms_linux_steam_state g_state;
static unsigned long long g_state_at;
static bool g_state_valid;
static bool g_job_running;
static bool g_job_install;
static char g_error[256];

static unsigned long long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

static bool format_path(char* out, size_t out_size, const char* format, ...) {
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(out, out_size, format, args);
    va_end(args);
    return written >= 0 && (size_t)written < out_size;
}

bool ms_linux_steam_vm_executable(const char* executable) {
    static const char* const names[] = {"krunkit", "gvproxy", "metalsharp-vmd", "metalsharp-steamview", "LinuxSteam"};
    const char* name;
    if (!executable)
        return false;
    if (strstr(executable, "/LinuxSteam.app/"))
        return true;
    name = strrchr(executable, '/');
    name = name ? name + 1 : executable;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return true;
    return false;
}

bool ms_linux_steam_command_path(const char* home, char* out, size_t out_size) {
    /* for the tests only: a stand-in command */
    const char* override = getenv("METALSHARP_LINUX_STEAM_COMMAND");
    char contents[PATH_MAX];
    if (!out || out_size == 0)
        return false;
    if (override && override[0])
        return format_path(out, out_size, "%s", override);
    return home && ms_wine_app_contents(home, contents, sizeof(contents)) &&
           format_path(out, out_size, "%s/share/steamvm/metalsharp-steam", contents);
}

bool ms_linux_steam_available(const char* home) {
    char command[PATH_MAX];
    return ms_wine_is_fex() && ms_linux_steam_command_path(home, command, sizeof(command)) &&
           access(command, X_OK) == 0;
}

bool ms_linux_steam_mode(const char* home) {
    return home && ms_wine_is_fex() && !strcmp(ms_config_steam_client(home), "linux") && ms_linux_steam_available(home);
}

const char* ms_linux_steam_intent_backend(const char* lane) {
    if (!lane)
        return NULL;
    if (!strcmp(lane, "dxmt") || !strcmp(lane, "dxmt_32"))
        return "mullion";
    if (!strcmp(lane, "vkd3d"))
        return "vkd3d";
    if (!strcmp(lane, "dxvk"))
        return "dxvk";
    if (!strcmp(lane, "wine_bare"))
        return "wine";
    return NULL;
}

/* A folder of this user's only (0700), never through a symbolic link. */
static bool private_dir(const char* path) {
    struct stat info;
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return false;
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid())
        return false;
    return (info.st_mode & 0777) == 0700 || chmod(path, 0700) == 0;
}

static bool intent_dir(const char* home, char* out, size_t out_size) {
    char run[PATH_MAX], vm_run[PATH_MAX];
    return home && format_path(run, sizeof(run), "%s/run", home) &&
           format_path(vm_run, sizeof(vm_run), "%s/run/linux-steam", home) &&
           format_path(out, out_size, "%s/run/linux-steam/launch-intent", home) && private_dir(run) &&
           private_dir(vm_run) && private_dir(out);
}

bool ms_linux_steam_write_intent(const char* home, unsigned appid, const char* lane) {
    const char* backend = ms_linux_steam_intent_backend(lane);
    char dir[PATH_MAX], path[PATH_MAX], staging[PATH_MAX], line[16];
    int fd, length;
    bool ok;
    if (!backend || appid == 0 || !intent_dir(home, dir, sizeof(dir)) ||
        !format_path(path, sizeof(path), "%s/%u", dir, appid) ||
        !format_path(staging, sizeof(staging), "%s/.%u.%ld.tmp", dir, appid, (long)getpid()))
        return false;
    (void)unlink(staging);
    fd = open(staging, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    length = snprintf(line, sizeof(line), "%s\n", backend);
    ok = length > 0 && write(fd, line, (size_t)length) == (ssize_t)length;
    ok = close(fd) == 0 && ok;
    /* in place at once, so the daemon never reads half a line */
    ok = ok && rename(staging, path) == 0;
    if (!ok)
        (void)unlink(staging);
    return ok;
}

void ms_linux_steam_clear_intent(const char* home, unsigned appid) {
    char path[PATH_MAX];
    if (home && appid && format_path(path, sizeof(path), "%s/run/linux-steam/launch-intent/%u", home, appid))
        (void)unlink(path);
}

/* What the command is started with: MetalSharp's folders for the virtual machine, the Steam prefix its games share,
 * and MetalSharp Wine's wine. */
static void command_environment(const char* home) {
    char path[PATH_MAX], contents[PATH_MAX];
    const char* search = getenv("PATH");
    if (format_path(path, sizeof(path), "%s/linux-steam", home))
        setenv("METALSHARP_VM_DIR", path, 1);
    if (format_path(path, sizeof(path), "%s/run/linux-steam", home))
        setenv("METALSHARP_VM_RUN", path, 1);
    if (format_path(path, sizeof(path), "%s/prefix-steam", home)) {
        setenv("METALSHARP_STEAM_PREFIX", path, 1);
        setenv("METALSHARP_GAME_PREFIX", path, 1);
    }
    if (ms_wine_app_contents(home, contents, sizeof(contents)) &&
        format_path(path, sizeof(path), "%s/MacOS/wine", contents))
        setenv("METALSHARP_WINE", path, 1);
    setenv("METALSHARP_HOME", home, 1);
    /* the system's tools (ssh, nc, curl, tar), whatever PATH the app was opened with */
    if (!search || !search[0])
        setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
    else if (format_path(path, sizeof(path), "%s:/usr/bin:/bin:/usr/sbin:/sbin", search))
        setenv("PATH", path, 1);
    unsetenv("WINEPREFIX");
    unsetenv("WINESERVER");
    unsetenv("DYLD_LIBRARY_PATH");
    unsetenv("DYLD_FALLBACK_LIBRARY_PATH");
    unsetenv("DYLD_INSERT_LIBRARIES");
}

/* Runs `metalsharp-steam <args>` in a session of its own, with no terminal, and waits up to seconds (0: no limit).
 * Its output goes to log (appended), or is captured (stdout only) when capture is given. Returns its exit status, or
 * one of the RUN_ results. */
static int run_command(const char* home, const char* const* args, unsigned seconds, const char* log, char* capture,
                       size_t capture_size, pid_t* pid_out) {
    char command[PATH_MAX];
    const char* argv[8];
    size_t argc = 0, used = 0;
    int out[2] = {-1, -1};
    int status = 0;
    bool lost = false;
    pid_t pid;
    unsigned long long deadline = seconds ? now_ms() + (unsigned long long)seconds * 1000ULL : 0;
    if (!ms_linux_steam_command_path(home, command, sizeof(command)) || access(command, X_OK) != 0)
        return RUN_FAILED;
    argv[argc++] = command;
    for (size_t i = 0; args && args[i] && argc + 1 < sizeof(argv) / sizeof(argv[0]); i++)
        argv[argc++] = args[i];
    argv[argc] = NULL;
    if (capture) {
        if (capture_size == 0 || pipe(out) != 0)
            return RUN_FAILED;
        capture[0] = '\0';
    }
    pid = fork();
    if (pid < 0) {
        if (capture) {
            close(out[0]);
            close(out[1]);
        }
        return RUN_FAILED;
    }
    if (pid == 0) {
        int null_in = open("/dev/null", O_RDONLY);
        int sink = log ? open(log, O_WRONLY | O_CREAT | O_APPEND, 0600) : open("/dev/null", O_WRONLY);
        (void)setsid();
        if (null_in >= 0)
            dup2(null_in, STDIN_FILENO);
        if (sink >= 0) {
            dup2(sink, STDOUT_FILENO);
            dup2(sink, STDERR_FILENO);
        }
        if (capture) {
            dup2(out[1], STDOUT_FILENO);
            close(out[0]);
            close(out[1]);
        }
        command_environment(home);
        execv(command, (char* const*)argv);
        _exit(127);
    }
    if (pid_out)
        *pid_out = pid;
    if (capture) {
        close(out[1]);
        (void)fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
    }
    for (;;) {
        pid_t done;
        if (capture) {
            struct pollfd poller = {out[0], POLLIN, 0};
            if (poll(&poller, 1, 100) > 0) {
                ssize_t got = read(out[0], capture + used, capture_size - 1 - used);
                if (got > 0) {
                    used += (size_t)got;
                    capture[used] = '\0';
                    if (used + 1 >= capture_size) /* full: the rest is not read */
                        used = capture_size - 1;
                }
            }
        } else {
            struct timespec pause = {0, 100000000L};
            nanosleep(&pause, NULL);
        }
        done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno == ECHILD) {
            lost = true;
            break;
        }
        if (deadline && now_ms() >= deadline) {
            struct timespec pause = {2, 0};
            kill(-pid, SIGTERM);
            kill(pid, SIGTERM);
            nanosleep(&pause, NULL);
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            if (capture)
                close(out[0]);
            return RUN_TIMED_OUT;
        }
    }
    if (capture) {
        /* what is left, without waiting on a program it started that kept the pipe open */
        for (;;) {
            ssize_t got = used + 1 < capture_size ? read(out[0], capture + used, capture_size - 1 - used) : 0;
            if (got <= 0)
                break;
            used += (size_t)got;
            capture[used] = '\0';
        }
        close(out[0]);
    }
    if (lost)
        return RUN_LOST;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return RUN_SIGNALED;
}

static bool log_path(const char* home, const char* name, char* out, size_t out_size) {
    char logs[PATH_MAX];
    if (!format_path(logs, sizeof(logs), "%s/logs", home) || !format_path(out, out_size, "%s/%s", logs, name))
        return false;
    (void)mkdir(logs, 0755);
    return true;
}

/* The last line of log that starts with prefix (or any non-empty line that is not a "==> " progress line when
 * prefix is NULL), without the prefix. */
static bool last_log_line(const char* log, const char* prefix, char* out, size_t out_size) {
    FILE* file = fopen(log, "rb");
    char line[512];
    bool found = false;
    long size;
    if (!file)
        return false;
    /* only the end of a long log */
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 65536)
        (void)fseek(file, size - 65536, SEEK_SET);
    else
        (void)fseek(file, 0, SEEK_SET);
    while (fgets(line, sizeof(line), file)) {
        size_t length = strcspn(line, "\r\n");
        line[length] = '\0';
        if (prefix ? strncmp(line, prefix, strlen(prefix)) != 0 : length == 0 || !strncmp(line, "==> ", 4))
            continue;
        snprintf(out, out_size, "%s", line + (prefix ? strlen(prefix) : 0));
        found = true;
    }
    fclose(file);
    return found;
}

static void set_error_from_log(const char* home, const char* name, const char* fallback) {
    char log[PATH_MAX], line[200] = "";
    if (!(log_path(home, name, log, sizeof(log)) && last_log_line(log, NULL, line, sizeof(line))))
        snprintf(line, sizeof(line), "%s", fallback);
    pthread_mutex_lock(&g_lock);
    snprintf(g_error, sizeof(g_error), "Linux Steam: %s", line);
    pthread_mutex_unlock(&g_lock);
}

static bool parse_state(const char* text, ms_linux_steam_state* state) {
    const char* start = strchr(text, '{');
    const char* end = strrchr(text, '}');
    char error[64];
    ms_json* json;
    const char* keys[] = {"installed", "vm_running", "steam_running", "installing"};
    bool* fields[] = {&state->installed, &state->vm_running, &state->steam_running, &state->installing};
    memset(state, 0, sizeof(*state));
    if (!start || !end || end < start)
        return false;
    json = ms_json_parse(start, (size_t)(end - start + 1), error, sizeof(error));
    if (!json || ms_json_type_of(json) != MS_JSON_OBJECT) {
        ms_json_free(json);
        return false;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        bool value = false;
        *fields[i] = ms_json_as_bool(ms_json_object_get(json, keys[i]), &value) && value;
    }
    ms_json_free(json);
    state->known = true;
    return true;
}

static void forget_state(void) {
    pthread_mutex_lock(&g_lock);
    g_state_valid = false;
    pthread_mutex_unlock(&g_lock);
}

bool ms_linux_steam_state_get(const char* home, ms_linux_steam_state* state, bool fresh) {
    const char* const args[] = {"status", "--json", NULL};
    char output[1024];
    ms_linux_steam_state read = {0};
    int code;
    if (!state)
        return false;
    memset(state, 0, sizeof(*state));
    pthread_mutex_lock(&g_lock);
    if (!fresh && g_state_valid && now_ms() - g_state_at < STATUS_CACHE_MS) {
        *state = g_state;
        pthread_mutex_unlock(&g_lock);
        return state->known;
    }
    pthread_mutex_unlock(&g_lock);
    code = run_command(home, args, STATUS_TIMEOUT_SECONDS, NULL, output, sizeof(output), NULL);
    if (code == RUN_FAILED || code == RUN_TIMED_OUT || !parse_state(output, &read))
        memset(&read, 0, sizeof(read));
    pthread_mutex_lock(&g_lock);
    g_state = read;
    g_state_at = now_ms();
    g_state_valid = true;
    pthread_mutex_unlock(&g_lock);
    *state = read;
    return read.known;
}

bool ms_linux_steam_client_running(const char* home) {
    ms_linux_steam_state state;
    return ms_linux_steam_state_get(home, &state, false) && state.steam_running;
}

static bool install_stage(const char* home, char* stage, size_t stage_size) {
    char log[PATH_MAX];
    return log_path(home, "linux-steam-setup.log", log, sizeof(log)) && last_log_line(log, "==> ", stage, stage_size);
}

bool ms_linux_steam_installing(const char* home, char* stage, size_t stage_size) {
    ms_linux_steam_state state;
    bool installing;
    pthread_mutex_lock(&g_lock);
    installing = g_job_running && g_job_install;
    pthread_mutex_unlock(&g_lock);
    if (!installing)
        installing = ms_linux_steam_state_get(home, &state, false) && state.installing;
    if (stage && stage_size) {
        stage[0] = '\0';
        if (installing && !install_stage(home, stage, stage_size))
            snprintf(stage, stage_size, "%s", "Starting");
    }
    return installing;
}

void ms_linux_steam_last_error(char* out, size_t out_size) {
    if (!out || !out_size)
        return;
    pthread_mutex_lock(&g_lock);
    snprintf(out, out_size, "%s", g_error);
    pthread_mutex_unlock(&g_lock);
}

/* The setup (install), then start and show; or start and show; in the background. */
static void* job_thread(void* argument) {
    linux_steam_job* job = argument;
    char log[PATH_MAX], setup_log[PATH_MAX];
    bool ok = log_path(job->home, "linux-steam.log", log, sizeof(log)) &&
              log_path(job->home, "linux-steam-setup.log", setup_log, sizeof(setup_log));
    if (ok && job->install) {
        const char* const install[] = {"install", NULL};
        int fd = open(setup_log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int code;
        if (fd >= 0)
            close(fd);
        /* no limit: it downloads Debian and Valve's client, and builds the virtual machine */
        code = run_command(job->home, install, 0, setup_log, NULL, 0, NULL);
        if (code == RUN_LOST) {
            ms_linux_steam_state state;
            ok = ms_linux_steam_state_get(job->home, &state, true) && state.installed;
        } else {
            ok = code == 0;
        }
        if (!ok)
            set_error_from_log(job->home, "linux-steam-setup.log", "the setup failed; see logs/linux-steam-setup.log");
    }
    forget_state();
    if (ok && job->start) {
        const char* const start[] = {"start", NULL};
        int code = run_command(job->home, start, START_TIMEOUT_SECONDS, log, NULL, 0, NULL);
        ok = code == 0 || code == RUN_LOST;
        if (!ok)
            set_error_from_log(job->home, "linux-steam.log", "Steam could not be started; see logs/linux-steam.log");
    }
    if (ok) {
        const char* const show[] = {"show", NULL};
        int code = run_command(job->home, show, SHOW_TIMEOUT_SECONDS, log, NULL, 0, NULL);
        if (code != 0 && code != RUN_LOST)
            set_error_from_log(job->home, "linux-steam.log", "Steam's window could not be shown");
    }
    forget_state();
    pthread_mutex_lock(&g_lock);
    g_job_running = false;
    g_job_install = false;
    pthread_mutex_unlock(&g_lock);
    free(job->home);
    free(job);
    return NULL;
}

static bool start_job(const char* home, bool install, bool start) {
    pthread_t thread;
    linux_steam_job* job = calloc(1, sizeof(*job));
    if (!job || !(job->home = strdup(home))) {
        free(job);
        return false;
    }
    job->install = install;
    job->start = start;
    pthread_mutex_lock(&g_lock);
    if (g_job_running) {
        pthread_mutex_unlock(&g_lock);
        free(job->home);
        free(job);
        return true; /* one is already on its way */
    }
    g_job_running = true;
    g_job_install = install;
    g_error[0] = '\0';
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&thread, NULL, job_thread, job) != 0) {
        pthread_mutex_lock(&g_lock);
        g_job_running = false;
        g_job_install = false;
        pthread_mutex_unlock(&g_lock);
        free(job->home);
        free(job);
        return false;
    }
    pthread_detach(thread);
    return true;
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

char* ms_linux_steam_launch_json(const char* home, int* status) {
    ms_linux_steam_state state;
    char stage[160] = "";
    bool busy, install;
    ms_json_writer writer;
    if (status)
        *status = 500;
    if (!ms_linux_steam_available(home))
        return error_json("This MetalSharp Wine does not carry Linux Steam");
    pthread_mutex_lock(&g_lock);
    busy = g_job_running;
    install = g_job_install;
    pthread_mutex_unlock(&g_lock);
    if (!busy) {
        if (!ms_linux_steam_state_get(home, &state, true))
            return error_json("Linux Steam's command did not answer; see logs/linux-steam.log");
        /* a setup started elsewhere (a terminal) is left to finish */
        install = state.installing || !state.installed;
        if (!state.installing && !start_job(home, !state.installed, !state.steam_running))
            return error_json("Linux Steam could not be started");
    }
    if (install)
        (void)ms_linux_steam_installing(home, stage, sizeof(stage));
    if (status)
        *status = 200;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "client");
    ms_json_writer_string(&writer, "linux");
    ms_json_writer_key(&writer, "installing");
    ms_json_writer_bool(&writer, install);
    ms_json_writer_key(&writer, "starting");
    ms_json_writer_bool(&writer, !install);
    ms_json_writer_key(&writer, "install_stage");
    ms_json_writer_string(&writer, stage);
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

bool ms_linux_steam_stop(const char* home, unsigned seconds) {
    const char* const stop[] = {"stop", NULL};
    char log[PATH_MAX];
    ms_linux_steam_state state;
    int code;
    if (!log_path(home, "linux-steam.log", log, sizeof(log)))
        return false;
    code = run_command(home, stop, seconds, log, NULL, 0, NULL);
    forget_state();
    if (ms_linux_steam_state_get(home, &state, true))
        return !state.vm_running && !state.steam_running;
    return code == 0 || code == RUN_LOST;
}

bool ms_linux_steam_stop_if_running(const char* home, unsigned seconds) {
    char command[PATH_MAX], run[PATH_MAX];
    ms_linux_steam_state state;
    struct stat info;
    if (!home || !ms_linux_steam_command_path(home, command, sizeof(command)) || access(command, X_OK) != 0 ||
        !format_path(run, sizeof(run), "%s/run/linux-steam", home) || stat(run, &info) != 0)
        return true; /* never started */
    if (ms_linux_steam_state_get(home, &state, true) && !state.vm_running && !state.steam_running)
        return true;
    return ms_linux_steam_stop(home, seconds);
}

char* ms_linux_steam_ensure_running(const char* home, unsigned wait_seconds) {
    const char* const start[] = {"start", NULL};
    char log[PATH_MAX];
    ms_linux_steam_state state;
    unsigned long long deadline = now_ms() + (unsigned long long)wait_seconds * 1000ULL;
    bool busy, install;
    pthread_mutex_lock(&g_lock);
    busy = g_job_running;
    install = g_job_install;
    pthread_mutex_unlock(&g_lock);
    if (install)
        return strdup("Linux Steam is still being set up");
    if (!ms_linux_steam_state_get(home, &state, true))
        return strdup("Linux Steam's command did not answer; see logs/linux-steam.log");
    if (state.steam_running)
        return NULL;
    if (state.installing)
        return strdup("Linux Steam is still being set up");
    if (!state.installed)
        return strdup("Linux Steam is not set up yet: use Start Steam in MetalSharp to set it up");
    /* a start already on its way (Start Steam) is waited for, not raced: the command takes one at a time */
    if (!busy && log_path(home, "linux-steam.log", log, sizeof(log)))
        (void)run_command(home, start, wait_seconds, log, NULL, 0, NULL);
    for (;;) {
        struct timespec pause = {1, 0};
        if (ms_linux_steam_state_get(home, &state, true) && state.steam_running)
            return NULL;
        if (now_ms() >= deadline)
            break;
        nanosleep(&pause, NULL);
    }
    {
        char message[160];
        snprintf(message, sizeof(message), "Linux Steam did not start within %u seconds; see logs/linux-steam.log",
                 wait_seconds);
        return strdup(message);
    }
}

char* ms_linux_steam_open_url(const char* home, unsigned appid, const char* lane, const char* url, pid_t* pid) {
    const char* const args[] = {"url", url, NULL};
    char log[PATH_MAX], line[200] = "", message[256];
    bool intent = ms_linux_steam_intent_backend(lane) != NULL;
    int code;
    if (!url || strncmp(url, "steam://", 8) != 0)
        return strdup("not a steam:// link");
    if (!log_path(home, "linux-steam.log", log, sizeof(log)))
        return strdup("path too long");
    if (intent && !ms_linux_steam_write_intent(home, appid, lane))
        return strdup("could not record the game's graphics backend for Linux Steam");
    code = run_command(home, args, URL_TIMEOUT_SECONDS, log, NULL, 0, pid);
    if (code == 0 || code == RUN_LOST)
        return NULL;
    /* the daemon must not take it for a later Play started in Steam */
    if (intent)
        ms_linux_steam_clear_intent(home, appid);
    if (!last_log_line(log, NULL, line, sizeof(line)))
        snprintf(line, sizeof(line), "%s", code == RUN_TIMED_OUT ? "it did not answer" : "it failed");
    snprintf(message, sizeof(message), "Linux Steam could not start the game: %s", line);
    return strdup(message);
}
