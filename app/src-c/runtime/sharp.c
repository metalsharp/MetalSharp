#include "metalsharp_backend/sharp.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
static bool save_array(const char* home, const ms_json* a) {
    char *d = join(home, "sharp-library"), *p, *raw;
    FILE* f;
    if (!d || !mkdir_p(d)) {
        free(d);
        return false;
    }
    p = join(d, "library.json");
    raw = ms_json_stringify(a);
    f = p ? fopen(p, "wb") : NULL;
    if (!f || !raw || fputs(raw, f) < 0) {
        if (f)
            fclose(f);
        free(d);
        free(p);
        free(raw);
        return false;
    }
    fclose(f);
    free(d);
    free(p);
    free(raw);
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
static char* new_id(void) {
    char b[80];
    snprintf(b, sizeof(b), "sharp_%llu", (unsigned long long)time(NULL) * 1000ULL + (unsigned long long)getpid());
    return strdup(b);
}
static char* app_json(const char* id, const char* name, const char* exe, const char* dir, const char* bottle_id) {
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
    ms_json_writer_string(&w, "auto");
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
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
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

typedef struct {
    const char* id;
    const char* name;
    const char* bottle_id;
    const char* url;
    const char* filename;
    bool msi;
    const char* executable_path;
    const char* fallback_executable_path;
} launcher_installer;

static const launcher_installer launcher_installers[] = {
    {"ea", "EA App", "EA-Prefix",
     "https://origin-a.akamaihd.net/EA-Desktop-Client-Download/installer-releases/EAappInstaller.exe",
     "EAappInstaller.exe", false, "drive_c/Program Files/Electronic Arts/EA Desktop/EA Desktop/EADesktop.exe",
     "drive_c/Program Files/Electronic Arts/EA Desktop/EA Desktop/EALauncher.exe"},
    {"rockstar", "Rockstar Games Launcher", "Rockstar-Prefix",
     "https://gamedownloads.rockstargames.com/public/installer/Rockstar-Games-Launcher.exe",
     "Rockstar-Games-Launcher.exe", false, "drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe", NULL},
    {"ubisoft", "Ubisoft Connect", "Ubisoft-Prefix", "https://ubi.li/4vxt9", "UbisoftConnectInstaller.exe", false,
     "drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/UbisoftConnect.exe",
     "drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/upc.exe"},
    {"battlenet", "Battle.net", "Battle-Net-Prefix",
     "https://us.battle.net/download/getInstaller?os=win&installer=Battle.net-Setup.exe", "Battle.net-Setup.exe", false,
     "drive_c/Program Files (x86)/Battle.net/Battle.net.exe",
     "drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe"},
};

static const launcher_installer* launcher_installer_for_id(const char* id) {
    if (!id)
        return NULL;
    for (size_t i = 0; i < sizeof(launcher_installers) / sizeof(launcher_installers[0]); i++)
        if (!strcmp(launcher_installers[i].id, id))
            return &launcher_installers[i];
    return NULL;
}

static bool write_launcher_bottle(const char* bottle_dir, const char* prefix, const char* installer_path,
                                  const char* log_path, const launcher_installer* launcher) {
    char *manifest = join(bottle_dir, "bottle.json"), *temporary = NULL, *raw = NULL;
    ms_json_writer writer;
    FILE* file = NULL;
    char stamp[32];
    bool ok = false;
    if (!manifest)
        return false;
    if (access(manifest, F_OK) == 0) {
        free(manifest);
        return true;
    }
    temporary = malloc(strlen(manifest) + 40);
    if (!temporary)
        goto done;
    snprintf(temporary, strlen(manifest) + 40, "%s.tmp.%ld", manifest, (long)getpid());
    snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "id");
    ms_json_writer_string(&writer, launcher->bottle_id);
    ms_json_writer_key(&writer, "name");
    ms_json_writer_string(&writer, launcher->name);
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
    ms_json_writer_string(&writer, "webview");
    ms_json_writer_key(&writer, "preferred_pipeline");
    ms_json_writer_string(&writer, "wine_bare");
    ms_json_writer_key(&writer, "installed_components");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "source_installer_path");
    ms_json_writer_string(&writer, installer_path);
    ms_json_writer_key(&writer, "source_installer_url");
    ms_json_writer_string(&writer, launcher->url);
    ms_json_writer_key(&writer, "installer_kind");
    ms_json_writer_string(&writer, launcher->msi ? "msi" : "exe");
    ms_json_writer_key(&writer, "game_install_path");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "runtime_assets");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "installed_app_detections");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "health");
    ms_json_writer_string(&writer, "needs_repair");
    ms_json_writer_key(&writer, "last_launch_log");
    ms_json_writer_string(&writer, log_path);
    ms_json_writer_key(&writer, "last_launch_pid");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "last_launch_status");
    ms_json_writer_string(&writer, "downloading");
    ms_json_writer_key(&writer, "last_launch_finished_at");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "created_at");
    ms_json_writer_string(&writer, stamp);
    ms_json_writer_key(&writer, "updated_at");
    ms_json_writer_string(&writer, stamp);
    ms_json_writer_object_end(&writer);
    raw = ms_json_writer_take(&writer);
    file = raw ? fopen(temporary, "wb") : NULL;
    if (!file || fputs(raw, file) < 0 || fflush(file) != 0 || fsync(fileno(file)) != 0)
        goto done;
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    if (rename(temporary, manifest) != 0)
        goto done;
    ok = true;
done:
    if (file)
        fclose(file);
    if (!ok && temporary)
        unlink(temporary);
    free(raw);
    free(temporary);
    free(manifest);
    return ok;
}

static int run_launcher_command(char* const argv[], const char* prefix, int log_fd) {
    int status = 0;
    pid_t pid = fork();
    if (pid == 0) {
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static pid_t run_launcher_command_async(char* const argv[], const char* prefix, int log_fd) {
    pid_t pid = fork();
    if (pid == 0) {
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static void reap_launcher_command(pid_t pid) {
    int status;
    if (pid <= 0)
        return;
    for (int attempt = 0; attempt < 100; attempt++) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno != EINTR))
            return;
        struct timespec wait = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&wait, NULL);
    }
    kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
}

static bool copy_regular_file_atomic(const char* source, const char* destination) {
    char* temporary;
    int input = -1, output = -1;
    bool ok = false;
    char buffer[1024 * 1024];
    ssize_t count;
    temporary = malloc(strlen(destination) + 32);
    if (!temporary)
        return false;
    snprintf(temporary, strlen(destination) + 32, "%s.tmp.%ld", destination, (long)getpid());
    input = open(source, O_RDONLY);
    output = open(temporary, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (input < 0 || output < 0)
        goto done;
    while ((count = read(input, buffer, sizeof(buffer))) > 0) {
        ssize_t written = 0;
        while (written < count) {
            ssize_t result = write(output, buffer + written, (size_t)(count - written));
            if (result <= 0)
                goto done;
            written += result;
        }
    }
    if (count != 0 || fsync(output) != 0 || close(output) != 0)
        goto done;
    output = -1;
    if (rename(temporary, destination) != 0)
        goto done;
    ok = true;
done:
    if (input >= 0)
        close(input);
    if (output >= 0)
        close(output);
    if (!ok)
        unlink(temporary);
    free(temporary);
    return ok;
}

static char* eos_extracted_msi(const char* root) {
    DIR* parent = opendir(root);
    struct dirent* entry;
    if (!parent)
        return NULL;
    while ((entry = readdir(parent)) != NULL) {
        DIR* child_dir;
        struct dirent* child_entry;
        char* child;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = join(root, entry->d_name);
        child_dir = child ? opendir(child) : NULL;
        if (!child_dir) {
            free(child);
            continue;
        }
        while ((child_entry = readdir(child_dir)) != NULL) {
            size_t length = strlen(child_entry->d_name);
            if (length > 4 && !strcasecmp(child_entry->d_name + length - 4, ".msi")) {
                char* result = join(child, child_entry->d_name);
                closedir(child_dir);
                closedir(parent);
                free(child);
                return result;
            }
        }
        closedir(child_dir);
        free(child);
    }
    closedir(parent);
    return NULL;
}

static void capture_eos_msi_worker(const char* extraction_root, const char* destination) {
    struct timespec short_wait = {.tv_sec = 0, .tv_nsec = 20000000};
    struct timespec stable_wait = {.tv_sec = 0, .tv_nsec = 100000000};
    for (int attempt = 0; attempt < 750; attempt++) {
        char* source = eos_extracted_msi(extraction_root);
        if (source) {
            struct stat first, second;
            if (stat(source, &first) == 0 && first.st_size > 16 * 1024 * 1024) {
                nanosleep(&stable_wait, NULL);
                if (stat(source, &second) == 0 && first.st_size == second.st_size &&
                    copy_regular_file_atomic(source, destination)) {
                    free(source);
                    _exit(0);
                }
            }
            free(source);
        }
        nanosleep(&short_wait, NULL);
    }
    _exit(1);
}

static char* windows_z_path(const char* path) {
    size_t length = strlen(path);
    char* result = malloc(length + 3);
    if (!result)
        return NULL;
    result[0] = 'Z';
    result[1] = ':';
    for (size_t i = 0; i <= length; i++)
        result[i + 2] = path[i] == '/' ? '\\' : path[i];
    return result;
}

static bool patch_eos_custom_actions(const char* idt_path) {
    static const char* actions[] = {"InitializeComponents", "CreateRegistryKeys", "RegisterProductID"};
    char* raw = read_text(idt_path);
    bool ok = raw != NULL;
    if (!raw)
        return false;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        char needle[96];
        char* match;
        snprintf(needle, sizeof(needle), "%s\t3073\t", actions[i]);
        match = strstr(raw, needle);
        if (!match) {
            ok = false;
            break;
        }
        memcpy(match + strlen(actions[i]) + 1, "3137", 4);
    }
    if (ok) {
        FILE* file = fopen(idt_path, "wb");
        ok = file && fputs(raw, file) >= 0 && fflush(file) == 0 && fsync(fileno(file)) == 0;
        if (file)
            fclose(file);
    }
    free(raw);
    return ok;
}

static char* idt_property(const char* property_path, const char* key) {
    char* raw = read_text(property_path);
    char* save = NULL;
    char* line;
    char* result = NULL;
    if (!raw)
        return NULL;
    for (line = strtok_r(raw, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        size_t key_length = strlen(key);
        if (!strncmp(line, key, key_length) && line[key_length] == '\t') {
            result = strdup(line + key_length + 1);
            break;
        }
    }
    free(raw);
    return result;
}

static int eos_reg_add(char* wine, const char* prefix, const char* reg_exe, const char* key, const char* value,
                       const char* type, const char* data, int log_fd) {
    char* const argv[] = {(char*)wine, (char*)reg_exe, "add", (char*)key,  "/v", (char*)value,
                          "/t",        (char*)type,    "/d",  (char*)data, "/f", NULL};
    return run_launcher_command(argv, prefix, log_fd);
}

static bool random_eos_session_guid(char output[37]) {
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    close(fd);
    bytes[6] = (unsigned char)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (unsigned char)((bytes[8] & 0x3f) | 0x80);
    snprintf(output, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", bytes[0], bytes[1],
             bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return true;
}

static bool downloaded_launcher_is_valid(const char* path, bool msi);
static bool regular_file(const char* path);

static bool epic_online_services_ready(const char* prefix, const char* bottle_installers) {
    char* marker = join(bottle_installers, "EpicOnlineServices.ready");
    char* user_helper =
        join(prefix, "drive_c/Program Files (x86)/Epic Games/Epic Online Services/EpicOnlineServicesUserHelper.exe");
    char* service_host = join(prefix, "drive_c/Program Files (x86)/Epic Games/Epic Online Services/service/"
                                      "EpicOnlineServicesHost.exe");
    bool ready = regular_file(marker) && regular_file(user_helper) && regular_file(service_host);
    free(marker);
    free(user_helper);
    free(service_host);
    return ready;
}

static bool prepare_epic_online_services(const char* home, char* wine, const char* prefix,
                                         const char* bottle_installers, int log_fd) {
    const char* reg32 = "C:\\windows\\syswow64\\reg.exe";
    const char* reg64 = "C:\\windows\\system32\\reg.exe";
    const char* sc = "C:\\windows\\system32\\sc.exe";
    char *eos_installer = join(
             prefix, "drive_c/Program Files/Epic Games/Launcher/Portal/Extras/EOS/EpicOnlineServicesInstaller.exe"),
         *extraction_root = join(prefix, "drive_c/ProgramData/Epic/EpicOnlineServices/EOSInstaller"),
         *captured = join(bottle_installers, "EpicOnlineServices.msi"),
         *patched = join(bottle_installers, "EpicOnlineServices-wine.msi"),
         *workspace = join(bottle_installers, "eos-msidb"), *custom_action = NULL, *property = NULL,
         *patched_windows = NULL, *workspace_windows = NULL, *product_code = NULL, *build_version = NULL,
         *user_helper = NULL, *service_host = NULL, *wineserver = NULL,
         *ready_marker = join(bottle_installers, "EpicOnlineServices.ready"),
         *lock_path = join(bottle_installers, "EpicOnlineServices.lock");
    pid_t watcher;
    int watcher_status = 0, status, lock_fd = -1;
    char session_guid[37];
    bool ok = false;
    if (!eos_installer || !extraction_root || !captured || !patched || !workspace || !ready_marker || !lock_path)
        goto done;
    lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0)
        goto done;
    {
        struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
        while (fcntl(lock_fd, F_SETLKW, &lock) != 0) {
            if (errno != EINTR)
                goto done;
        }
    }
    if (epic_online_services_ready(prefix, bottle_installers)) {
        dprintf(log_fd, "status=epic_online_services_already_ready\n");
        ok = true;
        goto done;
    }
    for (int attempt = 0; access(eos_installer, R_OK) != 0 && attempt < 3000; attempt++) {
        struct timespec wait = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&wait, NULL);
    }
    if (access(eos_installer, R_OK) != 0 || !remove_tree(workspace) || !mkdir_p(workspace))
        goto done;
    dprintf(log_fd, "stage=epic_online_services_capture\n");
    watcher = fork();
    if (watcher == 0)
        capture_eos_msi_worker(extraction_root, captured);
    if (watcher < 0)
        goto done;
    {
        char* const argv[] = {
            wine, (char*)eos_installer, "/upgrade", "productid=EpicGamesLauncher", "minversion=5.3.0", "/quiet", NULL};
        status = run_launcher_command(argv, prefix, log_fd);
    }
    while (waitpid(watcher, &watcher_status, 0) < 0 && errno == EINTR)
        ;
    dprintf(log_fd, "eos_probe_exit=%d\n", status);
    if (!WIFEXITED(watcher_status) || WEXITSTATUS(watcher_status) != 0 ||
        !downloaded_launcher_is_valid(captured, true) || !copy_regular_file_atomic(captured, patched))
        goto done;
    patched_windows = windows_z_path(patched);
    workspace_windows = windows_z_path(workspace);
    custom_action = join(workspace, "CustomAc.idt");
    property = join(workspace, "Property.idt");
    if (!patched_windows || !workspace_windows || !custom_action || !property)
        goto done;
    dprintf(log_fd, "stage=epic_online_services_patch\n");
    {
        char* const argv[] = {wine,
                              "C:\\windows\\system32\\msidb.exe",
                              "-d",
                              patched_windows,
                              "-f",
                              workspace_windows,
                              "-s",
                              "-e",
                              "CustomAction",
                              "Property",
                              NULL};
        /* msidb.exe is not installed into drive_c, but Wine resolves its builtin by name. */
        status = run_launcher_command(argv, prefix, log_fd);
    }
    if (status != 0 || !patch_eos_custom_actions(custom_action))
        goto done;
    product_code = idt_property(property, "ProductCode");
    build_version = idt_property(property, "EOSHBuildVersion");
    if (!product_code || !build_version)
        goto done;
    {
        char* const argv[] = {wine, "C:\\windows\\system32\\msidb.exe",
                              "-d", patched_windows,
                              "-f", workspace_windows,
                              "-i", "CustomAction",
                              NULL};
        status = run_launcher_command(argv, prefix, log_fd);
    }
    if (status != 0)
        goto done;
    dprintf(log_fd, "stage=epic_online_services_install\n");
    {
        char* const argv[] = {
            wine, "msiexec", "/i", patched_windows, "/qn", "EOSPRODUCTID=EpicGamesLauncher", "REBOOT=ReallySuppress",
            NULL};
        status = run_launcher_command(argv, prefix, log_fd);
    }
    if (status != 0)
        goto done;
    user_helper =
        join(prefix, "drive_c/Program Files (x86)/Epic Games/Epic Online Services/EpicOnlineServicesUserHelper.exe");
    service_host = join(prefix, "drive_c/Program Files (x86)/Epic Games/Epic Online Services/service/"
                                "EpicOnlineServicesHost.exe");
    if (!regular_file(user_helper) || !regular_file(service_host) || !random_eos_session_guid(session_guid))
        goto done;
    dprintf(log_fd, "stage=epic_online_services_register\n");
#define EOS_REG(key, value, type, data)                                                                                \
    do {                                                                                                               \
        if (eos_reg_add(wine, prefix, reg32, key, value, type, data, log_fd) != 0)                                     \
            goto done;                                                                                                 \
    } while (0)
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS", "MSIProductCode", "REG_SZ", product_code);
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\UserHelper", "Path", "REG_SZ",
            "C:\\Program Files (x86)\\Epic Games\\Epic Online Services\\EpicOnlineServicesUserHelper.exe");
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\UserHelper", "Version", "REG_SZ", "1.0.0");
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\UIHelper", "Path", "REG_SZ",
            "C:\\Program Files (x86)\\Epic Games\\Epic Online Services\\EpicOnlineServicesUIHelper.exe");
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\UIHelper", "Version", "REG_SZ", "1.0.0");
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\InstallHelper", "Path", "REG_SZ",
            "C:\\Program Files (x86)\\Epic Games\\Epic Online Services\\EpicOnlineServicesInstallHelper.exe");
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\InstallHelper", "Version", "REG_SZ", "1.0.0");
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\MainService", "Path", "REG_SZ",
            "C:\\Program Files (x86)\\Epic Games\\Epic Online Services\\service\\EpicOnlineServicesHost.exe");
    EOS_REG("HKLM\\SOFTWARE\\Epic Games\\EOS\\MainService", "Version", "REG_SZ", build_version);
#undef EOS_REG
    if (eos_reg_add(wine, prefix, reg64, "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                    "EOS_SESSION_GUID", "REG_SZ", session_guid, log_fd) != 0)
        goto done;
    {
        char* const query[] = {wine, (char*)sc, "query", "EpicOnlineServices", NULL};
        status = run_launcher_command(query, prefix, log_fd);
    }
    if (status != 0) {
        char* const create[] = {wine,
                                (char*)sc,
                                "create",
                                "EpicOnlineServices",
                                "binPath=",
                                "C:\\Program Files (x86)\\Epic Games\\Epic Online Services\\service\\"
                                "EpicOnlineServicesHost.exe",
                                "start=",
                                "demand",
                                "DisplayName=",
                                "Epic Online Services",
                                NULL};
        if (run_launcher_command(create, prefix, log_fd) != 0)
            goto done;
    }
    {
        char* const description[] = {wine,
                                     (char*)sc,
                                     "description",
                                     "EpicOnlineServices",
                                     "Runs background processes for applications using Epic Games services",
                                     NULL};
        (void)run_launcher_command(description, prefix, log_fd);
    }
    {
        char* const setup[] = {wine, user_helper, "--setup", "--msimode=NewInstall", NULL};
        status = run_launcher_command(setup, prefix, log_fd);
        if (status != 0 && status != 15)
            goto done;
    }
    {
        char* const verify[] = {
            wine, (char*)eos_installer, "/upgrade", "productid=EpicGamesLauncher", "minversion=5.3.0", "/quiet", NULL};
        if (run_launcher_command(verify, prefix, log_fd) != 0)
            goto done;
    }
    wineserver = join(home, "runtime/wine/bin/wineserver");
    if (!wineserver || access(wineserver, X_OK) != 0)
        goto done;
    {
        char* const stop_server[] = {wineserver, "-k", NULL};
        (void)run_launcher_command(stop_server, prefix, log_fd);
    }
    {
        char* const start[] = {wine, (char*)sc, "start", "EpicOnlineServices", NULL};
        if (run_launcher_command(start, prefix, log_fd) != 0)
            goto done;
    }
    {
        int marker_fd = open(ready_marker, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (marker_fd < 0 || dprintf(marker_fd, "product=%s\nversion=%s\n", product_code, build_version) < 0 ||
            fsync(marker_fd) != 0 || close(marker_fd) != 0) {
            if (marker_fd >= 0)
                close(marker_fd);
            unlink(ready_marker);
            goto done;
        }
    }
    dprintf(log_fd, "status=epic_online_services_ready\n");
    ok = true;
done:
    if (!ok)
        dprintf(log_fd, "status=epic_online_services_failed\n");
    remove_tree(workspace);
    free(eos_installer);
    free(extraction_root);
    free(captured);
    free(patched);
    free(workspace);
    free(custom_action);
    free(property);
    free(patched_windows);
    free(workspace_windows);
    free(product_code);
    free(build_version);
    free(user_helper);
    free(service_host);
    free(wineserver);
    free(ready_marker);
    free(lock_path);
    if (lock_fd >= 0)
        close(lock_fd);
    return ok;
}

static char* epic_game_root(const char* home) {
    char *base = join(home, "launcher-games/epic"), *location = base ? join(base, "location.txt") : NULL,
         *configured = location ? read_text(location) : NULL, *result = NULL;
    if (configured) {
        char* end = configured + strlen(configured);
        while (end > configured && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        size_t home_length = strlen(home);
        bool under_home =
            !strncmp(configured, home, home_length) && (configured[home_length] == 0 || configured[home_length] == '/');
        if (configured[0] == '/' && (under_home || !strncmp(configured, "/Volumes/", 9)))
            result = strdup(configured);
    }
    if (!result)
        result = base ? join(base, "games") : NULL;
    free(configured);
    free(location);
    free(base);
    return result;
}

static bool prepare_epic_game_install_permissions(const char* home, char* wine, const char* prefix,
                                                  const char* bottle_installers, int log_fd) {
    char *marker = join(bottle_installers, "EpicGameInstalls-v2.ready"), *game_root = epic_game_root(home),
         *dosdevices = join(prefix, "dosdevices"), *game_drive = dosdevices ? join(dosdevices, "g:") : NULL,
         *installer_drive = dosdevices ? join(dosdevices, "d:") : NULL,
         *installer_device = dosdevices ? join(dosdevices, "d::") : NULL,
         *wineserver = join(home, "runtime/wine/bin/wineserver");
    const char* reg64 = "C:\\windows\\system32\\reg.exe";
    const char* policy = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    bool ok = false;
    if (!marker || !game_root || !dosdevices || !game_drive || !installer_drive || !installer_device || !wineserver)
        goto done;
    if (regular_file(marker) && access(game_root, W_OK) == 0) {
        ok = true;
        goto done;
    }
    dprintf(log_fd, "stage=epic_game_install_permissions\n");
    if (!mkdir_p(game_root) || chmod(game_root, 0775) != 0 || !mkdir_p(dosdevices))
        goto done;
    {
        struct stat st;
        if (lstat(game_drive, &st) == 0) {
            if (!S_ISLNK(st.st_mode) || unlink(game_drive) != 0)
                goto done;
        } else if (errno != ENOENT) {
            goto done;
        }
        if (symlink(game_root, game_drive) != 0)
            goto done;
    }
    {
        char target[PATH_MAX];
        ssize_t length = readlink(installer_drive, target, sizeof(target) - 1);
        if (length > 0) {
            target[length] = 0;
            if (!strcmp(target, "/Volumes/Epic Games Launcher")) {
                unlink(installer_drive);
                unlink(installer_device);
            }
        }
    }
    if (eos_reg_add(wine, prefix, reg64, policy, "EnableLUA", "REG_DWORD", "0", log_fd) != 0 ||
        eos_reg_add(wine, prefix, reg64, policy, "ConsentPromptBehaviorAdmin", "REG_DWORD", "0", log_fd) != 0)
        goto done;
    {
        char* const stop_server[] = {wineserver, "-k", NULL};
        (void)run_launcher_command(stop_server, prefix, log_fd);
    }
    {
        char* const start_service[] = {wine, "C:\\windows\\system32\\sc.exe", "start", "EpicOnlineServices", NULL};
        if (run_launcher_command(start_service, prefix, log_fd) != 0)
            goto done;
    }
    {
        int marker_fd = open(marker, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (marker_fd < 0 || dprintf(marker_fd, "uac=disabled\ngameDrive=G:\\\ngameRoot=%s\n", game_root) < 0 ||
            fsync(marker_fd) != 0 || close(marker_fd) != 0) {
            if (marker_fd >= 0)
                close(marker_fd);
            unlink(marker);
            goto done;
        }
    }
    dprintf(log_fd, "status=epic_game_install_permissions_ready\n");
    ok = true;
done:
    if (!ok)
        dprintf(log_fd, "status=epic_game_install_permissions_failed\n");
    free(marker);
    free(game_root);
    free(dosdevices);
    free(game_drive);
    free(installer_drive);
    free(installer_device);
    free(wineserver);
    return ok;
}

static bool downloaded_launcher_is_valid(const char* path, bool msi) {
    struct stat st;
    unsigned char magic[4] = {0};
    FILE* file;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4096)
        return false;
    file = fopen(path, "rb");
    if (!file)
        return false;
    bool ok;
    if (msi) {
        static const unsigned char msi_magic[4] = {0xd0, 0xcf, 0x11, 0xe0};
        ok = fread(magic, 1, 4, file) == 4 && !memcmp(magic, msi_magic, sizeof(msi_magic));
    } else {
        ok = fread(magic, 1, 2, file) == 2 && magic[0] == 'M' && magic[1] == 'Z';
    }
    fclose(file);
    return ok;
}

static bool regular_file(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char* launcher_executable(const char* prefix, const launcher_installer* launcher) {
    char* path = join(prefix, launcher->executable_path);
    if (regular_file(path))
        return path;
    free(path);
    if (!launcher->fallback_executable_path)
        return NULL;
    path = join(prefix, launcher->fallback_executable_path);
    if (regular_file(path))
        return path;
    free(path);
    return NULL;
}

static int run_epic_client_with_online_services(const char* home, char* wine, const char* prefix,
                                                const char* bottle_installers, const char* executable, int log_fd) {
    char* eos_installer =
        join(prefix, "drive_c/Program Files/Epic Games/Launcher/Portal/Extras/EOS/EpicOnlineServicesInstaller.exe");
    pid_t bootstrap = -1;
    bool ready;
    if (!eos_installer)
        return -1;
    ready = epic_online_services_ready(prefix, bottle_installers);
    if (!ready && !regular_file(eos_installer)) {
        char* const bootstrap_argv[] = {wine, (char*)executable, NULL};
        dprintf(log_fd, "stage=epic_online_services_bootstrap\n");
        bootstrap = run_launcher_command_async(bootstrap_argv, prefix, log_fd);
        if (bootstrap < 0) {
            free(eos_installer);
            return -1;
        }
    }
    if (!ready && !prepare_epic_online_services(home, wine, prefix, bottle_installers, log_fd)) {
        char* wineserver = join(home, "runtime/wine/bin/wineserver");
        if (wineserver) {
            char* const stop_argv[] = {wineserver, "-k", NULL};
            (void)run_launcher_command(stop_argv, prefix, log_fd);
        }
        free(wineserver);
        reap_launcher_command(bootstrap);
        free(eos_installer);
        return -1;
    }
    if (!prepare_epic_game_install_permissions(home, wine, prefix, bottle_installers, log_fd)) {
        reap_launcher_command(bootstrap);
        free(eos_installer);
        return -1;
    }
    reap_launcher_command(bootstrap);
    free(eos_installer);
    dprintf(log_fd, "stage=epic_client_launch\n");
    {
        char* const launch_argv[] = {wine, (char*)executable, NULL};
        return run_launcher_command(launch_argv, prefix, log_fd);
    }
}

char* ms_sharp_launcher_status_json(const char* home) {
    ms_json_writer writer;
    char *bottles_dir = join(home, "bottles"), *result;
    if (!bottles_dir)
        return NULL;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "launchers");
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < sizeof(launcher_installers) / sizeof(launcher_installers[0]); i++) {
        const launcher_installer* launcher = &launcher_installers[i];
        char *bottle_dir = join(bottles_dir, launcher->bottle_id),
             *prefix = bottle_dir ? join(bottle_dir, "prefix") : NULL;
        char* executable = prefix ? launcher_executable(prefix, launcher) : NULL;
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "id");
        ms_json_writer_string(&writer, launcher->id);
        ms_json_writer_key(&writer, "prefixCreated");
        ms_json_writer_bool(&writer, prefix && access(prefix, F_OK) == 0);
        ms_json_writer_key(&writer, "installed");
        ms_json_writer_bool(&writer, executable != NULL);
        ms_json_writer_object_end(&writer);
        free(executable);
        free(prefix);
        free(bottle_dir);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(bottles_dir);
    return result;
}

static void run_launcher_target_worker(const char* home, const launcher_installer* launcher, const char* prefix,
                                       const char* target, bool setup, const char* log_path) {
    char *wine = NULL, *working_dir = NULL, *bottle_dir = NULL, *installers_dir = NULL, *epic_executable = NULL;
    int log_fd, status;
    long max_fd = sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < (max_fd > 0 && max_fd < 65536 ? max_fd : 1024); fd++)
        close(fd);
    log_fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (log_fd < 0)
        _exit(1);
    wine = join(home, "runtime/wine/bin/metalsharp-wine");
    if (!wine || access(wine, X_OK) != 0) {
        dprintf(log_fd, "status=wine_runtime_missing\n");
        close(log_fd);
        free(wine);
        _exit(1);
    }
    working_dir = strdup(target);
    if (working_dir) {
        char* slash = strrchr(working_dir, '/');
        if (slash) {
            *slash = 0;
            if (*working_dir)
                chdir(working_dir);
        }
    }
    dprintf(log_fd, "launcher=%s\nprefix=%s\ntarget=%s\nmode=%s\n", launcher->id, prefix, target,
            setup ? "setup" : "launch");
    if (!strcmp(launcher->id, "epic")) {
        bottle_dir = join(home, "bottles/Epic-Games-Prefix");
        installers_dir = bottle_dir ? join(bottle_dir, "installers") : NULL;
        if (!installers_dir) {
            status = 1;
            goto done;
        }
    }
    if (setup && launcher->msi) {
        char* const argv[] = {
            wine, "msiexec", "/i", (char*)target, !strcmp(launcher->id, "epic") ? "SKIP_AUTOLAUNCH=1" : NULL, NULL};
        status = run_launcher_command(argv, prefix, log_fd);
        if (status == 0 && !strcmp(launcher->id, "epic")) {
            epic_executable = launcher_executable(prefix, launcher);
            status = epic_executable ? run_epic_client_with_online_services(home, wine, prefix, installers_dir,
                                                                            epic_executable, log_fd)
                                     : 1;
        }
    } else if (!strcmp(launcher->id, "epic")) {
        status = run_epic_client_with_online_services(home, wine, prefix, installers_dir, target, log_fd);
    } else {
        char* const argv[] = {wine, (char*)target, NULL};
        status = run_launcher_command(argv, prefix, log_fd);
    }
    dprintf(log_fd, "status=exited\nexit_code=%d\n", status);
done:
    close(log_fd);
    free(working_dir);
    free(installers_dir);
    free(bottle_dir);
    free(epic_executable);
    free(wine);
    _exit(status == 0 ? 0 : 1);
}

char* ms_sharp_launcher_launch_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse(body ? (const char*)body : "", body ? length : 0, error, sizeof(error));
    char *launcher_id = NULL, *bottles_dir = NULL, *bottle_dir = NULL, *prefix = NULL, *installers_dir = NULL,
         *installer_path = NULL, *executable = NULL, *logs_dir = NULL, *log_path = NULL, *target = NULL;
    const launcher_installer* launcher;
    ms_json_writer writer;
    char* result = NULL;
    bool setup = false;
    pid_t child;
    int child_status = 0;
    if (!request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        !ms_json_as_string(ms_json_object_get(request, "launcher"), &launcher_id)) {
        ms_json_free(request);
        free(launcher_id);
        return failure("launcher required");
    }
    launcher = launcher_installer_for_id(launcher_id);
    if (!launcher) {
        result = failure("unknown launcher");
        goto done;
    }
    bottles_dir = join(home, "bottles");
    bottle_dir = bottles_dir ? join(bottles_dir, launcher->bottle_id) : NULL;
    prefix = bottle_dir ? join(bottle_dir, "prefix") : NULL;
    installers_dir = bottle_dir ? join(bottle_dir, "installers") : NULL;
    installer_path = installers_dir ? join(installers_dir, launcher->filename) : NULL;
    logs_dir = bottle_dir ? join(bottle_dir, "logs") : NULL;
    log_path = logs_dir ? join(logs_dir, "launcher-launch.log") : NULL;
    if (!prefix || access(prefix, F_OK) != 0 || !installer_path || !log_path) {
        result = failure("launcher prefix has not been created");
        goto done;
    }
    executable = launcher_executable(prefix, launcher);
    if (executable) {
        target = executable;
    } else if (downloaded_launcher_is_valid(installer_path, launcher->msi)) {
        target = installer_path;
        setup = true;
    } else {
        result = failure("launcher is still being prepared");
        goto done;
    }
    child = fork();
    if (child == 0) {
        if (setsid() < 0)
            _exit(1);
        pid_t worker = fork();
        if (worker < 0)
            _exit(1);
        if (worker > 0)
            _exit(0);
        run_launcher_target_worker(home, launcher, prefix, target, setup, log_path);
    }
    if (child < 0) {
        result = failure("failed to launch launcher");
        goto done;
    }
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        result = failure("failed to launch launcher");
        goto done;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "launcher");
    ms_json_writer_string(&writer, launcher->id);
    ms_json_writer_key(&writer, "setup");
    ms_json_writer_bool(&writer, setup);
    ms_json_writer_key(&writer, "message");
    if (setup) {
        char message[160];
        snprintf(message, sizeof(message), "%s is not installed yet; its cached installer was reopened.",
                 launcher->name);
        ms_json_writer_string(&writer, message);
    } else {
        char message[128];
        snprintf(message, sizeof(message), "Launching %s.", launcher->name);
        ms_json_writer_string(&writer, message);
    }
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
done:
    ms_json_free(request);
    free(launcher_id);
    free(bottles_dir);
    free(bottle_dir);
    free(prefix);
    free(installers_dir);
    free(installer_path);
    free(executable);
    free(logs_dir);
    free(log_path);
    return result;
}

static void run_launcher_installer_worker(const char* home, const launcher_installer* launcher, const char* prefix,
                                          const char* installer_path, const char* log_path) {
    char *partial = NULL, *wine = NULL, *installers_dir = NULL, *executable = NULL;
    int log_fd = -1, status;
    long max_fd = sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < (max_fd > 0 && max_fd < 65536 ? max_fd : 1024); fd++)
        close(fd);
    log_fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (log_fd < 0)
        _exit(1);
    dprintf(log_fd, "launcher=%s\nbottle=%s\nprefix=%s\nurl=%s\n", launcher->id, launcher->bottle_id, prefix,
            launcher->url);
    partial = malloc(strlen(installer_path) + 13);
    if (!partial)
        goto fail;
    sprintf(partial, "%s.part.XXXXXX", installer_path);
    {
        int partial_fd = mkstemp(partial);
        if (partial_fd < 0)
            goto fail;
        close(partial_fd);
    }
    {
        char* const curl_argv[] = {"/usr/bin/curl",
                                   "--fail",
                                   "--location",
                                   "--silent",
                                   "--show-error",
                                   "--proto",
                                   "=https",
                                   "--tlsv1.2",
                                   "--retry",
                                   "2",
                                   "--connect-timeout",
                                   "20",
                                   "--max-time",
                                   "1800",
                                   "--max-filesize",
                                   "2147483648",
                                   "-A",
                                   "MetalSharp/" MS_BACKEND_VERSION,
                                   "-o",
                                   partial,
                                   (char*)launcher->url,
                                   NULL};
        dprintf(log_fd, "stage=download\n");
        status = run_launcher_command(curl_argv, NULL, log_fd);
    }
    if (status != 0 || !downloaded_launcher_is_valid(partial, launcher->msi) || rename(partial, installer_path) != 0) {
        dprintf(log_fd, "status=download_failed\nexit_code=%d\n", status);
        goto fail;
    }
    dprintf(log_fd, "stage=prefix\n");
    wine = join(home, "runtime/wine/bin/metalsharp-wine");
    if (wine && access(wine, X_OK) == 0) {
        char* const wineboot_argv[] = {wine, "wineboot", "-u", NULL};
        status = run_launcher_command(wineboot_argv, prefix, log_fd);
        dprintf(log_fd, "wineboot_exit=%d\n", status);
        if (status != 0)
            goto fail;
    } else {
        dprintf(log_fd, "status=wine_runtime_missing\n");
        goto fail;
    }
    dprintf(log_fd, "stage=launch\n");
    if (launcher->msi) {
        char* const installer_argv[] = {
            wine, "msiexec", "/i", (char*)installer_path, !strcmp(launcher->id, "epic") ? "SKIP_AUTOLAUNCH=1" : NULL,
            NULL};
        status = run_launcher_command(installer_argv, prefix, log_fd);
    } else {
        char* const installer_argv[] = {wine, (char*)installer_path, NULL};
        status = run_launcher_command(installer_argv, prefix, log_fd);
    }
    dprintf(log_fd, "status=installer_exited\nexit_code=%d\n", status);
    if (status == 0 && !strcmp(launcher->id, "epic")) {
        installers_dir = strdup(installer_path);
        if (installers_dir) {
            char* slash = strrchr(installers_dir, '/');
            if (slash)
                *slash = 0;
        }
        executable = launcher_executable(prefix, launcher);
        if (!installers_dir || !executable ||
            run_epic_client_with_online_services(home, wine, prefix, installers_dir, executable, log_fd) != 0)
            status = 1;
    }
    close(log_fd);
    free(wine);
    free(partial);
    free(installers_dir);
    free(executable);
    _exit(status == 0 ? 0 : 1);
fail:
    if (partial)
        unlink(partial);
    dprintf(log_fd, "status=failed\n");
    close(log_fd);
    free(wine);
    free(partial);
    free(installers_dir);
    free(executable);
    _exit(1);
}

char* ms_sharp_launcher_install_json(const char* home, const unsigned char* body, size_t length) {
    char error[96];
    ms_json* request = ms_json_parse(body ? (const char*)body : "", body ? length : 0, error, sizeof(error));
    char *launcher_id = NULL, *bottles_dir = NULL, *bottle_dir = NULL, *prefix = NULL, *installers_dir = NULL,
         *logs_dir = NULL, *installer_path = NULL, *log_path = NULL;
    const launcher_installer* launcher;
    ms_json_writer writer;
    char* result = NULL;
    pid_t child;
    int child_status = 0;
    if (!request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        !ms_json_as_string(ms_json_object_get(request, "launcher"), &launcher_id)) {
        ms_json_free(request);
        free(launcher_id);
        return failure("launcher required");
    }
    launcher = launcher_installer_for_id(launcher_id);
    if (!launcher) {
        ms_json_free(request);
        free(launcher_id);
        return failure("unknown launcher");
    }
    bottles_dir = join(home, "bottles");
    bottle_dir = bottles_dir ? join(bottles_dir, launcher->bottle_id) : NULL;
    prefix = bottle_dir ? join(bottle_dir, "prefix") : NULL;
    installers_dir = bottle_dir ? join(bottle_dir, "installers") : NULL;
    logs_dir = bottle_dir ? join(bottle_dir, "logs") : NULL;
    installer_path = installers_dir ? join(installers_dir, launcher->filename) : NULL;
    log_path = logs_dir ? join(logs_dir, "launcher-install.log") : NULL;
    if (!bottles_dir || !bottle_dir || !prefix || !installers_dir || !logs_dir || !installer_path || !log_path ||
        !mkdir_p(prefix) || !mkdir_p(installers_dir) || !mkdir_p(logs_dir) ||
        !write_launcher_bottle(bottle_dir, prefix, installer_path, log_path, launcher)) {
        result = failure("failed to create launcher prefix");
        goto done;
    }
    child = fork();
    if (child == 0) {
        if (setsid() < 0)
            _exit(1);
        pid_t worker = fork();
        if (worker < 0)
            _exit(1);
        if (worker > 0)
            _exit(0);
        run_launcher_installer_worker(home, launcher, prefix, installer_path, log_path);
    }
    if (child < 0) {
        result = failure("failed to start launcher download");
        goto done;
    }
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        result = failure("failed to start launcher download");
        goto done;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "launcher");
    ms_json_writer_string(&writer, launcher->id);
    ms_json_writer_key(&writer, "name");
    ms_json_writer_string(&writer, launcher->name);
    ms_json_writer_key(&writer, "bottleId");
    ms_json_writer_string(&writer, launcher->bottle_id);
    ms_json_writer_key(&writer, "prefixPath");
    ms_json_writer_string(&writer, prefix);
    ms_json_writer_key(&writer, "installerPath");
    ms_json_writer_string(&writer, installer_path);
    ms_json_writer_key(&writer, "logPath");
    ms_json_writer_string(&writer, log_path);
    ms_json_writer_key(&writer, "message");
    ms_json_writer_string(&writer, "Download started. MetalSharp will launch the installer in its prefix.");
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
done:
    ms_json_free(request);
    free(launcher_id);
    free(bottles_dir);
    free(bottle_dir);
    free(prefix);
    free(installers_dir);
    free(logs_dir);
    free(installer_path);
    free(log_path);
    return result;
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
char* ms_sharp_library_json(const char* home) {
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
    bool needs_id = !strcmp(action, "uninstall") || !strcmp(action, "set-cover") ||
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
        if (!dir)
            dir = field(j, "installDir", exe);
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
        if (!strcmp(action, "set-cover") || !strcmp(action, "set-engine") || !strcmp(action, "set-launch-args") ||
            !strcmp(action, "set-cover-position")) {
            char* value = NULL;
            const char* key = NULL;
            if (!strcmp(action, "set-cover")) {
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
        }
        if (!strcmp(action, "launch") || !strcmp(action, "relaunch")) {
            const ms_json* app = NULL;
            char *exe_path = NULL, *bottle_id = NULL, *prefix = NULL;
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
                bottle_id = field(app, "bottle_id", "");
                if (bottle_id[0])
                    prefix = bottle_prefix(home, bottle_id);
            }
            if (!exe_path || !exe_path[0] || access(exe_path, F_OK) != 0) {
                free(exe_path);
                free(bottle_id);
                free(prefix);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("executable not found");
            }
            if (bottle_id[0] && (!prefix || !prefix[0])) {
                free(exe_path);
                free(bottle_id);
                free(prefix);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("Sharp application bottle prefix not found");
            }
            char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
            if (!wine || access(wine, X_OK) != 0) {
                free(wine);
                free(exe_path);
                free(bottle_id);
                free(prefix);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("MetalSharp Wine runtime not found");
            }
            pid_t pid = fork();
            if (pid < 0) {
                free(wine);
                free(exe_path);
                free(bottle_id);
                free(prefix);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("failed to launch application");
            }
            if (pid == 0) {
                if (prefix)
                    setenv("WINEPREFIX", prefix, 1);
                execl(wine, wine, exe_path, (char*)NULL);
                _exit(127);
            }
            free(wine);
            free(exe_path);
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
                ms_json_writer_string(&w, "wine_bare");
            }
            ms_json_writer_object_end(&w);
            o = ms_json_writer_take(&w);
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
