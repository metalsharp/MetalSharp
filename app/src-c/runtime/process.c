#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/ubisoft.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#endif
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct running_game {
    unsigned appid;
    pid_t pid;
    unsigned long long keep_until_ms;
    bool wine_fallback;
    /* Base name of the launched .exe, lower case; empty when unknown. */
    char executable[128];
    struct running_game* next;
} running_game;
typedef struct retired_child {
    pid_t pid;
    struct retired_child* next;
} retired_child;
static running_game* g_running;
static unsigned g_last_registered_appid;
static retired_child* g_retired_children;
static pthread_mutex_t g_running_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_background_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_background_task_generation;
static volatile sig_atomic_t g_background_shutdown_requested;

static unsigned long long process_monotonic_millis(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

unsigned long ms_process_background_task_generation(void) {
    unsigned long generation;
    pthread_mutex_lock(&g_background_task_mutex);
    generation = g_background_task_generation;
    pthread_mutex_unlock(&g_background_task_mutex);
    return generation;
}

bool ms_process_background_task_cancelled(unsigned long generation) {
    bool cancelled;
    if (g_background_shutdown_requested)
        return true;
    pthread_mutex_lock(&g_background_task_mutex);
    cancelled = generation != g_background_task_generation;
    pthread_mutex_unlock(&g_background_task_mutex);
    return cancelled;
}

bool ms_process_background_task_begin(unsigned long generation) {
    pthread_mutex_lock(&g_background_task_mutex);
    if (g_background_shutdown_requested || generation != g_background_task_generation) {
        pthread_mutex_unlock(&g_background_task_mutex);
        return false;
    }
    return true;
}

void ms_process_background_task_end(void) {
    pthread_mutex_unlock(&g_background_task_mutex);
}

bool ms_process_background_shutdown_requested(void) {
    return g_background_shutdown_requested != 0;
}

void ms_process_cancel_background_tasks(void) {
    pthread_mutex_lock(&g_background_task_mutex);
    g_background_task_generation++;
    pthread_mutex_unlock(&g_background_task_mutex);
}

void ms_process_request_background_shutdown(void) {
    g_background_shutdown_requested = 1;
}

static char* join_path(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (p)
        snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
}

typedef struct wine_process_list {
    pid_t* pids;
    size_t count;
    size_t capacity;
} wine_process_list;

static bool contains_ci(const char* value, const char* needle) {
    size_t length;
    if (!value || !needle || !needle[0])
        return false;
    length = strlen(needle);
    for (; *value; value++)
        if (!strncasecmp(value, needle, length))
            return true;
    return false;
}

static bool command_runs_executable(const char* command, const char* executable) {
    size_t length;
    if (!command || !executable || !executable[0])
        return false;
    length = strlen(executable);
    for (const char* at = command; *at; at++) {
        if (strncasecmp(at, executable, length))
            continue;
        bool starts = at == command || at[-1] == '\\' || at[-1] == '/' || at[-1] == ' ' || at[-1] == '"';
        bool ends = at[length] == '\0' || at[length] == ' ' || at[length] == '"';
        if (starts && ends)
            return true;
    }
    return false;
}

static bool process_executable_within(pid_t pid, const char* root) {
    char executable[PATH_MAX];
    size_t length;
    if (!root || !root[0])
        return false;
#if defined(__APPLE__)
    if (proc_pidpath((int)pid, executable, sizeof(executable)) <= 0)
        return false;
#elif defined(__linux__)
    char proc_path[64];
    ssize_t bytes;
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    bytes = readlink(proc_path, executable, sizeof(executable) - 1);
    if (bytes <= 0)
        return false;
    executable[bytes] = '\0';
#else
    (void)pid;
    return false;
#endif
    length = strlen(root);
    return !strncmp(executable, root, length) && (executable[length] == '\0' || executable[length] == '/');
}

static bool process_is_steam(const char* command) {
    return contains_ci(command, "steam.exe") || contains_ci(command, "steamwebhelper") ||
           contains_ci(command, "steamservice.exe") || contains_ci(command, "steam_osx") ||
           contains_ci(command, "Steam.app/Contents/MacOS");
}

static bool process_is_wine_helper(const char* command) {
    return contains_ci(command, " reg import ") || contains_ci(command, " regedit ") ||
           contains_ci(command, "reg.exe") || contains_ci(command, "regedit.exe") ||
           contains_ci(command, "wineboot.exe") || contains_ci(command, "winedevice.exe") ||
           contains_ci(command, "winedbg.exe") || contains_ci(command, "services.exe") ||
           contains_ci(command, "rpcss.exe") || contains_ci(command, "svchost.exe") ||
           contains_ci(command, "conhost.exe") || contains_ci(command, "lsass.exe") ||
           contains_ci(command, "plugplay.exe") || contains_ci(command, "explorer.exe") ||
           contains_ci(command, "winemenubuilder.exe");
}

static bool wine_game_process_owned(pid_t pid, const char* command, const char* prefix, const char* runtime) {
    return command && !process_is_steam(command) && !process_is_wine_helper(command) && contains_ci(command, ".exe") &&
           (strstr(command, prefix) != NULL || strstr(command, runtime) != NULL ||
            process_executable_within(pid, runtime));
}

static bool wine_process_list_add(wine_process_list* list, pid_t pid) {
    pid_t* expanded;
    size_t capacity;
    if (list->count == list->capacity) {
        capacity = list->capacity ? list->capacity * 2 : 16;
        expanded = realloc(list->pids, capacity * sizeof(*expanded));
        if (!expanded)
            return false;
        list->pids = expanded;
        list->capacity = capacity;
    }
    list->pids[list->count++] = pid;
    return true;
}

static wine_process_list find_wine_executables(const char* home, const char* executable) {
    wine_process_list result = {0};
    char prefix[PATH_MAX], runtime[PATH_MAX], line[4096];
    FILE* pipe;
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(runtime, sizeof(runtime), "%s/runtime/wine", home);
    pipe = popen("/bin/ps axo pid=,command=", "r");
    if (!pipe)
        return result;
    while (fgets(line, sizeof(line), pipe)) {
        char* command = line;
        char* end;
        char* newline;
        long raw_pid;
        while (*command == ' ' || *command == '\t')
            command++;
        errno = 0;
        raw_pid = strtol(command, &end, 10);
        if (errno != 0 || end == command || raw_pid <= 1 || raw_pid > INT_MAX || raw_pid == (long)getpid())
            continue;
        while (*end == ' ' || *end == '\t')
            end++;
        newline = strchr(end, '\n');
        if (newline)
            *newline = '\0';
        if (!wine_game_process_owned((pid_t)raw_pid, end, prefix, runtime))
            continue;
        if (executable && !command_runs_executable(end, executable))
            continue;
        if (!wine_process_list_add(&result, (pid_t)raw_pid))
            break;
    }
    pclose(pipe);
    return result;
}

static wine_process_list find_non_steam_wine_executables(const char* home) {
    return find_wine_executables(home, NULL);
}

static size_t kill_non_steam_wine_executables(const char* home, wine_process_list* killed) {
    wine_process_list processes = find_non_steam_wine_executables(home);
    size_t count = 0;
    for (size_t i = 0; i < processes.count; i++) {
        pid_t pid = processes.pids[i];
        if (kill(pid, SIGKILL) == 0 || errno == ESRCH) {
            if (killed)
                (void)wine_process_list_add(killed, pid);
            count++;
        }
    }
    free(processes.pids);
    return count;
}

static char* runtime_missing_error(const char* home) {
    char* wine = join_path(home, "runtime/wine/bin/wine");
    bool missing = !wine || access(wine, X_OK) != 0;
    if (!missing) {
        free(wine);
        return NULL;
    }
    const char* candidates[] = {"/opt/homebrew/bin/wine64", "/usr/bin/wine", "/usr/local/bin/wine"};
    char found[256];
    size_t used = 0;
    found[used++] = '[';
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], F_OK) == 0) {
            int n = snprintf(found + used, sizeof(found) - used, "%s\"%s\"", used > 1 ? "," : "", candidates[i]);
            if (n > 0 && (size_t)n < sizeof(found) - used)
                used += (size_t)n;
        }
    }
    if (used < sizeof(found))
        found[used++] = ']';
    if (used < sizeof(found))
        found[used] = '\0';
    char msg[768];
    snprintf(msg, sizeof(msg),
             "MetalSharp Wine runtime missing at %s — run setup. System/third-party Wine is intentionally not used "
             "(found: %s)",
             wine ? wine : "", found);
    free(wine);
    return strdup(msg);
}
static ms_json* parse_root(const char* body, size_t len) {
    char e[128];
    ms_json* v = ms_json_parse(body ? body : "", len, e, sizeof(e));
    if (!v || ms_json_type_of(v) != MS_JSON_OBJECT) {
        ms_json_free(v);
        return NULL;
    }
    return v;
}
static bool u64(const ms_json* r, const char* key, unsigned long long* out) {
    long long n;
    if (!ms_json_as_i64(ms_json_object_get(r, key), &n) || n < 0)
        return false;
    *out = (unsigned long long)n;
    return true;
}
static char* str(const ms_json* r, const char* key) {
    char* s = NULL;
    (void)ms_json_as_string(ms_json_object_get(r, key), &s);
    return s;
}
static char* error_json(const char* s) {
    ms_json_writer w;
    char* r;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, s);
    ms_json_writer_object_end(&w);
    r = ms_json_writer_take(&w);
    return r;
}
static void reap_retired_children(void) {
    retired_child** current = &g_retired_children;
    while (*current) {
        retired_child* child = *current;
        int status;
        pid_t waited = waitpid(child->pid, &status, WNOHANG);
        if (waited == child->pid || (waited < 0 && errno != EINTR)) {
            *current = child->next;
            free(child);
        } else
            current = &child->next;
    }
}

static void retire_child_if_needed(pid_t pid) {
    retired_child* child;
    int status;
    pid_t waited;
    if (pid <= 0)
        return;
    waited = waitpid(pid, &status, WNOHANG);
    if (waited != 0 && !(waited < 0 && errno == EINTR))
        return; /* Reaped already, not our child, or an unrecoverable wait error. */
    for (child = g_retired_children; child; child = child->next)
        if (child->pid == pid)
            return;
    child = calloc(1, sizeof(*child));
    if (child) {
        child->pid = pid;
        child->next = g_retired_children;
        g_retired_children = child;
    }
}

static void remember(unsigned appid, pid_t pid) {
    running_game* g;
    reap_retired_children();
    for (g = g_running; g; g = g->next)
        if (g->appid == appid) {
            if (g->pid != pid)
                retire_child_if_needed(g->pid);
            g->pid = pid;
            g->wine_fallback = false;
            return;
        }
    g = calloc(1, sizeof(*g));
    if (g) {
        g->appid = appid;
        g->pid = pid;
        g->wine_fallback = false;
        g->next = g_running;
        g_running = g;
    }
}

void ms_process_register_pending_game(unsigned appid, pid_t pid, unsigned grace_seconds) {
    running_game* g;
    if (appid == 0 || pid <= 0)
        return;
    pthread_mutex_lock(&g_running_mutex);
    g_last_registered_appid = appid;
    remember(appid, pid);
    for (g = g_running; g; g = g->next) {
        if (g->appid == appid) {
            g->keep_until_ms = process_monotonic_millis() + (unsigned long long)grace_seconds * 1000ULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_running_mutex);
}

static void set_executable(unsigned appid, const char* executable) {
    const char* base = executable ? executable : "";
    for (const char* at = base; *at; at++)
        if (*at == '/' || *at == '\\')
            base = at + 1;
    for (running_game* g = g_running; g; g = g->next)
        if (g->appid == appid) {
            size_t i = 0;
            for (; base[i] && i + 1 < sizeof(g->executable); i++)
                g->executable[i] = (char)tolower((unsigned char)base[i]);
            g->executable[i] = '\0';
            return;
        }
}

void ms_process_register_game(unsigned appid, pid_t pid) {
    if (appid > 0 && pid > 0) {
        pthread_mutex_lock(&g_running_mutex);
        g_last_registered_appid = appid;
        remember(appid, pid);
        set_executable(appid, NULL);
        pthread_mutex_unlock(&g_running_mutex);
    }
}

void ms_process_register_game_executable(unsigned appid, pid_t pid, const char* executable) {
    if (appid > 0 && pid > 0) {
        pthread_mutex_lock(&g_running_mutex);
        g_last_registered_appid = appid;
        remember(appid, pid);
        set_executable(appid, executable);
        pthread_mutex_unlock(&g_running_mutex);
    }
}
static void forget(unsigned appid) {
    running_game** p = &g_running;
    while (*p) {
        if ((*p)->appid == appid) {
            running_game* old = *p;
            retire_child_if_needed(old->pid);
            *p = old->next;
            free(old);
            return;
        }
        p = &(*p)->next;
    }
}
static bool signal_game_process(pid_t pid, int signal_number) {
    if (kill(-pid, signal_number) == 0)
        return true;
    if (errno != ESRCH)
        return false;
    return kill(pid, signal_number) == 0 || errno == ESRCH;
}

static bool process_group_active(pid_t pid) {
    return kill(-pid, 0) == 0 || errno == EPERM;
}

static bool active(pid_t pid) {
    int status;
    pid_t waited;
    if (pid <= 0)
        return false;
    /* A Wine leader may exit while its game remains in the process group. Keep
     * that group registered for status and stop requests, but reap the leader
     * so its zombie does not make a completed launch look active forever. */
    waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
        return process_group_active(pid);
    if (waited == 0)
        return true;
    if (waited < 0 && errno != ECHILD)
        return errno == EINTR || errno == EPERM;
    if (kill(pid, 0) == 0 || errno == EPERM)
        return true;
    return process_group_active(pid);
}
static void prune(void) {
    running_game** p = &g_running;
    reap_retired_children();
    while (*p) {
        if (!active((*p)->pid) && (*p)->keep_until_ms <= process_monotonic_millis()) {
            running_game* old = *p;
            *p = old->next;
            free(old);
        } else
            p = &(*p)->next;
    }
}
static char* find_exe(const char* root, unsigned depth) {
    DIR* d;
    struct dirent* e;
    if (depth > 4)
        return NULL;
    d = opendir(root);
    if (!d)
        return NULL;
    while ((e = readdir(d)) != NULL) {
        char *p, *found;
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        p = join_path(root, e->d_name);
        if (!p)
            continue;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && strlen(e->d_name) > 4 &&
            strcasecmp(e->d_name + strlen(e->d_name) - 4, ".exe") == 0) {
            if (!strstr(e->d_name, "setup") && !strstr(e->d_name, "redist") && !strstr(e->d_name, "uninstall")) {
                closedir(d);
                return p;
            }
        }
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            found = find_exe(p, depth + 1);
            if (found) {
                free(p);
                closedir(d);
                return found;
            }
        }
        free(p);
    }
    closedir(d);
    return NULL;
}
static char* appid_exe(const char* home, unsigned appid) {
    char id[64];
    char *dir, *exe;
    snprintf(id, sizeof(id), "games/%u", appid);
    dir = join_path(home, id);
    if (!dir)
        return NULL;
    exe = find_exe(dir, 0);
    free(dir);
    return exe;
}
static char* spawn_exe(const char* home, const char* exe, pid_t* pid_out) {
    bool windows = strlen(exe) > 4 && strcasecmp(exe + strlen(exe) - 4, ".exe") == 0;
    char* wine = join_path(home, "runtime/wine/bin/metalsharp-wine");
    if (wine && access(wine, X_OK) != 0) {
        free(wine);
        wine = join_path(home, "runtime/wine/bin/wine");
    }
    pid_t pid = fork();
    if (pid < 0) {
        free(wine);
        return strdup(strerror(errno));
    }
    if (pid == 0) {
        if (windows && wine && access(wine, X_OK) == 0) {
            char* prefix = join_path(home, "prefix-steam");
            if (prefix)
                setenv("WINEPREFIX", prefix, 1);
            execl(wine, wine, exe, (char*)NULL);
            free(prefix);
        } else
            execl(exe, exe, (char*)NULL);
        _exit(127);
    }
    free(wine);
    *pid_out = pid;
    return NULL;
}

char* ms_process_launch_json(const char* home, const char* body, size_t len, int* status) {
    ms_json* r = parse_root(body, len);
    char *exe = NULL, *err;
    unsigned long long aid = 0;
    pid_t pid = 0;
    ms_json_writer w;
    char* out;
    if (status)
        *status = 500;
    if (!r) {
        return error_json("invalid JSON object");
    }
    exe = str(r, "exePath");
    (void)u64(r, "steamAppId", &aid);
    if ((!exe || exe[0] == '\0') && aid > 0) {
        free(exe);
        exe = appid_exe(home, (unsigned)aid);
    }
    if (!exe || exe[0] == '\0') {
        char* runtime_error = runtime_missing_error(home);
        free(exe);
        ms_json_free(r);
        if (runtime_error) {
            char* out = error_json(runtime_error);
            free(runtime_error);
            return out;
        }
        return error_json("executable path required");
    }
    err = spawn_exe(home, exe, &pid);
    if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", err);
        free(err);
        free(exe);
        ms_json_free(r);
        return error_json(msg);
    }
    if (aid > 0)
        ms_process_register_game((unsigned)aid, pid);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned)pid);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    if (status)
        *status = 200;
    free(exe);
    ms_json_free(r);
    return out;
}

char* ms_process_launch_auto_json(const char* home, const char* body, size_t len, int* status) {
    /* /game/launch-auto is the direct pipeline entry point. It is not
     * the same operation as /steam/launch-game with its default Steam route. */
    return ms_steam_launch_auto_json(home, body, len, status);
}

char* ms_process_running_json(const char* home) {
    running_game* g;
    ms_json_writer w;
    char* out;
    bool odyssey_registered = false;
    pid_t odyssey_pid = 0;
    wine_process_list wine_processes = find_non_steam_wine_executables(home);
    pid_t eve_pid = ms_steam_eve_process_pid(home);
    pid_t marvel_rivals_pid = ms_steam_marvel_rivals_process_pid(home);
    pid_t baldurs_gate_3_pid = ms_steam_baldurs_gate_3_process_pid(home);
    ms_ubisoft_register_running_games(home);
    if (eve_pid > 0)
        ms_process_register_game(8500, eve_pid);
    if (marvel_rivals_pid > 0)
        ms_process_register_game(2767030, marvel_rivals_pid);
    if (baldurs_gate_3_pid > 0)
        ms_process_register_game(1086940, baldurs_gate_3_pid);
    pthread_mutex_lock(&g_running_mutex);
    if (wine_processes.count > 0 && g_last_registered_appid > 0) {
        for (g = g_running; g; g = g->next)
            if (g->appid == g_last_registered_appid && !active(g->pid)) {
                /* The launched process and its group are gone. A game whose
                 * executable is known runs on only while that executable does
                 * (closing it from inside the game stops it); otherwise any
                 * other Wine program in the prefix stands in for it. */
                if (g->executable[0]) {
                    wine_process_list same = find_wine_executables(home, g->executable);
                    if (same.count > 0) {
                        g->pid = same.pids[0];
                        g->wine_fallback = true;
                    }
                    free(same.pids);
                } else {
                    g->pid = wine_processes.pids[0];
                    g->wine_fallback = true;
                }
                break;
            }
    }
    prune();
    for (g = g_running; g; g = g->next)
        if (g->appid == 812140) {
            odyssey_registered = true;
            break;
        }
    if (!odyssey_registered)
        odyssey_pid = ms_steam_odyssey_activity_pid(home);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "running");
    ms_json_writer_array_begin(&w);
    for (g = g_running; g; g = g->next) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, g->appid);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)g->pid);
        ms_json_writer_object_end(&w);
    }
    if (!odyssey_registered && odyssey_pid > 0) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, 812140);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)odyssey_pid);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    pthread_mutex_unlock(&g_running_mutex);
    free(wine_processes.pids);
    return out;
}

char* ms_process_kill_json(const char* home, const char* body, size_t len, int* status) {
    ms_json* r = parse_root(body, len);
    unsigned long long pid64 = 0, aid = 0;
    pid_t pid = 0;
    bool registered = false;
    bool wine_fallback = false;
    running_game* g;
    ms_json_writer w;
    char* out;
    if (status)
        *status = 400;
    if (!r)
        return error_json("invalid JSON object");
    (void)u64(r, "pid", &pid64);
    (void)u64(r, "appid", &aid);
    if (aid == 8500) {
        pid_t eve_pid = ms_steam_eve_process_pid(home);
        pthread_mutex_lock(&g_running_mutex);
        forget((unsigned)aid);
        pthread_mutex_unlock(&g_running_mutex);
        if (!ms_steam_stop_eve_processes(home)) {
            ms_json_free(r);
            if (status)
                *status = 500;
            return error_json("failed to stop EVE Online launcher and game processes");
        }
        ms_json_free(r);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)(eve_pid > 0 ? eve_pid : pid64));
        ms_json_writer_object_end(&w);
        if (status)
            *status = 200;
        return ms_json_writer_take(&w);
    }
    if (aid == 2767030) {
        pid_t marvel_pid = ms_steam_marvel_rivals_process_pid(home);
        pthread_mutex_lock(&g_running_mutex);
        forget((unsigned)aid);
        pthread_mutex_unlock(&g_running_mutex);
        if (!ms_steam_stop_marvel_rivals_processes(home)) {
            ms_json_free(r);
            if (status)
                *status = 500;
            return error_json("failed to stop Marvel Rivals game processes");
        }
        ms_json_free(r);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)(marvel_pid > 0 ? marvel_pid : pid64));
        ms_json_writer_object_end(&w);
        if (status)
            *status = 200;
        return ms_json_writer_take(&w);
    }
    if (aid == 1086940) {
        pid_t game_pid = ms_steam_baldurs_gate_3_process_pid(home);
        pthread_mutex_lock(&g_running_mutex);
        forget((unsigned)aid);
        pthread_mutex_unlock(&g_running_mutex);
        if (!ms_steam_stop_baldurs_gate_3_processes(home)) {
            ms_json_free(r);
            if (status)
                *status = 500;
            return error_json("failed to stop Baldur's Gate 3 processes");
        }
        ms_json_free(r);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)(game_pid > 0 ? game_pid : pid64));
        ms_json_writer_object_end(&w);
        if (status)
            *status = 200;
        return ms_json_writer_take(&w);
    }
    if (aid == 812140) {
        bool stop_ok;
        pid_t activity_pid = ms_steam_odyssey_activity_pid(home);
        ms_steam_cancel_background_tasks();
        pthread_mutex_lock(&g_running_mutex);
        for (g = g_running; g; g = g->next)
            if (g->appid == (unsigned)aid) {
                pid = g->pid;
                registered = true;
                wine_fallback = g->wine_fallback;
                break;
            }
        forget((unsigned)aid);
        pthread_mutex_unlock(&g_running_mutex);
        if (pid > 1 && active(pid))
            (void)(registered ? signal_game_process(pid, SIGKILL) : kill(pid, SIGKILL) == 0);
        stop_ok = ms_steam_stop_odyssey_processes(home);
        ms_json_free(r);
        if (!stop_ok) {
            if (status)
                *status = 500;
            return error_json("failed to stop Assassin's Creed Odyssey and Ubisoft Connect processes");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)(pid > 0 ? pid : activity_pid));
        ms_json_writer_object_end(&w);
        out = ms_json_writer_take(&w);
        if (status)
            *status = 200;
        return out;
    }
    if (aid) {
        pthread_mutex_lock(&g_running_mutex);
        for (g = g_running; g; g = g->next)
            if (g->appid == (unsigned)aid) {
                pid = g->pid;
                registered = true;
                break;
            }
    } else {
        pthread_mutex_lock(&g_running_mutex);
    }
    if (pid == 0 && pid64 > 0) {
        pid = (pid_t)pid64;
        for (g = g_running; g; g = g->next)
            if (g->pid == pid) {
                registered = true;
                break;
            }
    }
    if (pid <= 0) {
        pthread_mutex_unlock(&g_running_mutex);
        if (aid > 0) {
            wine_process_list killed = {0};
            (void)kill_non_steam_wine_executables(home, &killed);
            if (killed.count > 0) {
                ms_json_writer_init(&w);
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "ok");
                ms_json_writer_bool(&w, true);
                ms_json_writer_key(&w, "pid");
                ms_json_writer_u64(&w, (unsigned)killed.pids[0]);
                ms_json_writer_object_end(&w);
                out = ms_json_writer_take(&w);
                if (status)
                    *status = 200;
                free(killed.pids);
                ms_json_free(r);
                return out;
            }
            free(killed.pids);
        }
        ms_json_free(r);
        return error_json("pid required");
    }
    if (status)
        *status = 500;
    if (!(registered ? signal_game_process(pid, SIGKILL) : (kill(pid, SIGKILL) == 0 || errno == ESRCH))) {
        if (aid > 0) {
            size_t fallback_count = kill_non_steam_wine_executables(home, NULL);
            if (fallback_count > 0) {
                if (aid)
                    forget((unsigned)aid);
                pthread_mutex_unlock(&g_running_mutex);
                ms_json_free(r);
                ms_json_writer_init(&w);
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "ok");
                ms_json_writer_bool(&w, true);
                ms_json_writer_key(&w, "pid");
                ms_json_writer_u64(&w, (unsigned)pid);
                ms_json_writer_object_end(&w);
                if (status)
                    *status = 200;
                return ms_json_writer_take(&w);
            }
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "failed to kill pid %d: %s", (int)pid, strerror(errno));
        pthread_mutex_unlock(&g_running_mutex);
        ms_json_free(r);
        return error_json(msg);
    }
    if (wine_fallback)
        (void)kill_non_steam_wine_executables(home, NULL);
    if (aid)
        forget((unsigned)aid);
    pthread_mutex_unlock(&g_running_mutex);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned)pid);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    if (status)
        *status = 200;
    ms_json_free(r);
    return out;
}

char* ms_process_force_quit_json(const char* home, int* status) {
    running_game* g;
    ms_json_writer w;
    char* out;
    size_t count = 0;
    wine_process_list killed_wine = {0};
    ms_steam_cancel_background_tasks();
    (void)kill_non_steam_wine_executables(home, &killed_wine);
    pthread_mutex_lock(&g_running_mutex);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "terminated");
    ms_json_writer_array_begin(&w);
    for (g = g_running; g;) {
        running_game* next = g->next;
        if (signal_game_process(g->pid, SIGKILL)) {
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "appid");
            ms_json_writer_u64(&w, g->appid);
            ms_json_writer_key(&w, "pid");
            ms_json_writer_u64(&w, (unsigned)g->pid);
            ms_json_writer_object_end(&w);
            count++;
        }
        free(g);
        g = next;
    }
    for (size_t i = 0; i < killed_wine.count; i++) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned)killed_wine.pids[i]);
        ms_json_writer_object_end(&w);
        count++;
    }
    g_running = NULL;
    pthread_mutex_unlock(&g_running_mutex);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "errors");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "count");
    ms_json_writer_u64(&w, count);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    if (status)
        *status = 200;
    free(killed_wine.pids);
    return out;
}
char* ms_process_force_kill_json(const char* home, int* status) {
    ms_json_writer w;
    char* out;
    size_t count = kill_non_steam_wine_executables(home, NULL);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "terminated_count");
    ms_json_writer_u64(&w, count);
    ms_json_writer_key(&w, "backendPid");
    ms_json_writer_u64(&w, (unsigned)getpid());
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    if (status)
        *status = 200;
    return out;
}
char* ms_process_prepare_json(const char* home, const char* body, size_t len, int* status) {
    ms_json* r = parse_root(body, len);
    unsigned long long aid;
    char id[64], *dir, *marker, *appid_file;
    FILE* f;
    ms_json_writer w;
    char* out;
    if (status)
        *status = 400;
    if (!r || !u64(r, "appid", &aid) || aid == 0) {
        ms_json_free(r);
        return error_json("appid required");
    }
    snprintf(id, sizeof(id), "games/%llu", aid);
    if (status)
        *status = 500;
    dir = join_path(home, id);
    if (!dir || access(dir, F_OK) != 0) {
        free(dir);
        ms_json_free(r);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, "game directory not found");
        ms_json_writer_key(&w, "canonical_endpoint");
        ms_json_writer_string(&w, "/mtsp/prepare");
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    appid_file = join_path(dir, "steam_appid.txt");
    marker = join_path(dir, ".metalsharp_prepared");
    f = appid_file ? fopen(appid_file, "wb") : NULL;
    if (f) {
        fprintf(f, "%llu", aid);
        fclose(f);
    }
    f = marker ? fopen(marker, "wb") : NULL;
    if (f) {
        fputs("prepared: game_type=dxmt", f);
        fclose(f);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "alreadyPrepared");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "gameType");
    ms_json_writer_string(&w, "dxmt");
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, aid);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    if (status)
        *status = 200;
    free(dir);
    free(marker);
    free(appid_file);
    ms_json_free(r);
    return out;
}
