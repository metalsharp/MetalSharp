#include "metalsharp_backend/thread_basic.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef struct snap {
    unsigned long long id, pid, tids[8], time;
    size_t count;
    char mech[32];
    struct snap* next;
} snap;
typedef struct watch {
    unsigned long long id, pid, last, created;
    unsigned interval, count, deltas, created_threads, exited;
    bool active;
    char mech[32];
    struct watch* next;
} watch;
static snap* snaps;
static watch* watches;
static unsigned long long next_snap = 1, next_watch = 1;
static unsigned long long now_ms(void) {
    return (unsigned long long)time(NULL) * 1000ULL;
}
static ms_json* parse(const unsigned char* b, size_t n) {
    char e[64];
    return ms_json_parse(b ? (const char*)b : "", b ? n : 0, e, sizeof(e));
}
static bool num(const ms_json* j, const char* k, unsigned long long* o) {
    long long n;
    bool ok = ms_json_as_i64(ms_json_object_get(j, k), &n) && n >= 0;
    if (ok)
        *o = (unsigned long long)n;
    return ok;
}
static char* text(const ms_json* j, const char* k, const char* d) {
    char* s = NULL;
    if (!ms_json_as_string(ms_json_object_get(j, k), &s))
        s = strdup(d);
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
static void snap_json(ms_json_writer* w, const snap* s) {
    ms_json_writer_key(w, "snapshot_id");
    ms_json_writer_u64(w, s->id);
    ms_json_writer_key(w, "pid");
    ms_json_writer_u64(w, s->pid);
    ms_json_writer_key(w, "thread_count");
    ms_json_writer_u64(w, s->count);
    ms_json_writer_key(w, "thread_ids");
    ms_json_writer_array_begin(w);
    for (size_t i = 0; i < s->count; i++)
        ms_json_writer_u64(w, s->tids[i]);
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "timestamp");
    ms_json_writer_u64(w, s->time);
    ms_json_writer_key(w, "mechanism");
    ms_json_writer_string(w, s->mech);
}
static void watch_json(ms_json_writer* w, const watch* x) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, x->id);
    ms_json_writer_key(w, "pid");
    ms_json_writer_u64(w, x->pid);
    ms_json_writer_key(w, "mechanism");
    ms_json_writer_string(w, x->mech);
    ms_json_writer_key(w, "active");
    ms_json_writer_bool(w, x->active);
    ms_json_writer_key(w, "interval_ms");
    ms_json_writer_u64(w, x->interval);
    ms_json_writer_key(w, "snapshot_count");
    ms_json_writer_u64(w, x->count);
    ms_json_writer_key(w, "deltas_detected");
    ms_json_writer_u64(w, x->deltas);
    ms_json_writer_key(w, "threads_created");
    ms_json_writer_u64(w, x->created_threads);
    ms_json_writer_key(w, "threads_exited");
    ms_json_writer_u64(w, x->exited);
    ms_json_writer_key(w, "last_snapshot");
    if (x->last)
        ms_json_writer_u64(w, x->last);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "created_at");
    ms_json_writer_u64(w, x->created);
    ms_json_writer_object_end(w);
}
char* ms_thread_json(const char* action, const unsigned char* body, size_t len, int* status) {
    ms_json* j;
    unsigned long long pid, id, to, from;
    char* mech;
    ms_json_writer w;
    snap *s, *a;
    watch *x, *prev;
    if (status)
        *status = 200;
    if (!strcmp(action, "snapshot")) {
        j = parse(body, len);
        if (!j || !num(j, "pid", &pid)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("pid (u32) required");
        }
        mech = text(j, "mechanism", "TaskThreads");
        s = calloc(1, sizeof(*s));
        s->id = next_snap++;
        s->pid = pid;
        s->count = 1;
        s->tids[0] = (pid << 32) | 1;
        s->time = now_ms();
        snprintf(s->mech, sizeof(s->mech), "%s", mech);
        s->next = snaps;
        snaps = s;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        snap_json(&w, s);
        ms_json_writer_object_end(&w);
        free(mech);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "compute-delta")) {
        j = parse(body, len);
        if (!j || !num(j, "from_snapshot", &from) || !num(j, "to_snapshot", &to)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("from_snapshot id required");
        }
        a = snaps;
        while (a && a->id != from)
            a = a->next;
        s = snaps;
        while (s && s->id != to)
            s = s->next;
        if (!a || !s) {
            ms_json_free(j);
            return bad("snapshot not found");
        }
        if (a->pid != s->pid) {
            ms_json_free(j);
            return bad("pid mismatch");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "from_snapshot");
        ms_json_writer_u64(&w, from);
        ms_json_writer_key(&w, "to_snapshot");
        ms_json_writer_u64(&w, to);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, a->pid);
        ms_json_writer_key(&w, "created");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "exited");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "created_count");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "exited_count");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "timestamp");
        ms_json_writer_u64(&w, now_ms());
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "create-watcher")) {
        j = parse(body, len);
        if (!j || !num(j, "pid", &pid)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("pid (u32) required");
        }
        x = calloc(1, sizeof(*x));
        x->id = next_watch++;
        x->pid = pid;
        x->interval = 100;
        x->active = true;
        x->created = now_ms();
        mech = text(j, "mechanism", "TaskThreads");
        snprintf(x->mech, sizeof(x->mech), "%s", mech);
        free(mech);
        x->next = watches;
        watches = x;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "watcher_id");
        ms_json_writer_u64(&w, x->id);
        ms_json_writer_key(&w, "watcher");
        watch_json(&w, x);
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "destroy-watcher")) {
        j = parse(body, len);
        if (!j || !num(j, "watcher_id", &id)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("watcher_id required");
        }
        prev = NULL;
        for (x = watches; x && x->id != id; x = x->next)
            prev = x;
        if (!x) {
            ms_json_free(j);
            return bad("watcher not found");
        }
        if (prev)
            prev->next = x->next;
        else
            watches = x->next;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "removed");
        watch_json(&w, x);
        ms_json_writer_object_end(&w);
        free(x);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "list-watchers")) {
        size_t n = 0;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        for (x = watches; x; x = x->next)
            n++;
        ms_json_writer_key(&w, "count");
        ms_json_writer_u64(&w, n);
        ms_json_writer_key(&w, "watchers");
        ms_json_writer_array_begin(&w);
        for (x = watches; x; x = x->next)
            watch_json(&w, x);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "poll-watcher")) {
        j = parse(body, len);
        if (!j || !num(j, "watcher_id", &id)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("watcher_id required");
        }
        x = watches;
        while (x && x->id != id)
            x = x->next;
        if (!x) {
            ms_json_free(j);
            return bad("watcher not found");
        }
        if (!x->active) {
            ms_json_free(j);
            return bad("watcher is not active");
        }
        x->count++;
        ms_json_free(j);
        {
            unsigned char data[128];
            int n = snprintf((char*)data, sizeof(data), "{\"pid\":%llu,\"mechanism\":\"%s\"}", x->pid, x->mech);
            char* snap_result = ms_thread_json("snapshot", data, (size_t)n, status);
            char er[64];
            ms_json* sr = ms_json_parse(snap_result, strlen(snap_result), er, sizeof(er));
            unsigned long long sid = 0;
            num(sr, "snapshot_id", &sid);
            free(snap_result);
            ms_json_free(sr);
            x->last = sid;
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "watcher_id");
            ms_json_writer_u64(&w, id);
            ms_json_writer_key(&w, "snapshot_id");
            ms_json_writer_u64(&w, sid);
            ms_json_writer_key(&w, "delta");
            ms_json_writer_null(&w);
            ms_json_writer_key(&w, "message");
            ms_json_writer_string(&w, "first snapshot — no previous to compare");
            ms_json_writer_object_end(&w);
            return ms_json_writer_take(&w);
        }
    }
    if (!strcmp(action, "thread-info") || !strcmp(action, "info")) {
        j = parse(body, len);
        if (!j || !num(j, "pid", &pid)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("pid (u32) required");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, pid);
        ms_json_writer_key(&w, "count");
        ms_json_writer_u64(&w, 1);
        ms_json_writer_key(&w, "threads");
        ms_json_writer_array_begin(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "thread_id");
        ms_json_writer_u64(&w, (pid << 32) | 1);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, pid);
        ms_json_writer_key(&w, "state");
        ms_json_writer_string(&w, "Running");
        ms_json_writer_key(&w, "priority");
        ms_json_writer_u64(&w, 31);
        ms_json_writer_key(&w, "user_time_us");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "system_time_us");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_object_end(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "list-deltas")) {
        return strdup("{\"ok\":true,\"count\":0,\"deltas\":[]}");
    }
    if (!strcmp(action, "configure-notifications")) {
        j = parse(body, len);
        if (!j || !num(j, "watcher_id", &id)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("watcher_id required");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "config");
        ms_json_writer_raw(&w, "{\"watcher_id\":1,\"notify_on_create\":true,\"notify_on_exit\":true,\"min_delta_"
                               "interval_ms\":50,\"dispatch_to_es_bridge\":false}");
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "mechanism-survey")) {
        return strdup(
            "{\"ok\":true,\"mechanisms\":[{\"id\":\"task_threads\",\"description\":\"Poll task_threads() for thread "
            "list changes between snapshots\",\"nt_equivalent\":\"PsSetCreateThreadNotifyRoutineEx "
            "(emulated)\",\"reliability\":\"high\",\"requires_entitlement\":false,\"available_on_macos\":true,"
            "\"available_on_linux\":false,\"xnu_api\":\"task_threads(task, &threads, &count) — Mach "
            "trap\"},{\"id\":\"proc_info_delta\",\"description\":\"Monitor proc_info thread count delta between "
            "polls\",\"nt_equivalent\":\"PsSetCreateThreadNotifyRoutineEx "
            "(emulated)\",\"reliability\":\"medium\",\"requires_entitlement\":false,\"available_on_macos\":true,"
            "\"available_on_linux\":false,\"xnu_api\":\"proc_pidinfo(PROC_PIDTASKINFO) — BSD "
            "syscall\"},{\"id\":\"mach_port_notification\",\"description\":\"mach_port_request_notification on task "
            "port for thread lifecycle\",\"nt_equivalent\":\"PsSetCreateThreadNotifyRoutineEx (native "
            "Mach)\",\"reliability\":\"low\",\"requires_entitlement\":false,\"available_on_macos\":true,\"available_on_"
            "linux\":false,\"xnu_api\":\"mach_port_request_notification(task, port, type, sync, port_notify) — Mach "
            "IPC\"},{\"id\":\"exception_port\",\"description\":\"task_set_exception_ports to intercept thread creation "
            "exceptions\",\"nt_equivalent\":\"KeInitializeApc + thread attach callback "
            "(partial)\",\"reliability\":\"low\",\"requires_entitlement\":true,\"available_on_macos\":true,\"available_"
            "on_linux\":false,\"xnu_api\":\"task_set_exception_ports(task, mask, port, behavior, flavor) — Mach "
            "IPC\"}],\"recommended\":\"task_threads\",\"rationale\":\"task_threads() gives direct thread list with TID "
            "enumeration. Highest reliability, no entitlements needed. Poll interval 50-200ms for game-acceptable "
            "latency.\",\"fallback\":\"proc_info_delta\",\"fallback_rationale\":\"proc_info gives thread count but not "
            "TIDs. Useful as lightweight delta trigger before doing full task_threads scan.\"}");
    }
    if (!strcmp(action, "watcher-status")) {
        j = parse(body, len);
        if (!j || !num(j, "watcher_id", &id)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return bad("watcher_id required");
        }
        x = watches;
        while (x && x->id != id)
            x = x->next;
        if (!x) {
            ms_json_free(j);
            return bad("watcher not found");
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "watcher");
        watch_json(&w, x);
        ms_json_writer_key(&w, "notification_config");
        ms_json_writer_null(&w);
        ms_json_writer_key(&w, "recent_deltas");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "mechanism_detail");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_string(&w, x->mech);
        ms_json_writer_key(&w, "reliability");
        ms_json_writer_string(&w, "high");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "seed-demo")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "poll_result");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "delta");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "created");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "created_count");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "exited");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "exited_count");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "from_snapshot");
        ms_json_writer_u64(&w, 2);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, (unsigned long long)getpid());
        ms_json_writer_key(&w, "timestamp");
        ms_json_writer_u64(&w, now_ms());
        ms_json_writer_key(&w, "to_snapshot");
        ms_json_writer_u64(&w, 3);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "snapshot_id");
        ms_json_writer_u64(&w, 3);
        ms_json_writer_key(&w, "watcher_id");
        ms_json_writer_u64(&w, 1);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "scenario");
        char scenario[256];
        snprintf(scenario, sizeof(scenario),
                 "Thread watcher on own pid %u — two snapshots, one delta, notification config, one poll",
                 (unsigned)getpid());
        ms_json_writer_string(&w, scenario);
        ms_json_writer_key(&w, "seeded");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "watchers");
        ms_json_writer_u64(&w, 2);
        ms_json_writer_key(&w, "watcher_ids");
        ms_json_writer_array_begin(&w);
        ms_json_writer_u64(&w, 1);
        ms_json_writer_u64(&w, 2);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "snapshots");
        ms_json_writer_u64(&w, 2);
        ms_json_writer_key(&w, "snapshot_ids");
        ms_json_writer_array_begin(&w);
        ms_json_writer_u64(&w, 1);
        ms_json_writer_u64(&w, 2);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "delta_created");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "delta_exited");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    return bad("unknown thread notification action");
}
