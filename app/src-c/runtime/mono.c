#include "metalsharp_backend/mono.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
static pid_t g_mono_pid;
static char* g_mono_marker_path;
static char g_mono_error[256];

static bool mkdir_p(const char* path) {
    char* copy = strdup(path);
    char* p;
    bool ok = copy != NULL;
    if (!ok)
        return false;
    for (p = copy + 1; ok && *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(copy, 0755) != 0 && errno != EEXIST)
            ok = false;
        *p = '/';
    }
    if (ok && mkdir(copy, 0755) != 0 && errno != EEXIST)
        ok = false;
    free(copy);
    return ok;
}

static char* join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    int s = x && a[x - 1] != '/' ? 1 : 0;
    char* p = malloc(x + y + s + 1);
    if (p)
        snprintf(p, x + y + s + 1, "%s%s%s", a, s ? "/" : "", b);
    return p;
}

static const char* prefix_path_suffix(const char* kind) {
    return !strcmp(kind, "steam") ? "prefix-steam" : "bottles/gog-prefix/prefix";
}

static bool mono_child_running(void) {
    int wait_status;
    pid_t waited;
    if (g_mono_pid <= 0)
        return false;
    waited = waitpid(g_mono_pid, &wait_status, WNOHANG);
    if (waited == 0)
        return true;
    if (waited == g_mono_pid) {
        if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
            if (!g_mono_marker_path || !mkdir_p(g_mono_marker_path))
                snprintf(g_mono_error, sizeof(g_mono_error), "Wine Mono installer completed but its version marker could not be created");
            else
                g_mono_error[0] = 0;
        } else {
            snprintf(g_mono_error, sizeof(g_mono_error), "Wine Mono installer exited with status %d",
                     WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1);
        }
    }
    g_mono_pid = 0;
    return false;
}

static char* mono_prefix(const char* home, const char* kind) {
    return join(home, prefix_path_suffix(kind));
}

static char* mono_marker(const char* home, const char* kind) {
    char* prefix = mono_prefix(home, kind);
    char* marker = prefix ? join(prefix, "drive_c/windows/mono/wine-mono-11.2.0") : NULL;
    free(prefix);
    return marker;
}

static char* mono_cache_path(const char* home) {
    char* cache = join(home, "cache/wine-mono");
    char* path = cache ? join(cache, "wine-mono-11.2.0-x86.msi") : NULL;
    free(cache);
    return path;
}

static const char* prefix_kind(const char* body, size_t len) {
    char er[64];
    ms_json* j = ms_json_parse(body ? body : "", body ? len : 0, er, sizeof(er));
    char* s = NULL;
    const ms_json* v = j ? ms_json_object_get(j, "prefix") : NULL;
    if (v)
        ms_json_as_string(v, &s);
    ms_json_free(j);
    if (s && (!strcmp(s, "gog") || !strcmp(s, "steam"))) {
        const char* out = !strcmp(s, "steam") ? "steam" : "gog";
        free(s);
        return out;
    }
    free(s);
    return "gog";
}
static char* status(const char* home, const char* kind) {
    char* p = mono_prefix(home, kind);
    char* marker = mono_marker(home, kind);
    char* mono = p ? join(p, "drive_c/windows/mono/mono-2.0") : NULL;
    char* cache = mono_cache_path(home);
    bool running = mono_child_running();
    bool installed = marker && access(marker, F_OK) == 0;
    bool any_mono = mono && access(mono, F_OK) == 0;
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "prefixKind");
    ms_json_writer_string(&w, kind);
    ms_json_writer_key(&w, "latestVersion");
    ms_json_writer_string(&w, "11.2.0");
    ms_json_writer_key(&w, "installedVersion");
    if (installed)
        ms_json_writer_string(&w, "11.2.0");
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, any_mono);
    ms_json_writer_key(&w, "upToDate");
    ms_json_writer_bool(&w, installed);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, running);
    ms_json_writer_key(&w, "stalled");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "pid");
    if (running)
        ms_json_writer_i64(&w, g_mono_pid);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "startedAt");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "elapsedSeconds");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "logPath");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "targetVersion");
    ms_json_writer_string(&w, "11.2.0");
    ms_json_writer_key(&w, "lastError");
    if (g_mono_error[0])
        ms_json_writer_string(&w, g_mono_error);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "msiCached");
    ms_json_writer_bool(&w, cache && access(cache, F_OK) == 0);
    ms_json_writer_key(&w, "downloading");
    ms_json_writer_bool(&w, running);
    ms_json_writer_key(&w, "downloadBytes");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_key(&w, "downloadTotal");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_key(&w, "downloadError");
    ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(p);
    free(marker);
    free(mono);
    free(cache);
    return o;
}
char* ms_mono_status_json(const char* home, const char* kind) {
    return status(home, !strcmp(kind, "steam") ? "steam" : "gog");
}
char* ms_mono_install_json(const char* home, const char* body, size_t len) {
    const char* kind = prefix_kind(body, len);
    char *prefix = mono_prefix(home, kind), *marker = mono_marker(home, kind);
    char *cache_dir = join(home, "cache/wine-mono"), *cache = mono_cache_path(home);
    char *wine = join(home, "runtime/wine/bin/wine");
    bool installed = marker && access(marker, F_OK) == 0;
    bool running = mono_child_running();
    char* s;
    ms_json_writer w;
    char* o;
    if (installed) {
        s = status(home, kind);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "alreadyInstalled");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "status");
        ms_json_writer_raw(&w, s ? s : "{}");
        ms_json_writer_object_end(&w);
        o = ms_json_writer_take(&w);
        free(s);
        free(prefix); free(marker); free(cache_dir); free(cache); free(wine);
        return o;
    }
    if (running || !prefix || !cache_dir || !cache || !wine || access(wine, X_OK) != 0 || !mkdir_p(cache_dir)) {
        free(prefix); free(marker); free(cache_dir); free(cache); free(wine);
        return strdup("{\"ok\":false,\"error\":\"Wine Mono installer is unavailable\"}");
    }
    g_mono_pid = fork();
    if (g_mono_pid < 0) {
        free(prefix); free(marker); free(cache_dir); free(cache); free(wine);
        return strdup("{\"ok\":false,\"error\":\"failed to start Wine Mono installer\"}");
    }
    if (g_mono_pid == 0) {
        char* runtime_lib = join(home, "runtime/wine/lib");
        char* unix_lib = join(home, "runtime/wine/lib/wine/x86_64-unix");
        char library_path[2048];
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        if (runtime_lib && unix_lib) {
            snprintf(library_path, sizeof(library_path), "%s:%s", runtime_lib, unix_lib);
            setenv("DYLD_FALLBACK_LIBRARY_PATH", library_path, 1);
        }
        if (access(cache, F_OK) != 0) {
            pid_t download_pid = fork();
            int download_status;
            if (download_pid == 0) {
                execl("/usr/bin/curl", "curl", "--fail", "--location", "--silent", "--show-error",
                      "-o", cache,
                      "https://github.com/wine-mono/wine-mono/releases/download/wine-mono-11.2.0/wine-mono-11.2.0-x86.msi",
                      (char*)NULL);
                _exit(127);
            }
            if (download_pid < 0 || waitpid(download_pid, &download_status, 0) != download_pid
                || !WIFEXITED(download_status) || WEXITSTATUS(download_status) != 0)
                _exit(1);
        }
        execl(wine, wine, "msiexec", "/i", cache, "/qn", (char*)NULL);
        _exit(127);
    }
    free(g_mono_marker_path);
    g_mono_marker_path = strdup(marker);
    g_mono_error[0] = 0;
    s = status(home, kind);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_i64(&w, g_mono_pid);
    ms_json_writer_key(&w, "downloading");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "status");
    ms_json_writer_raw(&w, s ? s : "{}");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(s);
    free(prefix); free(marker); free(cache_dir); free(cache); free(wine);
    return o;
}
char* ms_mono_reset_json(const char* home, const char* body, size_t len) {
    const char* kind = prefix_kind(body, len);
    char* s;
    if (g_mono_pid > 0 && mono_child_running()) {
        (void)kill(g_mono_pid, SIGTERM);
        (void)waitpid(g_mono_pid, NULL, 0);
        g_mono_pid = 0;
    }
    free(g_mono_marker_path);
    g_mono_marker_path = NULL;
    g_mono_error[0] = 0;
    s = status(home, kind);
    char* prefix_path = join(home, !strcmp(kind, "steam") ? "prefix-steam" : "bottles/gog-prefix/prefix");
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "killedProcesses");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_key(&w, "prefix");
    ms_json_writer_string(&w, prefix_path ? prefix_path : "");
    ms_json_writer_key(&w, "status");
    ms_json_writer_raw(&w, s ? s : "{}");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(prefix_path);
    free(s);
    return o;
}
