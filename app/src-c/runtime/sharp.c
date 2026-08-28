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
            char *exe_path = NULL, *work_dir = NULL, *bottle_id = NULL, *prefix = NULL;
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
                if (bottle_id[0])
                    prefix = bottle_prefix(home, bottle_id);
            }
            if (!exe_path || !exe_path[0] || access(exe_path, F_OK) != 0) {
                free(exe_path);
                free(work_dir);
                free(bottle_id);
                free(prefix);
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
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("MetalSharp Wine runtime not found");
            }
            pid_t pid = fork();
            if (pid < 0) {
                free(wine);
                free(exe_path);
                free(work_dir);
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
                if (work_dir && work_dir[0])
                    (void)chdir(work_dir);
                execl(wine, wine, exe_path, (char*)NULL);
                _exit(127);
            }
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
