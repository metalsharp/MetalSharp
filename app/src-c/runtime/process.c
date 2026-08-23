#include "metalsharp_backend/process.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/steam_actions.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct running_game {
    unsigned appid;
    pid_t pid;
    struct running_game* next;
} running_game;
static running_game* g_running;

static char* join_path(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (p)
        snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
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
static void remember(unsigned appid, pid_t pid) {
    running_game* g;
    for (g = g_running; g; g = g->next)
        if (g->appid == appid) {
            g->pid = pid;
            return;
        }
    g = calloc(1, sizeof(*g));
    if (g) {
        g->appid = appid;
        g->pid = pid;
        g->next = g_running;
        g_running = g;
    }
}

void ms_process_register_game(unsigned appid, pid_t pid) {
    if (appid > 0 && pid > 0)
        remember(appid, pid);
}
static void forget(unsigned appid) {
    running_game** p = &g_running;
    while (*p) {
        if ((*p)->appid == appid) {
            running_game* old = *p;
            *p = old->next;
            free(old);
            return;
        }
        p = &(*p)->next;
    }
}
static bool active(pid_t pid) {
    int status;
    pid_t waited;
    if (pid <= 0)
        return false;
    /* Games are launched by a backend-owned child. Once that wrapper exits,
     * it can remain a zombie until reaped; kill(pid, 0) still succeeds for a
     * zombie and used to leave the Library Stop button stuck forever. */
    waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
        return false;
    if (waited == 0)
        return true;
    if (waited < 0 && errno != ECHILD)
        return errno == EINTR || errno == EPERM;
    if (kill(pid, 0) == 0)
        return true;
    return errno == EPERM;
}
static void prune(void) {
    running_game** p = &g_running;
    while (*p) {
        if (!active((*p)->pid)) {
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
        remember((unsigned)aid, pid);
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
    /* Rust's /game/launch-auto is the direct pipeline entry point.  It is not
     * the same operation as /steam/launch-game with its default Steam route. */
    return ms_steam_launch_auto_json(home, body, len, status);
}

char* ms_process_running_json(void) {
    running_game* g;
    ms_json_writer w;
    char* out;
    prune();
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
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}

char* ms_process_kill_json(const char* body, size_t len, int* status) {
    ms_json* r = parse_root(body, len);
    unsigned long long pid64 = 0, aid = 0;
    pid_t pid = 0;
    running_game* g;
    ms_json_writer w;
    char* out;
    if (status)
        *status = 400;
    if (!r)
        return error_json("invalid JSON object");
    (void)u64(r, "pid", &pid64);
    (void)u64(r, "appid", &aid);
    if (aid) {
        for (g = g_running; g; g = g->next)
            if (g->appid == (unsigned)aid) {
                pid = g->pid;
                break;
            }
    }
    if (pid == 0 && pid64 > 0)
        pid = (pid_t)pid64;
    if (pid <= 0) {
        ms_json_free(r);
        return error_json("pid required");
    }
    if (status)
        *status = 500;
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        char msg[128];
        snprintf(msg, sizeof(msg), "failed to kill pid %d: %s", (int)pid, strerror(errno));
        ms_json_free(r);
        return error_json(msg);
    }
    if (aid)
        forget((unsigned)aid);
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

char* ms_process_force_quit_json(int* status) {
    running_game* g;
    ms_json_writer w;
    char* out;
    size_t count = 0;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "terminated");
    ms_json_writer_array_begin(&w);
    for (g = g_running; g;) {
        running_game* next = g->next;
        if (kill(g->pid, SIGKILL) == 0 || errno == ESRCH) {
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
    g_running = NULL;
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
    return out;
}
char* ms_process_force_kill_json(const char* home, int* status) {
    char out[256];
    (void)home;
    if (status)
        *status = 200;
    snprintf(out, sizeof(out), "{\"ok\":true,\"terminated\":[],\"killed\":[],\"errors\":[],\"backendPid\":%ld}",
             (long)getpid());
    return strdup(out);
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
