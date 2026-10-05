#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/sharp.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/logs.h"
#include "metalsharp_backend/steam_actions.h"
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
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

typedef struct sharp_running_app {
    char* id;
    pid_t pid;
    bool process_group;
    char* working_dir;
    char* runtime_dir;
    char* home;
    time_t started;
    struct sharp_running_app* next;
} sharp_running_app;

static sharp_running_app* g_sharp_running;
/* Exited apps waiting for their installs to be added to the library. */
static sharp_running_app* g_sharp_finished;
static pthread_mutex_t g_sharp_running_mutex = PTHREAD_MUTEX_INITIALIZER;

static char* failure(const char* s) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, s);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
static char* join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    int slash = x && a[x - 1] != '/' ? 1 : 0;
    char* p = malloc(x + y + slash + 1);
    if (p)
        snprintf(p, x + y + slash + 1, "%s%s%s", a, slash ? "/" : "", b);
    return p;
}
static char* parent_dir(const char* path) {
    const char* slash = path ? strrchr(path, '/') : NULL;
    if (!path || !path[0])
        return NULL;
    if (!slash)
        return strdup(".");
    if (slash == path)
        return strdup("/");
    return strndup(path, (size_t)(slash - path));
}
static bool mkdir_p(const char* path) {
    char* p = strdup(path);
    size_t i;
    if (!p)
        return false;
    for (i = 1; p[i]; i++)
        if (p[i] == '/') {
            p[i] = 0;
            mkdir(p, 0755);
            p[i] = '/';
        }
    if (mkdir(p, 0755) != 0 && errno != EEXIST) {
        free(p);
        return false;
    }
    free(p);
    return true;
}
static char* manifest_path(const char* home) {
    char *d = join(home, "sharp-library"), *p = d ? join(d, "library.json") : NULL;
    free(d);
    return p;
}
static char* read_text(const char* path) {
    FILE* f = fopen(path, "rb");
    long n;
    char* s;
    size_t got;
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f)
            fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    s = malloc((size_t)n + 1);
    if (!s) {
        fclose(f);
        return NULL;
    }
    got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = 0;
    return s;
}
static ms_json* load_array(const char* home) {
    char *p = manifest_path(home), *raw = p ? read_text(p) : NULL;
    char e[96];
    ms_json* j;
    if (!raw) {
        free(p);
        return ms_json_parse("[]", 2, e, sizeof(e));
    }
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    free(p);
    return j && ms_json_type_of(j) == MS_JSON_ARRAY ? j : (ms_json_free(j), ms_json_parse("[]", 2, e, sizeof(e)));
}
static bool write_text_atomic(const char* path, const char* raw) {
    char* temporary = NULL;
    FILE* f = NULL;
    int temporary_fd = -1;
    bool ok = false;
    if (!path || !raw)
        return false;
    size_t temporary_size = strlen(path) + 48;
    temporary = malloc(temporary_size);
    if (temporary)
        snprintf(temporary, temporary_size, "%s.tmp.XXXXXX", path);
    temporary_fd = temporary ? mkstemp(temporary) : -1;
    f = temporary_fd >= 0 ? fdopen(temporary_fd, "wb") : NULL;
    if (!f || fputs(raw, f) < 0) {
        if (f)
            fclose(f);
        else if (temporary_fd >= 0)
            close(temporary_fd);
        if (temporary)
            unlink(temporary);
        free(temporary);
        return false;
    }
    bool flushed = fflush(f) == 0 && fsync(fileno(f)) == 0;
    bool closed = fclose(f) == 0;
    ok = flushed && closed && chmod(temporary, 0600) == 0 && rename(temporary, path) == 0;
    if (!ok)
        unlink(temporary);
    free(temporary);
    return ok;
}
static bool save_array(const char* home, const ms_json* a) {
    char *d = join(home, "sharp-library"), *p, *raw;
    bool ok;
    if (!d || !mkdir_p(d)) {
        free(d);
        return false;
    }
    p = join(d, "library.json");
    raw = ms_json_stringify(a);
    ok = p && raw && write_text_atomic(p, raw);
    free(d);
    free(p);
    free(raw);
    return ok;
}
static char* field(const ms_json* j, const char* key, const char* fallback) {
    char* s = NULL;
    if (j)
        ms_json_as_string(ms_json_object_get(j, key), &s);
    if (!s)
        s = strdup(fallback ? fallback : "");
    return s;
}
static char* new_id(void) {
    char b[80];
    snprintf(b, sizeof(b), "sharp_%llu", (unsigned long long)time(NULL) * 1000ULL + (unsigned long long)getpid());
    return strdup(b);
}
static char* app_json_from(const char* id, const char* name, const char* exe, const char* dir, const char* bottle_id,
                           const char* engine, const char* adopted_from, time_t scanned_at) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, id);
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, name);
    ms_json_writer_key(&w, "exe_path");
    ms_json_writer_string(&w, exe);
    ms_json_writer_key(&w, "install_dir");
    ms_json_writer_string(&w, dir);
    ms_json_writer_key(&w, "cover");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "cover_position_x");
    ms_json_writer_u64(&w, 50);
    ms_json_writer_key(&w, "cover_position_y");
    ms_json_writer_u64(&w, 50);
    ms_json_writer_key(&w, "engine");
    ms_json_writer_string(&w, engine && engine[0] ? engine : "auto");
    ms_json_writer_key(&w, "launch_args");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "user_launch_args");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "bottle_id");
    if (bottle_id && bottle_id[0])
        ms_json_writer_string(&w, bottle_id);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed_at");
    ms_json_writer_u64(&w, (unsigned long long)time(NULL));
    ms_json_writer_key(&w, "size_bytes");
    ms_json_writer_u64(&w, 0);
    if (adopted_from) {
        ms_json_writer_key(&w, "adopted_from");
        ms_json_writer_string(&w, adopted_from);
        ms_json_writer_key(&w, "install_scan_done");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "install_scanned_at");
        ms_json_writer_u64(&w, (unsigned long long)scanned_at);
    }
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}

static char* app_json(const char* id, const char* name, const char* exe, const char* dir, const char* bottle_id) {
    return app_json_from(id, name, exe, dir, bottle_id, "auto", NULL, 0);
}

static void sharp_entry_free(sharp_running_app* entry) {
    free(entry->id);
    free(entry->working_dir);
    free(entry->runtime_dir);
    free(entry->home);
    free(entry);
}

/* Unlink an entry; keep it for the install scan when its run is over. */
static void sharp_retire_locked(sharp_running_app** link) {
    sharp_running_app* old = *link;
    *link = old->next;
    if (old->home) {
        old->next = g_sharp_finished;
        g_sharp_finished = old;
    } else
        sharp_entry_free(old);
}

static void sharp_forget_locked(const char* id) {
    sharp_running_app** current = &g_sharp_running;
    while (*current) {
        if (!strcmp((*current)->id, id)) {
            sharp_retire_locked(current);
            return;
        }
        current = &(*current)->next;
    }
}

static char* sharp_resolve_path(const char* path) {
    char* resolved = path && path[0] ? realpath(path, NULL) : NULL;
    return resolved ? resolved : (path && path[0] ? strdup(path) : NULL);
}

static void sharp_remember(const char* id, pid_t pid, bool process_group, const char* working_dir, const char* home) {
    sharp_running_app* entry;
    if (!id || !id[0] || pid <= 1)
        return;
    char* resolved_working_dir = sharp_resolve_path(working_dir);
    char* runtime_dir = join(home, "runtime/wine");
    char* resolved_runtime_dir = runtime_dir ? sharp_resolve_path(runtime_dir) : NULL;
    free(runtime_dir);
    pthread_mutex_lock(&g_sharp_running_mutex);
    sharp_forget_locked(id);
    entry = calloc(1, sizeof(*entry));
    if (entry) {
        entry->id = strdup(id);
        if (entry->id) {
            entry->pid = pid;
            entry->process_group = process_group;
            entry->working_dir = resolved_working_dir;
            entry->runtime_dir = resolved_runtime_dir;
            entry->home = home ? strdup(home) : NULL;
            entry->started = time(NULL);
            entry->next = g_sharp_running;
            g_sharp_running = entry;
            resolved_working_dir = NULL;
            resolved_runtime_dir = NULL;
        } else
            free(entry);
    }
    pthread_mutex_unlock(&g_sharp_running_mutex);
    free(resolved_working_dir);
    free(resolved_runtime_dir);
}

static bool sharp_process_active(pid_t pid) {
    int status;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
        return false;
    if (waited == 0)
        return true;
    if (waited < 0 && errno != ECHILD)
        return errno == EINTR || errno == EPERM;
    return kill(pid, 0) == 0 || errno == EPERM;
}

static bool sharp_group_active(pid_t pid) {
    if (kill(-pid, 0) == 0)
        return true;
    return errno != ESRCH;
}

static bool sharp_path_within(const char* path, const char* root) {
    size_t length = root ? strlen(root) : 0;
    return path && root && length > 0 && strncmp(path, root, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static bool sharp_process_cwd_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    struct proc_vnodepathinfo info;
    int bytes = proc_pidinfo((int)pid, PROC_PIDVNODEPATHINFO, 0, &info, (int)sizeof(info));
    return bytes == (int)sizeof(info) && sharp_path_within(info.pvi_cdir.vip_path, root);
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

static bool sharp_process_executable_within(pid_t pid, const char* root) {
#ifdef __APPLE__
    char executable[PROC_PIDPATHINFO_MAXSIZE];
    int bytes = proc_pidpath((int)pid, executable, sizeof(executable));
    return bytes > 0 && sharp_path_within(executable, root);
#else
    (void)pid;
    (void)root;
    return false;
#endif
}

static bool sharp_detached_process_matches(const sharp_running_app* entry, pid_t pid) {
    return entry && pid > 1 && pid != getpid() && pid != entry->pid && entry->working_dir && entry->runtime_dir &&
           ((sharp_process_cwd_within(pid, entry->working_dir) &&
             sharp_process_executable_within(pid, entry->runtime_dir)) ||
            sharp_process_executable_within(pid, entry->working_dir));
}

static bool sharp_signal_detached_processes(const sharp_running_app* entry, int signal_number, bool* found) {
#ifdef __APPLE__
    FILE* pipe = popen("/bin/ps axo pid=", "r");
    char line[64];
    bool ok = true;
    *found = false;
    if (!pipe)
        return false;
    while (fgets(line, sizeof(line), pipe)) {
        char* end;
        errno = 0;
        long value = strtol(line, &end, 10);
        if (errno || end == line || value <= 1 || value > INT_MAX)
            continue;
        pid_t pid = (pid_t)value;
        if (!sharp_detached_process_matches(entry, pid))
            continue;
        *found = true;
        if (signal_number != 0 && kill(pid, signal_number) != 0 && errno != ESRCH)
            ok = false;
    }
    if (pclose(pipe) != 0)
        ok = false;
    return ok;
#else
    (void)entry;
    (void)signal_number;
    *found = false;
    return true;
#endif
}

static bool sharp_entry_active(const sharp_running_app* entry) {
    bool detached_active = false;
    bool leader_active = sharp_process_active(entry->pid);
    bool group_active = entry->process_group && sharp_group_active(entry->pid);
    bool scan_ok = sharp_signal_detached_processes(entry, 0, &detached_active);
    return leader_active || group_active || detached_active || !scan_ok;
}

static bool sharp_signal_entry(const sharp_running_app* entry, int signal_number) {
    pid_t pid = entry->pid;
    int result = kill(entry->process_group ? -pid : pid, signal_number);
    if (entry->process_group && result != 0 && errno == ESRCH)
        result = kill(pid, signal_number);
    bool sent = result == 0 || errno == ESRCH;
    bool detached_found = false;
    bool detached_ok = sharp_signal_detached_processes(entry, signal_number, &detached_found);
    return sent && detached_ok;
}

static void sharp_prune_locked(void) {
    sharp_running_app** current = &g_sharp_running;
    while (*current) {
        if (!sharp_entry_active(*current))
            sharp_retire_locked(current);
        else
            current = &(*current)->next;
    }
}

static bool sharp_terminate_locked(sharp_running_app* entry) {
    bool sent = sharp_signal_entry(entry, SIGTERM);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000};
    for (int i = 0; i < 20; i++) {
        if (!sharp_entry_active(entry))
            break;
        nanosleep(&delay, NULL);
    }
    if (sharp_entry_active(entry) && !sharp_signal_entry(entry, SIGKILL))
        sent = false;
    int status;
    for (int i = 0; i < 20; i++) {
        pid_t waited = waitpid(entry->pid, &status, WNOHANG);
        if (waited == entry->pid || (waited < 0 && errno == ECHILD))
            break;
        nanosleep(&delay, NULL);
    }
    if (sharp_entry_active(entry))
        sent = false;
    return sent;
}

static void sharp_adopt_finished(void);

char* ms_sharp_running_json(void) {
    ms_json_writer writer;
    char* result;
    pthread_mutex_lock(&g_sharp_running_mutex);
    sharp_prune_locked();
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "running");
    ms_json_writer_array_begin(&writer);
    for (sharp_running_app* entry = g_sharp_running; entry; entry = entry->next) {
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "id");
        ms_json_writer_string(&writer, entry->id);
        ms_json_writer_key(&writer, "pid");
        ms_json_writer_u64(&writer, (unsigned long long)entry->pid);
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    pthread_mutex_unlock(&g_sharp_running_mutex);
    /* An app that just exited may have installed a launcher or game. */
    sharp_adopt_finished();
    return result;
}

char* ms_sharp_track_running_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request =
        ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2, error, sizeof(error));
    char* id = field(request, "id", "");
    long long pid_value = 0;
    ms_json* apps = load_array(home);
    char* working_dir = NULL;
    bool found = false;
    for (size_t i = 0; apps && i < ms_json_array_length(apps); i++) {
        const ms_json* app = ms_json_array_get(apps, i);
        char* app_id = field(app, "id", "");
        found = !strcmp(app_id, id);
        free(app_id);
        if (found) {
            working_dir = field(app, "install_dir", "");
            break;
        }
    }
    bool valid = request && id[0] && found && working_dir && working_dir[0] &&
                 ms_json_as_i64(ms_json_object_get(request, "pid"), &pid_value) && pid_value > 1 &&
                 pid_value <= INT_MAX && (kill((pid_t)pid_value, 0) == 0 || errno == EPERM);
    if (valid)
        sharp_remember(id, (pid_t)pid_value, false, working_dir, home);
    free(id);
    free(working_dir);
    ms_json_free(apps);
    ms_json_free(request);
    if (!valid)
        return failure("Sharp Library application or running process not found");
    return strdup("{\"ok\":true}");
}

char* ms_sharp_stop_json(const unsigned char* body, size_t length, int* status) {
    char error[96];
    ms_json* request =
        ms_json_parse((const char*)(body ? body : (const unsigned char*)"{}"), body ? length : 2, error, sizeof(error));
    char* id = field(request, "id", "");
    bool stopped = false;
    if (status)
        *status = 400;
    if (!request || !id[0]) {
        free(id);
        ms_json_free(request);
        return failure("Sharp Library application id required");
    }
    pthread_mutex_lock(&g_sharp_running_mutex);
    sharp_prune_locked();
    for (sharp_running_app* entry = g_sharp_running; entry; entry = entry->next) {
        if (!strcmp(entry->id, id)) {
            stopped = sharp_terminate_locked(entry);
            if (stopped)
                sharp_forget_locked(id);
            break;
        }
    }
    pthread_mutex_unlock(&g_sharp_running_mutex);
    free(id);
    ms_json_free(request);
    if (!stopped)
        return failure("Sharp Library application is not running or could not be stopped");
    if (status)
        *status = 200;
    return strdup("{\"ok\":true}");
}

char* ms_sharp_stop_all_json(int* status) {
    size_t stopped = 0;
    bool failed = false;
    pthread_mutex_lock(&g_sharp_running_mutex);
    sharp_running_app** current = &g_sharp_running;
    while (*current) {
        sharp_running_app* entry = *current;
        if (sharp_terminate_locked(entry))
            stopped++;
        else {
            failed = true;
            current = &entry->next;
            continue;
        }
        *current = entry->next;
        sharp_entry_free(entry);
    }
    pthread_mutex_unlock(&g_sharp_running_mutex);
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, !failed);
    ms_json_writer_key(&writer, "stopped");
    ms_json_writer_u64(&writer, stopped);
    if (failed) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, "one or more Sharp Library applications could not be stopped");
    }
    ms_json_writer_object_end(&writer);
    if (status)
        *status = failed ? 500 : 200;
    return ms_json_writer_take(&writer);
}

static bool contains_ci(const char* text, const char* needle) {
    size_t n = needle ? strlen(needle) : 0;
    if (!text || n == 0)
        return false;
    for (; *text; text++)
        if (!strncasecmp(text, needle, n))
            return true;
    return false;
}

static bool is_moonscraper_installer(const char* path) {
    const char* name = path ? strrchr(path, '/') : NULL;
    name = name ? name + 1 : path;
    return name && !strncasecmp(name, "msce.", 5) && contains_ci(name, ".installer.") && strlen(name) > 4 &&
           !strcasecmp(name + strlen(name) - 4, ".exe");
}

static const char* innoextract_binary(void) {
    static const char* prefixes[] = {"/opt/homebrew/bin/", "/usr/local/bin/", "/usr/bin/"};
    static char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(path, sizeof(path), "%sinnoextract", prefixes[i]);
        if (access(path, X_OK) == 0)
            return path;
    }
    return NULL;
}

static bool remove_tree(const char* path) {
    struct stat st;
    if (!path || lstat(path, &st) != 0)
        return !path || errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    DIR* dir = opendir(path);
    struct dirent* entry;
    if (!dir)
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = join(path, entry->d_name);
        if (!child || !remove_tree(child)) {
            free(child);
            closedir(dir);
            return false;
        }
        free(child);
    }
    closedir(dir);
    return rmdir(path) == 0;
}

static bool write_moonscraper_bottle(const char* home, const char* source, const char* install_dir,
                                     const char* executable, const char* prefix) {
    const char* id = "installer_moonscraper";
    char *root = join(home, "bottles"), *dir = root ? join(root, id) : NULL,
         *path = dir ? join(dir, "bottle.json") : NULL;
    FILE* file;
    ms_json_writer writer;
    char* raw;
    char stamp[32];
    bool ok = false;
    if (!root || !dir || !path || !mkdir_p(dir))
        goto done;
    snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "id");
    ms_json_writer_string(&writer, id);
    ms_json_writer_key(&writer, "name");
    ms_json_writer_string(&writer, "MoonScraper Chart Editor");
    ms_json_writer_key(&writer, "custom_name");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "bottle_type");
    ms_json_writer_string(&writer, "installer");
    ms_json_writer_key(&writer, "steam_app_id");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "prefix_path");
    ms_json_writer_string(&writer, prefix);
    ms_json_writer_key(&writer, "arch");
    ms_json_writer_string(&writer, "win64");
    ms_json_writer_key(&writer, "runtime_profile");
    ms_json_writer_string(&writer, "game_install");
    ms_json_writer_key(&writer, "preferred_pipeline");
    ms_json_writer_string(&writer, "wine_bare");
    ms_json_writer_key(&writer, "installed_components");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "source_installer_path");
    ms_json_writer_string(&writer, source);
    ms_json_writer_key(&writer, "installer_kind");
    ms_json_writer_string(&writer, "inno");
    ms_json_writer_key(&writer, "game_install_path");
    ms_json_writer_string(&writer, install_dir);
    ms_json_writer_key(&writer, "runtime_assets");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "installed_app_detections");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "name");
    ms_json_writer_string(&writer, "MoonScraper Chart Editor");
    ms_json_writer_key(&writer, "exe_path");
    ms_json_writer_string(&writer, executable);
    ms_json_writer_key(&writer, "source");
    ms_json_writer_string(&writer, "native_inno_extract");
    ms_json_writer_object_end(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "health");
    ms_json_writer_string(&writer, "ready");
    ms_json_writer_key(&writer, "last_launch_log");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "last_launch_pid");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "last_launch_status");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "last_launch_finished_at");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "created_at");
    ms_json_writer_string(&writer, stamp);
    ms_json_writer_key(&writer, "updated_at");
    ms_json_writer_string(&writer, stamp);
    ms_json_writer_object_end(&writer);
    raw = ms_json_writer_take(&writer);
    file = raw ? fopen(path, "wb") : NULL;
    if (file && fputs(raw, file) >= 0)
        ok = true;
    if (file)
        fclose(file);
    free(raw);
done:
    free(root);
    free(dir);
    free(path);
    return ok;
}

/* Installers and launchers usually offer drive letters, not folders, so an
 * external disk reachable only as Z:\Volumes\<name> is unusable in their
 * pickers. Give every mounted, writable external volume its own letter in the
 * prefix (Y: downwards, like the Ubisoft prefix's Y:). C: and Z: are never
 * touched: MetalSharp and Steam rely on Z:\Volumes\... paths. */
static void map_external_volume_letters(const char* prefix) {
    struct stat root_info;
    DIR* volumes;
    struct dirent* entry;
    char dosdevices[PATH_MAX];
    if (!prefix || !prefix[0] || stat("/", &root_info) != 0)
        return;
    snprintf(dosdevices, sizeof(dosdevices), "%s/dosdevices", prefix);
    if (access(dosdevices, F_OK) != 0 || !(volumes = opendir("/Volumes")))
        return;
    while ((entry = readdir(volumes)) != NULL) {
        char volume[PATH_MAX];
        struct stat link_info, volume_info;
        struct statfs fs;
        bool mapped = false;
        if (entry->d_name[0] == '.')
            continue;
        snprintf(volume, sizeof(volume), "/Volumes/%s", entry->d_name);
        /* The boot volume appears as a symlink to "/"; disk images mount read-only;
         * system volumes (Recovery, Preboot, ...) are mounted hidden from Finder. */
        if (lstat(volume, &link_info) != 0 || S_ISLNK(link_info.st_mode) || stat(volume, &volume_info) != 0 ||
            !S_ISDIR(volume_info.st_mode) || volume_info.st_dev == root_info.st_dev || statfs(volume, &fs) != 0 ||
            (fs.f_flags & (MNT_RDONLY | MNT_DONTBROWSE)))
            continue;
        for (char letter = 'd'; letter <= 'y' && !mapped; letter++) {
            char drive[PATH_MAX], target[PATH_MAX];
            ssize_t length;
            snprintf(drive, sizeof(drive), "%s/%c:", dosdevices, letter);
            length = readlink(drive, target, sizeof(target) - 1);
            if (length > 0) {
                char resolved[PATH_MAX];
                target[length] = '\0';
                mapped = realpath(drive, resolved) && !strcmp(resolved, volume);
            }
        }
        for (char letter = 'y'; letter >= 'd' && !mapped; letter--) {
            char drive[PATH_MAX];
            struct stat existing;
            snprintf(drive, sizeof(drive), "%s/%c:", dosdevices, letter);
            if (lstat(drive, &existing) == 0)
                continue;
            mapped = symlink(volume, drive) == 0;
        }
    }
    closedir(volumes);
}

static char* bottle_prefix(const char* home, const char* bottle_id) {
    char *root, *dir, *path, *raw, *prefix = NULL;
    char error[96];
    ms_json* object;
    if (!bottle_id || !bottle_id[0])
        return NULL;
    root = join(home, "bottles");
    dir = root ? join(root, bottle_id) : NULL;
    path = dir ? join(dir, "bottle.json") : NULL;
    raw = path ? read_text(path) : NULL;
    object = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    if (object && ms_json_type_of(object) == MS_JSON_OBJECT)
        ms_json_as_string(ms_json_object_get(object, "prefix_path"), &prefix);
    ms_json_free(object);
    free(raw);
    free(root);
    free(dir);
    free(path);
    return prefix;
}

static char* extract_moonscraper(const char* home, const char* source, char** install_dir_out, char** bottle_id_out) {
    const char* extractor = innoextract_binary();
    const char* bottle_id = "installer_moonscraper";
    char *bottle_dir = join(home, "bottles/installer_moonscraper"), *prefix = NULL, *staging = NULL, *app_dir = NULL,
         *executable = NULL, *install_dir = NULL, *install_parent = NULL, *log_dir = NULL, *log_path = NULL;
    int status = 0;
    pid_t pid;
    FILE* log = NULL;
    if (!extractor || !source || access(source, R_OK) != 0 || !bottle_dir)
        goto fail;
    prefix = join(bottle_dir, "prefix");
    staging = join(bottle_dir, "native-inno-extract.tmp");
    app_dir = staging ? join(staging, "app") : NULL;
    executable = app_dir ? join(app_dir, "Moonscraper Chart Editor.exe") : NULL;
    install_dir = prefix ? join(prefix, "drive_c/Program Files/Moonscraper Chart Editor") : NULL;
    install_parent = prefix ? join(prefix, "drive_c/Program Files") : NULL;
    log_dir = join(bottle_dir, "logs");
    log_path = log_dir ? join(log_dir, "native-inno-extract.log") : NULL;
    if (!prefix || !staging || !app_dir || !executable || !install_dir || !install_parent || !log_dir || !log_path ||
        !mkdir_p(prefix) || !mkdir_p(log_dir) || !remove_tree(staging) || !mkdir_p(staging))
        goto fail;
    log = fopen(log_path, "wb");
    if (!log)
        goto fail;
    fprintf(log, "installer_kind=inno\nstrategy=native_inno_extract\nsource=%s\ndestination=%s\n", source, install_dir);
    fflush(log);
    pid = fork();
    if (pid == 0) {
        char* const args[] = {(char*)extractor, "--extract", "--silent", "--output-dir", staging, (char*)source, NULL};
        dup2(fileno(log), STDOUT_FILENO);
        dup2(fileno(log), STDERR_FILENO);
        execv(extractor, args);
        _exit(127);
    }
    if (pid <= 0)
        goto fail;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || access(executable, R_OK) != 0)
        goto fail;
    if (!remove_tree(install_dir) || !mkdir_p(install_parent) || rename(app_dir, install_dir) != 0)
        goto fail;
    {
        char* installed_executable = join(install_dir, "Moonscraper Chart Editor.exe");
        bool bottle_ok =
            installed_executable && write_moonscraper_bottle(home, source, install_dir, installed_executable, prefix);
        free(installed_executable);
        if (!bottle_ok)
            goto fail;
    }
    fprintf(log, "status=complete\n");
    fclose(log);
    log = NULL;
    remove_tree(staging);
    *install_dir_out = install_dir;
    *bottle_id_out = strdup(bottle_id);
    free(bottle_dir);
    free(prefix);
    free(staging);
    free(app_dir);
    free(executable);
    free(log_dir);
    free(log_path);
    free(install_parent);
    return *install_dir_out && *bottle_id_out ? strdup("Moonscraper Chart Editor") : NULL;
fail:
    if (log)
        fclose(log);
    free(bottle_dir);
    free(prefix);
    free(staging);
    free(app_dir);
    free(executable);
    free(install_dir);
    free(install_parent);
    free(log_dir);
    free(log_path);
    return NULL;
}

static bool has_id(const ms_json* a, const char* id) {
    size_t i;
    for (i = 0; i < ms_json_array_length(a); i++) {
        char* s = field(ms_json_array_get(a, i), "id", "");
        bool yes = !strcmp(s, id);
        free(s);
        if (yes)
            return true;
    }
    return false;
}
static bool append_raw(const char* home, const char* raw) {
    ms_json *a = load_array(home), *newa;
    ms_json_writer w;
    char* serialized;
    char e[64];
    bool ok;
    if (!a)
        return false;
    ms_json_writer_init(&w);
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < ms_json_array_length(a); i++) {
        char* old = ms_json_stringify(ms_json_array_get(a, i));
        ms_json_writer_raw(&w, old ? old : "{}");
        free(old);
    }
    ms_json_writer_raw(&w, raw);
    ms_json_writer_array_end(&w);
    serialized = ms_json_writer_take(&w);
    newa = serialized ? ms_json_parse(serialized, strlen(serialized), e, sizeof(e)) : NULL;
    ok = newa && save_array(home, newa);
    ms_json_free(newa);
    ms_json_free(a);
    free(serialized);
    return ok;
}
static bool copy_cover(const char* home, const char* id, const char* src, char* filename, size_t filename_size) {
    struct stat st;
    FILE *in, *out;
    char *dir, *dst;
    const char* ext = strrchr(src, '.');
    unsigned char buf[8192];
    size_t n;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 5 * 1024 * 1024)
        return false;
    if (!ext || !ext[1])
        ext = ".jpg";
    snprintf(filename, filename_size, "%s%s", id, ext);
    dir = join(home, "sharp-library");
    if (!dir || !mkdir_p(dir)) {
        free(dir);
        return false;
    }
    dst = join(dir, filename);
    free(dir);
    in = fopen(src, "rb");
    out = dst ? fopen(dst, "wb") : NULL;
    if (!in || !out) {
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        free(dst);
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0 && fwrite(buf, 1, n, out) == n) {
    }
    bool ok = ferror(in) == 0 && ferror(out) == 0;
    fclose(in);
    fclose(out);
    free(dst);
    return ok;
}
static bool replace_field(const char* home, const char* id, const char* key, const char* value) {
    ms_json *a = load_array(home), *newa;
    ms_json_writer w;
    char* serial;
    char e[64];
    bool found = false, ok = false;
    if (!a)
        return false;
    ms_json_writer_init(&w);
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < ms_json_array_length(a); i++) {
        const ms_json* item = ms_json_array_get(a, i);
        char* item_id = field(item, "id", "");
        if (!strcmp(item_id, id)) {
            found = true;
            ms_json_writer_object_begin(&w);
            for (size_t k = 0; k < ms_json_object_length(item); k++) {
                const char* name = ms_json_object_key_at(item, k);
                ms_json_writer_key(&w, name);
                if (!strcmp(name, key))
                    ms_json_writer_raw(&w, value);
                else {
                    char* old = ms_json_stringify(ms_json_object_value_at(item, k));
                    ms_json_writer_raw(&w, old ? old : "null");
                    free(old);
                }
            }
            ms_json_writer_object_end(&w);
        } else {
            char* old = ms_json_stringify(item);
            ms_json_writer_raw(&w, old ? old : "{}");
            free(old);
        }
        free(item_id);
    }
    ms_json_writer_array_end(&w);
    serial = ms_json_writer_take(&w);
    newa = serial ? ms_json_parse(serial, strlen(serial), e, sizeof(e)) : NULL;
    ok = found && newa && save_array(home, newa);
    free(serial);
    ms_json_free(newa);
    ms_json_free(a);
    return ok;
}
/* ---- Programs installed by a Sharp app (setup -> launcher -> game) ----
 * When a Sharp app exits, whatever it installed into its prefix while it ran is
 * added to the library on its own: new Windows uninstall entries (DisplayIcon)
 * and new Start Menu/Desktop shortcuts. Launchers that a setup installed also
 * get their new game folders picked up, since most don't register the game. */
typedef struct {
    char* name;
    char* exe;
    off_t size;
} sharp_candidate;

typedef struct {
    sharp_candidate* items;
    size_t count;
    size_t capacity;
} sharp_candidates;

static void candidates_free(sharp_candidates* list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].exe);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* Installers extract with the archive's mtimes, so "new" means created. */
static bool created_since(const struct stat* info, time_t since) {
    return info->st_birthtimespec.tv_sec >= since || info->st_mtime >= since;
}

static bool adoptable_exe(const char* path, off_t* size) {
    static const char* const skipped[] = {"unins",   "crash", "report",  "redist",  "dxsetup", "update",   "helper",
                                          "elevate", "setup", "install", "msiexec", "dotnet",  "vc_redist"};
    const char* base = path ? strrchr(path, '/') : NULL;
    char lower[256];
    size_t length;
    struct stat info;
    base = base ? base + 1 : path;
    if (!base || stat(path, &info) != 0 || !S_ISREG(info.st_mode) || contains_ci(path, "/drive_c/windows/") ||
        contains_ci(path, "/steamapps/"))
        return false;
    length = strlen(base);
    if (length < 5 || length >= sizeof(lower) || strcasecmp(base + length - 4, ".exe"))
        return false;
    for (size_t i = 0; i <= length; i++)
        lower[i] = (char)tolower((unsigned char)base[i]);
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++)
        if (strstr(lower, skipped[i]))
            return false;
    if (size)
        *size = info.st_size;
    return true;
}

static void candidates_add(sharp_candidates* list, const char* name, const char* exe) {
    char* resolved;
    off_t size = 0;
    if (!exe || !adoptable_exe(exe, &size) || !(resolved = realpath(exe, NULL)))
        return;
    for (size_t i = 0; i < list->count; i++)
        if (!strcmp(list->items[i].exe, resolved)) {
            free(resolved);
            return;
        }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 8;
        sharp_candidate* grown = realloc(list->items, capacity * sizeof(*grown));
        if (!grown) {
            free(resolved);
            return;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    sharp_candidate* item = &list->items[list->count++];
    if (name && name[0])
        item->name = strdup(name);
    else {
        const char* base = strrchr(resolved, '/');
        item->name = strdup(base ? base + 1 : resolved);
        char* dot = item->name ? strrchr(item->name, '.') : NULL;
        if (dot)
            *dot = '\0';
    }
    item->exe = resolved;
    item->size = size;
}

/* "C:\dir\app.exe" -> <prefix>/drive_c/dir/app.exe; other letters via dosdevices. */
static char* windows_to_unix(const char* prefix, const char* win) {
    char link[PATH_MAX], base[PATH_MAX];
    char* relative;
    char* result;
    if (!win || strlen(win) < 4 || !isalpha((unsigned char)win[0]) || win[1] != ':' ||
        (win[2] != '\\' && win[2] != '/'))
        return NULL;
    snprintf(link, sizeof(link), "%s/dosdevices/%c:", prefix, (char)tolower((unsigned char)win[0]));
    if (!realpath(link, base))
        return NULL;
    relative = strdup(win + 3);
    if (!relative)
        return NULL;
    for (char* p = relative; *p; p++)
        if (*p == '\\')
            *p = '/';
    result = join(base, relative);
    free(relative);
    return result;
}

/* A .reg string value with its \\ and \" escapes undone, or NULL. */
static char* reg_value(const char* line, const char* name) {
    size_t name_length = strlen(name);
    const char* start;
    const char* end;
    char *value, *out;
    if (line[0] != '"' || strncmp(line + 1, name, name_length) || strncmp(line + 1 + name_length, "\"=\"", 3))
        return NULL;
    start = line + name_length + 4;
    end = strrchr(start, '"');
    if (!end || !(value = strndup(start, (size_t)(end - start))))
        return NULL;
    out = value;
    for (const char* in = value; *in; in++) {
        if (*in == '\\' && (in[1] == '\\' || in[1] == '"'))
            in++;
        *out++ = *in;
    }
    *out = '\0';
    return value;
}

static void adopt_uninstall_entry(const char* prefix, const char* name, char* icon, sharp_candidates* out) {
    char* exe = icon;
    char* path;
    if (!icon || !icon[0] || contains_ci(icon, "Package Cache") || (name && !strcmp(name, "Steam")))
        return;
    if (exe[0] == '"') {
        char* quote = strchr(++exe, '"');
        if (quote)
            *quote = '\0';
    }
    char* comma = strrchr(exe, ',');
    if (comma && !contains_ci(comma, ".exe"))
        *comma = '\0'; /* ",0" icon index */
    path = windows_to_unix(prefix, exe);
    candidates_add(out, name, path);
    free(path);
}

/* Wine stamps every registry key with its last write: "[key] <unix time>". */
static void scan_uninstall_registry(const char* prefix, time_t since, sharp_candidates* out) {
    static const char* const files[] = {"system.reg", "user.reg"};
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        char* path = join(prefix, files[f]);
        char* text = path ? read_text(path) : NULL;
        char *name = NULL, *icon = NULL;
        bool in_section = false;
        free(path);
        for (char* line = text; line && *line;) {
            char* next = strchr(line, '\n');
            if (next)
                *next = '\0';
            size_t length = strlen(line);
            if (length && line[length - 1] == '\r')
                line[length - 1] = '\0';
            if (line[0] == '[') {
                if (in_section)
                    adopt_uninstall_entry(prefix, name, icon, out);
                free(name);
                free(icon);
                name = icon = NULL;
                const char* key_end = strstr(line, "] ");
                const char* marker = strstr(line, "\\\\Uninstall\\\\");
                in_section = false;
                if (key_end && marker && marker < key_end) {
                    const char* key = marker + strlen("\\\\Uninstall\\\\");
                    in_section = strtoll(key_end + 2, NULL, 10) >= (long long)since && strncmp(key, "Steam App", 9);
                }
            } else if (in_section) {
                char* value;
                if ((value = reg_value(line, "DisplayName"))) {
                    free(name);
                    name = value;
                } else if ((value = reg_value(line, "DisplayIcon"))) {
                    free(icon);
                    icon = value;
                }
            }
            line = next ? next + 1 : NULL;
        }
        if (in_section)
            adopt_uninstall_entry(prefix, name, icon, out);
        free(name);
        free(icon);
        free(text);
    }
}

/* Shell links store the target as an ANSI and/or UTF-16LE path; take the first
 * "X:\...\name.exe" in either encoding. */
static char* shortcut_target(const char* path) {
    FILE* file = fopen(path, "rb");
    unsigned char buffer[16384];
    char text[sizeof(buffer) + 1];
    size_t length;
    char* found = NULL;
    if (!file)
        return NULL;
    length = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    for (int utf16 = 0; utf16 < 2 && !found; utf16++) {
        size_t text_length = 0;
        for (size_t i = 0; i < length; i++) {
            if (utf16 && buffer[i] == 0)
                continue;
            text[text_length++] = buffer[i] >= 32 && buffer[i] < 127 ? (char)buffer[i] : '\n';
        }
        text[text_length] = '\0';
        for (char* p = text; *p && !found; p++) {
            if (!isalpha((unsigned char)p[0]) || p[1] != ':' || p[2] != '\\')
                continue;
            char* end = strchr(p, '\n');
            char* exe = strcasestr(p, ".exe");
            if (exe && (!end || exe < end))
                found = strndup(p, (size_t)(exe + 4 - p));
        }
    }
    return found;
}

static void scan_shortcuts(const char* prefix, const char* directory, time_t since, unsigned depth,
                           sharp_candidates* out) {
    DIR* dir;
    struct dirent* entry;
    if (depth > 3 || !(dir = opendir(directory)))
        return;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        struct stat info;
        char* path;
        if (entry->d_name[0] == '.' || !(path = join(directory, entry->d_name)))
            continue;
        if (stat(path, &info) == 0) {
            if (S_ISDIR(info.st_mode))
                scan_shortcuts(prefix, path, since, depth + 1, out);
            else if (S_ISREG(info.st_mode) && created_since(&info, since) && length > 4 &&
                     !strcasecmp(entry->d_name + length - 4, ".lnk")) {
                char* target = shortcut_target(path);
                char* unix_path = target ? windows_to_unix(prefix, target) : NULL;
                char* name = strndup(entry->d_name, length - 4);
                candidates_add(out, name, unix_path);
                free(name);
                free(unix_path);
                free(target);
            }
        }
        free(path);
    }
    closedir(dir);
}

/* Biggest new executable per folder, skipping folders inside one that already
 * produced a game (Unreal's Binaries/Win64 shipping exe sits under the stub).
 * Past the first levels only new folders are entered ("Games/Foo/Publisher/Foo"). */
static void scan_new_game_folders(const char* directory, time_t since, unsigned depth, sharp_candidates* out) {
    DIR* dir;
    struct dirent* entry;
    char* best = NULL;
    off_t best_size = -1;
    if (depth > 6 || !(dir = opendir(directory)))
        return;
    char** subdirs = NULL;
    size_t subdir_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        struct stat info;
        char* path;
        off_t size;
        if (entry->d_name[0] == '.' || !strcasecmp(entry->d_name, "steamapps") ||
            !strcmp(entry->d_name, "node_modules") || !(path = join(directory, entry->d_name)))
            continue;
        if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
            if (depth >= 2 && info.st_birthtimespec.tv_sec < since) {
                free(path);
                continue;
            }
            char** grown = realloc(subdirs, (subdir_count + 1) * sizeof(*subdirs));
            if (grown) {
                subdirs = grown;
                subdirs[subdir_count++] = path;
                path = NULL;
            }
        } else if (lstat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_birthtimespec.tv_sec >= since &&
                   adoptable_exe(path, &size) && size > best_size) {
            free(best);
            best = path;
            best_size = size;
            path = NULL;
        }
        free(path);
    }
    closedir(dir);
    if (best)
        candidates_add(out, NULL, best);
    for (size_t i = 0; i < subdir_count; i++) {
        if (!best)
            scan_new_game_folders(subdirs[i], since, depth + 1, out);
        free(subdirs[i]);
    }
    free(subdirs);
    free(best);
}

static void collect_installed_programs(const char* prefix, const ms_json* parent, time_t since, sharp_candidates* out) {
    char path[PATH_MAX];
    DIR* dir;
    struct dirent* entry;
    scan_uninstall_registry(prefix, since, out);
    snprintf(path, sizeof(path), "%s/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs", prefix);
    scan_shortcuts(prefix, path, since, 0, out);
    snprintf(path, sizeof(path), "%s/drive_c/users", prefix);
    if ((dir = opendir(path)) != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/drive_c/users/%s/AppData/Roaming/Microsoft/Windows/Start Menu/Programs",
                     prefix, entry->d_name);
            scan_shortcuts(prefix, path, since, 0, out);
            snprintf(path, sizeof(path), "%s/drive_c/users/%s/Desktop", prefix, entry->d_name);
            scan_shortcuts(prefix, path, since, 3, out);
        }
        closedir(dir);
    }
    char* adopted_from = field(parent, "adopted_from", "");
    if (adopted_from[0]) {
        /* A launcher: games land beside it, in Program Files, or on a mapped drive. */
        char* install_dir = field(parent, "install_dir", "");
        char* beside = install_dir[0] ? parent_dir(install_dir) : NULL;
        if (beside)
            scan_new_game_folders(beside, since, 1, out);
        snprintf(path, sizeof(path), "%s/drive_c/Program Files", prefix);
        scan_new_game_folders(path, since, 1, out);
        snprintf(path, sizeof(path), "%s/drive_c/Program Files (x86)", prefix);
        scan_new_game_folders(path, since, 1, out);
        for (char letter = 'd'; letter <= 'y'; letter++) {
            char link[PATH_MAX], volume[PATH_MAX];
            snprintf(link, sizeof(link), "%s/dosdevices/%c:", prefix, letter);
            if (realpath(link, volume) && strcmp(volume, "/") && !strncmp(volume, "/Volumes/", 9))
                scan_new_game_folders(volume, since, 0, out);
        }
        free(beside);
        free(install_dir);
    }
    free(adopted_from);
}

static bool library_has_exe(const ms_json* apps, const char* exe) {
    for (size_t i = 0; i < ms_json_array_length(apps); i++) {
        char* path = field(ms_json_array_get(apps, i), "exe_path", "");
        char* resolved = path[0] ? realpath(path, NULL) : NULL;
        bool same = !strcmp(resolved ? resolved : path, exe);
        free(resolved);
        free(path);
        if (same)
            return true;
    }
    return false;
}

/* Like replace_field, but adds the key when the app doesn't have it yet. */
static bool set_field(const char* home, const char* id, const char* key, const char* raw_value) {
    ms_json *apps = load_array(home), *rewritten = NULL;
    ms_json_writer w;
    char error[64];
    bool found = false, ok;
    if (!apps)
        return false;
    ms_json_writer_init(&w);
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < ms_json_array_length(apps); i++) {
        const ms_json* item = ms_json_array_get(apps, i);
        char* item_id = field(item, "id", "");
        if (!strcmp(item_id, id)) {
            found = true;
            ms_json_writer_object_begin(&w);
            for (size_t k = 0; k < ms_json_object_length(item); k++) {
                const char* name = ms_json_object_key_at(item, k);
                if (!strcmp(name, key))
                    continue;
                char* old = ms_json_stringify(ms_json_object_value_at(item, k));
                ms_json_writer_key(&w, name);
                ms_json_writer_raw(&w, old ? old : "null");
                free(old);
            }
            ms_json_writer_key(&w, key);
            ms_json_writer_raw(&w, raw_value);
            ms_json_writer_object_end(&w);
        } else {
            char* old = ms_json_stringify(item);
            ms_json_writer_raw(&w, old ? old : "{}");
            free(old);
        }
        free(item_id);
    }
    ms_json_writer_array_end(&w);
    char* raw = ms_json_writer_take(&w);
    rewritten = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    ok = found && rewritten && save_array(home, rewritten);
    ms_json_free(rewritten);
    ms_json_free(apps);
    free(raw);
    return ok;
}

static bool installer_like(const char* exe) {
    const char* base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    return contains_ci(base, "setup") || contains_ci(base, "install");
}

/* Stage the app's graphics route DLLs next to its exe, the way Steam games
 * are staged; a route change first removes the previous route's DLLs. Setup
 * and installer exes are left alone (they usually sit in ~/Downloads). */
static void sharp_stage_engine(const char* home, const char* engine, const char* exe, const char* install_dir) {
    if (!home || !exe || !exe[0] || installer_like(exe) || access(exe, F_OK) != 0)
        return;
    char* game_dir = install_dir && install_dir[0] ? strdup(install_dir) : parent_dir(exe);
    if (!game_dir)
        return;
    if (!engine || !engine[0] || !strcmp(engine, "auto") || !strcmp(engine, "wine_bare"))
        ms_steam_cleanup_route_dlls(home, engine ? engine : "auto", game_dir, exe);
    else
        (void)ms_steam_stage_route_for_executable(home, engine, game_dir, exe);
    free(game_dir);
}

static void adopt_installed_programs(const char* home, const char* parent_id, time_t since, unsigned depth) {
    ms_json* apps = load_array(home);
    const ms_json* parent = NULL;
    sharp_candidates found = {0};
    char *bottle_id = NULL, *engine = NULL, *prefix = NULL;
    char** adopted = NULL;
    size_t adopted_count = 0;
    for (size_t i = 0; apps && i < ms_json_array_length(apps) && !parent; i++) {
        char* id = field(ms_json_array_get(apps, i), "id", "");
        if (!strcmp(id, parent_id))
            parent = ms_json_array_get(apps, i);
        free(id);
    }
    if (!parent)
        goto done;
    bottle_id = field(parent, "bottle_id", "");
    engine = field(parent, "engine", "auto");
    /* Bottle-less apps run in the Steam prefix (metalsharp-wine's default). */
    prefix = bottle_id[0] ? bottle_prefix(home, bottle_id) : join(home, "prefix-steam");
    if (!prefix)
        goto done;
    /* Installers and launchers often start each other (setup runs the launcher
     * when it finishes) outside our tracking, so they look back to their last
     * scan rather than only to this run's start. */
    char* own_exe = field(parent, "exe_path", "");
    char* adopted_from = field(parent, "adopted_from", "");
    long long scanned_at = 0;
    if ((adopted_from[0] || installer_like(own_exe)) &&
        ms_json_as_i64(ms_json_object_get(parent, "install_scanned_at"), &scanned_at) && scanned_at > 0 &&
        (time_t)scanned_at < since)
        since = (time_t)scanned_at;
    free(adopted_from);
    free(own_exe);
    /* Registry stamps and file times are whole seconds; allow a little slack. */
    time_t scan_started = time(NULL);
    collect_installed_programs(prefix, parent, since - 2, &found);
    for (size_t i = 0; i < found.count; i++) {
        if (library_has_exe(apps, found.items[i].exe))
            continue;
        unsigned long long base = (unsigned long long)time(NULL) * 1000ULL;
        char id[80];
        for (unsigned long long n = 0;; n++) {
            snprintf(id, sizeof(id), "sharp_%llu", base + n);
            if (!has_id(apps, id))
                break;
        }
        /* "StellaSora" from the uninstall entry is the launcher; keep the game's name free. */
        const char* exe_name = strrchr(found.items[i].exe, '/');
        char* name = NULL;
        if (contains_ci(exe_name ? exe_name : found.items[i].exe, "launcher") &&
            !contains_ci(found.items[i].name, "launcher") && asprintf(&name, "%s Launcher", found.items[i].name) < 0)
            name = NULL;
        char* dir = parent_dir(found.items[i].exe);
        char* raw = dir ? app_json_from(id, name ? name : found.items[i].name, found.items[i].exe, dir,
                                        bottle_id[0] ? bottle_id : NULL, engine, parent_id, since)
                        : NULL;
        if (raw && append_raw(home, raw)) {
            char message[PATH_MAX + 256];
            snprintf(message, sizeof(message), "Sharp Library added %s (%s), installed by %s",
                     name ? name : found.items[i].name, found.items[i].exe, parent_id);
            ms_log_event(home, message);
            sharp_stage_engine(home, engine, found.items[i].exe, dir);
            char** grown = realloc(adopted, (adopted_count + 1) * sizeof(*adopted));
            if (grown && (grown[adopted_count] = strdup(id)) != NULL)
                adopted_count++;
            if (grown)
                adopted = grown;
            ms_json_free(apps);
            apps = load_array(home);
            if (!apps) {
                free(raw);
                free(dir);
                free(name);
                break;
            }
        }
        free(raw);
        free(dir);
        free(name);
    }
    {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%lld", (long long)scan_started);
        (void)set_field(home, parent_id, "install_scanned_at", stamp);
    }
    /* A launcher the setup started may already have installed its game. */
    for (size_t i = 0; i < adopted_count; i++)
        if (depth < 2)
            adopt_installed_programs(home, adopted[i], since, depth + 1);
done:
    for (size_t i = 0; i < adopted_count; i++)
        free(adopted[i]);
    free(adopted);
    candidates_free(&found);
    free(bottle_id);
    free(engine);
    free(prefix);
    ms_json_free(apps);
}

/* Scan the installs of every Sharp app that exited since the last call. */
static void sharp_adopt_finished(void) {
    sharp_running_app* finished;
    pthread_mutex_lock(&g_sharp_running_mutex);
    sharp_prune_locked();
    finished = g_sharp_finished;
    g_sharp_finished = NULL;
    pthread_mutex_unlock(&g_sharp_running_mutex);
    while (finished) {
        sharp_running_app* next = finished->next;
        adopt_installed_programs(finished->home, finished->id, finished->started, 0);
        sharp_entry_free(finished);
        finished = next;
    }
}

/* One-time pass for installers run before this existed (or while the backend
 * was down): look back to when the installer was added. */
static void sharp_adopt_catch_up(const char* home) {
    ms_json* apps = load_array(home);
    for (size_t i = 0; apps && i < ms_json_array_length(apps); i++) {
        const ms_json* app = ms_json_array_get(apps, i);
        const ms_json* done = ms_json_object_get(app, "install_scan_done");
        bool scanned = false;
        if (done && ms_json_as_bool(done, &scanned) && scanned)
            continue;
        char* id = field(app, "id", "");
        char* exe = field(app, "exe_path", "");
        long long installed_at = 0;
        if (id[0] && installer_like(exe) && ms_json_as_i64(ms_json_object_get(app, "installed_at"), &installed_at) &&
            installed_at > 0) {
            adopt_installed_programs(home, id, (time_t)installed_at, 0);
            (void)set_field(home, id, "install_scan_done", "true");
        }
        free(exe);
        free(id);
    }
    ms_json_free(apps);
}

char* ms_sharp_library_json(const char* home) {
    sharp_adopt_finished();
    sharp_adopt_catch_up(home);
    ms_json* a = load_array(home);
    char* raw;
    ms_json_writer w;
    char* o;
    if (!a)
        return failure("failed to read Sharp Library");
    raw = ms_json_stringify(a);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "apps");
    ms_json_writer_raw(&w, raw ? raw : "[]");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(raw);
    ms_json_free(a);
    return o;
}
char* ms_sharp_action_json(const char* home, const unsigned char* body, size_t length, const char* action) {
    char er[96];
    ms_json* j = ms_json_parse(body ? (const char*)body : "", body ? length : 0, er, sizeof(er));
    char *id = NULL, *src = NULL, *exe = NULL, *name = NULL, *dir = NULL, *raw = NULL;
    ms_json_writer w;
    char* o;
    bool needs_id = !strcmp(action, "uninstall") || !strcmp(action, "rename") || !strcmp(action, "set-cover") ||
                    !strcmp(action, "set-cover-position") || !strcmp(action, "set-launch-args") ||
                    !strcmp(action, "set-engine") || !strcmp(action, "launch") || !strcmp(action, "doctor") ||
                    !strcmp(action, "relaunch");
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return failure("invalid JSON body");
    }
    if (!strcmp(action, "install") || !strcmp(action, "import")) {
        char* bottle_id = NULL;
        if (!strcmp(action, "install")) {
            src = field(j, "srcPath", "");
            if (!src[0]) {
                free(src);
                ms_json_free(j);
                return failure("srcPath required");
            }
            if (is_moonscraper_installer(src)) {
                name = extract_moonscraper(home, src, &dir, &bottle_id);
                exe = dir ? join(dir, "Moonscraper Chart Editor.exe") : NULL;
                if (!name || !exe || !dir || !bottle_id) {
                    free(bottle_id);
                    free(name);
                    free(exe);
                    free(dir);
                    free(src);
                    ms_json_free(j);
                    return failure(!innoextract_binary() ? "innoextract is required for MoonScraper installers"
                                                         : "MoonScraper native extraction failed");
                }
            } else {
                exe = strdup(src);
            }
        } else {
            char* bottle = field(j, "bottleId", "");
            exe = field(j, "exePath", "");
            if (!bottle[0] || !exe[0]) {
                free(bottle);
                free(exe);
                ms_json_free(j);
                return failure("bottleId and exePath required");
            }
            free(bottle);
        }
        id = new_id();
        if (!name)
            name = field(j, "name", exe);
        if (!dir) {
            dir = field(j, "installDir", "");
            if (!dir[0]) {
                free(dir);
                dir = parent_dir(exe);
            }
        }
        raw = app_json(id, name, exe, dir, bottle_id);
        if (!raw || !append_raw(home, raw)) {
            free(id);
            free(bottle_id);
            free(src);
            free(exe);
            free(name);
            free(dir);
            free(raw);
            ms_json_free(j);
            return failure("failed to save Sharp Library");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "app");
        ms_json_writer_raw(&w, raw);
        ms_json_writer_object_end(&w);
        o = ms_json_writer_take(&w);
        free(id);
        free(bottle_id);
        free(src);
        free(exe);
        free(name);
        free(dir);
        free(raw);
        ms_json_free(j);
        return o;
    }
    if (needs_id) {
        if (!strcmp(action, "set-cover") && (!ms_json_object_get(j, "id") || !ms_json_object_get(j, "coverPath"))) {
            ms_json_free(j);
            return failure("id and coverPath required");
        }
        id = field(j, "id", "");
        if (!id[0]) {
            free(id);
            ms_json_free(j);
            return failure("id required");
        }
        ms_json* a = load_array(home);
        bool exists = a && has_id(a, id);
        if (!exists && strcmp(action, "doctor")) {
            free(id);
            ms_json_free(a);
            ms_json_free(j);
            return failure("app not found");
        }
        if (!strcmp(action, "uninstall") && exists) {
            ms_json_writer aw;
            char* serial;
            ms_json* newa;
            ms_json_writer_init(&aw);
            ms_json_writer_array_begin(&aw);
            for (size_t i = 0; i < ms_json_array_length(a); i++) {
                char* item_id = field(ms_json_array_get(a, i), "id", "");
                if (strcmp(item_id, id)) {
                    char* item = ms_json_stringify(ms_json_array_get(a, i));
                    ms_json_writer_raw(&aw, item ? item : "{}");
                    free(item);
                }
                free(item_id);
            }
            ms_json_writer_array_end(&aw);
            serial = ms_json_writer_take(&aw);
            char ee[64];
            newa = serial ? ms_json_parse(serial, strlen(serial), ee, sizeof(ee)) : NULL;
            if (!newa || !save_array(home, newa)) {
                free(serial);
                ms_json_free(newa);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("failed to save Sharp Library");
            }
            free(serial);
            ms_json_free(newa);
        }
        if (!strcmp(action, "rename") || !strcmp(action, "set-cover") || !strcmp(action, "set-engine") ||
            !strcmp(action, "set-launch-args") || !strcmp(action, "set-cover-position")) {
            char* value = NULL;
            const char* key = NULL;
            if (!strcmp(action, "rename")) {
                char* requested_name = field(j, "name", "");
                char* start = requested_name;
                char* end;
                while (*start && isspace((unsigned char)*start))
                    start++;
                end = start + strlen(start);
                while (end > start && isspace((unsigned char)end[-1]))
                    *--end = '\0';
                if (!start[0] || strlen(start) > 512) {
                    bool empty = !start[0];
                    free(requested_name);
                    ms_json_free(a);
                    free(id);
                    ms_json_free(j);
                    return failure(empty ? "name is required" : "name must be 512 bytes or fewer");
                }
                value = ms_json_quote(start);
                key = "name";
                free(requested_name);
            } else if (!strcmp(action, "set-cover")) {
                char* cover = field(j, "coverPath", "");
                char filename[256];
                if (!cover[0] || !copy_cover(home, id, cover, filename, sizeof(filename))) {
                    free(cover);
                    ms_json_free(a);
                    free(id);
                    ms_json_free(j);
                    return failure(!cover[0] ? "id and coverPath required" : "Cover image not found");
                }
                value = ms_json_quote(filename);
                key = "cover";
                free(cover);
            } else if (!strcmp(action, "set-engine")) {
                char* engine = field(j, "engine", "wine_bare");
                value = ms_json_quote(engine);
                key = "engine";
                free(engine);
            } else if (!strcmp(action, "set-launch-args")) {
                const ms_json* args = ms_json_object_get(j, "args");
                value = args ? ms_json_stringify(args) : strdup("[]");
                key = "user_launch_args";
            } else {
                long long x = 50, y = 50;
                ms_json_as_i64(ms_json_object_get(j, "x"), &x);
                ms_json_as_i64(ms_json_object_get(j, "y"), &y);
                if (x < 0)
                    x = 0;
                if (x > 100)
                    x = 100;
                if (y < 0)
                    y = 0;
                if (y > 100)
                    y = 100;
                char b[32];
                snprintf(b, sizeof(b), "%lld", x);
                value = strdup(b);
                key = "cover_position_x";
                if (!replace_field(home, id, key, value)) {
                    free(value);
                    ms_json_free(a);
                    free(id);
                    ms_json_free(j);
                    return failure("failed to save Sharp Library");
                }
                free(value);
                snprintf(b, sizeof(b), "%lld", y);
                value = strdup(b);
                key = "cover_position_y";
            }
            if (!value || !replace_field(home, id, key, value)) {
                free(value);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("failed to save Sharp Library");
            }
            free(value);
            if (!strcmp(action, "set-engine")) {
                for (size_t i = 0; i < ms_json_array_length(a); i++) {
                    const ms_json* app = ms_json_array_get(a, i);
                    char* item_id = field(app, "id", "");
                    if (!strcmp(item_id, id)) {
                        char* engine = field(j, "engine", "wine_bare");
                        char* exe = field(app, "exe_path", "");
                        char* dir = field(app, "install_dir", "");
                        sharp_stage_engine(home, engine, exe, dir);
                        free(dir);
                        free(exe);
                        free(engine);
                    }
                    free(item_id);
                }
            }
        }
        if (!strcmp(action, "launch") || !strcmp(action, "relaunch")) {
            const ms_json* app = NULL;
            char *exe_path = NULL, *work_dir = NULL, *bottle_id = NULL, *prefix = NULL, *engine = NULL;
            for (size_t i = 0; i < ms_json_array_length(a); i++) {
                char* item_id = field(ms_json_array_get(a, i), "id", "");
                if (!strcmp(item_id, id)) {
                    app = ms_json_array_get(a, i);
                    free(item_id);
                    break;
                }
                free(item_id);
            }
            if (app) {
                exe_path = field(app, "exe_path", "");
                work_dir = field(app, "install_dir", "");
                bottle_id = field(app, "bottle_id", "");
                engine = field(j, "engine", "");
                if (!engine[0]) {
                    free(engine);
                    engine = field(app, "engine", "auto");
                }
                if (bottle_id[0])
                    prefix = bottle_prefix(home, bottle_id);
            }
            if (!exe_path || !exe_path[0] || access(exe_path, F_OK) != 0) {
                free(exe_path);
                free(work_dir);
                free(bottle_id);
                free(prefix);
                free(engine);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("executable not found");
            }
            if (bottle_id[0] && (!prefix || !prefix[0])) {
                free(exe_path);
                free(work_dir);
                free(bottle_id);
                free(prefix);
                free(engine);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("Sharp application bottle prefix not found");
            }
            char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
            if (!wine || access(wine, X_OK) != 0) {
                free(wine);
                free(exe_path);
                free(work_dir);
                free(bottle_id);
                free(prefix);
                free(engine);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("MetalSharp Wine runtime not found");
            }
            {
                /* Bottle-less apps run in the Steam prefix (metalsharp-wine's default). */
                char* effective_prefix = prefix && prefix[0] ? strdup(prefix) : join(home, "prefix-steam");
                map_external_volume_letters(effective_prefix);
                free(effective_prefix);
            }
            if (!strcmp(action, "launch"))
                sharp_stage_engine(home, engine, exe_path, work_dir);
            pid_t pid = fork();
            if (pid < 0) {
                free(wine);
                free(exe_path);
                free(work_dir);
                free(bottle_id);
                free(prefix);
                free(engine);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("failed to launch application");
            }
            if (pid == 0) {
                (void)setpgid(0, 0);
                if (prefix)
                    setenv("WINEPREFIX", prefix, 1);
                struct stat work_stat;
                if (!work_dir || !work_dir[0] || stat(work_dir, &work_stat) != 0 || !S_ISDIR(work_stat.st_mode)) {
                    char* executable_dir = parent_dir(exe_path);
                    free(work_dir);
                    work_dir = executable_dir;
                }
                if (work_dir && work_dir[0]) {
                    ms_steam_deploy_controller_input_shims(home, work_dir);
                    (void)chdir(work_dir);
                }
                ms_steam_apply_launch_preferences(home);
                if (engine && strcmp(engine, "wine_bare") && strcmp(engine, "auto"))
                    ms_steam_apply_graphics_route(home, engine);
                const char* launch_wrapper = getenv("METALSHARP_WINE_LAUNCH_WRAPPER");
                char* launch_argv[] = {
                    (char*)(launch_wrapper && access(launch_wrapper, X_OK) == 0 ? launch_wrapper : wine), exe_path,
                    NULL};
                execv(launch_argv[0], launch_argv);
                _exit(127);
            }
            (void)setpgid(pid, pid);
            if (!strcmp(action, "launch"))
                sharp_remember(id, pid, true, work_dir, home);
            const char* reported_pipeline = engine && engine[0] ? engine : "auto";
            if (!strcmp(reported_pipeline, "auto"))
                reported_pipeline = "wine_bare";
            free(wine);
            free(exe_path);
            free(work_dir);
            free(bottle_id);
            free(prefix);
            ms_json_free(a);
            ms_json_free(j);
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "pid");
            ms_json_writer_u64(&w, (unsigned long long)pid);
            if (!strcmp(action, "relaunch")) {
                ms_json_writer_key(&w, "installing");
                ms_json_writer_bool(&w, true);
                ms_json_writer_key(&w, "message");
                ms_json_writer_string(&w, "Bottle installer relaunched");
            } else {
                ms_json_writer_key(&w, "gameType");
                ms_json_writer_string(&w, "native");
                ms_json_writer_key(&w, "pipeline");
                ms_json_writer_string(&w, reported_pipeline);
            }
            ms_json_writer_object_end(&w);
            o = ms_json_writer_take(&w);
            free(engine);
            free(id);
            return o;
        }
        ms_json_free(a);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        if (!strcmp(action, "doctor")) {
            ms_json_writer_key(&w, "report");
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "id");
            ms_json_writer_string(&w, id);
            ms_json_writer_key(&w, "ready");
            ms_json_writer_bool(&w, true);
            ms_json_writer_object_end(&w);
        } else if (!strcmp(action, "launch")) {
            ms_json_writer_key(&w, "warnings");
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
        }
        ms_json_writer_object_end(&w);
        o = ms_json_writer_take(&w);
        free(id);
        ms_json_free(j);
        return o;
    }
    ms_json_free(j);
    return failure("unknown Sharp Library action");
}
char* ms_sharp_cover_path(const char* home, const char* id) {
    ms_json* a = load_array(home);
    char* result = NULL;
    if (!a || !id || !id[0]) {
        ms_json_free(a);
        return NULL;
    }
    for (size_t i = 0; i < ms_json_array_length(a); i++) {
        const ms_json* item = ms_json_array_get(a, i);
        char* item_id = field(item, "id", "");
        if (!strcmp(item_id, id)) {
            char* cover = field(item, "cover", "");
            if (cover[0] && !strchr(cover, '/') && !strchr(cover, '\\')) {
                char* dir = join(home, "sharp-library");
                result = dir ? join(dir, cover) : NULL;
                free(dir);
                if (result && access(result, R_OK) != 0) {
                    free(result);
                    result = NULL;
                }
            }
            free(cover);
            free(item_id);
            break;
        }
        free(item_id);
    }
    ms_json_free(a);
    return result;
}
