#include "metalsharp_backend/gog.h"
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

typedef struct {
    char product_id[181];
    pid_t pid;
} gog_pid_entry;
static gog_pid_entry gog_pids[32];
static size_t gog_pid_count;
static bool remove_tree(const char* path);

static void gog_track_pid(const char* product_id, pid_t pid) {
    for (size_t i = 0; i < gog_pid_count; i++)
        if (!strcmp(gog_pids[i].product_id, product_id)) {
            gog_pids[i].pid = pid;
            return;
        }
    if (gog_pid_count < sizeof(gog_pids) / sizeof(gog_pids[0])) {
        snprintf(gog_pids[gog_pid_count].product_id, sizeof(gog_pids[gog_pid_count].product_id), "%s", product_id);
        gog_pids[gog_pid_count++].pid = pid;
    }
}

static pid_t gog_tracked_pid(const char* product_id) {
    for (size_t i = 0; i < gog_pid_count; i++)
        if (!strcmp(gog_pids[i].product_id, product_id))
            return gog_pids[i].pid;
    return 0;
}

typedef struct {
    pid_t pid;
    char* log_path;
} gog_reap_job;

static void* gog_reap_worker(void* raw) {
    gog_reap_job* job = raw;
    int wait_status;
    pid_t waited = waitpid(job->pid, &wait_status, 0);
    FILE* log = fopen(job->log_path, "a");
    if (log) {
        if (waited == job->pid && WIFEXITED(wait_status))
            fprintf(log, "gogdl exited with Some(%d)\\n", WEXITSTATUS(wait_status));
        else if (waited == job->pid && WIFSIGNALED(wait_status))
            fprintf(log, "gogdl exited with None\\n");
        fclose(log);
    }
    free(job->log_path);
    free(job);
    return NULL;
}

static void gog_watch_child(pid_t pid, const char* log_path) {
    gog_reap_job* job = malloc(sizeof(*job));
    pthread_t thread;
    if (!job)
        return;
    job->pid = pid;
    job->log_path = strdup(log_path ? log_path : "");
    if (!job->log_path || pthread_create(&thread, NULL, gog_reap_worker, job) != 0) {
        free(job->log_path);
        free(job);
        return;
    }
    pthread_detach(thread);
}

static char* err(const char* s) {
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
static char* read_text(const char* p) {
    FILE* f = fopen(p, "rb");
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
static char* library_path(const char* home) {
    char *d = join(home, "gog"), *p = d ? join(d, "library.json") : NULL;
    free(d);
    return p;
}
static char* auth_path(const char* home) {
    char *d = join(home, "gog_store"), *p = d ? join(d, "auth.json") : NULL;
    free(d);
    return p;
}
static ms_json* load_games(const char* home) {
    char *p = library_path(home), *raw = p ? read_text(p) : NULL;
    char e[64];
    ms_json* j;
    const ms_json* games;
    char* copy;
    if (!raw) {
        free(p);
        return ms_json_parse("[]", 2, e, sizeof(e));
    }
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    free(p);
    if (!j)
        return ms_json_parse("[]", 2, e, sizeof(e));
    if (ms_json_type_of(j) == MS_JSON_ARRAY)
        return j;
    games = ms_json_type_of(j) == MS_JSON_OBJECT ? (ms_json*)ms_json_object_get(j, "games") : NULL;
    copy = games ? ms_json_stringify(games) : NULL;
    ms_json_free(j);
    if (!copy)
        return ms_json_parse("[]", 2, e, sizeof(e));
    j = ms_json_parse(copy, strlen(copy), e, sizeof(e));
    free(copy);
    return j && ms_json_type_of(j) == MS_JSON_ARRAY ? j : (ms_json_free(j), ms_json_parse("[]", 2, e, sizeof(e)));
}

static bool load_last_sync(const char* home, unsigned long long* value) {
    char *p = library_path(home), *raw = p ? read_text(p) : NULL;
    char e[64];
    double number;
    ms_json* j;
    const ms_json* stamp;
    free(p);
    if (value)
        *value = 0;
    if (!raw)
        return false;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    stamp = j && ms_json_type_of(j) == MS_JSON_OBJECT ? ms_json_object_get(j, "lastSyncAt") : NULL;
    if (!stamp || !ms_json_as_number(stamp, &number) || number < 0) {
        ms_json_free(j);
        return false;
    }
    if (value)
        *value = (unsigned long long)number;
    ms_json_free(j);
    return true;
}

static bool save_games_at(const char* home, const ms_json* a, unsigned long long last_sync_at) {
    char *d = join(home, "gog"), *p, *games_raw, *raw;
    FILE* f;
    ms_json_writer writer;
    if (!d || !mkdir_p(d)) {
        free(d);
        return false;
    }
    p = join(d, "library.json");
    games_raw = ms_json_stringify(a);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "games");
    ms_json_writer_raw(&writer, games_raw ? games_raw : "[]");
    ms_json_writer_key(&writer, "lastSyncAt");
    if (last_sync_at)
        ms_json_writer_u64(&writer, last_sync_at);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_object_end(&writer);
    raw = ms_json_writer_take(&writer);
    f = p ? fopen(p, "wb") : NULL;
    if (!f || !raw || fputs(raw, f) < 0) {
        if (f)
            fclose(f);
        free(d);
        free(p);
        free(games_raw);
        free(raw);
        return false;
    }
    fclose(f);
    free(d);
    free(p);
    free(games_raw);
    free(raw);
    return true;
}

static bool save_games(const char* home, const ms_json* a) {
    unsigned long long last_sync_at = 0;
    (void)load_last_sync(home, &last_sync_at);
    return save_games_at(home, a, last_sync_at);
}
static bool valid_product_id(const char* value) {
    size_t length = value ? strlen(value) : 0;
    if (length == 0 || length > 180)
        return false;
    for (size_t i = 0; i < length; i++)
        if (!(isalnum((unsigned char)value[i]) || value[i] == '_' || value[i] == '-' || value[i] == '.'))
            return false;
    return true;
}

static char* field(const ms_json* j, const char* key, const char* fallback) {
    char* s = NULL;
    if (j)
        ms_json_as_string(ms_json_object_get(j, key), &s);
    if (!s)
        s = strdup(fallback ? fallback : "");
    return s;
}
static bool authenticated(const char* home) {
    char* p = auth_path(home);
    struct stat st;
    bool yes = p && stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    free(p);
    return yes;
}

static char* gog_access_token(const char* home) {
    char *path = auth_path(home), *raw = path ? read_text(path) : NULL, *token = NULL;
    char error[128];
    ms_json* value;
    free(path);
    if (!raw)
        return NULL;
    value = ms_json_parse(raw, strlen(raw), error, sizeof(error));
    free(raw);
    if (value) {
        ms_json_as_string(ms_json_object_get(value, "access_token"), &token);
        /* gogdl persists auth.json keyed by its client id, while `gogdl
         * auth` prints the inner credential object. Accept both forms so a
         * successful OAuth flow remains usable after the app restarts. */
        if (!token && ms_json_type_of(value) == MS_JSON_OBJECT) {
            for (size_t i = 0; i < ms_json_object_length(value) && !token; i++) {
                const ms_json* credential = ms_json_object_value_at(value, i);
                if (credential && ms_json_type_of(credential) == MS_JSON_OBJECT)
                    ms_json_as_string(ms_json_object_get(credential, "access_token"), &token);
            }
        }
    }
    ms_json_free(value);
    return token;
}

static char* fetch_gog_url(const char* home, const char* url, bool with_token, char* error, size_t error_size) {
    char *token = with_token ? gog_access_token(home) : NULL, header[1024],
         output_path[] = "/tmp/metalsharp-gog-sync-XXXXXX", *body = NULL;
    int fd;
    pid_t pid, waited;
    int status;
    if (with_token && (!token || !*token)) {
        free(token);
        snprintf(error, error_size, "GOG credentials missing access token");
        return NULL;
    }
    header[0] = 0;
    if (with_token && snprintf(header, sizeof(header), "Authorization: Bearer %s", token) >= (int)sizeof(header)) {
        free(token);
        snprintf(error, error_size, "GOG access token is too long");
        return NULL;
    }
    fd = mkstemp(output_path);
    free(token);
    if (fd < 0) {
        snprintf(error, error_size, "failed to create GOG sync output");
        return NULL;
    }
    pid = fork();
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        close(fd);
        if (with_token)
            execlp("curl", "curl", "--fail", "--location", "--silent", "--show-error", "--header", header, url,
                   (char*)NULL);
        else
            execlp("curl", "curl", "--fail", "--location", "--silent", "--show-error", url, (char*)NULL);
        _exit(127);
    }
    close(fd);
    do
        waited = waitpid(pid, &status, 0);
    while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(output_path);
        snprintf(error, error_size, "GOG library request failed");
        return NULL;
    }
    body = read_text(output_path);
    unlink(output_path);
    if (!body)
        snprintf(error, error_size, "GOG library response was empty");
    return body;
}

static char* fetch_gog_owned(const char* home, char* error, size_t error_size) {
    return fetch_gog_url(home, "https://embed.gog.com/user/data/games", true, error, error_size);
}

static char* gogdl_path(const char* home) {
    const char* explicit_path = getenv("METALSHARP_GOGDL_BIN");
    const char* path_env = getenv("PATH");
    char* path_copy;
    if (explicit_path && *explicit_path && access(explicit_path, X_OK) == 0)
        return strdup(explicit_path);
    char* candidates[] = {join(home, "tools/gogdl"), join(home, "runtime/gogdl")};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        bool found = candidates[i] && access(candidates[i], X_OK) == 0;
        if (found) {
            for (size_t j = i + 1; j < sizeof(candidates) / sizeof(candidates[0]); j++)
                free(candidates[j]);
            return candidates[i];
        }
        free(candidates[i]);
    }
    if (!path_env)
        return NULL;
    path_copy = strdup(path_env);
    if (!path_copy)
        return NULL;
    for (char* dir = strtok(path_copy, ":"); dir; dir = strtok(NULL, ":")) {
        char* candidate = join(dir, "gogdl");
        if (candidate && access(candidate, X_OK) == 0) {
            free(path_copy);
            return candidate;
        }
        free(candidate);
    }
    free(path_copy);
    return NULL;
}

static bool gogdl_available(const char* home) {
    char* path = gogdl_path(home);
    bool found = path != NULL;
    free(path);
    return found;
}
static void deploy_gog_oauth_helper(const char* home) {
    char* dir = join(home, "tools/gog-oauth-helper");
    char* marker;
    FILE* file;
    if (!dir || !mkdir_p(dir)) {
        free(dir);
        return;
    }
    marker = join(dir, ".inline-helper");
    file = marker ? fopen(marker, "ab") : NULL;
    if (file)
        fclose(file);
    free(marker);
    free(dir);
}

static bool run_bootstrap_argv(const char* const argv[], const char* context, char* error, size_t error_size) {
    pid_t pid = fork();
    int wait_status = 0;
    pid_t waited;
    if (pid < 0) {
        snprintf(error, error_size, "%s failed to start: %s", context, strerror(errno));
        return false;
    }
    if (pid == 0) {
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    do
        waited = waitpid(pid, &wait_status, 0);
    while (waited < 0 && errno == EINTR);
    if (waited == pid && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
        return true;
    snprintf(error, error_size, "%s failed", context);
    return false;
}

static const char* bootstrap_tool(const char* name, char* path, size_t path_size) {
    const char* fixed[] = {"/usr/bin", "/opt/homebrew/bin", "/usr/local/bin"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        snprintf(path, path_size, "%s/%s", fixed[i], name);
        if (access(path, X_OK) == 0)
            return path;
    }
    const char* env_path = getenv("PATH");
    char* copy = env_path ? strdup(env_path) : NULL;
    char* save = NULL;
    for (char* dir = copy ? strtok_r(copy, ":", &save) : NULL; dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(path, path_size, "%s/%s", dir, name);
        if (access(path, X_OK) == 0) {
            free(copy);
            return path;
        }
    }
    free(copy);
    return NULL;
}

static bool write_gogdl_wrapper(const char* home, char* error, size_t error_size) {
    char* wrapper = join(home, "tools/gogdl");
    char* target = join(home, "tools/gogdl-venv/bin/gogdl");
    char* tools = join(home, "tools");
    FILE* file;
    bool tools_ok = tools && mkdir_p(tools);
    free(tools);
    if (!wrapper || !target || !tools_ok) {
        snprintf(error, error_size, "failed to prepare GOG support wrapper");
        free(wrapper);
        free(target);
        return false;
    }
    file = fopen(wrapper, "wb");
    if (!file) {
        snprintf(error, error_size, "failed to write GOG support wrapper");
        free(wrapper);
        free(target);
        return false;
    }
    fprintf(file, "#!/bin/sh\nexec '%s' \"$@\"\n", target);
    fclose(file);
    chmod(wrapper, 0755);
    free(wrapper);
    free(target);
    return true;
}

static bool ensure_gogdl_available(const char* home, char* error, size_t error_size) {
    char venv[PATH_MAX], venv_python[PATH_MAX], source[PATH_MAX], git_dir[PATH_MAX];
    if (gogdl_available(home)) {
        deploy_gog_oauth_helper(home);
        return true;
    }
    snprintf(venv, sizeof(venv), "%s/tools/gogdl-venv", home);
    snprintf(venv_python, sizeof(venv_python), "%s/bin/python", venv);
    if (access(venv_python, X_OK) == 0) {
        if (!write_gogdl_wrapper(home, error, error_size) || !gogdl_available(home))
            return false;
        deploy_gog_oauth_helper(home);
        return true;
    }
    char python[PATH_MAX], git[PATH_MAX];
    if (!bootstrap_tool("python3", python, sizeof(python))) {
        snprintf(error, error_size, "python3 is required to prepare GOG support");
        return false;
    }
    if (!bootstrap_tool("git", git, sizeof(git))) {
        snprintf(error, error_size, "git is required to prepare GOG support");
        return false;
    }
    char* tools = join(home, "tools");
    bool tools_ok = tools && mkdir_p(tools);
    free(tools);
    if (!tools_ok) {
        snprintf(error, error_size, "failed to create GOG tools dir");
        return false;
    }
    if (access(venv_python, X_OK) != 0) {
        const char* argv[] = {python, "-m", "venv", venv, NULL};
        if (!run_bootstrap_argv(argv, "GOG support environment setup", error, error_size))
            return false;
    }
    snprintf(source, sizeof(source), "%s/tools/heroic-gogdl", home);
    snprintf(git_dir, sizeof(git_dir), "%s/.git", source);
    if (access(git_dir, F_OK) != 0) {
        if (access(source, F_OK) == 0 && !remove_tree(source)) {
            snprintf(error, error_size, "failed to reset GOG support source");
            return false;
        }
        const char* argv[] = {git,
                              "clone",
                              "--depth",
                              "1",
                              "--recurse-submodules",
                              "https://github.com/Heroic-Games-Launcher/heroic-gogdl.git",
                              source,
                              NULL};
        if (!run_bootstrap_argv(argv, "GOG support source setup", error, error_size))
            return false;
    } else {
        const char* argv[] = {git, "-C", source, "submodule", "update", "--init", "--recursive", NULL};
        if (!run_bootstrap_argv(argv, "GOG support source refresh", error, error_size))
            return false;
    }
    const char* pip_argv[] = {venv_python, "-m", "pip", "install", "--upgrade", source, NULL};
    if (!run_bootstrap_argv(pip_argv, "GOG support install", error, error_size))
        return false;
    if (!write_gogdl_wrapper(home, error, error_size))
        return false;
    char* wrapper = join(home, "tools/gogdl");
    const char* verify_argv[] = {wrapper, "--version", NULL};
    bool verified = wrapper && run_bootstrap_argv(verify_argv, "GOG support verification", error, error_size);
    free(wrapper);
    if (!verified)
        return false;
    deploy_gog_oauth_helper(home);
    return true;
}

static bool spawn_gogdl_download(const char* home, const char* product_id, const char* platform,
                                 const char* install_root, const char* language, pid_t* pid_out, char** log_out) {
    char* binary = gogdl_path(home);
    char* log_dir = join(home, "logs/gog");
    char log_path[2048];
    pid_t pid;
    if (!binary || !log_dir || !mkdir_p(log_dir)) {
        free(binary);
        free(log_dir);
        return false;
    }
    snprintf(log_path, sizeof(log_path), "%s/install-%s-%llu.log", log_dir, product_id, (unsigned long long)time(NULL));
    pid = fork();
    if (pid < 0) {
        free(binary);
        free(log_dir);
        return false;
    }
    if (pid == 0) {
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        char* support = join(home, "gogdl/gog-support");
        char* config = join(home, "gogdl");
        char* auth = auth_path(home);
        if (support)
            mkdir_p(support);
        if (config)
            setenv("GOGDL_CONFIG_PATH", config, 1);
        if (support)
            setenv("GOGDL_SUPPORT_PATH", support, 1);
        char* args[20];
        int n = 0;
        args[n++] = binary;
        args[n++] = "--auth-config-path";
        args[n++] = auth ? auth : (char*)"";
        args[n++] = "download";
        args[n++] = (char*)product_id;
        args[n++] = "--platform";
        args[n++] = (char*)platform;
        args[n++] = "--path";
        args[n++] = (char*)install_root;
        args[n++] = "--support";
        args[n++] = support ? support : (char*)"";
        args[n++] = "--with-dlcs";
        if (language && *language) {
            args[n++] = "--lang";
            args[n++] = (char*)language;
        }
        args[n] = NULL;
        execv(binary, args);
        _exit(127);
    }
    *pid_out = pid;
    *log_out = strdup(log_path);
    if (*log_out)
        gog_watch_child(pid, *log_out);
    free(binary);
    free(log_dir);
    return *log_out != NULL;
}

static bool spawn_gogdl_launch(const char* home, const char* product_id, const char* platform, const char* folder,
                               const char* engine, pid_t* pid_out, char** log_out) {
    char *binary = gogdl_path(home), *log_dir = join(home, "logs/gog"),
         *wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* prefix = join(home, "bottles/gog-prefix/prefix");
    char log_path[2048];
    pid_t pid;
    if (!binary || !log_dir || !wine || !prefix || !mkdir_p(log_dir)) {
        free(binary);
        free(log_dir);
        free(wine);
        free(prefix);
        return false;
    }
    snprintf(log_path, sizeof(log_path), "%s/launch-%s-%llu.log", log_dir, product_id, (unsigned long long)time(NULL));
    pid = fork();
    if (pid < 0) {
        free(binary);
        free(log_dir);
        free(wine);
        free(prefix);
        return false;
    }
    if (pid == 0) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        char* auth = auth_path(home);
        char* config = join(home, "gogdl");
        char* support = join(home, "gogdl/gog-support");
        if (config)
            setenv("GOGDL_CONFIG_PATH", config, 1);
        if (support)
            setenv("GOGDL_SUPPORT_PATH", support, 1);
        setenv("MS_GRAPHICS_BACKEND", engine && *engine ? engine : "auto", 1);
        char* args[] = {binary,       "--auth-config-path", auth ? auth : (char*)"",
                        "launch",     (char*)folder,        (char*)product_id,
                        "--platform", (char*)platform,      "--wine",
                        wine,         "--wine-prefix",      prefix,
                        NULL};
        execv(binary, args);
        _exit(127);
    }
    *pid_out = pid;
    *log_out = strdup(log_path);
    if (*log_out)
        gog_watch_child(pid, *log_out);
    free(binary);
    free(log_dir);
    free(wine);
    free(prefix);
    return *log_out != NULL;
}
static char* gog_import_folder(const char* root, const char* product_id) {
    char info_name[256];
    struct stat st;
    DIR* dir;
    struct dirent* entry;
    snprintf(info_name, sizeof(info_name), "goggame-%s.info", product_id);
    char* direct = join(root, info_name);
    if (direct && stat(direct, &st) == 0 && S_ISREG(st.st_mode)) {
        char* result = strdup(root);
        free(direct);
        return result;
    }
    free(direct);
    dir = opendir(root);
    if (!dir)
        return NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char* child = join(root, entry->d_name);
        char* info = child ? join(child, info_name) : NULL;
        bool found = info && stat(info, &st) == 0 && S_ISREG(st.st_mode);
        free(info);
        if (found) {
            closedir(dir);
            return child;
        }
        free(child);
    }
    closedir(dir);
    return NULL;
}

/* Resolve launch inputs from MetalSharp's persisted library rather than from
 * renderer-supplied paths.  The renderer intentionally sends only a product
 * ID, and accepting an arbitrary gameFolder here would also let a local IPC
 * caller redirect gogdl outside the registered installation. */
static bool gog_launch_record(const char* home, const char* product_id, char** title_out, char** platform_out,
                              char** install_root_out, char** game_folder_out) {
    ms_json* games = load_games(home);
    bool found = false;
    if (title_out)
        *title_out = NULL;
    if (platform_out)
        *platform_out = NULL;
    if (install_root_out)
        *install_root_out = NULL;
    if (game_folder_out)
        *game_folder_out = NULL;
    if (!games)
        return false;
    for (size_t i = 0; i < ms_json_array_length(games); i++) {
        const ms_json* game = ms_json_array_get(games, i);
        char *candidate = field(game, "productId", ""), *stored_folder = NULL, *resolved_folder = NULL;
        if (strcmp(candidate, product_id)) {
            free(candidate);
            continue;
        }
        free(candidate);
        if (title_out)
            *title_out = field(game, "title", product_id);
        if (platform_out)
            *platform_out = field(game, "platform", "windows");
        if (install_root_out)
            *install_root_out = field(game, "installRoot", "");
        stored_folder = field(game, "gameFolder", "");
        if (*stored_folder)
            resolved_folder = gog_import_folder(stored_folder, product_id);
        if (!resolved_folder && install_root_out && *install_root_out && **install_root_out)
            resolved_folder = gog_import_folder(*install_root_out, product_id);
        if (game_folder_out)
            *game_folder_out = resolved_folder;
        else
            free(resolved_folder);
        free(stored_folder);
        found = resolved_folder != NULL;
        break;
    }
    ms_json_free(games);
    if (!found) {
        free(title_out ? *title_out : NULL);
        free(platform_out ? *platform_out : NULL);
        free(install_root_out ? *install_root_out : NULL);
        if (title_out)
            *title_out = NULL;
        if (platform_out)
            *platform_out = NULL;
        if (install_root_out)
            *install_root_out = NULL;
        if (game_folder_out)
            *game_folder_out = NULL;
    }
    return found;
}

static bool run_gogdl_auth(const char* home, const char* code) {
    char *binary = gogdl_path(home), *auth = auth_path(home), *auth_dir = join(home, "gog_store");
    char *config = join(home, "gogdl"), *support = join(home, "gogdl/gog-support");
    pid_t pid;
    int wait_status;
    if (!binary || !auth || !auth_dir || !mkdir_p(auth_dir) || !mkdir_p(config ? config : home)) {
        free(binary);
        free(auth);
        free(auth_dir);
        free(config);
        free(support);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        free(binary);
        free(auth);
        free(auth_dir);
        free(config);
        free(support);
        return false;
    }
    if (pid == 0) {
        if (config)
            setenv("GOGDL_CONFIG_PATH", config, 1);
        if (support) {
            mkdir_p(support);
            setenv("GOGDL_SUPPORT_PATH", support, 1);
        }
        char* args[] = {binary, "--auth-config-path", auth, "auth", "--code", (char*)code, NULL};
        execv(binary, args);
        _exit(127);
    }
    free(binary);
    free(auth);
    free(auth_dir);
    free(config);
    free(support);
    if (waitpid(pid, &wait_status, 0) < 0)
        return false;
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static bool run_gogdl_import(const char* home, const char* product_id, const char* folder) {
    char *binary = gogdl_path(home), *log_dir = join(home, "logs/gog");
    char log_path[2048];
    pid_t pid;
    int wait_status;
    if (!binary || !log_dir || !mkdir_p(log_dir)) {
        free(binary);
        free(log_dir);
        return false;
    }
    snprintf(log_path, sizeof(log_path), "%s/import-%s-%llu.log", log_dir, product_id, (unsigned long long)time(NULL));
    pid = fork();
    if (pid < 0) {
        free(binary);
        free(log_dir);
        return false;
    }
    if (pid == 0) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        char* auth = auth_path(home);
        char* config = join(home, "gogdl");
        char* support = join(home, "gogdl/gog-support");
        if (config)
            setenv("GOGDL_CONFIG_PATH", config, 1);
        if (support)
            setenv("GOGDL_SUPPORT_PATH", support, 1);
        char* args[] = {binary, "--auth-config-path", auth ? auth : (char*)"", "import", (char*)folder, NULL};
        execv(binary, args);
        _exit(127);
    }
    free(binary);
    free(log_dir);
    if (waitpid(pid, &wait_status, 0) < 0)
        return false;
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static bool remove_tree(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        struct dirent* e;
        if (!d)
            return false;
        while ((e = readdir(d)) != NULL) {
            char* child;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            child = join(path, e->d_name);
            if (!child || !remove_tree(child)) {
                free(child);
                closedir(d);
                return false;
            }
            free(child);
        }
        closedir(d);
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

static bool clear_gogdl_manifest(const char* home, const char* product_id) {
    char* manifests = join(home, "gogdl/heroic_gogdl/manifests");
    char* manifest = manifests ? join(manifests, product_id) : NULL;
    bool ok = !manifest || unlink(manifest) == 0 || errno == ENOENT;
    free(manifest);
    free(manifests);
    return ok;
}

static bool safe_gog_install_path(const char* path, const char* home) {
    char* resolved;
    bool safe;
    if (!path || !*path || !strcmp(path, "/") || !strcmp(path, ".") || !strcmp(path, ".."))
        return false;
    resolved = realpath(path, NULL);
    if (!resolved)
        return false;
    safe = strcmp(resolved, "/") != 0 && strcmp(resolved, home) != 0 && strcmp(resolved, "/Applications") != 0 &&
           strcmp(resolved, "/Users") != 0 && strcmp(resolved, "/Volumes") != 0;
    free(resolved);
    return safe;
}
static char* status_json(const char* home) {
    char prefix[1024], wine_path[1024];
    char *auth = join(home, "gog_store/auth.json"), *config = join(home, "gogdl"),
         *support = join(home, "gogdl/gog-support"), *oauth = join(home, "tools/gog-oauth-helper"), *gogdl = NULL,
         *drive_c;
    ms_json_writer w;
    char* o;
    struct stat prefix_stat;
    bool prefix_initialized;
    const char* state;
    snprintf(prefix, sizeof(prefix), "%s/bottles/gog-prefix/prefix", home);
    snprintf(wine_path, sizeof(wine_path), "%s/runtime/wine/bin/metalsharp-wine", home);
    if (access(wine_path, X_OK) != 0)
        snprintf(wine_path, sizeof(wine_path), "%s/runtime/wine/bin/wine", home);
    gogdl = gogdl_path(home);
    drive_c = join(prefix, "drive_c");
    prefix_initialized = stat(prefix, &prefix_stat) == 0 && S_ISDIR(prefix_stat.st_mode) && drive_c != NULL &&
                         access(drive_c, F_OK) == 0;
    free(drive_c);
    if (!gogdl)
        state = "missing_gogdl";
    else if (!prefix_initialized)
        state = "needs_prefix";
    else if (!authenticated(home))
        state = "needs_login";
    else
        state = "ready";
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, "gog");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "GOG");
    ms_json_writer_key(&w, "status");
    ms_json_writer_string(&w, state);
    ms_json_writer_key(&w, "ready");
    ms_json_writer_bool(&w, !strcmp(state, "ready"));
    ms_json_writer_key(&w, "authUrl");
    ms_json_writer_string(&w, "https://auth.gog.com/"
                              "auth?client_id=46899977096215655&redirect_uri=https%3A%2F%2Fembed.gog.com%2Fon_login_"
                              "success%3Forigin%3Dclient&response_type=code&layout=galaxy");
    ms_json_writer_key(&w, "authenticated");
    ms_json_writer_bool(&w, authenticated(home));
    ms_json_writer_key(&w, "gogdlAvailable");
    ms_json_writer_bool(&w, gogdl != NULL);
    ms_json_writer_key(&w, "gogdlPath");
    if (gogdl)
        ms_json_writer_string(&w, gogdl);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "authConfigPath");
    ms_json_writer_string(&w, auth ? auth : "");
    ms_json_writer_key(&w, "configPath");
    ms_json_writer_string(&w, config ? config : "");
    ms_json_writer_key(&w, "supportPath");
    ms_json_writer_string(&w, support ? support : "");
    ms_json_writer_key(&w, "oauthHelperPath");
    ms_json_writer_string(&w, oauth ? oauth : "");
    ms_json_writer_key(&w, "oauthHelperAvailable");
    {
        char* marker = oauth ? join(oauth, ".inline-helper") : NULL;
        ms_json_writer_bool(&w, marker && access(marker, F_OK) == 0);
        free(marker);
    }
    ms_json_writer_key(&w, "oauthHelperScript");
    ms_json_writer_string(&w, "(inline Electron BrowserWindow)");
    ms_json_writer_key(&w, "bottleId");
    ms_json_writer_string(&w, "gog-prefix");
    ms_json_writer_key(&w, "winePrefix");
    ms_json_writer_string(&w, prefix);
    ms_json_writer_key(&w, "prefixInitialized");
    ms_json_writer_bool(&w, prefix_initialized);
    ms_json_writer_key(&w, "winePath");
    ms_json_writer_string(&w, wine_path);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(auth);
    free(config);
    free(support);
    free(oauth);
    free(gogdl);
    return o;
}
static bool gog_initialize_prefix(const char* home, char* error, size_t error_size) {
    char* prefix = join(home, "bottles/gog-prefix/prefix");
    char* drive_c;
    char* wine;
    struct stat st;
    pid_t pid, waited;
    int wait_status;
    if (!prefix || !mkdir_p(prefix)) {
        snprintf(error, error_size, "failed to create GOG prefix");
        free(prefix);
        return false;
    }
    drive_c = join(prefix, "drive_c");
    if (drive_c && stat(drive_c, &st) == 0 && S_ISDIR(st.st_mode)) {
        free(drive_c);
        free(prefix);
        return true;
    }
    free(drive_c);
    wine = join(home, "runtime/wine/bin/metalsharp-wine");
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        wine = join(home, "runtime/wine/bin/wine");
    }
    if (!wine || access(wine, X_OK) != 0) {
        snprintf(error, error_size, "MetalSharp Wine not found: %s/runtime/wine/bin/metalsharp-wine", home);
        free(wine);
        free(prefix);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(error, error_size, "failed to initialize GOG prefix");
        free(wine);
        free(prefix);
        return false;
    }
    if (pid == 0) {
        char* args[] = {wine, "wineboot", "-u", NULL};
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEMSYNC", "0", 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("MS_FWD_COMPAT_GL_CTX", "1", 1);
        execv(wine, args);
        _exit(127);
    }
    do
        waited = waitpid(pid, &wait_status, 0);
    while (waited < 0 && errno == EINTR);
    free(wine);
    free(prefix);
    if (waited == pid && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
        return true;
    snprintf(error, error_size, "wineboot failed with %d",
             waited == pid && WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1);
    return false;
}

static char* gog_error_with_status(const char* home, const char* message) {
    ms_json_writer w;
    char* status = status_json(home);
    char* out;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message);
    ms_json_writer_key(&w, "status");
    ms_json_writer_raw(&w, status ? status : "{}");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(status);
    return out;
}

char* ms_gog_status_json(const char* home) {
    ms_json_writer w;
    char *o, *s = status_json(home);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "status");
    ms_json_writer_raw(&w, s ? s : "{}");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(s);
    return o;
}
static char* refresh_game_json(const char* home, const ms_json* game);

char* ms_gog_games_json(const char* home) {
    ms_json *a = load_games(home), *refreshed = NULL;
    char *raw, *refresh_serialized = NULL;
    ms_json_writer refresh_writer, w;
    char* o;
    char error[128];
    if (!a)
        return err("failed to read GOG library");
    ms_json_writer_init(&refresh_writer);
    ms_json_writer_array_begin(&refresh_writer);
    for (size_t i = 0; i < ms_json_array_length(a); i++) {
        char* game = refresh_game_json(home, ms_json_array_get(a, i));
        ms_json_writer_raw(&refresh_writer, game ? game : "{}");
        free(game);
    }
    ms_json_writer_array_end(&refresh_writer);
    refresh_serialized = ms_json_writer_take(&refresh_writer);
    refreshed =
        refresh_serialized ? ms_json_parse(refresh_serialized, strlen(refresh_serialized), error, sizeof(error)) : NULL;
    if (refreshed && ms_json_type_of(refreshed) == MS_JSON_ARRAY) {
        (void)save_games(home, refreshed);
        raw = ms_json_stringify(refreshed);
    } else {
        raw = ms_json_stringify(a);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "games");
    ms_json_writer_raw(&w, raw ? raw : "[]");
    ms_json_writer_key(&w, "status");
    {
        char* s = status_json(home);
        ms_json_writer_raw(&w, s ? s : "{}");
        free(s);
    }
    ms_json_writer_key(&w, "lastSyncAt");
    {
        unsigned long long last_sync_at = 0;
        if (load_last_sync(home, &last_sync_at) && last_sync_at)
            ms_json_writer_u64(&w, last_sync_at);
        else
            ms_json_writer_null(&w);
    }
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(raw);
    free(refresh_serialized);
    ms_json_free(refreshed);
    ms_json_free(a);
    return o;
}
static char* game_json_ex(const char* id, const char* title, const char* status, const char* platform,
                          const char* install_root, const char* game_folder, pid_t install_pid, pid_t launch_pid,
                          const char* log_path) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "productId");
    ms_json_writer_string(&w, id);
    ms_json_writer_key(&w, "title");
    ms_json_writer_string(&w, title);
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, platform && *platform ? platform : "windows");
    ms_json_writer_key(&w, "slug");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "imageUrl");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "iconUrl");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installRoot");
    if (install_root && *install_root)
        ms_json_writer_string(&w, install_root);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "gameFolder");
    if (game_folder && *game_folder)
        ms_json_writer_string(&w, game_folder);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "primaryExe");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "primaryTaskName");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, !strcmp(status, "installed"));
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, !strcmp(status, "running"));
    ms_json_writer_key(&w, "status");
    ms_json_writer_string(&w, status);
    ms_json_writer_key(&w, "downloadSizeBytes");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "diskSizeBytes");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "lastInstallPid");
    if (install_pid > 0)
        ms_json_writer_u64(&w, (unsigned)install_pid);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "lastLaunchPid");
    if (launch_pid > 0)
        ms_json_writer_u64(&w, (unsigned)launch_pid);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "lastLogPath");
    if (log_path && *log_path)
        ms_json_writer_string(&w, log_path);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "lastError");
    ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}

static char* game_json(const char* id, const char* title, const char* status) {
    return game_json_ex(id, title, status, "windows", NULL, NULL, 0, 0, NULL);
}

static char* gog_metadata_game_json(const char* raw_game, const ms_json* metadata) {
    char error[96];
    ms_json* game = ms_json_parse(raw_game ? raw_game : "{}", raw_game ? strlen(raw_game) : 2, error, sizeof(error));
    const ms_json* images = metadata ? ms_json_object_get(metadata, "images") : NULL;
    char *background = NULL, *icon = NULL, *slug = NULL;
    ms_json_writer writer;
    char* result;
    if (!game || ms_json_type_of(game) != MS_JSON_OBJECT) {
        ms_json_free(game);
        return strdup(raw_game ? raw_game : "{}");
    }
    if (images && ms_json_type_of(images) == MS_JSON_OBJECT) {
        (void)ms_json_as_string(ms_json_object_get(images, "background"), &background);
        (void)ms_json_as_string(ms_json_object_get(images, "icon"), &icon);
    }
    (void)ms_json_as_string(metadata ? ms_json_object_get(metadata, "slug") : NULL, &slug);
    if (background && background[0] == '/' && background[1] == '/') {
        char* normalized = malloc(strlen(background) + 7);
        if (normalized) {
            sprintf(normalized, "https:%s", background);
            free(background);
            background = normalized;
        }
    }
    if (icon && icon[0] == '/' && icon[1] == '/') {
        char* normalized = malloc(strlen(icon) + 7);
        if (normalized) {
            sprintf(normalized, "https:%s", icon);
            free(icon);
            icon = normalized;
        }
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < ms_json_object_length(game); i++) {
        const char* key = ms_json_object_key_at(game, i);
        const ms_json* value = ms_json_object_value_at(game, i);
        ms_json_writer_key(&writer, key);
        if (!strcmp(key, "imageUrl") && background)
            ms_json_writer_string(&writer, background);
        else if (!strcmp(key, "iconUrl") && icon)
            ms_json_writer_string(&writer, icon);
        else if (!strcmp(key, "slug") && slug)
            ms_json_writer_string(&writer, slug);
        else {
            char* encoded = ms_json_stringify(value);
            ms_json_writer_raw(&writer, encoded ? encoded : "null");
            free(encoded);
        }
    }
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(background);
    free(icon);
    free(slug);
    ms_json_free(game);
    return result;
}

static char* refresh_game_json(const char* home, const ms_json* game) {
    char *id = field(game, "productId", ""), *root = field(game, "installRoot", ""), *folder = NULL,
         *status = field(game, "status", ""), *serialized;
    long long pid_value = 0;
    bool installed, running;
    ms_json_writer writer;
    if (!*root) {
        root = join(home, "gog-games");
        if (root) {
            char* with_id = join(root, id);
            free(root);
            root = with_id;
        }
    }
    if (root && *id)
        folder = gog_import_folder(root, id);
    installed = folder != NULL;
    (void)ms_json_as_i64(ms_json_object_get(game, "lastLaunchPid"), &pid_value);
    running = pid_value > 0 && kill((pid_t)pid_value, 0) == 0;
    if (running) {
        free(status);
        status = strdup("running");
    } else if (installed && (!*status || !strcmp(status, "not_installed") || !strcmp(status, "downloading"))) {
        free(status);
        status = strdup("installed");
    } else if (!*status) {
        free(status);
        status = strdup("not_installed");
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < ms_json_object_length(game); i++) {
        const char* key = ms_json_object_key_at(game, i);
        const ms_json* value = ms_json_object_value_at(game, i);
        ms_json_writer_key(&writer, key);
        if (!strcmp(key, "installRoot") && installed)
            ms_json_writer_string(&writer, root);
        else if (!strcmp(key, "gameFolder") && installed)
            ms_json_writer_string(&writer, folder);
        else if (!strcmp(key, "installed"))
            ms_json_writer_bool(&writer, installed);
        else if (!strcmp(key, "running"))
            ms_json_writer_bool(&writer, running);
        else if (!strcmp(key, "status"))
            ms_json_writer_string(&writer, status);
        else {
            char* raw = ms_json_stringify(value);
            ms_json_writer_raw(&writer, raw ? raw : "null");
            free(raw);
        }
    }
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    free(id);
    free(root);
    free(folder);
    free(status);
    return serialized;
}

static char* gog_sync_library(const char* home) {
    char error[160], *body = fetch_gog_owned(home, error, sizeof(error)), parse_error[128];
    ms_json *response = NULL, *existing = NULL, *games = NULL, *result = NULL;
    ms_json_writer writer;
    char* serialized = NULL;
    unsigned long long now = (unsigned long long)time(NULL);
    if (!body)
        return err(error);
    response = ms_json_parse(body, strlen(body), parse_error, sizeof(parse_error));
    free(body);
    games = response && ms_json_type_of(response) == MS_JSON_OBJECT ? (ms_json*)ms_json_object_get(response, "owned")
                                                                    : NULL;
    if (!games || ms_json_type_of(games) != MS_JSON_ARRAY) {
        ms_json_free(response);
        return err("failed to parse GOG library response");
    }
    existing = load_games(home);
    ms_json_writer_init(&writer);
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < ms_json_array_length(games); i++) {
        const ms_json* id_value = ms_json_array_get(games, i);
        char id[181];
        double number;
        char* text = NULL;
        const ms_json* previous = NULL;
        if (ms_json_as_string(id_value, &text)) {
            snprintf(id, sizeof(id), "%s", text);
            free(text);
        } else if (ms_json_as_number(id_value, &number) && number >= 0)
            snprintf(id, sizeof(id), "%.0f", number);
        else
            continue;
        if (!valid_product_id(id))
            continue;
        for (size_t j = 0; j < ms_json_array_length(existing); j++) {
            char* old_id = field(ms_json_array_get(existing, j), "productId", "");
            if (!strcmp(old_id, id))
                previous = ms_json_array_get(existing, j);
            free(old_id);
            if (previous)
                break;
        }
        {
            char title[190];
            char url[256];
            char metadata_error[160];
            char* metadata_raw;
            char* metadata_title = NULL;
            char* base = NULL;
            char* enriched = NULL;
            ms_json* metadata = NULL;
            snprintf(title, sizeof(title), "GOG %s", id);
            snprintf(url, sizeof(url), "https://api.gog.com/products/%s", id);
            metadata_raw = fetch_gog_url(home, url, false, metadata_error, sizeof(metadata_error));
            if (metadata_raw) {
                metadata = ms_json_parse(metadata_raw, strlen(metadata_raw), parse_error, sizeof(parse_error));
                free(metadata_raw);
                if (metadata) {
                    metadata_title = field(metadata, "title", "");
                    if (metadata_title && *metadata_title)
                        snprintf(title, sizeof(title), "%s", metadata_title);
                }
            }
            if (previous)
                base = ms_json_stringify(previous);
            else
                base = game_json(id, title, "not_installed");
            enriched = metadata ? gog_metadata_game_json(base, metadata) : NULL;
            ms_json_writer_raw(&writer, enriched ? enriched : (base ? base : "{}"));
            free(enriched);
            free(base);
            free(metadata_title);
            ms_json_free(metadata);
        }
    }
    ms_json_writer_array_end(&writer);
    serialized = ms_json_writer_take(&writer);
    result = serialized ? ms_json_parse(serialized, strlen(serialized), parse_error, sizeof(parse_error)) : NULL;
    if (!result || !save_games_at(home, result, now)) {
        free(serialized);
        ms_json_free(result);
        ms_json_free(existing);
        ms_json_free(response);
        return err("failed to write GOG library cache");
    }
    free(serialized);
    ms_json_free(result);
    ms_json_free(existing);
    ms_json_free(response);
    return ms_gog_games_json(home);
}

static bool append_game(const char* home, const char* raw) {
    ms_json *a = load_games(home), *newa, *incoming;
    ms_json_writer w;
    char *serial, *incoming_id;
    char e[64];
    bool ok, replaced = false;
    if (!a)
        return false;
    incoming = ms_json_parse(raw ? raw : "{}", raw ? strlen(raw) : 2, e, sizeof(e));
    incoming_id = field(incoming, "productId", "");
    ms_json_writer_init(&w);
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < ms_json_array_length(a); i++) {
        const ms_json* old_game = ms_json_array_get(a, i);
        char* old_id = field(old_game, "productId", "");
        if (strcmp(old_id, incoming_id) != 0) {
            char* old = ms_json_stringify(old_game);
            ms_json_writer_raw(&w, old ? old : "{}");
            free(old);
        } else if (incoming && old_game) {
            if (replaced) {
                free(old_id);
                continue;
            }
            replaced = true;
            ms_json_writer_object_begin(&w);
            for (size_t j = 0; j < ms_json_object_length(incoming); j++) {
                const char* key = ms_json_object_key_at(incoming, j);
                const ms_json* value = ms_json_object_value_at(incoming, j);
                const ms_json* preserved = value;
                if ((!strcmp(key, "title") && ms_json_type_of(value) == MS_JSON_STRING) ||
                    ((!strcmp(key, "slug") || !strcmp(key, "imageUrl") || !strcmp(key, "iconUrl") ||
                      !strcmp(key, "primaryExe") || !strcmp(key, "primaryTaskName") ||
                      !strcmp(key, "downloadSizeBytes") || !strcmp(key, "diskSizeBytes")) &&
                     ms_json_type_of(value) == MS_JSON_NULL)) {
                    char* incoming_value = NULL;
                    if (!strcmp(key, "title") && ms_json_as_string(value, &incoming_value) &&
                        !strcmp(incoming_value, incoming_id))
                        preserved = ms_json_object_get(old_game, key);
                    else if (ms_json_type_of(value) == MS_JSON_NULL && ms_json_object_get(old_game, key))
                        preserved = ms_json_object_get(old_game, key);
                    free(incoming_value);
                }
                ms_json_writer_key(&w, key);
                {
                    char* serialized_value = ms_json_stringify(preserved ? preserved : value);
                    ms_json_writer_raw(&w, serialized_value ? serialized_value : "null");
                    free(serialized_value);
                }
            }
            ms_json_writer_object_end(&w);
        }
        free(old_id);
    }
    if (!replaced)
        ms_json_writer_raw(&w, raw ? raw : "{}");
    ms_json_writer_array_end(&w);
    serial = ms_json_writer_take(&w);
    newa = serial ? ms_json_parse(serial, strlen(serial), e, sizeof(e)) : NULL;
    ok = newa && save_games(home, newa);
    free(serial);
    free(incoming_id);
    ms_json_free(incoming);
    ms_json_free(newa);
    ms_json_free(a);
    return ok;
}
static bool remove_game(const char* home, const char* id) {
    ms_json *a = load_games(home), *newa;
    ms_json_writer w;
    char* serial;
    char e[64];
    bool found = false, ok = false;
    if (!a)
        return false;
    ms_json_writer_init(&w);
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < ms_json_array_length(a); i++) {
        char* game_id = field(ms_json_array_get(a, i), "productId", "");
        if (!strcmp(game_id, id))
            found = true;
        else {
            char* old = ms_json_stringify(ms_json_array_get(a, i));
            ms_json_writer_raw(&w, old ? old : "{}");
            free(old);
        }
        free(game_id);
    }
    ms_json_writer_array_end(&w);
    serial = ms_json_writer_take(&w);
    newa = serial ? ms_json_parse(serial, strlen(serial), e, sizeof(e)) : NULL;
    ok = found && newa && save_games(home, newa);
    free(serial);
    ms_json_free(newa);
    ms_json_free(a);
    return ok;
}

static char* single_game_json(const char* home, const char* id) {
    ms_json* games = load_games(home);
    ms_json_writer w;
    char* result = NULL;
    size_t i;
    if (!games)
        return err("failed to read GOG library");
    for (i = 0; i < ms_json_array_length(games); i++) {
        char* candidate = field(ms_json_array_get(games, i), "productId", "");
        if (!strcmp(candidate, id)) {
            char* raw = ms_json_stringify(ms_json_array_get(games, i));
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "game");
            ms_json_writer_raw(&w, raw ? raw : "{}");
            ms_json_writer_object_end(&w);
            result = ms_json_writer_take(&w);
            free(raw);
            free(candidate);
            break;
        }
        free(candidate);
    }
    ms_json_free(games);
    return result ? result : err("game not found");
}

static char* gog_progress_json(const char* home, const char* id) {
    ms_json* games = load_games(home);
    ms_json_writer w;
    char* result = NULL;
    for (size_t i = 0; games && i < ms_json_array_length(games); i++) {
        char* candidate = field(ms_json_array_get(games, i), "productId", "");
        if (!strcmp(candidate, id)) {
            const ms_json* game = ms_json_array_get(games, i);
            bool installed = false;
            long long pid_value = 0;
            char* log_path = field(game, "lastLogPath", "");
            char* log = *log_path ? read_text(log_path) : NULL;
            double percent = installed ? 100.0 : 0.0;
            ms_json_as_bool(ms_json_object_get(game, "installed"), &installed);
            percent = installed ? 100.0 : 0.0;
            ms_json_as_i64(ms_json_object_get(game, "lastInstallPid"), &pid_value);
            if (log) {
                char* cursor = log;
                while ((cursor = strstr(cursor, "Progress:")) != NULL) {
                    char* end;
                    double value = strtod(cursor + 9, &end);
                    if (end != cursor + 9)
                        percent = value;
                    cursor = end;
                }
            }
            long long exit_code = 0;
            bool has_exit_code = false;
            if (log) {
                char* marker = strstr(log, "gogdl exited with Some(");
                if (marker) {
                    char* end;
                    exit_code = strtoll(marker + 23, &end, 10);
                    has_exit_code = end != marker + 23;
                }
            }
            bool active = pid_value > 0 && (kill((pid_t)pid_value, 0) == 0 || errno == EPERM);
            char* response_game = NULL;
            char* current_status = field(game, "status", "");
            if (!active && has_exit_code && !strcmp(current_status, "downloading")) {
                char* root = field(game, "installRoot", "");
                char* title = field(game, "title", id);
                char* folder = *root ? gog_import_folder(root, id) : NULL;
                if (exit_code == 0 && folder && gogdl_available(home) && run_gogdl_import(home, id, folder))
                    response_game =
                        game_json_ex(id, title, "installed", "windows", root, folder, (pid_t)pid_value, 0, log_path);
                else
                    response_game = game_json_ex(id, title, "install_failed", "windows", root, folder, 0, 0, log_path);
                if (response_game)
                    append_game(home, response_game);
                free(folder);
                free(root);
                free(title);
            }
            free(current_status);
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "productId");
            ms_json_writer_string(&w, id);
            ms_json_writer_key(&w, "percent");
            ms_json_writer_double(&w, percent);
            ms_json_writer_key(&w, "active");
            ms_json_writer_bool(&w, active);
            ms_json_writer_key(&w, "exitCode");
            if (has_exit_code)
                ms_json_writer_i64(&w, exit_code);
            else
                ms_json_writer_null(&w);
            ms_json_writer_key(&w, "logPath");
            if (*log_path)
                ms_json_writer_string(&w, log_path);
            else
                ms_json_writer_null(&w);
            ms_json_writer_key(&w, "game");
            char* raw = response_game ? strdup(response_game) : ms_json_stringify(game);
            ms_json_writer_raw(&w, raw ? raw : "{}");
            ms_json_writer_object_end(&w);
            result = ms_json_writer_take(&w);
            free(raw);
            free(response_game);
            free(log);
            free(log_path);
            free(candidate);
            break;
        }
        free(candidate);
    }
    ms_json_free(games);
    return result ? result : err("game not found");
}

static char* gog_stop_json(const char* home, const char* id) {
    ms_json* games = load_games(home);
    for (size_t i = 0; games && i < ms_json_array_length(games); i++) {
        char* candidate = field(ms_json_array_get(games, i), "productId", "");
        if (!strcmp(candidate, id)) {
            pid_t tracked = gog_tracked_pid(id);
            bool installed = false;
            ms_json_as_bool(ms_json_object_get(ms_json_array_get(games, i), "installed"), &installed);
            if (tracked > 0) {
                kill(tracked, SIGTERM);
            }
            char* title = field(ms_json_array_get(games, i), "title", id);
            char* stopped_game = game_json(id, title, installed ? "installed" : "not_installed");
            if (stopped_game)
                append_game(home, stopped_game);
            ms_json_writer w;
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "killedPids");
            ms_json_writer_array_begin(&w);
            if (tracked > 0)
                ms_json_writer_u64(&w, (unsigned)tracked);
            ms_json_writer_array_end(&w);
            ms_json_writer_key(&w, "game");
            if (stopped_game)
                ms_json_writer_raw(&w, stopped_game);
            else
                ms_json_writer_null(&w);
            ms_json_writer_object_end(&w);
            free(stopped_game);
            free(title);
            free(candidate);
            ms_json_free(games);
            return ms_json_writer_take(&w);
        }
        free(candidate);
    }
    ms_json_free(games);
    return err("game not found");
}

char* ms_gog_action_json(const char* home, const char* action, const unsigned char* body, size_t len) {
    char e[96];
    ms_json* j = NULL;
    const ms_json* v;
    char *s = NULL, *title = NULL, *raw = NULL;
    if (!strcmp(action, "auth-code")) {
        j = ms_json_parse(body ? (const char*)body : "", body ? len : 0, e, sizeof(e));
        v = j ? ms_json_object_get(j, "code") : NULL;
        if (!v || !ms_json_as_string(v, &s) || !s[0]) {
            free(s);
            ms_json_free(j);
            return gog_error_with_status(home, "missing authorization code");
        }
        if (!gogdl_available(home)) {
            free(s);
            ms_json_free(j);
            return gog_error_with_status(home, "gogdl binary not found; install it or set METALSHARP_GOGDL_BIN");
        }
        if (!run_gogdl_auth(home, s)) {
            free(s);
            ms_json_free(j);
            return gog_error_with_status(home, "gogdl auth failed");
        }
        free(s);
        ms_json_free(j);
        char* auth_file = auth_path(home);
        bool auth_written = auth_file && access(auth_file, F_OK) == 0;
        free(auth_file);
        if (!auth_written)
            return gog_error_with_status(home, "gogdl auth did not write auth.json");
        {
            ms_json_writer w;
            char* status_value = status_json(home);
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "authenticated");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "status");
            ms_json_writer_raw(&w, status_value ? status_value : "{}");
            ms_json_writer_object_end(&w);
            free(status_value);
            return ms_json_writer_take(&w);
        }
    }
    if (!strcmp(action, "logout")) {
        char* p = auth_path(home);
        if (p)
            unlink(p);
        free(p);
        return ms_gog_status_json(home);
    }
    if (!strcmp(action, "initialize-prefix")) {
        char error[256];
        if (!ensure_gogdl_available(home, error, sizeof(error)))
            return gog_error_with_status(home, error);
        if (!gog_initialize_prefix(home, error, sizeof(error)))
            return gog_error_with_status(home, error);
        return ms_gog_status_json(home);
    }
    if (!strcmp(action, "remove-prefix")) {
        char* p = join(home, "bottles/gog-prefix");
        bool ok = p && remove_tree(p);
        free(p);
        if (!ok)
            return err("failed to remove GOG prefix");
        return ms_gog_status_json(home);
    }
    if (!strcmp(action, "sync")) {
        if (!gogdl_available(home))
            return gog_error_with_status(home, "gogdl binary not found; install it or set METALSHARP_GOGDL_BIN");
        if (!authenticated(home))
            return gog_error_with_status(home, "GOG is not authenticated");
        return gog_sync_library(home);
    }
    if (!strcmp(action, "games"))
        return ms_gog_games_json(home);
    bool needs_id = !strcmp(action, "install") || !strcmp(action, "import") || !strcmp(action, "progress") ||
                    !strcmp(action, "play") || !strcmp(action, "stop") || !strcmp(action, "uninstall");
    if (needs_id) {
        j = ms_json_parse(body ? (const char*)body : "", body ? len : 0, e, sizeof(e));
        v = j ? ms_json_object_get(j, "productId") : NULL;
        if (!v)
            v = j ? ms_json_object_get(j, "id") : NULL;
        if (!v || !ms_json_as_string(v, &s) || !s[0]) {
            free(s);
            ms_json_free(j);
            return err("missing productId");
        }
        if (!strcmp(action, "uninstall")) {
            char *root = NULL, *folder = NULL;
            ms_json* games = load_games(home);
            if (games) {
                for (size_t i = 0; i < ms_json_array_length(games); i++) {
                    char* candidate = field(ms_json_array_get(games, i), "productId", "");
                    if (!strcmp(candidate, s)) {
                        root = field(ms_json_array_get(games, i), "installRoot", "");
                        break;
                    }
                    free(candidate);
                }
            }
            if (root && *root)
                folder = gog_import_folder(root, s);
            if (folder && !safe_gog_install_path(folder, home)) {
                free(folder);
                free(root);
                ms_json_free(games);
                free(s);
                ms_json_free(j);
                return err("refusing to remove unsafe GOG install path");
            }
            if (folder && !remove_tree(folder)) {
                free(folder);
                free(root);
                ms_json_free(games);
                free(s);
                ms_json_free(j);
                return err("failed to remove GOG install folder");
            }
            if (!clear_gogdl_manifest(home, s)) {
                free(folder);
                free(root);
                ms_json_free(games);
                free(s);
                ms_json_free(j);
                return err("failed to clear GOG download manifest");
            }
            bool ok = remove_game(home, s);
            char* removed_path = folder ? strdup(folder) : NULL;
            free(folder);
            free(root);
            ms_json_free(games);
            if (!ok) {
                free(s);
                ms_json_free(j);
                free(removed_path);
                return err("game not found");
            }
            char* removed_game = game_json(s, s, "not_installed");
            ms_json_writer response;
            free(s);
            ms_json_free(j);
            ms_json_writer_init(&response);
            ms_json_writer_object_begin(&response);
            ms_json_writer_key(&response, "ok");
            ms_json_writer_bool(&response, true);
            ms_json_writer_key(&response, "removedPath");
            if (removed_path)
                ms_json_writer_string(&response, removed_path);
            else
                ms_json_writer_null(&response);
            ms_json_writer_key(&response, "game");
            ms_json_writer_raw(&response, removed_game ? removed_game : "{}");
            ms_json_writer_object_end(&response);
            free(removed_game);
            free(removed_path);
            return ms_json_writer_take(&response);
        }
        if (!strcmp(action, "progress")) {
            char* result = gog_progress_json(home, s);
            free(s);
            ms_json_free(j);
            return result;
        }
        if (!strcmp(action, "stop")) {
            char* result = gog_stop_json(home, s);
            free(s);
            ms_json_free(j);
            return result;
        }
        if (!valid_product_id(s)) {
            free(s);
            ms_json_free(j);
            return err("invalid productId");
        }
        title = field(j, "title", s);
        if (!strcmp(action, "play") && !gogdl_available(home)) {
            free(s);
            free(title);
            ms_json_free(j);
            return err("gogdl binary not found; install it or set METALSHARP_GOGDL_BIN");
        }
        if (!strcmp(action, "play")) {
            char *folder = NULL, *install_root = NULL, *platform = NULL, *stored_title = NULL;
            char* engine = field(j, "engine", "auto");
            pid_t launch_pid;
            char* log_path = NULL;
            if (!gog_launch_record(home, s, &stored_title, &platform, &install_root, &folder)) {
                free(stored_title);
                free(folder);
                free(install_root);
                free(platform);
                free(engine);
                free(s);
                free(title);
                ms_json_free(j);
                return err("game is not installed or imported");
            }
            free(title);
            title = stored_title;
            if (!platform || (strcmp(platform, "windows") && strcmp(platform, "osx") && strcmp(platform, "linux"))) {
                free(folder);
                free(install_root);
                free(platform);
                free(engine);
                free(s);
                free(title);
                ms_json_free(j);
                return err("platform must be windows, osx, or linux");
            }
            char* prefix = join(home, "bottles/gog-prefix/prefix");
            bool started = prefix && mkdir_p(prefix) &&
                           spawn_gogdl_launch(home, s, platform, folder, engine, &launch_pid, &log_path);
            if (!started) {
                free(prefix);
                free(folder);
                free(install_root);
                free(platform);
                free(engine);
                free(log_path);
                free(s);
                free(title);
                ms_json_free(j);
                return err("failed to start GOG launch");
            }
            gog_track_pid(s, launch_pid);
            raw = game_json_ex(s, title, "running", platform, install_root, folder, 0, launch_pid, log_path);
            bool ok = raw && append_game(home, raw);
            free(raw);
            if (!ok) {
                free(prefix);
                free(folder);
                free(install_root);
                free(platform);
                free(engine);
                free(log_path);
                free(s);
                free(title);
                ms_json_free(j);
                return err("failed to save GOG library");
            }
            ms_json_writer response;
            char* game = game_json_ex(s, title, "running", platform, install_root, folder, 0, launch_pid, log_path);
            ms_json_writer_init(&response);
            ms_json_writer_object_begin(&response);
            ms_json_writer_key(&response, "ok");
            ms_json_writer_bool(&response, true);
            ms_json_writer_key(&response, "pid");
            ms_json_writer_u64(&response, (unsigned)launch_pid);
            ms_json_writer_key(&response, "logPath");
            ms_json_writer_string(&response, log_path);
            ms_json_writer_key(&response, "winePrefix");
            ms_json_writer_string(&response, prefix);
            ms_json_writer_key(&response, "game");
            ms_json_writer_raw(&response, game ? game : "{}");
            ms_json_writer_object_end(&response);
            free(game);
            free(prefix);
            free(folder);
            free(install_root);
            free(platform);
            free(engine);
            free(log_path);
            free(s);
            free(title);
            ms_json_free(j);
            return ms_json_writer_take(&response);
        }
        if (!strcmp(action, "import")) {
            char* import_root = field(j, "installPath", "");
            if (*import_root) {
                char* folder = gog_import_folder(import_root, s);
                if (!folder) {
                    char message[512];
                    snprintf(message, sizeof(message), "goggame-%s.info not found under %s", s, import_root);
                    free(import_root);
                    free(s);
                    free(title);
                    ms_json_free(j);
                    return err(message);
                }
                if (!gogdl_available(home) || !run_gogdl_import(home, s, folder)) {
                    free(folder);
                    free(import_root);
                    free(s);
                    free(title);
                    ms_json_free(j);
                    return err("gogdl import failed");
                }
                raw = game_json_ex(s, title, "installed", "windows", import_root, folder, 0, 0, NULL);
                bool imported_ok = raw && append_game(home, raw);
                free(raw);
                if (!imported_ok) {
                    free(folder);
                    free(import_root);
                    free(s);
                    free(title);
                    ms_json_free(j);
                    return err("failed to save GOG library");
                }
                char* game = game_json_ex(s, title, "installed", "windows", import_root, folder, 0, 0, NULL);
                ms_json_writer response;
                ms_json_writer_init(&response);
                ms_json_writer_object_begin(&response);
                ms_json_writer_key(&response, "ok");
                ms_json_writer_bool(&response, true);
                ms_json_writer_key(&response, "game");
                ms_json_writer_raw(&response, game ? game : "{}");
                ms_json_writer_object_end(&response);
                free(game);
                free(folder);
                free(import_root);
                free(s);
                free(title);
                ms_json_free(j);
                return ms_json_writer_take(&response);
            }
            free(import_root);
            raw = game_json(s, title, "not_installed");
            bool ok = raw && append_game(home, raw);
            free(raw);
            if (!ok) {
                free(s);
                free(title);
                ms_json_free(j);
                return err("failed to save GOG library");
            }
        }
        if (!strcmp(action, "install")) {
            char* platform = field(j, "platform", "windows");
            char* install_root = field(j, "installPath", "");
            char default_root[2048];
            pid_t install_pid;
            char* log_path = NULL;
            if (!*install_root) {
                snprintf(default_root, sizeof(default_root), "%s/gog-games/%s", home, s);
                free(install_root);
                install_root = strdup(default_root);
            }
            char* language = field(j, "language", "");
            if (!platform || (strcmp(platform, "windows") && strcmp(platform, "osx") && strcmp(platform, "linux"))) {
                free(platform);
                free(install_root);
                free(language);
                free(s);
                free(title);
                ms_json_free(j);
                return err("platform must be windows, osx, or linux");
            }
            if (!gogdl_available(home)) {
                free(platform);
                free(install_root);
                free(language);
                free(s);
                free(title);
                ms_json_free(j);
                return err("gogdl binary not found; install it or set METALSHARP_GOGDL_BIN");
            }
            bool started = false;
            if (install_root && mkdir_p(install_root)) {
                char* existing_folder = gog_import_folder(install_root, s);
                /* A previous uninstall could have removed the files but left gogdl's
                 * product manifest behind.  Let gogdl build a fresh manifest when
                 * there is no intact installation to resume. */
                if (!existing_folder)
                    clear_gogdl_manifest(home, s);
                free(existing_folder);
                started = spawn_gogdl_download(home, s, platform, install_root, language, &install_pid, &log_path);
            }
            free(language);
            if (!started) {
                free(platform);
                free(install_root);
                free(log_path);
                free(s);
                free(title);
                ms_json_free(j);
                return err("failed to start GOG download");
            }
            gog_track_pid(s, install_pid);
            raw = game_json_ex(s, title, "downloading", platform, install_root, NULL, install_pid, 0, log_path);
            bool ok = raw && append_game(home, raw);
            free(raw);
            if (!ok) {
                free(platform);
                free(install_root);
                free(log_path);
                free(s);
                free(title);
                ms_json_free(j);
                return err("failed to save GOG library");
            }
            ms_json_writer response;
            char* game = game_json_ex(s, title, "downloading", platform, install_root, NULL, install_pid, 0, log_path);
            ms_json_writer_init(&response);
            ms_json_writer_object_begin(&response);
            ms_json_writer_key(&response, "ok");
            ms_json_writer_bool(&response, true);
            ms_json_writer_key(&response, "pid");
            ms_json_writer_u64(&response, (unsigned)install_pid);
            ms_json_writer_key(&response, "logPath");
            ms_json_writer_string(&response, log_path);
            ms_json_writer_key(&response, "game");
            ms_json_writer_raw(&response, game ? game : "{}");
            ms_json_writer_object_end(&response);
            free(game);
            free(platform);
            free(install_root);
            free(log_path);
            free(s);
            free(title);
            ms_json_free(j);
            return ms_json_writer_take(&response);
        }
        {
            char* result = single_game_json(home, s);
            free(s);
            free(title);
            ms_json_free(j);
            return result;
        }
    }
    ms_json_free(j);
    return strdup("{\"ok\":true}");
}
