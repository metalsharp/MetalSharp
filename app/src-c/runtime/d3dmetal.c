#include "metalsharp_backend/d3dmetal.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef struct dstate {
    char id[129], name[256], game_dir[1024], game_exe[1024], error[256], step[9][20];
    unsigned appid;
    bool ready;
    unsigned long long updated;
    struct dstate* next;
} dstate;
static dstate* states;
static unsigned long long now_ms(void) {
    return (unsigned long long)time(NULL) * 1000ULL;
}
static ms_json* parse(const unsigned char* b, size_t n) {
    char e[64];
    return ms_json_parse(b ? (const char*)b : "", b ? n : 0, e, sizeof(e));
}
static bool num(const ms_json* j, const char* k, unsigned long long* o) {
    long long n;
    bool ok = ms_json_as_i64(ms_json_object_get(j, k), &n) && n > 0;
    if (ok)
        *o = (unsigned long long)n;
    return ok;
}
static char* text(const ms_json* j, const char* a, const char* b) {
    char* s = NULL;
    if (!ms_json_as_string(ms_json_object_get(j, a), &s) && b)
        ms_json_as_string(ms_json_object_get(j, b), &s);
    return s;
}
static char* bad(const char* s) {
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
static dstate* find_state_memory(const char* id) {
    dstate* s;
    for (s = states; s; s = s->next)
        if (!strcmp(s->id, id))
            return s;
    return NULL;
}
static char* path_join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    char* p = malloc(x + y + 2);
    if (!p)
        return NULL;
    snprintf(p, x + y + 2, "%s/%s", a, b);
    return p;
}
static bool mkdirs(const char* path) {
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
static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    long n;
    size_t got;
    char* s;
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
static void copy_field(char* dst, size_t n, const ms_json* j, const char* key, const char* fallback) {
    char* s = NULL;
    if (!ms_json_as_string(ms_json_object_get(j, key), &s))
        s = strdup(fallback);
    snprintf(dst, n, "%s", s ? s : "");
    free(s);
}
static dstate* load_state(const char* home, const char* id) {
    char *d = path_join(home, "d3dmetal-gptk/bottles"), *p, *raw;
    char e[64];
    ms_json* j;
    dstate* s;
    long long appid;
    bool ready;
    if (!d)
        return NULL;
    p = path_join(d, id);
    free(d);
    if (!p)
        return NULL;
    d = path_join(p, "state.json");
    free(p);
    raw = d ? read_file(d) : NULL;
    free(d);
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        ms_json_free(j);
        return NULL;
    }
    copy_field(s->id, sizeof(s->id), j, "bottle_id", id);
    copy_field(s->name, sizeof(s->name), j, "name", "D3DMetal Game");
    copy_field(s->game_dir, sizeof(s->game_dir), j, "game_dir", "");
    copy_field(s->game_exe, sizeof(s->game_exe), j, "game_exe", "");
    copy_field(s->error, sizeof(s->error), j, "last_error", "");
    copy_field(s->step[0], 20, j, "gptk_homebrew", "missing");
    copy_field(s->step[1], 20, j, "rosetta", "missing");
    copy_field(s->step[2], 20, j, "gptk_payload", "missing");
    copy_field(s->step[3], 20, j, "x64_redist", "missing");
    copy_field(s->step[4], 20, j, "seed", "missing");
    copy_field(s->step[5], 20, j, "gptk3", "missing");
    if (ms_json_as_i64(ms_json_object_get(j, "appid"), &appid) && appid > 0)
        s->appid = (unsigned)appid;
    if (ms_json_as_bool(ms_json_object_get(j, "play_ready"), &ready))
        s->ready = ready;
    if (ms_json_as_i64(ms_json_object_get(j, "updated_at"), &appid) && appid > 0)
        s->updated = (unsigned long long)appid;
    ms_json_free(j);
    s->next = states;
    states = s;
    return s;
}
static void step(ms_json_writer* w, const char* key, const char* value) {
    ms_json_writer_key(w, key);
    ms_json_writer_string(w, value);
}
static void state_json(ms_json_writer* w, const dstate* s) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "schema");
    ms_json_writer_u64(w, 1);
    step(w, "bottle_id", s->id);
    ms_json_writer_key(w, "appid");
    ms_json_writer_u64(w, s->appid);
    step(w, "name", s->name);
    step(w, "game_dir", s->game_dir);
    ms_json_writer_key(w, "game_exe");
    if (s->game_exe[0])
        ms_json_writer_string(w, s->game_exe);
    else
        ms_json_writer_null(w);
    step(w, "gptk_homebrew", s->step[0]);
    step(w, "rosetta", s->step[1]);
    step(w, "gptk_payload", s->step[2]);
    step(w, "x64_redist", s->step[3]);
    step(w, "seed", s->step[4]);
    step(w, "gptk3", s->step[5]);
    ms_json_writer_key(w, "play_ready");
    ms_json_writer_bool(w, s->ready);
    ms_json_writer_key(w, "last_error");
    if (s->error[0])
        ms_json_writer_string(w, s->error);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "last_launch_pid");
    ms_json_writer_null(w);
    ms_json_writer_key(w, "last_launch_log");
    ms_json_writer_null(w);
    ms_json_writer_key(w, "last_launch_status");
    ms_json_writer_null(w);
    ms_json_writer_key(w, "updated_at");
    ms_json_writer_u64(w, s->updated);
    ms_json_writer_object_end(w);
}
static bool save_state(const char* home, const dstate* s) {
    char *d = path_join(home, "d3dmetal-gptk/bottles"), *p, *raw;
    FILE* f;
    ms_json_writer w;
    if (!d || !mkdirs(d)) {
        free(d);
        return false;
    }
    p = path_join(d, s->id);
    free(d);
    if (!p || !mkdirs(p)) {
        free(p);
        return false;
    }
    d = path_join(p, "state.json");
    free(p);
    if (!d)
        return false;
    ms_json_writer_init(&w);
    state_json(&w, s);
    raw = ms_json_writer_take(&w);
    f = fopen(d, "wb");
    if (!f || !raw || fputs(raw, f) < 0) {
        if (f)
            fclose(f);
        free(d);
        free(raw);
        return false;
    }
    fclose(f);
    free(d);
    free(raw);
    return true;
}
static dstate* state_for(const char* home, const char* id) {
    dstate* s = find_state_memory(id);
    return s ? s : load_state(home, id);
}
static void actions_json(ms_json_writer* w, const dstate* s) {
    const char* ids[] = {"install_homebrew_gptk", "install_rosetta", "repair_gptk_payload",
                         "install_x64_redist",    "seed_prefix",     "play_d3dmetal"};
    const char* labels[] = {"Repair Homebrew GPTK", "Repair Rosetta", "Repair GPTK Payload",
                            "Repair Redist",        "Seed Prefix",    "Play D3DMetal"};
    const char* steps[] = {"gptk_homebrew", "rosetta", "gptk_payload", "x64_redist", "seed", "play_ready"};
    ms_json_writer_array_begin(w);
    for (size_t i = 0; i < 6; i++) {
        const char* sv = i == 0   ? s->step[0]
                         : i == 1 ? s->step[1]
                         : i == 2 ? s->step[2]
                         : i == 3 ? s->step[3]
                         : i == 4 ? s->step[4]
                                  : (s->ready ? "installed" : "missing");
        ms_json_writer_object_begin(w);
        step(w, "id", ids[i]);
        step(w, "label", labels[i]);
        ms_json_writer_key(w, "enabled");
        ms_json_writer_bool(w, i == 5 ? s->ready
                                      : strcmp(sv, "installed") != 0 && strcmp(sv, "updated") != 0 &&
                                            strcmp(sv, "seeded") != 0);
        step(w, "state", sv);
        step(w, "detail", steps[i]);
        ms_json_writer_object_end(w);
    }
    ms_json_writer_array_end(w);
}
static bool resolve_id(const ms_json* j, char* id, size_t n, unsigned long long* appid) {
    char* s = NULL;
    const ms_json* v = ms_json_object_get(j, "bottleId");
    if (!v)
        v = ms_json_object_get(j, "bottle_id");
    if (v)
        ms_json_as_string(v, &s);
    if (s && s[0])
        snprintf(id, n, "%s", s);
    else if (num(j, "appid", appid))
        snprintf(id, n, "steam_%llu", *appid);
    else {
        free(s);
        return false;
    }
    if (!*appid) {
        const ms_json* a = ms_json_object_get(j, "appid");
        if (a)
            num(j, "appid", appid);
    }
    free(s);
    return id[0] && strlen(id) <= 128;
}
char* ms_d3dmetal_json(const char* home, const char* action, const unsigned char* body, size_t len, int* status) {
    if (!strcmp(action, "repair-gptk3")) {
        pid_t pid = fork();
        int wait_status = 0;
        if (pid == 0) {
            execl("/usr/bin/open", "open", "https://developer.apple.com/download/all/?q=game%20porting%20toolkit",
                  (char*)NULL);
            _exit(127);
        }
        if (pid >= 0)
            while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
            }
        if (pid >= 0 && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
            return strdup("{\"ok\":true,\"download_opened\":true,\"download_required\":true,\"download_url\":\"https://"
                          "developer.apple.com/download/all/?q=game%20porting%20toolkit\"}");
        return bad("Could not launch /usr/bin/open");
    }
    ms_json* j = parse(body, len);
    char id[129] = {0}, *s, *game;
    unsigned long long appid = 0;
    dstate* st;
    ms_json_writer w;
    char* o;
    if (status)
        *status = 200;
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return bad("invalid JSON body");
    }
    if (!strcmp(action, "save")) {
        if (!num(j, "appid", &appid)) {
            ms_json_free(j);
            return bad("appid required");
        }
        if (!resolve_id(j, id, sizeof(id), &appid)) {
            ms_json_free(j);
            return bad("appid or bottleId required");
        }
        game = text(j, "gameDir", "game_dir");
        if (!game || !game[0]) {
            free(game);
            ms_json_free(j);
            return bad("game directory required");
        }
        st = state_for(home, id);
        if (!st) {
            st = calloc(1, sizeof(*st));
            st->next = states;
            states = st;
        }
        snprintf(st->id, sizeof(st->id), "%s", id);
        st->appid = (unsigned)appid;
        s = text(j, "name", NULL);
        snprintf(st->name, sizeof(st->name), "%s", s && s[0] ? s : "D3DMetal Game");
        free(s);
        snprintf(st->game_dir, sizeof(st->game_dir), "%s", game);
        snprintf(st->game_exe, sizeof(st->game_exe), "%s/game.exe", game);
        snprintf(st->step[0], 20, "missing");
        snprintf(st->step[1], 20, "missing");
        snprintf(st->step[2], 20, "missing");
        snprintf(st->step[3], 20, "missing");
        snprintf(st->step[4], 20, "missing");
        snprintf(st->step[5], 20, "missing");
        st->ready = false;
        st->updated = now_ms();
        save_state(home, st);
        free(game);
        ms_json_free(j);
        goto respond_state;
    }
    if (!resolve_id(j, id, sizeof(id), &appid)) {
        ms_json_free(j);
        return bad("appid or bottleId required");
    }
    st = state_for(home, id);
    if (!st) {
        ms_json_free(j);
        {
            char e[220];
            snprintf(e, sizeof(e), "D3DMetal GPTK state not found for %s", id);
            return bad(e);
        }
    }
    if (!strcmp(action, "install-homebrew-gptk")) {
        snprintf(st->step[0], 20, "installed");
    } else if (!strcmp(action, "install-rosetta")) {
        snprintf(st->step[1], 20, "installed");
    } else if (!strcmp(action, "repair-gptk-payload")) {
        if (strcmp(st->step[0], "installed")) {
            free(j);
            return bad("Homebrew GPTK must be installed before repairing the payload");
        }
        snprintf(st->step[2], 20, "updated");
    } else if (!strcmp(action, "install-x64-redist")) {
        snprintf(st->step[3], 20, "installed");
    } else if (!strcmp(action, "seed-prefix")) {
        snprintf(st->step[4], 20, "seeded");
    } else if (!strcmp(action, "repair-gptk3")) {
        snprintf(st->step[5], 20, "installed");
    } else if (!strcmp(action, "play")) {
        if (!st->ready) {
            free(j);
            return bad("D3DMetal bottle is not ready to play");
        }
        free(j);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "launch");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, st->appid);
        ms_json_writer_key(&w, "bottle_id");
        ms_json_writer_string(&w, st->id);
        ms_json_writer_key(&w, "game_exe");
        ms_json_writer_string(&w, st->game_exe);
        ms_json_writer_key(&w, "launch_mode");
        ms_json_writer_string(&w, "d3dmetal_direct_game_exe");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    } else if (!strcmp(action, "status")) {
    } else {
        free(j);
        return bad("unknown D3DMetal action");
    }
    st->ready = !strcmp(st->step[0], "installed") && !strcmp(st->step[1], "installed") &&
                !strcmp(st->step[2], "updated") && !strcmp(st->step[3], "installed") && !strcmp(st->step[4], "seeded");
    st->updated = now_ms();
    save_state(home, st);
    free(j);
respond_state:
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "state");
    state_json(&w, st);
    ms_json_writer_key(&w, "actions");
    actions_json(&w, st);
    ms_json_writer_key(&w, "gptk3_installed");
    ms_json_writer_bool(&w, !strcmp(st->step[5], "installed"));
    ms_json_writer_key(&w, "gptk3_dmg_found");
    ms_json_writer_bool(&w, false);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
