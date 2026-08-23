#include "metalsharp_backend/sharp.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
static char* app_json(const char* id, const char* name, const char* exe, const char* dir) {
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
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed_at");
    ms_json_writer_u64(&w, (unsigned long long)time(NULL));
    ms_json_writer_key(&w, "size_bytes");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
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
        if (!strcmp(action, "install")) {
            src = field(j, "srcPath", "");
            if (!src[0]) {
                free(src);
                ms_json_free(j);
                return failure("srcPath required");
            }
            exe = strdup(src);
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
        name = field(j, "name", exe);
        dir = field(j, "installDir", exe);
        raw = app_json(id, name, exe, dir);
        if (!raw || !append_raw(home, raw)) {
            free(id);
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
            char* exe_path = NULL;
            for (size_t i = 0; i < ms_json_array_length(a); i++) {
                char* item_id = field(ms_json_array_get(a, i), "id", "");
                if (!strcmp(item_id, id)) {
                    app = ms_json_array_get(a, i);
                    free(item_id);
                    break;
                }
                free(item_id);
            }
            if (app)
                exe_path = field(app, "exe_path", "");
            if (!exe_path || !exe_path[0] || access(exe_path, F_OK) != 0) {
                free(exe_path);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("executable not found");
            }
            char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
            if (!wine || access(wine, X_OK) != 0) {
                free(wine);
                free(exe_path);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("MetalSharp Wine runtime not found");
            }
            pid_t pid = fork();
            if (pid < 0) {
                free(wine);
                free(exe_path);
                ms_json_free(a);
                free(id);
                ms_json_free(j);
                return failure("failed to launch application");
            }
            if (pid == 0) {
                execl(wine, wine, exe_path, (char*)NULL);
                _exit(127);
            }
            free(wine);
            free(exe_path);
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
