#include "metalsharp_backend/bottle_actions.h"
#include "metalsharp_backend/bottles.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/setup.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char* fail(const char* s) {
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
    char* p = malloc(x + y + 2);
    if (p)
        snprintf(p, x + y + 2, "%s/%s", a, b);
    return p;
}
static void obj_string(ms_json_writer* w, const char* key, const char* value) {
    ms_json_writer_key(w, key);
    ms_json_writer_string(w, value ? value : "");
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
static char* id_from(const unsigned char* b, size_t n) {
    char e[64];
    ms_json* j = ms_json_parse(b ? (const char*)b : "", b ? n : 0, e, sizeof(e));
    char* s = NULL;
    const ms_json* v = j ? ms_json_object_get(j, "id") : NULL;
    if (v)
        ms_json_as_string(v, &s);
    ms_json_free(j);
    return s;
}
static char* manifest(const char* home, const char* id) {
    char *dir, *p, *raw;
    FILE* f;
    long n;
    size_t got;
    if (!id || strstr(id, "..") || strchr(id, '/'))
        return NULL;
    dir = join(home, "bottles");
    p = dir ? join(dir, id) : NULL;
    free(dir);
    dir = p ? p : NULL;
    p = dir ? join(dir, "bottle.json") : NULL;
    free(dir);
    f = p ? fopen(p, "rb") : NULL;
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f)
            fclose(f);
        free(p);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        free(p);
        return NULL;
    }
    raw = malloc((size_t)n + 1);
    if (!raw) {
        fclose(f);
        free(p);
        return NULL;
    }
    got = fread(raw, 1, (size_t)n, f);
    fclose(f);
    free(p);
    raw[got] = 0;
    return raw;
}
static const char* override_key(const char* key) {
    if (!strcmp(key, "installerOpens"))
        return "installer_opens";
    if (!strcmp(key, "finalAppDetected"))
        return "final_app_detected";
    if (!strcmp(key, "finalAppLaunches"))
        return "final_app_launches";
    if (!strcmp(key, "knownMissingRuntime"))
        return "known_missing_runtime";
    if (!strcmp(key, "notes"))
        return "notes";
    return NULL;
}
static bool save_matrix_record(const char* home, const ms_json* request, const char* id) {
    char *wrapper = ms_bottles_matrix_json(home), *dir, *path, *serial;
    char e[96];
    ms_json *root, *cases, *new_cases;
    ms_json_writer w;
    FILE* f;
    bool found = false, ok = false;
    if (!wrapper)
        return false;
    root = ms_json_parse(wrapper, strlen(wrapper), e, sizeof(e));
    free(wrapper);
    cases = root ? (ms_json*)ms_json_object_get(root, "cases") : NULL;
    if (!root || !cases || ms_json_type_of(cases) != MS_JSON_ARRAY) {
        ms_json_free(root);
        return false;
    }
    ms_json_writer_init(&w);
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < ms_json_array_length(cases); i++) {
        const ms_json* item = ms_json_array_get(cases, i);
        char* item_id = NULL;
        ms_json_as_string(ms_json_object_get(item, "id"), &item_id);
        if (item_id && !strcmp(item_id, id)) {
            found = true;
            ms_json_writer_object_begin(&w);
            for (size_t k = 0; k < ms_json_object_length(item); k++) {
                const char* key = ms_json_object_key_at(item, k);
                const char* body_key = NULL;
                char* encoded = NULL;
                ms_json_writer_key(&w, key);
                if (!strcmp(key, "evidence_updated_at")) {
                    char stamp[32];
                    snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
                    ms_json_writer_string(&w, stamp);
                    continue;
                }
                for (size_t b = 0; b < ms_json_object_length(request); b++) {
                    const char* candidate = ms_json_object_key_at(request, b);
                    if (override_key(candidate) && !strcmp(override_key(candidate), key)) {
                        body_key = candidate;
                        break;
                    }
                }
                if (body_key) {
                    encoded = ms_json_stringify(ms_json_object_get(request, body_key));
                    ms_json_writer_raw(&w, encoded ? encoded : "null");
                    free(encoded);
                } else {
                    encoded = ms_json_stringify(ms_json_object_value_at(item, k));
                    ms_json_writer_raw(&w, encoded ? encoded : "null");
                    free(encoded);
                }
            }
            ms_json_writer_object_end(&w);
        } else {
            char* encoded = ms_json_stringify(item);
            ms_json_writer_raw(&w, encoded ? encoded : "{}");
            free(encoded);
        }
        free(item_id);
    }
    ms_json_writer_array_end(&w);
    serial = ms_json_writer_take(&w);
    new_cases = serial ? ms_json_parse(serial, strlen(serial), e, sizeof(e)) : NULL;
    if (found && new_cases && ms_json_type_of(new_cases) == MS_JSON_ARRAY) {
        dir = join(home, "bottles");
        if (dir && mkdir_p(dir)) {
            path = join(dir, "compatibility-matrix.json");
            f = path ? fopen(path, "wb") : NULL;
            if (f && fputs(serial, f) >= 0) {
                fclose(f);
                ok = true;
            } else if (f)
                fclose(f);
            free(path);
        }
        free(dir);
    }
    free(serial);
    ms_json_free(new_cases);
    ms_json_free(root);
    return ok;
}
static const char* profile_arch(const char* profile) {
    if (!strcmp(profile, "m10_32") || !strcmp(profile, "m11_32") || !strcmp(profile, "win32_dotnet"))
        return "win32";
    if (!strcmp(profile, "m11") || !strcmp(profile, "m12") || !strcmp(profile, "vkd3d") || !strcmp(profile, "m13") ||
        !strcmp(profile, "d3dmetal") || !strcmp(profile, "dotnet") || !strcmp(profile, "fna_arm64") ||
        !strcmp(profile, "fna_x86"))
        return "win64";
    return "wow64";
}
static const char* pipeline_profile(const char* pipeline) {
    if (!strcmp(pipeline, "m9"))
        return "m9";
    if (!strcmp(pipeline, "m10"))
        return "m10";
    if (!strcmp(pipeline, "m11"))
        return "m11";
    if (!strcmp(pipeline, "m12"))
        return "m12";
    if (!strcmp(pipeline, "vkd3d"))
        return "vkd3d";
    if (!strcmp(pipeline, "m13"))
        return "m13";
    if (!strcmp(pipeline, "d3dmetal"))
        return "d3dmetal";
    if (!strcmp(pipeline, "fna_arm64"))
        return "fna_arm64";
    return "plain";
}
static const char* profile_pipeline(const char* profile) {
    if (!strcmp(profile, "plain") || !strcmp(profile, "launcher") || !strcmp(profile, "game_install") ||
        !strcmp(profile, "dotnet") || !strcmp(profile, "webview") || !strcmp(profile, "java_launcher"))
        return "wine_bare";
    if (!strcmp(profile, "m9") || !strcmp(profile, "win32_dotnet"))
        return "m9";
    if (!strcmp(profile, "m10") || !strcmp(profile, "m10_32"))
        return "m10";
    if (!strcmp(profile, "m11") || !strcmp(profile, "m11_32"))
        return "m11";
    if (!strcmp(profile, "m12"))
        return "m12";
    if (!strcmp(profile, "m13"))
        return "m13";
    if (!strcmp(profile, "d3dmetal"))
        return "d3dmetal";
    if (!strcmp(profile, "fna_arm64") || !strcmp(profile, "fna_x86"))
        return "fna_arm64";
    return "vkd3d";
}
static char* rewrite_manifest(const char* home, const char* id, const ms_json* request, const char* action) {
    char *old = manifest(home, id), *path, *dir, *serial;
    char e[96];
    ms_json* j;
    ms_json_writer w;
    FILE* f;
    if (!old)
        return NULL;
    j = ms_json_parse(old, strlen(old), e, sizeof(e));
    free(old);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return NULL;
    }
    char *profile = NULL, *name = NULL, *preferred = NULL, *detected_game_dir = NULL;
    long long appid = 0;
    bool wrote_game_dir = false;
    if (ms_json_as_i64(ms_json_object_get(j, "steam_app_id"), &appid) && appid > 0 && appid <= 0xffffffffLL)
        detected_game_dir = ms_steam_game_dir(home, (unsigned)appid);
    if (!strcmp(action, "set-runtime-profile"))
        profile = NULL, ms_json_as_string(ms_json_object_get(request, "profile"), &profile);
    if (!strcmp(action, "edit")) {
        ms_json_as_string(ms_json_object_get(request, "name"), &name);
        ms_json_as_string(ms_json_object_get(request, "preferredPipeline"), &preferred);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    for (size_t k = 0; k < ms_json_object_length(j); k++) {
        const char* key = ms_json_object_key_at(j, k);
        ms_json_writer_key(&w, key);
        if (profile && !strcmp(key, "runtime_profile")) {
            ms_json_writer_string(&w, profile);
        } else if (profile && !strcmp(key, "preferred_pipeline")) {
            ms_json_writer_string(&w, profile_pipeline(profile));
        } else if (profile && !strcmp(key, "arch")) {
            ms_json_writer_string(&w, profile_arch(profile));
        } else if (profile && !strcmp(key, "health")) {
            ms_json_writer_string(&w, "needs_repair");
        } else if (!strcmp(key, "game_install_path") && detected_game_dir) {
            ms_json_writer_string(&w, detected_game_dir);
            wrote_game_dir = true;
        } else if (name && !strcmp(key, "name")) {
            ms_json_writer_string(&w, name);
        } else if (preferred && !strcmp(key, "preferred_pipeline")) {
            ms_json_writer_string(&w, preferred);
        } else if (preferred && !strcmp(key, "runtime_profile")) {
            ms_json_writer_string(&w, pipeline_profile(preferred));
        } else if (preferred && !strcmp(key, "arch")) {
            ms_json_writer_string(&w, profile_arch(pipeline_profile(preferred)));
        } else if (preferred && !strcmp(key, "health")) {
            ms_json_writer_string(&w, "needs_repair");
        } else if (!strcmp(key, "updated_at")) {
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
            ms_json_writer_string(&w, stamp);
        } else {
            char* v = ms_json_stringify(ms_json_object_value_at(j, k));
            ms_json_writer_raw(&w, v ? v : "null");
            free(v);
        }
    }
    if (detected_game_dir && !wrote_game_dir) {
        ms_json_writer_key(&w, "game_install_path");
        ms_json_writer_string(&w, detected_game_dir);
    }
    ms_json_writer_object_end(&w);
    serial = ms_json_writer_take(&w);
    dir = join(home, "bottles");
    path = dir ? join(dir, id) : NULL;
    free(dir);
    dir = path ? join(path, "bottle.json") : NULL;
    free(path);
    if (dir) {
        f = fopen(dir, "wb");
        if (f && serial && fputs(serial, f) >= 0)
            fclose(f);
        else {
            if (f)
                fclose(f);
            free(serial);
            serial = NULL;
        }
        free(dir);
    }
    free(profile);
    free(name);
    free(preferred);
    free(detected_game_dir);
    ms_json_free(j);
    return serial;
}
static char* verify_directx_json(const char* home, const char* id) {
    static const char* expected[] = {
        "d3dx9_24.dll",  "d3dx9_25.dll",  "d3dx9_26.dll",    "d3dx9_27.dll",       "d3dx9_28.dll",
        "d3dx9_29.dll",  "d3dx9_30.dll",  "d3dx9_31.dll",    "d3dx9_32.dll",       "d3dx9_33.dll",
        "d3dx9_34.dll",  "d3dx9_35.dll",  "d3dx9_36.dll",    "d3dx9_37.dll",       "d3dx9_38.dll",
        "d3dx9_39.dll",  "d3dx9_40.dll",  "d3dx9_41.dll",    "d3dx9_42.dll",       "d3dx9_43.dll",
        "d3dx10_33.dll", "d3dx10_34.dll", "d3dx10_35.dll",   "d3dx10_36.dll",      "d3dx10_37.dll",
        "d3dx10_38.dll", "d3dx10_39.dll", "d3dx10_40.dll",   "d3dx10_41.dll",      "d3dx10_42.dll",
        "d3dx10_43.dll", "d3dx11_42.dll", "d3dx11_43.dll",   "D3DCompiler_42.dll", "D3DCompiler_43.dll",
        "xinput1_3.dll", "xaudio2_7.dll", "x3daudio1_7.dll", "XAPOFX1_5.dll"};
    char *raw = manifest(home, id), *prefix = NULL;
    char e[96];
    ms_json* j;
    size_t count = sizeof(expected) / sizeof(expected[0]), present = 0;
    ms_json_writer w;
    char* o;
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return NULL;
    }
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    obj_string(&w, "id", id);
    ms_json_writer_key(&w, "present");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < count; i++) {
        char *sys = prefix ? join(prefix, "drive_c/windows/system32") : NULL,
             *wow = prefix ? join(prefix, "drive_c/windows/syswow64") : NULL, *a = sys ? join(sys, expected[i]) : NULL,
             *b = wow ? join(wow, expected[i]) : NULL;
        bool found = (a && access(a, F_OK) == 0) || (b && access(b, F_OK) == 0);
        if (found) {
            present++;
            ms_json_writer_string(&w, expected[i]);
        }
        free(sys);
        free(wow);
        free(a);
        free(b);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "missing");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < count; i++) {
        char *sys = prefix ? join(prefix, "drive_c/windows/system32") : NULL,
             *wow = prefix ? join(prefix, "drive_c/windows/syswow64") : NULL, *a = sys ? join(sys, expected[i]) : NULL,
             *b = wow ? join(wow, expected[i]) : NULL;
        bool found = (a && access(a, F_OK) == 0) || (b && access(b, F_OK) == 0);
        if (!found)
            ms_json_writer_string(&w, expected[i]);
        free(sys);
        free(wow);
        free(a);
        free(b);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "complete");
    ms_json_writer_bool(&w, present == count);
    ms_json_writer_key(&w, "present_count");
    ms_json_writer_u64(&w, present);
    ms_json_writer_key(&w, "missing_count");
    ms_json_writer_u64(&w, count - present);
    ms_json_writer_key(&w, "bottle_id");
    ms_json_writer_string(&w, id);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(prefix);
    ms_json_free(j);
    return o;
}
static char* doctor_bottle_json(const char* home, const char* id) {
    char *raw = manifest(home, id), *name = NULL, *prefix = NULL, *game = NULL, *health = NULL, *launch_status = NULL;
    char e[96];
    ms_json* j;
    bool prefix_ok, game_ok = false, components_ok = true;
    size_t components = 0, detections = 0;
    ms_json_writer w;
    char* o;
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return NULL;
    }
    ms_json_as_string(ms_json_object_get(j, "name"), &name);
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    ms_json_as_string(ms_json_object_get(j, "game_install_path"), &game);
    ms_json_as_string(ms_json_object_get(j, "health"), &health);
    ms_json_as_string(ms_json_object_get(j, "last_launch_status"), &launch_status);
    {
        const ms_json* v = ms_json_object_get(j, "installed_components");
        if (v && ms_json_type_of(v) == MS_JSON_ARRAY) {
            components = ms_json_array_length(v);
            components_ok = true;
            for (size_t i = 0; i < components; i++) {
                char *state = NULL, *component_id = NULL;
                const ms_json* component = ms_json_array_get(v, i);
                ms_json_as_string(ms_json_object_get(component, "state"), &state);
                ms_json_as_string(ms_json_object_get(component, "id"), &component_id);
                if (!state || (strcmp(state, "installed") && strcmp(state, "ready"))) {
                    components_ok = false;
                }
                if (state && !strcmp(state, "installed") && component_id &&
                    (!strcmp(component_id, "directx_jun2010") || !strcmp(component_id, "d3d9")) &&
                    (!prefix || access(prefix, F_OK) != 0)) {
                    components_ok = false;
                }
                free(state);
                free(component_id);
            }
        }
        v = ms_json_object_get(j, "installed_app_detections");
        if (v && ms_json_type_of(v) == MS_JSON_ARRAY)
            detections = ms_json_array_length(v);
    }
    prefix_ok = prefix && prefix[0] && access(prefix, F_OK) == 0;
    game_ok = game && game[0] && access(game, F_OK) == 0;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "report");
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id", id);
    obj_string(&w, "name", name ? name : id);
    ms_json_writer_key(&w, "ready");
    ms_json_writer_bool(&w, prefix_ok && components_ok && (!health || strcmp(health, "failed")) &&
                                (!launch_status || strcmp(launch_status, "failed")));
    obj_string(&w, "summary",
               prefix_ok && components_ok && (!health || strcmp(health, "failed")) &&
                       (!launch_status || strcmp(launch_status, "failed"))
                   ? "Bottle runtime checks passed"
                   : "Bottle needs runtime preparation or repair");
    ms_json_writer_key(&w, "checks");
    ms_json_writer_array_begin(&w);
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id", "prefix");
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, prefix_ok);
    obj_string(&w, "detail", prefix ? prefix : "");
    ms_json_writer_object_end(&w);
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id", "components");
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, components > 0);
    char detail[64];
    snprintf(detail, sizeof(detail), "%zu tracked components", components);
    obj_string(&w, "detail", detail);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id", "app_detection");
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, detections > 0);
    snprintf(detail, sizeof(detail), "%zu candidate apps detected", detections);
    obj_string(&w, "detail", detail);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id", "game_runtime_assets");
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, game_ok);
    obj_string(&w, "detail", game ? game : "game install path unavailable");
    ms_json_writer_object_end(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "actions");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "component_sources");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(name);
    free(prefix);
    free(game);
    free(health);
    free(launch_status);
    ms_json_free(j);
    return o;
}
#define MAX_TRACKED_BOTTLE_CHILDREN 64

typedef struct {
    volatile sig_atomic_t pid;
    volatile sig_atomic_t finished;
    volatile sig_atomic_t status;
    volatile sig_atomic_t operation;
    char home[4096];
    char id[129];
    char component[129];
} tracked_bottle_child;

static tracked_bottle_child tracked_bottle_children[MAX_TRACKED_BOTTLE_CHILDREN];

static void reap_bottle_children(int signal_number) {
    int saved = errno, status;
    pid_t child;
    (void)signal_number;
    while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < MAX_TRACKED_BOTTLE_CHILDREN; i++) {
            if (tracked_bottle_children[i].pid == child) {
                tracked_bottle_children[i].status = status;
                tracked_bottle_children[i].finished = 1;
                break;
            }
        }
    }
    errno = saved;
}

static void track_bottle_child(const char* home, const char* id, pid_t pid, const char* component) {
    for (size_t i = 0; i < MAX_TRACKED_BOTTLE_CHILDREN; i++) {
        if (!tracked_bottle_children[i].pid) {
            snprintf(tracked_bottle_children[i].home, sizeof(tracked_bottle_children[i].home), "%s", home);
            snprintf(tracked_bottle_children[i].id, sizeof(tracked_bottle_children[i].id), "%s", id);
            snprintf(tracked_bottle_children[i].component, sizeof(tracked_bottle_children[i].component), "%s",
                     component ? component : "");
            tracked_bottle_children[i].operation = component ? 2 : 1;
            tracked_bottle_children[i].status = 0;
            tracked_bottle_children[i].finished = 0;
            tracked_bottle_children[i].pid = pid;
            return;
        }
    }
}

static bool mark_windows_finished(const char* home, const char* id, int status) {
    char *raw = manifest(home, id), *dir, *path, *serial;
    char e[96];
    ms_json* j;
    ms_json_writer w;
    FILE* f;
    bool ok = false, has_status = false, has_finished = false, has_health = false;
    const char* result = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "exited" : "failed";
    if (!raw)
        return false;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return false;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    for (size_t k = 0; k < ms_json_object_length(j); k++) {
        const char* key = ms_json_object_key_at(j, k);
        ms_json_writer_key(&w, key);
        if (!strcmp(key, "last_launch_status")) {
            has_status = true;
            ms_json_writer_string(&w, result);
        } else if (!strcmp(key, "last_launch_finished_at")) {
            has_finished = true;
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
            ms_json_writer_string(&w, stamp);
        } else if (!strcmp(key, "health")) {
            has_health = true;
            ms_json_writer_string(&w, !strcmp(result, "exited") ? "ready" : "needs_repair");
        } else {
            char* v = ms_json_stringify(ms_json_object_value_at(j, k));
            ms_json_writer_raw(&w, v ? v : "null");
            free(v);
        }
    }
    if (!has_status)
        obj_string(&w, "last_launch_status", result);
    if (!has_finished) {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
        obj_string(&w, "last_launch_finished_at", stamp);
    }
    if (!has_health)
        obj_string(&w, "health", !strcmp(result, "exited") ? "ready" : "needs_repair");
    ms_json_writer_object_end(&w);
    serial = ms_json_writer_take(&w);
    dir = join(home, "bottles");
    path = dir ? join(dir, id) : NULL;
    free(dir);
    dir = path ? join(path, "bottle.json") : NULL;
    free(path);
    f = dir ? fopen(dir, "wb") : NULL;
    if (f && serial && fputs(serial, f) >= 0) {
        fclose(f);
        ok = true;
    } else if (f)
        fclose(f);
    free(dir);
    free(serial);
    ms_json_free(j);
    return ok;
}
static bool mark_component_state(const char*, const char*, const char*, const char*);

static void poll_windows_completion(void) {
    for (size_t i = 0; i < MAX_TRACKED_BOTTLE_CHILDREN; i++) {
        if (tracked_bottle_children[i].finished) {
            char home[sizeof(tracked_bottle_children[i].home)];
            char id[sizeof(tracked_bottle_children[i].id)];
            char component[sizeof(tracked_bottle_children[i].component)];
            int status = tracked_bottle_children[i].status;
            int operation = tracked_bottle_children[i].operation;
            snprintf(home, sizeof(home), "%s", tracked_bottle_children[i].home);
            snprintf(id, sizeof(id), "%s", tracked_bottle_children[i].id);
            snprintf(component, sizeof(component), "%s", tracked_bottle_children[i].component);
            tracked_bottle_children[i].finished = 0;
            tracked_bottle_children[i].pid = 0;
            tracked_bottle_children[i].home[0] = 0;
            tracked_bottle_children[i].id[0] = 0;
            tracked_bottle_children[i].component[0] = 0;
            tracked_bottle_children[i].operation = 0;
            if (operation == 2)
                (void)mark_component_state(home, id, component,
                                           WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "installed" : "missing");
            else
                (void)mark_windows_finished(home, id, status);
        }
    }
}
static bool mark_windows_started(const char* home, const char* id, pid_t pid, const char* logpath) {
    char *raw = manifest(home, id), *dir, *path, *serial;
    char e[96];
    ms_json* j;
    ms_json_writer w;
    FILE* f;
    bool ok = false, has_health = false, has_pid = false, has_log = false, has_status = false;
    if (!raw)
        return false;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return false;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    for (size_t k = 0; k < ms_json_object_length(j); k++) {
        const char* key = ms_json_object_key_at(j, k);
        ms_json_writer_key(&w, key);
        if (!strcmp(key, "health")) {
            has_health = true;
            ms_json_writer_string(&w, "needs_repair");
        } else if (!strcmp(key, "last_launch_pid")) {
            has_pid = true;
            ms_json_writer_u64(&w, (unsigned long long)pid);
        } else if (!strcmp(key, "last_launch_log")) {
            has_log = true;
            ms_json_writer_string(&w, logpath);
        } else if (!strcmp(key, "last_launch_status")) {
            has_status = true;
            ms_json_writer_string(&w, "running");
        } else if (!strcmp(key, "updated_at")) {
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
            ms_json_writer_string(&w, stamp);
        } else {
            char* v = ms_json_stringify(ms_json_object_value_at(j, k));
            ms_json_writer_raw(&w, v ? v : "null");
            free(v);
        }
    }
    if (!has_health) {
        obj_string(&w, "health", "needs_repair");
    }
    if (!has_pid) {
        ms_json_writer_key(&w, "last_launch_pid");
        ms_json_writer_u64(&w, (unsigned long long)pid);
    }
    if (!has_log) {
        obj_string(&w, "last_launch_log", logpath);
    }
    if (!has_status) {
        obj_string(&w, "last_launch_status", "running");
    }
    ms_json_writer_object_end(&w);
    serial = ms_json_writer_take(&w);
    dir = join(home, "bottles");
    path = dir ? join(dir, id) : NULL;
    free(dir);
    dir = path ? join(path, "bottle.json") : NULL;
    free(path);
    f = dir ? fopen(dir, "wb") : NULL;
    if (f && serial && fputs(serial, f) >= 0) {
        fclose(f);
        ok = true;
    } else if (f)
        fclose(f);
    free(dir);
    free(serial);
    ms_json_free(j);
    return ok;
}
static bool mark_component_state(const char* home, const char* id, const char* component, const char* state) {
    char* raw = manifest(home, id);
    char e[96];
    ms_json* j;
    ms_json_writer w;
    char* serial;
    char* dir;
    char* path;
    FILE* f;
    bool found = false, has_updated = false;
    if (!raw)
        return false;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return false;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    for (size_t k = 0; k < ms_json_object_length(j); k++) {
        const char* key = ms_json_object_key_at(j, k);
        const ms_json* value = ms_json_object_value_at(j, k);
        ms_json_writer_key(&w, key);
        if (!strcmp(key, "updated_at")) {
            char stamp[32];
            has_updated = true;
            snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
            ms_json_writer_string(&w, stamp);
        } else if (!strcmp(key, "health")) {
            const ms_json* components = ms_json_object_get(j, "installed_components");
            bool ready = !strcmp(state, "installed");
            if (components && ms_json_type_of(components) == MS_JSON_ARRAY) {
                ready = true;
                for (size_t i = 0; i < ms_json_array_length(components); i++) {
                    const ms_json* item = ms_json_array_get(components, i);
                    char* item_id = NULL;
                    char* item_state = NULL;
                    ms_json_as_string(ms_json_object_get(item, "id"), &item_id);
                    ms_json_as_string(ms_json_object_get(item, "state"), &item_state);
                    if ((item_id && !strcmp(item_id, component) ? state : item_state) == NULL ||
                        strcmp(item_id && !strcmp(item_id, component) ? state : item_state, "installed"))
                        ready = false;
                    free(item_id);
                    free(item_state);
                }
            }
            ms_json_writer_string(&w, ready ? "ready" : "needs_repair");
        } else if (!strcmp(key, "installed_components") && ms_json_type_of(value) == MS_JSON_ARRAY) {
            ms_json_writer_array_begin(&w);
            for (size_t i = 0; i < ms_json_array_length(value); i++) {
                const ms_json* item = ms_json_array_get(value, i);
                char* item_id = NULL;
                ms_json_as_string(ms_json_object_get(item, "id"), &item_id);
                if (item_id && !strcmp(item_id, component)) {
                    found = true;
                    ms_json_writer_object_begin(&w);
                    for (size_t q = 0; q < ms_json_object_length(item); q++) {
                        const char* item_key = ms_json_object_key_at(item, q);
                        ms_json_writer_key(&w, item_key);
                        if (!strcmp(item_key, "state"))
                            ms_json_writer_string(&w, state);
                        else {
                            char* encoded = ms_json_stringify(ms_json_object_value_at(item, q));
                            ms_json_writer_raw(&w, encoded ? encoded : "null");
                            free(encoded);
                        }
                    }
                    ms_json_writer_object_end(&w);
                } else {
                    char* encoded = ms_json_stringify(item);
                    ms_json_writer_raw(&w, encoded ? encoded : "null");
                    free(encoded);
                }
                free(item_id);
            }
            ms_json_writer_array_end(&w);
        } else {
            char* encoded = ms_json_stringify(value);
            ms_json_writer_raw(&w, encoded ? encoded : "null");
            free(encoded);
        }
    }
    if (!has_updated) {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
        obj_string(&w, "updated_at", stamp);
    }
    ms_json_writer_object_end(&w);
    serial = ms_json_writer_take(&w);
    dir = join(home, "bottles");
    path = dir ? join(dir, id) : NULL;
    free(dir);
    dir = path ? join(path, "bottle.json") : NULL;
    free(path);
    f = dir ? fopen(dir, "wb") : NULL;
    if (f && serial) {
        found = fputs(serial, f) >= 0 && found;
        fclose(f);
    } else if (f)
        fclose(f);
    free(dir);
    free(serial);
    ms_json_free(j);
    return found;
}
static char* start_windows_version_json(const char* home, const char* id, const char* version) {
    char *raw = manifest(home, id), *wine = join(home, "runtime/wine/bin/metalsharp-wine"), *prefix = NULL, *logdir,
         *logpath;
    char e[96];
    ms_json* j;
    FILE* log;
    pid_t pid;
    ms_json_writer w;
    char* o;
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        free(wine);
        return NULL;
    }
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        wine = join(home, "runtime/wine/bin/wine");
    }
    if (!wine || access(wine, X_OK) != 0 || !prefix) {
        free(wine);
        free(prefix);
        ms_json_free(j);
        return NULL;
    }
    logdir = join(home, "bottles");
    char* tmp = logdir ? join(logdir, id) : NULL;
    free(logdir);
    logdir = tmp ? join(tmp, "logs") : NULL;
    free(tmp);
    if (!logdir || !mkdir_p(logdir)) {
        free(wine);
        free(prefix);
        free(logdir);
        ms_json_free(j);
        return NULL;
    }
    char filename[128];
    snprintf(filename, sizeof(filename), "windows-version-%s-%llu.log", version, (unsigned long long)time(NULL));
    logpath = join(logdir, filename);
    free(logdir);
    log = logpath ? fopen(logpath, "ab") : NULL;
    if (!log) {
        free(wine);
        free(prefix);
        free(logpath);
        ms_json_free(j);
        return NULL;
    }
    fprintf(log, "windows_version=%s\nprefix=%s\n--- wine output ---\n", version, prefix);
    fflush(log);
    int fd = fileno(log);
    sigset_t blocked, oldmask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blocked, &oldmask);
    signal(SIGCHLD, reap_bottle_children);
    pid = fork();
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        fclose(log);
        free(wine);
        free(prefix);
        free(logpath);
        ms_json_free(j);
        return NULL;
    }
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        char* args[] = {wine, "reg", "add", "HKCU\\Software\\Wine", "/v", "Version", "/d", (char*)version, "/f", NULL};
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "none", 1);
        char library_env[4096];
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        execv(wine, args);
        _exit(127);
    }
    fclose(log);
    mark_windows_started(home, id, pid, logpath);
    track_bottle_child(home, id, pid, NULL);
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "repair");
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id",
               !strcmp(version, "win7")    ? "windows_version_win7"
               : !strcmp(version, "win10") ? "windows_version_win10"
                                           : "windows_version_win11");
    obj_string(&w, "status", "started");
    char detail[128];
    snprintf(detail, sizeof(detail), "Started Windows version mode update to %s", version);
    obj_string(&w, "detail", detail);
    ms_json_writer_key(&w, "asset_path");
    ms_json_writer_null(&w);
    obj_string(&w, "log_path", logpath);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned long long)pid);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(wine);
    free(prefix);
    free(logpath);
    ms_json_free(j);
    return o;
}
static char* merge_synced_manifest(const char* path, const ms_json* game, const char* id) {
    FILE* f;
    long n;
    char *raw, *name = NULL, *game_dir = NULL, *serial, *v;
    size_t got;
    char e[96];
    ms_json* j;
    ms_json_writer w;
    long long appid;
    if (!path || !game || !ms_json_as_i64(ms_json_object_get(game, "appid"), &appid))
        return NULL;
    f = fopen(path, "rb");
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
    raw = malloc((size_t)n + 1);
    if (!raw) {
        fclose(f);
        return NULL;
    }
    got = fread(raw, 1, (size_t)n, f);
    fclose(f);
    raw[got] = 0;
    j = ms_json_parse(raw, got, e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return NULL;
    }
    ms_json_as_string(ms_json_object_get(game, "name"), &name);
    ms_json_as_string(ms_json_object_get(game, "wine_game_path"), &game_dir);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    for (size_t k = 0; k < ms_json_object_length(j); k++) {
        const char* key = ms_json_object_key_at(j, k);
        ms_json_writer_key(&w, key);
        if (!strcmp(key, "id"))
            ms_json_writer_string(&w, id);
        else if (!strcmp(key, "steam_app_id"))
            ms_json_writer_u64(&w, (unsigned long long)appid);
        else if (!strcmp(key, "name") && ms_json_type_of(ms_json_object_get(j, "custom_name")) == MS_JSON_NULL)
            ms_json_writer_string(&w, name && name[0] ? name : id);
        else if (!strcmp(key, "game_install_path") && game_dir)
            ms_json_writer_string(&w, game_dir);
        else if (!strcmp(key, "updated_at")) {
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
            ms_json_writer_string(&w, stamp);
        } else {
            v = ms_json_stringify(ms_json_object_value_at(j, k));
            ms_json_writer_raw(&w, v ? v : "null");
            free(v);
        }
    }
    ms_json_writer_object_end(&w);
    serial = ms_json_writer_take(&w);
    free(name);
    free(game_dir);
    ms_json_free(j);
    return serial;
}
static bool write_synced_manifest(const char* home, const ms_json* game) {
    long long appid;
    char *name = NULL, *game_dir = NULL, *root, *dir, *path, *raw;
    FILE* f;
    ms_json_writer w;
    bool ok = false;
    if (!ms_json_as_i64(ms_json_object_get(game, "appid"), &appid) || appid <= 0 || appid > 0xffffffffLL)
        return false;
    ms_json_as_string(ms_json_object_get(game, "name"), &name);
    ms_json_as_string(ms_json_object_get(game, "wine_game_path"), &game_dir);
    root = join(home, "bottles");
    if (!root || !mkdir_p(root)) {
        free(root);
        free(name);
        free(game_dir);
        return false;
    }
    char id[64];
    snprintf(id, sizeof(id), "steam_%lld", appid);
    dir = join(root, id);
    path = dir ? join(dir, "bottle.json") : NULL;
    if (!dir || !mkdir_p(dir)) {
        free(root);
        free(dir);
        free(path);
        free(name);
        free(game_dir);
        return false;
    }
    if (access(path, F_OK) == 0) {
        char* merged = merge_synced_manifest(path, game, id);
        FILE* existing = merged ? fopen(path, "wb") : NULL;
        bool merged_ok = existing && fputs(merged, existing) >= 0;
        if (existing)
            fclose(existing);
        free(merged);
        free(root);
        free(dir);
        free(path);
        free(name);
        free(game_dir);
        return merged_ok;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id", id);
    obj_string(&w, "name", name && name[0] ? name : id);
    ms_json_writer_key(&w, "custom_name");
    ms_json_writer_null(&w);
    obj_string(&w, "bottle_type", "steam");
    ms_json_writer_key(&w, "steam_app_id");
    ms_json_writer_u64(&w, (unsigned long long)appid);
    char* prefix = join(home, "prefix-steam");
    obj_string(&w, "prefix_path", prefix ? prefix : "");
    free(prefix);
    obj_string(&w, "arch", "wow64");
    obj_string(&w, "runtime_profile", "vkd3d");
    obj_string(&w, "preferred_pipeline", "vkd3d");
    ms_json_writer_key(&w, "installed_components");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "source_installer_path");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installer_kind");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "game_install_path");
    if (game_dir)
        ms_json_writer_string(&w, game_dir);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "runtime_assets");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "installed_app_detections");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    obj_string(&w, "health", game_dir ? "ready" : "new");
    ms_json_writer_key(&w, "last_launch_log");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "last_launch_pid");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "last_launch_status");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "last_launch_finished_at");
    ms_json_writer_null(&w);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
    obj_string(&w, "created_at", stamp);
    obj_string(&w, "updated_at", stamp);
    ms_json_writer_object_end(&w);
    raw = ms_json_writer_take(&w);
    f = path ? fopen(path, "wb") : NULL;
    if (f && raw && fputs(raw, f) >= 0) {
        fclose(f);
        ok = true;
    } else if (f)
        fclose(f);
    free(raw);
    free(root);
    free(dir);
    free(path);
    free(name);
    free(game_dir);
    return ok;
}
static char* sync_steam_result(const char* home) {
    char *library = ms_steam_library_json(home), *list;
    char e[96];
    ms_json *root, *games;
    ms_json_writer w;
    char* o;
    size_t count = 0;
    if (!library)
        return fail("failed to read Steam library");
    root = ms_json_parse(library, strlen(library), e, sizeof(e));
    free(library);
    games = root ? (ms_json*)ms_json_object_get(root, "games") : NULL;
    if (games && ms_json_type_of(games) == MS_JSON_ARRAY)
        for (size_t i = 0; i < ms_json_array_length(games); i++)
            if (write_synced_manifest(home, ms_json_array_get(games, i)))
                count++;
    list = ms_bottles_list_json(home);
    ms_json_free(root);
    if (!list)
        return fail("failed to list synced bottles");
    root = ms_json_parse(list, strlen(list), e, sizeof(e));
    free(list);
    games = root ? (ms_json*)ms_json_object_get(root, "bottles") : NULL;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "bottles");
    {
        char* raw = games ? ms_json_stringify(games) : strdup("[]");
        ms_json_writer_raw(&w, raw ? raw : "[]");
        free(raw);
    }
    ms_json_writer_key(&w, "count");
    ms_json_writer_u64(&w, games && ms_json_type_of(games) == MS_JSON_ARRAY ? ms_json_array_length(games) : count);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    ms_json_free(root);
    return o;
}
static char* start_component_installer_json(const char* home, const char* id, const char* component,
                                            const char* installer) {
    char* raw = manifest(home, id);
    char* prefix = NULL;
    char* wine = join(home, "runtime/wine/bin/metalsharp-wine");
    char* dir = NULL;
    char* tmp = NULL;
    char* logpath = NULL;
    char* out = NULL;
    char e[96];
    ms_json* j;
    FILE* log;
    pid_t pid;
    ms_json_writer w;
    if (!raw)
        goto fail;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT)
        goto fail_json;
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    if (!prefix || !installer || access(installer, F_OK) != 0 || !wine || access(wine, X_OK) != 0)
        goto fail_json;
    dir = join(home, "bottles");
    tmp = dir ? join(dir, id) : NULL;
    free(dir);
    dir = tmp ? join(tmp, "logs") : NULL;
    free(tmp);
    if (!dir || !mkdir_p(dir))
        goto fail_json;
    tmp = join(dir, component);
    free(dir);
    dir = tmp;
    if (dir && !mkdir_p(dir))
        goto fail_json;
    tmp = dir ? join(dir, "repair.log") : NULL;
    free(dir);
    logpath = tmp;
    log = logpath ? fopen(logpath, "ab") : NULL;
    if (!log)
        goto fail_json;
    fprintf(log, "component=%s\ninstaller=%s\nprefix=%s\n", component, installer, prefix);
    fflush(log);
    sigset_t blocked, oldmask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blocked, &oldmask);
    signal(SIGCHLD, reap_bottle_children);
    pid = fork();
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        fclose(log);
        goto fail_json;
    }
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        char library_env[4096];
        int fd = fileno(log);
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        execl(wine, wine, installer, (char*)NULL);
        _exit(127);
    }
    fclose(log);
    track_bottle_child(home, id, pid, component);
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned long long)pid);
    obj_string(&w, "log_path", logpath);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(prefix);
    free(wine);
    free(logpath);
    ms_json_free(j);
    return out;
fail_json:
    ms_json_free(j);
fail:
    free(prefix);
    free(wine);
    free(logpath);
    return NULL;
}
static char* start_bottle_tool_json(const char* home, const char* id, const char* action) {
    char *raw = manifest(home, id), *wine = join(home, "runtime/wine/bin/metalsharp-wine"), *prefix = NULL, *dir = NULL,
         *tmp = NULL, *logpath = NULL, *regpath = NULL, *o;
    char e[96];
    ms_json* j;
    FILE* log;
    pid_t pid;
    ms_json_writer w;
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        free(wine);
        return NULL;
    }
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    if (!prefix || !prefix[0] || !wine || access(wine, X_OK) != 0) {
        free(prefix);
        free(wine);
        ms_json_free(j);
        return NULL;
    }
    dir = join(home, "bottles");
    tmp = dir ? join(dir, id) : NULL;
    free(dir);
    dir = tmp ? join(tmp, "logs") : NULL;
    free(tmp);
    if (!dir || !mkdir_p(dir)) {
        free(prefix);
        free(wine);
        free(dir);
        ms_json_free(j);
        return NULL;
    }
    tmp = join(dir, !strcmp(action, "apply-font-subs") ? "font-subs.log" : "post-wineboot.log");
    free(dir);
    logpath = tmp;
    log = logpath ? fopen(logpath, "ab") : NULL;
    if (!log) {
        free(prefix);
        free(wine);
        free(logpath);
        ms_json_free(j);
        return NULL;
    }
    if (!strcmp(action, "apply-font-subs")) {
        tmp = join(prefix, "drive_c");
        if (tmp)
            mkdir_p(tmp);
        regpath = tmp ? join(tmp, "metalsharp-fontsubs.reg") : NULL;
        free(tmp);
        if (regpath) {
            FILE* reg = fopen(regpath, "wb");
            if (reg) {
                fputs("REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\Software\\Wine\\Fonts\\Replacements]\r\n\"Arial\"="
                      "\"Arial\"\r\n",
                      reg);
                fclose(reg);
            }
        }
    } else {
        tmp = join(prefix, "drive_c");
        if (tmp)
            mkdir_p(tmp);
        free(tmp);
    }
    fprintf(log, "action=%s\nprefix=%s\n", action, prefix);
    fflush(log);
    int fd = fileno(log);
    pid = fork();
    if (pid < 0) {
        fclose(log);
        free(prefix);
        free(wine);
        free(logpath);
        free(regpath);
        ms_json_free(j);
        return NULL;
    }
    if (pid == 0) {
        char library_env[4096];
        char* args[4];
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "none", 1);
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (regpath) {
            args[0] = wine;
            args[1] = "regedit";
            args[2] = regpath;
            args[3] = NULL;
        } else {
            args[0] = wine;
            args[1] = "wineboot";
            args[2] = "-u";
            args[3] = NULL;
        }
        execv(wine, args);
        _exit(127);
    }
    fclose(log);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    obj_string(&w, "id", id);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned long long)pid);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(prefix);
    free(wine);
    free(logpath);
    free(regpath);
    ms_json_free(j);
    return o;
}
static char* prepare_bottle_json(const char* home, const char* id) {
    char *raw = manifest(home, id), *prefix = NULL, *dir, *tmp;
    char e[96];
    ms_json* j;
    char* report;
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return NULL;
    }
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    if (!prefix || !prefix[0]) {
        free(prefix);
        ms_json_free(j);
        return NULL;
    }
    dir = join(prefix, "drive_c");
    if (dir)
        mkdir_p(dir);
    free(dir);
    dir = join(home, "bottles");
    tmp = dir ? join(dir, id) : NULL;
    free(dir);
    dir = tmp ? join(tmp, "logs") : NULL;
    free(tmp);
    if (dir)
        mkdir_p(dir);
    free(dir);
    dir = join(home, "bottles");
    tmp = dir ? join(dir, id) : NULL;
    free(dir);
    dir = tmp ? join(tmp, "installers") : NULL;
    free(tmp);
    if (dir)
        mkdir_p(dir);
    free(dir);
    {
        char* system32 = join(prefix, "drive_c/windows/system32");
        if (system32 && access(system32, F_OK) == 0) {
            char* seed = start_bottle_tool_json(home, id, "seed-post-wineboot");
            free(seed);
        }
        free(system32);
    }
    free(prefix);
    ms_json_free(j);
    report = doctor_bottle_json(home, id);
    return report;
}
static bool copy_component_file(const char* source, const char* target) {
    FILE *in = fopen(source, "rb"), *out = NULL;
    unsigned char buffer[8192];
    size_t n;
    bool ok = false;
    if (!in)
        return false;
    out = fopen(target, "wb");
    if (!out)
        goto done;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n)
            goto done;
    ok = ferror(in) == 0;
done:
    if (out)
        fclose(out);
    fclose(in);
    return ok;
}

static bool vcpp_prefix_ready(const char* prefix) {
    const char* x64[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};
    const char* x86[] = {"vcruntime140.dll", "msvcp140.dll"};
    char* system32 = join(prefix, "drive_c/windows/system32");
    char* syswow64 = join(prefix, "drive_c/windows/syswow64");
    bool ready = system32 && syswow64;
    for (size_t i = 0; ready && i < sizeof(x64) / sizeof(x64[0]); i++) {
        char* path = join(system32, x64[i]);
        struct stat info;
        ready = path && stat(path, &info) == 0 && info.st_size > 10000;
        free(path);
    }
    for (size_t i = 0; ready && i < sizeof(x86) / sizeof(x86[0]); i++) {
        char* path = join(syswow64, x86[i]);
        struct stat info;
        ready = path && stat(path, &info) == 0 && info.st_size > 10000;
        free(path);
    }
    free(system32);
    free(syswow64);
    return ready;
}

static bool seed_vcpp_runtime_dlls(const char* home, const char* prefix) {
    const char* x64_required[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};
    const char* x86_required[] = {"vcruntime140.dll", "msvcp140.dll"};
    const char* seed_names[] = {"concrt140.dll",       "msvcp140.dll",       "msvcp140_1.dll",
                                "msvcp140_2.dll",       "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
                                "vcomp140.dll",         "vcruntime140.dll",   "vcruntime140_1.dll"};
    char *x64_source = join(home, "runtime/wine/lib/wine/x86_64-windows"),
         *x86_source = join(home, "runtime/wine/lib/wine/i386-windows"),
         *system32 = join(prefix, "drive_c/windows/system32"), *syswow64 = join(prefix, "drive_c/windows/syswow64");
    bool ok = x64_source && x86_source && system32 && syswow64 && mkdir_p(system32) && mkdir_p(syswow64);
    for (size_t i = 0; ok && i < sizeof(x64_required) / sizeof(x64_required[0]); i++) {
        char* source = join(x64_source, x64_required[i]);
        ok = source && access(source, R_OK) == 0;
        free(source);
    }
    for (size_t i = 0; ok && i < sizeof(x86_required) / sizeof(x86_required[0]); i++) {
        char* source = join(x86_source, x86_required[i]);
        ok = source && access(source, R_OK) == 0;
        free(source);
    }
    for (size_t i = 0; ok && i < sizeof(seed_names) / sizeof(seed_names[0]); i++) {
        char *source = join(x64_source, seed_names[i]), *target = join(system32, seed_names[i]);
        if (source && access(source, R_OK) == 0)
            ok = copy_component_file(source, target);
        free(source);
        free(target);
        source = join(x86_source, seed_names[i]);
        target = join(syswow64, seed_names[i]);
        if (source && access(source, R_OK) == 0)
            ok = ok && copy_component_file(source, target);
        free(source);
        free(target);
    }
    free(x64_source);
    free(x86_source);
    free(system32);
    free(syswow64);
    return ok;
}

static char* seed_vcpp_for_steam_bottle(const char* home, const char* id) {
    char *raw = manifest(home, id), *prefix = NULL, *pipeline = NULL;
    char error[96];
    ms_json* j;
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), error, sizeof(error));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT)
        goto done;
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    ms_json_as_string(ms_json_object_get(j, "preferred_pipeline"), &pipeline);
    if (!prefix || (pipeline && !strcmp(pipeline, "d3dmetal")) || vcpp_prefix_ready(prefix))
        goto done;
    if (!seed_vcpp_runtime_dlls(home, prefix)) {
        char* failure = strdup("VC++ x64/x86 runtime DLLs were not available while saving the bottle");
        free(prefix);
        free(pipeline);
        ms_json_free(j);
        return failure;
    }
done:
    free(prefix);
    free(pipeline);
    ms_json_free(j);
    return NULL;
}

static bool component_artifact_available(const char* home, const char* component) {
    const char* installer = NULL;
    const char* runtime_file = NULL;
    if (!strcmp(component, "mono-arm64"))
        runtime_file = "runtime/mono-arm64/bin/mono";
    else if (!strcmp(component, "mono-x86"))
        runtime_file = "runtime/mono-x86/bin/mono";
    else if (!strcmp(component, "fna"))
        runtime_file = "runtime/fnalibs/libFNA3D.0.dylib";
    else if (!strcmp(component, "xna"))
        runtime_file = "runtime/fnalibs/libFAudio.0.dylib";
    else if (!strcmp(component, "sdl2"))
        runtime_file = "runtime/fnalibs/libSDL2-2.0.0.dylib";
    else if (!strcmp(component, "fna3d"))
        runtime_file = "runtime/fnalibs/libFNA3D.0.dylib";
    else if (!strcmp(component, "faudio"))
        runtime_file = "runtime/fnalibs/libFAudio.0.dylib";
    if (runtime_file) {
        char* path = join(home, runtime_file);
        bool available = path && access(path, R_OK) == 0;
        free(path);
        return available;
    }
    if (!strcmp(component, "vkd3d_d3d12") || !strcmp(component, "vkd3d_d3d12core") ||
        !strcmp(component, "vkd3d_dxgi") || !strcmp(component, "dxvk_d3d9") || !strcmp(component, "dxvk_d3d11") ||
        !strcmp(component, "dxvk_d3d10core")) {
        const char* dir = strstr(component, "vkd3d_") ? "vkd3d/vkd3d-proton/x86_64-windows" : "vkd3d/dxvk/x86_64-windows";
        const char* filename = !strcmp(component, "vkd3d_d3d12") ? "d3d12.dll"
                              : !strcmp(component, "vkd3d_d3d12core") ? "d3d12core.dll"
                              : !strcmp(component, "vkd3d_dxgi") ? "dxgi.dll"
                              : !strcmp(component, "dxvk_d3d9") ? "d3d9.dll"
                              : !strcmp(component, "dxvk_d3d10core") ? "d3d10core.dll"
                                                                       : "d3d11.dll";
        char* root = join(home, dir);
        char* file = root ? join(root, filename) : NULL;
        bool available = file && access(file, R_OK) == 0;
        free(root);
        free(file);
        return available;
    }
    if (!strcmp(component, "directx_jun2010"))
        installer = "runtime/redist/DirectX/Jun2010/DXSETUP.exe";
    else if (!strcmp(component, "vcrun2019_x64"))
        installer = "runtime/redist/vcredist/vc_redist.x64.exe";
    else if (!strcmp(component, "vcrun2019_x86"))
        installer = "runtime/redist/vcredist/vc_redist.x86.exe";
    else if (!strcmp(component, "vcrun2013"))
        installer = "runtime/redist/vcredist_x64.exe";
    else if (!strcmp(component, "vcrun2010"))
        installer = "runtime/redist/vcredist/2010/vcredist_x64.exe";
    else if (!strcmp(component, "dotnet40"))
        installer = "runtime/redist/DotNet/4.0/dotNetFx40_Full_x86_x64.exe";
    else if (!strcmp(component, "dotnet48"))
        installer = "runtime/redist/DotNet/4.8/ndp48-x86-x64-allos-enu.exe";
    if (installer) {
        char* path = join(home, installer);
        bool available = path && access(path, F_OK) == 0;
        free(path);
        return available;
    }
    if (!strcmp(component, "d3d11") || !strcmp(component, "dxgi") || !strcmp(component, "d3d10core") ||
        !strcmp(component, "d3d10_1") || !strcmp(component, "winemetal")) {
        const char* dirs[] = {"runtime/wine/lib/dxmt/x86_64-windows", "runtime/wine/lib/dxmt/i386-windows",
                              "runtime/wine/lib/dxmt_m12/x86_64-windows", "runtime/wine/lib/wine/x86_64-windows",
                              "runtime/wine/lib/wine/i386-windows"};
        char name[128];
        snprintf(name, sizeof(name), "%s.dll", component);
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            char* dir = join(home, dirs[i]);
            char* file = dir ? join(dir, name) : NULL;
            bool found = file && access(file, F_OK) == 0;
            free(dir);
            free(file);
            if (found)
                return true;
        }
        return false;
    }
    return true;
}

static char* repair_component_json(const char* home, const char* id, const unsigned char* body, size_t len,
                                   int* status) {
    char e[96], *component = NULL, *tool, *raw;
    ms_json* j;
    bool dry = false;
    ms_json_writer w;
    char* o;
    const ms_json* v;
    raw = body ? strndup((const char*)body, len) : strdup("{}");
    j = raw ? ms_json_parse(raw, strlen(raw), e, sizeof(e)) : NULL;
    free(raw);
    v = j ? ms_json_object_get(j, "component") : NULL;
    if (!v || !ms_json_as_string(v, &component) || !component[0]) {
        free(component);
        ms_json_free(j);
        if (status)
            *status = 400;
        return fail("component required");
    }
    v = ms_json_object_get(j, "dryRun");
    if (v)
        ms_json_as_bool(v, &dry);
    if (dry) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "repair");
        ms_json_writer_object_begin(&w);
        obj_string(&w, "id", component);
        bool available = component_artifact_available(home, component);
        obj_string(&w, "status", available ? "available" : "asset_missing");
        obj_string(&w, "detail",
                   available ? "component repair is available through MetalSharp Wine"
                             : "component installer or runtime artifact not found locally");
        ms_json_writer_key(&w, "pid");
        ms_json_writer_null(&w);
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        o = ms_json_writer_take(&w);
        free(component);
        ms_json_free(j);
        return o;
    }
    if (!strcmp(component, "d3d11") || !strcmp(component, "dxgi") || !strcmp(component, "d3d10core") ||
        !strcmp(component, "d3d10_1") || !strcmp(component, "winemetal")) {
        char* manifest_raw = manifest(home, id);
        char* prefix = NULL;
        char* arch = NULL;
        ms_json* manifest_json = NULL;
        if (manifest_raw) {
            manifest_json = ms_json_parse(manifest_raw, strlen(manifest_raw), e, sizeof(e));
            free(manifest_raw);
        }
        if (manifest_json && ms_json_type_of(manifest_json) == MS_JSON_OBJECT) {
            ms_json_as_string(ms_json_object_get(manifest_json, "arch"), &arch);
            ms_json_as_string(ms_json_object_get(manifest_json, "prefix_path"), &prefix);
        }
        if (arch && !strcmp(arch, "win32") && prefix) {
            char* source = NULL;
            char* source_name = NULL;
            char* target_dir = join(prefix, "drive_c/windows/system32");
            char* target = NULL;
            char dll_name[256];
            bool copied = false;
            snprintf(dll_name, sizeof(dll_name), "%s.dll", component);
            if (strcmp(component, "d3d10_1")) {
                source = join(home, "runtime/wine/lib/dxmt/i386-windows");
                source_name = source ? join(source, dll_name) : NULL;
            } else {
                source_name = join(home, "runtime/wine/lib/wine/i386-windows/d3d10_1.dll");
            }
            target = target_dir ? join(target_dir, dll_name) : NULL;
            if (source_name && access(source_name, F_OK) == 0 && target_dir && mkdir_p(target_dir))
                copied = copy_component_file(source_name, target);
            (void)mark_component_state(home, id, component, copied ? "installed" : "missing");
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "repair");
            ms_json_writer_object_begin(&w);
            obj_string(&w, "id", component);
            obj_string(&w, "status", copied ? "installed" : "asset_missing");
            obj_string(&w, "detail",
                       copied ? "Staged 32-bit runtime artifact into bottle prefix"
                              : "32-bit runtime asset not found locally");
            ms_json_writer_key(&w, "asset_path");
            if (source_name)
                ms_json_writer_string(&w, source_name);
            else
                ms_json_writer_null(&w);
            ms_json_writer_key(&w, "log_path");
            ms_json_writer_null(&w);
            ms_json_writer_key(&w, "pid");
            ms_json_writer_null(&w);
            ms_json_writer_object_end(&w);
            ms_json_writer_object_end(&w);
            o = ms_json_writer_take(&w);
            free(source);
            free(source_name);
            free(target_dir);
            free(target);
            free(prefix);
            free(arch);
            ms_json_free(manifest_json);
            free(component);
            ms_json_free(j);
            return o;
        }
        free(prefix);
        free(arch);
        ms_json_free(manifest_json);
    }
    {
        const char* relative = NULL;
        if (!strcmp(component, "directx_jun2010"))
            relative = "runtime/redist/DirectX/Jun2010/DXSETUP.exe";
        else if (!strcmp(component, "vcrun2019_x64"))
            relative = "runtime/redist/vcredist/vc_redist.x64.exe";
        else if (!strcmp(component, "vcrun2019_x86"))
            relative = "runtime/redist/vcredist/vc_redist.x86.exe";
        else if (!strcmp(component, "vcrun2013"))
            relative = "runtime/redist/vcredist_x64.exe";
        else if (!strcmp(component, "vcrun2010"))
            relative = "runtime/redist/vcredist/2010/vcredist_x64.exe";
        else if (!strcmp(component, "dotnet40"))
            relative = "runtime/redist/DotNet/4.0/dotNetFx40_Full_x86_x64.exe";
        else if (!strcmp(component, "dotnet48"))
            relative = "runtime/redist/DotNet/4.8/ndp48-x86-x64-allos-enu.exe";
        if (relative) {
            char* installer = join(home, relative);
            tool = start_component_installer_json(home, id, component, installer);
            free(installer);
        } else if (!strcmp(component, "corefonts")) {
            tool = start_bottle_tool_json(home, id, "apply-font-subs");
        } else {
            tool = start_bottle_tool_json(home, id, "seed-post-wineboot");
        }
    }
    if (!tool) {
        free(component);
        ms_json_free(j);
        if (status)
            *status = 500;
        return fail("MetalSharp Wine not found or bottle prefix unavailable");
    }
    (void)mark_component_state(home, id, component, "needs_repair");
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "repair");
    ms_json_writer_object_begin(&w);
    obj_string(&w, "id", component);
    obj_string(&w, "status", "started");
    obj_string(&w, "detail", "Started component repair in bottle");
    ms_json_writer_key(&w, "pid");
    {
        ms_json* tj = ms_json_parse(tool, strlen(tool), e, sizeof(e));
        long long pid = 0;
        if (tj)
            ms_json_as_i64(ms_json_object_get(tj, "pid"), &pid);
        ms_json_writer_i64(&w, pid);
        ms_json_free(tj);
    }
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(tool);
    free(component);
    ms_json_free(j);
    return o;
}
static bool valid_bottle_id(const char* id) {
    size_t n;
    if (!id || (n = strlen(id)) == 0 || n > 128)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)id[i]) || id[i] == '_' || id[i] == '-'))
            return false;
    return true;
}
char* ms_bottle_relaunch_installer_json(const char* home, const unsigned char* body, size_t len, int* status) {
    char *id = id_from(body, len), *raw, *prefix = NULL, *installer = NULL, *wine, *dir, *logpath;
    char e[96];
    ms_json* j;
    FILE* log;
    pid_t pid;
    ms_json_writer w;
    char* o;
    if (status)
        *status = 200;
    if (!valid_bottle_id(id)) {
        bool missing = !id || !id[0];
        free(id);
        if (status)
            *status = 400;
        return fail(missing ? "id required" : "invalid bottle id");
    }
    raw = manifest(home, id);
    if (!raw) {
        free(id);
        return fail("bottle not found");
    }
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        free(id);
        return fail("invalid bottle manifest");
    }
    ms_json_as_string(ms_json_object_get(j, "prefix_path"), &prefix);
    ms_json_as_string(ms_json_object_get(j, "source_installer_path"), &installer);
    wine = join(home, "runtime/wine/bin/metalsharp-wine");
    if (!prefix || !installer || !installer[0] || access(installer, X_OK) != 0 || !wine || access(wine, X_OK) != 0) {
        free(id);
        free(prefix);
        free(installer);
        free(wine);
        ms_json_free(j);
        return fail("installer source or MetalSharp Wine not found");
    }
    dir = join(home, "bottles");
    char* tmp = dir ? join(dir, id) : NULL;
    free(dir);
    dir = tmp ? join(tmp, "logs") : NULL;
    free(tmp);
    if (!dir || !mkdir_p(dir)) {
        free(id);
        free(prefix);
        free(installer);
        free(wine);
        free(dir);
        ms_json_free(j);
        return fail("failed to create bottle log directory");
    }
    logpath = join(dir, "installer-relaunch.log");
    free(dir);
    log = logpath ? fopen(logpath, "ab") : NULL;
    if (!log) {
        free(id);
        free(prefix);
        free(installer);
        free(wine);
        free(logpath);
        ms_json_free(j);
        return fail("failed to create installer log");
    }
    fprintf(log, "installer=%s\nprefix=%s\n", installer, prefix);
    fflush(log);
    pid = fork();
    if (pid < 0) {
        fclose(log);
        free(id);
        free(prefix);
        free(installer);
        free(wine);
        free(logpath);
        ms_json_free(j);
        return fail("failed to launch installer");
    }
    if (pid == 0) {
        char library_env[4096];
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        int fd = fileno(log);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        execl(wine, wine, installer, (char*)NULL);
        _exit(127);
    }
    fclose(log);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "installing");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, (unsigned long long)pid);
    ms_json_writer_key(&w, "message");
    ms_json_writer_string(&w, "Bottle installer relaunched");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(id);
    free(prefix);
    free(installer);
    free(wine);
    free(logpath);
    ms_json_free(j);
    return o;
}
void ms_bottle_poll(void) {
    poll_windows_completion();
}
char* ms_bottle_action_json(const char* home, const char* action, const unsigned char* body, size_t len, int* status) {
    char *id, *raw, *o;
    poll_windows_completion();
    if (status)
        *status = 200;
    if (!strcmp(action, "sync-steam"))
        return sync_steam_result(home);
    id = id_from(body, len);
    if (!id || !id[0]) {
        free(id);
        if (status)
            *status = 400;
        return fail(!strcmp(action, "set-runtime-profile") ? "id and profile required" : "id required");
    }
    if (!valid_bottle_id(id)) {
        free(id);
        if (status)
            *status = 400;
        return fail("invalid bottle id");
    }
    if (!strcmp(action, "record-compatibility")) {
        char e[96];
        ms_json* j = ms_json_parse(body ? (const char*)body : "", body ? len : 0, e, sizeof(e));
        bool ok = j && ms_json_type_of(j) == MS_JSON_OBJECT && save_matrix_record(home, j, id);
        ms_json_free(j);
        free(id);
        if (!ok) {
            if (status)
                *status = 404;
            return fail("compatibility case not found");
        }
        return ms_bottles_matrix_json(home);
    }
    if (!strcmp(action, "prepare")) {
        char* prepared = prepare_bottle_json(home, id);
        free(id);
        if (!prepared) {
            if (status)
                *status = 404;
            return fail("bottle not found");
        }
        return prepared;
    }
    if (!strcmp(action, "repair-component")) {
        char* repaired = repair_component_json(home, id, body, len, status);
        free(id);
        return repaired;
    }
    if (!strcmp(action, "apply-font-subs") || !strcmp(action, "seed-post-wineboot")) {
        char* tool = start_bottle_tool_json(home, id, action);
        free(id);
        if (!tool) {
            if (status)
                *status = 500;
            return fail("MetalSharp Wine not found or bottle prefix unavailable");
        }
        return tool;
    }
    if (strcmp(action, "sync-steam") && strcmp(action, "record-compatibility") && strcmp(action, "verify-directx") &&
        strcmp(action, "doctor") && strcmp(action, "refresh") && strcmp(action, "set-runtime-profile") &&
        strcmp(action, "edit") && strcmp(action, "set-windows-version") && strcmp(action, "get") &&
        strcmp(action, "apply-font-subs") && strcmp(action, "seed-post-wineboot") && strcmp(action, "prepare") &&
        strcmp(action, "repair-component")) {
        free(id);
        if (status)
            *status = 404;
        return fail("unsupported bottle action");
    }
    if (!strcmp(action, "verify-directx")) {
        char* report = verify_directx_json(home, id);
        free(id);
        if (!report) {
            if (status)
                *status = 404;
            return fail("bottle not found");
        }
        return report;
    }
    if (!strcmp(action, "doctor")) {
        char* report = doctor_bottle_json(home, id);
        free(id);
        if (!report) {
            if (status)
                *status = 404;
            return fail("bottle not found");
        }
        return report;
    }
    if (!strcmp(action, "refresh") || !strcmp(action, "set-runtime-profile") || !strcmp(action, "edit") ||
        !strcmp(action, "set-windows-version")) {
        char e[96];
        ms_json* j = ms_json_parse(body ? (const char*)body : "", body ? len : 0, e, sizeof(e));
        char *updated = NULL, *profile = NULL, *version = NULL;
        const ms_json* v = j ? ms_json_object_get(j, "profile") : NULL;
        if (!strcmp(action, "set-runtime-profile")) {
            if (!v || !ms_json_as_string(v, &profile) || !profile[0]) {
                free(profile);
                ms_json_free(j);
                free(id);
                if (status)
                    *status = 400;
                return fail("id and profile required");
            }
            if (strcmp(profile, "plain") && strcmp(profile, "launcher") && strcmp(profile, "game_install") &&
                strcmp(profile, "m9") && strcmp(profile, "m10") && strcmp(profile, "m10_32") &&
                strcmp(profile, "m11") && strcmp(profile, "m11_32") && strcmp(profile, "m12") &&
                strcmp(profile, "vkd3d") && strcmp(profile, "m13") && strcmp(profile, "d3dmetal") &&
                strcmp(profile, "dotnet") && strcmp(profile, "win32_dotnet") && strcmp(profile, "webview") &&
                strcmp(profile, "java_launcher") && strcmp(profile, "fna_arm64") && strcmp(profile, "fna_x86")) {
                free(profile);
                ms_json_free(j);
                free(id);
                return fail("unknown runtime profile");
            }
        }
        if (!strcmp(action, "set-windows-version")) {
            v = ms_json_object_get(j, "version");
            if (!v || !ms_json_as_string(v, &version) || !version[0]) {
                free(profile);
                free(version);
                ms_json_free(j);
                free(id);
                if (status)
                    *status = 400;
                return fail("version required");
            }
            if (strcmp(version, "win7") && strcmp(version, "win10") && strcmp(version, "win11")) {
                free(profile);
                free(version);
                ms_json_free(j);
                free(id);
                if (status)
                    *status = 400;
                return fail("windows version must be win7, win10, or win11");
            }
        }
        if (!strcmp(action, "edit")) {
            bool has_name = j && ms_json_object_get(j, "name") != NULL,
                 has_pipeline = j && ms_json_object_get(j, "preferredPipeline") != NULL;
            if (!has_name && !has_pipeline) {
                ms_json_free(j);
                free(id);
                return fail("name or preferredPipeline required");
            }
        }
        if (!strcmp(action, "set-windows-version")) {
            char* repair = start_windows_version_json(home, id, version);
            free(profile);
            free(version);
            ms_json_free(j);
            free(id);
            if (!repair) {
                if (status)
                    *status = 500;
                return fail("MetalSharp Wine not found or bottle prefix unavailable");
            }
            return repair;
        }
        if (!strcmp(action, "refresh"))
            updated = manifest(home, id);
        else
            updated = rewrite_manifest(home, id, j, action);
        if (!updated) {
            free(profile);
            free(version);
            ms_json_free(j);
            free(id);
            return fail("bottle not found");
        }
        char* vcpp_error = NULL;
        char* route_error = NULL;
        if (!strcmp(action, "edit"))
            vcpp_error = seed_vcpp_for_steam_bottle(home, id);
        if (!strcmp(action, "edit") || !strcmp(action, "set-runtime-profile") || !strcmp(action, "refresh"))
            route_error = ms_steam_prepare_bottle_route_json(home, id);
        ms_json_writer rw;
        ms_json_writer_init(&rw);
        ms_json_writer_object_begin(&rw);
        ms_json_writer_key(&rw, "ok");
        ms_json_writer_bool(&rw, true);
        if (!strcmp(action, "set-windows-version")) {
            ms_json_writer_key(&rw, "repair");
            ms_json_writer_object_begin(&rw);
            ms_json_writer_key(&rw, "id");
            ms_json_writer_string(&rw, id);
            ms_json_writer_key(&rw, "version");
            ms_json_writer_string(&rw, version);
            ms_json_writer_key(&rw, "ok");
            ms_json_writer_bool(&rw, true);
            ms_json_writer_object_end(&rw);
        } else {
            ms_json_writer_key(&rw, "bottle");
            ms_json_writer_raw(&rw, updated);
            ms_json_writer_key(&rw, "preflight");
            ms_json_writer_object_begin(&rw);
            ms_json_writer_key(&rw, "ok");
            ms_json_writer_bool(&rw, vcpp_error == NULL && route_error == NULL);
            if (vcpp_error || route_error) {
                ms_json_writer_key(&rw, "error");
                ms_json_writer_string(&rw, vcpp_error ? vcpp_error : route_error);
            } else {
                ms_json_writer_key(&rw, "skipped");
                ms_json_writer_bool(&rw, true);
                ms_json_writer_key(&rw, "reason");
                ms_json_writer_string(&rw, "VC++ runtime already seeded or not applicable");
            }
            ms_json_writer_object_end(&rw);
        }
        ms_json_writer_object_end(&rw);
        o = ms_json_writer_take(&rw);
        free(vcpp_error);
        free(route_error);
        free(updated);
        free(profile);
        free(version);
        ms_json_free(j);
        free(id);
        return o;
    }
    if (!strcmp(action, "get")) {
        raw = manifest(home, id);
        if (!raw) {
            char e[256];
            snprintf(e, sizeof(e), "bottle %s not found", id);
            free(id);
            return fail(e);
        }
        ms_json_writer w;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "bottle");
        ms_json_writer_raw(&w, raw);
        ms_json_writer_object_end(&w);
        o = ms_json_writer_take(&w);
        free(raw);
        free(id);
        return o;
    }
    raw = manifest(home, id);
    if (!raw) {
        char e[256];
        snprintf(e, sizeof(e), "bottle %s not found", id);
        free(id);
        return fail(e);
    }
    free(raw);
    free(id);
    return strdup("{\"ok\":true}");
}
