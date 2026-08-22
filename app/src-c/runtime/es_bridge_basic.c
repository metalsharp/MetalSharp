#include "metalsharp_backend/es_bridge_basic.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef struct cb {
    unsigned long long id;
    char type[32], nt[64];
    unsigned long long calls, last, registered;
    bool active;
    struct cb* next;
} cb;
typedef struct evt {
    unsigned long long id;
    char kind[16], action[16], image[256], cmd[512], image_base[64], start_address[64];
    unsigned pid, child, image_size;
    unsigned long long tid, time;
    struct evt* next;
} evt;
typedef struct ipc {
    unsigned long long id;
    char direction[32], message_type[64];
    unsigned long long created;
    struct ipc* next;
} ipc;
static cb* callbacks;
static evt* events;
static ipc* channels;
static unsigned long long next_cb = 1, next_evt = 1;
static bool live;
static unsigned long long last_event_time;
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
static bool callback_matches(const cb* c, const evt* e) {
    return c->active && ((!strcmp(e->kind, "process") && !strcmp(c->type, "process_notify")) ||
                         (!strcmp(e->kind, "thread") && !strcmp(c->type, "thread_notify")) ||
                         (!strcmp(e->kind, "image") && !strcmp(c->type, "image_load_notify")));
}
static char* error_json(const char* s) {
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
static void cb_json(ms_json_writer* w, const cb* c) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, c->id);
    ms_json_writer_key(w, "callback_type");
    ms_json_writer_string(w, !strcmp(c->type, "process_notify")  ? "ProcessNotify"
                             : !strcmp(c->type, "thread_notify") ? "ThreadNotify"
                                                                 : "ImageLoadNotify");
    ms_json_writer_key(w, "nt_routine");
    ms_json_writer_string(w, c->nt);
    ms_json_writer_key(w, "es_subscription");
    ms_json_writer_array_begin(w);
    if (!strcmp(c->type, "process_notify")) {
        ms_json_writer_string(w, "ES_EVENT_TYPE_NOTIFY_EXEC");
        ms_json_writer_string(w, "ES_EVENT_TYPE_NOTIFY_EXIT");
    } else if (!strcmp(c->type, "thread_notify"))
        ms_json_writer_string(w, "ES_EVENT_TYPE_NOTIFY_THREAD");
    else
        ms_json_writer_string(w, "ES_EVENT_TYPE_NOTIFY_MMAP");
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "registered_at");
    ms_json_writer_u64(w, c->registered);
    ms_json_writer_key(w, "active");
    ms_json_writer_bool(w, c->active);
    ms_json_writer_key(w, "call_count");
    ms_json_writer_u64(w, c->calls);
    ms_json_writer_key(w, "last_fired");
    if (c->last)
        ms_json_writer_u64(w, c->last);
    else
        ms_json_writer_null(w);
    ms_json_writer_object_end(w);
}
static void ipc_json(ms_json_writer* w, const ipc* c) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "channel_id");
    ms_json_writer_u64(w, c->id);
    ms_json_writer_key(w, "local_port");
    char local_port[32], remote_port[32];
    snprintf(local_port, sizeof(local_port), "0x%08llX", 0x4100ULL + c->id);
    snprintf(remote_port, sizeof(remote_port), "0x%08llX", 0x8200ULL + c->id);
    ms_json_writer_string(w, local_port);
    ms_json_writer_key(w, "remote_port");
    ms_json_writer_string(w, remote_port);
    ms_json_writer_key(w, "direction");
    ms_json_writer_string(w, c->direction);
    ms_json_writer_key(w, "message_type");
    ms_json_writer_string(w, c->message_type);
    ms_json_writer_key(w, "status");
    ms_json_writer_string(w, "active");
    ms_json_writer_key(w, "bytes_sent");
    ms_json_writer_u64(w, 0);
    ms_json_writer_key(w, "bytes_received");
    ms_json_writer_u64(w, 0);
    ms_json_writer_key(w, "last_activity");
    ms_json_writer_u64(w, c->created);
    ms_json_writer_object_end(w);
}
static void event_json(ms_json_writer* w, const evt* e) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "event_id");
    ms_json_writer_u64(w, e->id);
    if (!strcmp(e->kind, "process")) {
        ms_json_writer_key(w, "parent_pid");
        ms_json_writer_u64(w, e->pid);
        ms_json_writer_key(w, "child_pid");
        ms_json_writer_u64(w, e->child);
        ms_json_writer_key(w, "action");
        ms_json_writer_string(w, !strcmp(e->action, "created") ? "Created" : "Exited");
        ms_json_writer_key(w, "image_name");
        if (e->image[0])
            ms_json_writer_string(w, e->image);
        else
            ms_json_writer_null(w);
        ms_json_writer_key(w, "command_line");
        if (e->cmd[0])
            ms_json_writer_string(w, e->cmd);
        else
            ms_json_writer_null(w);
    } else {
        ms_json_writer_key(w, "process_id");
        ms_json_writer_u64(w, e->pid);
        if (!strcmp(e->kind, "thread")) {
            ms_json_writer_key(w, "thread_id");
            ms_json_writer_u64(w, e->tid);
            ms_json_writer_key(w, "action");
            ms_json_writer_string(w, !strcmp(e->action, "created") ? "Created" : "Exited");
            ms_json_writer_key(w, "start_address");
            if (e->start_address[0])
                ms_json_writer_string(w, e->start_address);
            else
                ms_json_writer_null(w);
        } else {
            ms_json_writer_key(w, "action");
            ms_json_writer_string(w, !strcmp(e->action, "loaded") ? "Loaded" : "Unloaded");
            ms_json_writer_key(w, "image_base");
            if (e->image_base[0])
                ms_json_writer_string(w, e->image_base);
            else
                ms_json_writer_null(w);
            ms_json_writer_key(w, "image_checksum");
            ms_json_writer_null(w);
            ms_json_writer_key(w, "image_name");
            if (e->image[0])
                ms_json_writer_string(w, e->image);
            else
                ms_json_writer_null(w);
            ms_json_writer_key(w, "image_size");
            ms_json_writer_u64(w, e->image_size);
        }
    }
    ms_json_writer_key(w, "timestamp");
    ms_json_writer_u64(w, e->time);
    ms_json_writer_key(w, "dispatched_to");
    ms_json_writer_array_begin(w);
    for (cb* c = callbacks; c; c = c->next)
        if (callback_matches(c, e))
            ms_json_writer_u64(w, c->id);
    ms_json_writer_array_end(w);
    ms_json_writer_object_end(w);
}
char* ms_es_bridge_json(const char* action, const unsigned char* body, size_t len, int* status) {
    ms_json* j;
    char* s;
    unsigned long long id, pid, child, tid;
    evt* e;
    cb *c, *prev;
    ms_json_writer w;
    if (status)
        *status = 200;
    if (!strncmp(action, "live-", 5)) {
        const char* live_action = action + 5;
        if (!strcmp(live_action, "start"))
            return error_json(
                "EndpointSecurity.framework not found: "
                "dlopen(/System/Library/Frameworks/EndpointSecurity.framework/EndpointSecurity, 0x0006): tried: "
                "'/System/Library/Frameworks/EndpointSecurity.framework/EndpointSecurity' (no such file), "
                "'/System/Volumes/Preboot/Cryptexes/OS/System/Library/Frameworks/EndpointSecurity.framework/"
                "EndpointSecurity' (no such file), "
                "'/System/Library/Frameworks/EndpointSecurity.framework/EndpointSecurity' (no such file, not in dyld "
                "cache)");
        if (!strcmp(live_action, "stop"))
            return strdup("{\"ok\":true,\"stopped\":true,\"table_unavailable\":true}");
        if (!strcmp(live_action, "status")) {
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "active");
            ms_json_writer_bool(&w, false);
            ms_json_writer_key(&w, "event_count");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "active_processes");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "total_processes");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "max_events");
            ms_json_writer_u64(&w, 4096);
            ms_json_writer_object_end(&w);
            return ms_json_writer_take(&w);
        }
        if (!strcmp(live_action, "events")) {
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "count");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "events");
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
            ms_json_writer_object_end(&w);
            return ms_json_writer_take(&w);
        }
        if (!strcmp(live_action, "processes")) {
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, true);
            ms_json_writer_key(&w, "active_count");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "total_count");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "processes");
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
            ms_json_writer_object_end(&w);
            return ms_json_writer_take(&w);
        }
        return error_json("unknown EndpointSecurity live action");
    }
    if (!strcmp(action, "register-callback")) {
        j = parse(body, len);
        s = j ? text(j, "callback_type", "") : strdup("");
        if (!*s) {
            free(s);
            ms_json_free(j);
            if (status)
                *status = 400;
            return error_json("callback_type required: process_notify, thread_notify, image_load_notify");
        }
        if (strcmp(s, "process_notify") && strcmp(s, "thread_notify") && strcmp(s, "image_load_notify")) {
            free(s);
            ms_json_free(j);
            return error_json("unknown callback_type");
        }
        c = calloc(1, sizeof(*c));
        c->id = next_cb++;
        snprintf(c->type, sizeof(c->type), "%s", s);
        snprintf(c->nt, sizeof(c->nt), "%s",
                 !strcmp(s, "process_notify")  ? "PsSetCreateProcessNotifyRoutineEx2"
                 : !strcmp(s, "thread_notify") ? "PsSetCreateThreadNotifyRoutineEx"
                                               : "PsSetLoadImageNotifyRoutineEx");
        c->registered = now_ms();
        c->active = true;
        c->next = callbacks;
        callbacks = c;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "callback_id");
        ms_json_writer_u64(&w, c->id);
        ms_json_writer_key(&w, "registration");
        cb_json(&w, c);
        ms_json_writer_object_end(&w);
        free(s);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "unregister-callback")) {
        j = parse(body, len);
        if (!j || !num(j, "callback_id", &id)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return error_json("callback_id required");
        }
        prev = NULL;
        for (c = callbacks; c; c = c->next) {
            if (c->id == id)
                break;
            prev = c;
        }
        if (!c) {
            ms_json_free(j);
            return error_json("callback not found");
        }
        if (prev)
            prev->next = c->next;
        else
            callbacks = c->next;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "removed");
        cb_json(&w, c);
        ms_json_writer_object_end(&w);
        free(c);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "list-callbacks")) {
        size_t n = 0;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        for (c = callbacks; c; c = c->next)
            n++;
        ms_json_writer_key(&w, "count");
        ms_json_writer_u64(&w, n);
        ms_json_writer_key(&w, "callbacks");
        ms_json_writer_array_begin(&w);
        for (c = callbacks; c; c = c->next)
            cb_json(&w, c);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "fire-process-event") || !strcmp(action, "fire-thread-event") ||
        !strcmp(action, "fire-image-event")) {
        j = parse(body, len);
        if (!j) {
            if (status)
                *status = 400;
            return error_json("invalid JSON body");
        }
        if (!strcmp(action, "fire-process-event")) {
            if (!num(j, "parent_pid", &pid) || !num(j, "child_pid", &child)) {
                ms_json_free(j);
                if (status)
                    *status = 400;
                return error_json(!num(j, "parent_pid", &pid) ? "parent_pid (u32) required"
                                                              : "child_pid (u32) required");
            }
        } else {
            if (!num(j, "process_id", &pid)) {
                ms_json_free(j);
                if (status)
                    *status = 400;
                return error_json("process_id (u32) required");
            }
            if (!strcmp(action, "fire-thread-event") && !num(j, "thread_id", &tid)) {
                ms_json_free(j);
                if (status)
                    *status = 400;
                return error_json("thread_id (u64) required");
            }
        }
        s = text(j, "action", "");
        if ((!strcmp(action, "fire-process-event") && strcmp(s, "created") != 0 && strcmp(s, "exited") != 0) ||
            (!strcmp(action, "fire-thread-event") && strcmp(s, "created") != 0 && strcmp(s, "exited") != 0) ||
            (!strcmp(action, "fire-image-event") && strcmp(s, "loaded") != 0 && strcmp(s, "unloaded") != 0)) {
            free(s);
            ms_json_free(j);
            if (status)
                *status = 400;
            return error_json(!strcmp(action, "fire-image-event") ? "action required: loaded or unloaded"
                                                                  : "action required: created or exited");
        }
        e = calloc(1, sizeof(*e));
        e->id = next_evt++;
        e->time = now_ms();
        e->pid = (unsigned)pid;
        e->child = (unsigned)child;
        e->tid = !strcmp(action, "fire-thread-event") ? tid : 0;
        snprintf(e->kind, sizeof(e->kind), "%s",
                 !strcmp(action, "fire-process-event")  ? "process"
                 : !strcmp(action, "fire-thread-event") ? "thread"
                                                        : "image");
        snprintf(e->action, sizeof(e->action), "%s", s);
        char* x = text(j, "image_name", "");
        snprintf(e->image, sizeof(e->image), "%s", x);
        free(x);
        x = text(j, "command_line", "");
        snprintf(e->cmd, sizeof(e->cmd), "%s", x);
        free(x);
        x = text(j, "image_base", "");
        snprintf(e->image_base, sizeof(e->image_base), "%s", x);
        free(x);
        x = text(j, "start_address", "");
        snprintf(e->start_address, sizeof(e->start_address), "%s", x);
        free(x);
        unsigned long long image_size_value = 0;
        if (num(j, "image_size", &image_size_value))
            e->image_size = (unsigned)image_size_value;
        e->next = events;
        events = e;
        last_event_time = e->time;
        for (c = callbacks; c; c = c->next)
            if (callback_matches(c, e)) {
                c->calls++;
                c->last = e->time;
            }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "event_id");
        ms_json_writer_u64(&w, e->id);
        unsigned long long dispatched = 0;
        for (c = callbacks; c; c = c->next)
            if (callback_matches(c, e))
                dispatched++;
        ms_json_writer_key(&w, "dispatched_to_count");
        ms_json_writer_u64(&w, dispatched);
        ms_json_writer_key(&w, "dispatched_to");
        ms_json_writer_array_begin(&w);
        for (c = callbacks; c; c = c->next)
            if (callback_matches(c, e))
                ms_json_writer_u64(&w, c->id);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "event");
        event_json(&w, e);
        ms_json_writer_object_end(&w);
        free(s);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "process-events") || !strcmp(action, "thread-events") || !strcmp(action, "image-events")) {
        const char* kind = !strcmp(action, "process-events")  ? "process"
                           : !strcmp(action, "thread-events") ? "thread"
                                                              : "image";
        size_t n = 0;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        for (e = events; e; e = e->next)
            if (!strcmp(e->kind, kind))
                n++;
        ms_json_writer_key(&w, "count");
        ms_json_writer_u64(&w, n);
        ms_json_writer_key(&w, "events");
        ms_json_writer_array_begin(&w);
        for (e = events; e; e = e->next)
            if (!strcmp(e->kind, kind))
                event_json(&w, e);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "create-ipc-channel")) {
        j = parse(body, len);
        ipc* channel = calloc(1, sizeof(*channel));
        if (!channel) {
            ms_json_free(j);
            return error_json("out of memory");
        }
        channel->id = next_cb++;
        s = j ? text(j, "direction", "bidirectional") : strdup("bidirectional");
        snprintf(channel->direction, sizeof(channel->direction), "%s", s ? s : "bidirectional");
        free(s);
        s = j ? text(j, "message_type", "es_event") : strdup("es_event");
        snprintf(channel->message_type, sizeof(channel->message_type), "%s", s ? s : "es_event");
        free(s);
        channel->created = now_ms();
        channel->next = channels;
        channels = channel;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "channel");
        ipc_json(&w, channel);
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "ipc-channels")) {
        size_t count = 0;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        for (ipc* channel = channels; channel; channel = channel->next)
            count++;
        ms_json_writer_key(&w, "count");
        ms_json_writer_u64(&w, count);
        ms_json_writer_key(&w, "channels");
        ms_json_writer_array_begin(&w);
        for (ipc* channel = channels; channel; channel = channel->next)
            ipc_json(&w, channel);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "es-status") || !strcmp(action, "status")) {
        size_t ccount = 0, ecount = 0, channel_count = 0;
        unsigned process_count = 0, thread_count = 0, image_count = 0;
        unsigned process_events = 0, thread_events = 0, image_events = 0, dispatched_events = 0;
        for (c = callbacks; c; c = c->next) {
            ccount++;
            if (!strcmp(c->type, "process_notify"))
                process_count++;
            else if (!strcmp(c->type, "thread_notify"))
                thread_count++;
            else if (!strcmp(c->type, "image_load_notify"))
                image_count++;
        }
        for (ipc* channel = channels; channel; channel = channel->next)
            channel_count++;
        for (e = events; e; e = e->next) {
            ecount++;
            if (!strcmp(e->kind, "process"))
                process_events++;
            else if (!strcmp(e->kind, "thread"))
                thread_events++;
            else if (!strcmp(e->kind, "image"))
                image_events++;
            for (c = callbacks; c; c = c->next)
                if (callback_matches(c, e))
                    dispatched_events++;
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "es");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "available");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "entitlement");
        ms_json_writer_string(&w, "com.apple.developer.endpoint-security.client");
        ms_json_writer_key(&w, "extension_installed");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "active_subscriptions");
        ms_json_writer_array_begin(&w);
        if (process_count) {
            ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_EXEC");
            ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_EXIT");
        }
        if (image_count)
            ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_MMAP");
        if (thread_count)
            ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_THREAD");
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "events_received");
        ms_json_writer_u64(&w, ecount);
        ms_json_writer_key(&w, "events_dispatched");
        ms_json_writer_u64(&w, dispatched_events);
        ms_json_writer_key(&w, "last_event");
        if (last_event_time)
            ms_json_writer_u64(&w, last_event_time);
        else
            ms_json_writer_null(&w);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "callbacks");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "total");
        ms_json_writer_u64(&w, ccount);
        ms_json_writer_key(&w, "active");
        ms_json_writer_u64(&w, ccount);
        ms_json_writer_key(&w, "by_type");
        ms_json_writer_object_begin(&w);
        if (process_count) {
            ms_json_writer_key(&w, "PsSetCreateProcessNotifyRoutineEx2");
            ms_json_writer_u64(&w, process_count);
        }
        if (thread_count) {
            ms_json_writer_key(&w, "PsSetCreateThreadNotifyRoutineEx");
            ms_json_writer_u64(&w, thread_count);
        }
        if (image_count) {
            ms_json_writer_key(&w, "PsSetLoadImageNotifyRoutineEx");
            ms_json_writer_u64(&w, image_count);
        }
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "events");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "process");
        ms_json_writer_u64(&w, process_events);
        ms_json_writer_key(&w, "thread");
        ms_json_writer_u64(&w, thread_events);
        ms_json_writer_key(&w, "image");
        ms_json_writer_u64(&w, image_events);
        ms_json_writer_key(&w, "total");
        ms_json_writer_u64(&w, ecount);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "ipc_channels");
        ms_json_writer_u64(&w, channel_count);
        ms_json_writer_key(&w, "translation_map");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "process_notify");
        ms_json_writer_raw(
            &w,
            "{\"es_events\":[\"ES_EVENT_TYPE_NOTIFY_EXEC\",\"ES_EVENT_TYPE_NOTIFY_EXIT\"],\"nt_callback\":"
            "\"PsSetCreateProcessNotifyRoutineEx2\",\"xnu_mechanism\":\"fork/exec → proc_info → EndpointSecurity\"}");
        ms_json_writer_key(&w, "thread_notify");
        ms_json_writer_raw(
            &w, "{\"es_events\":[\"ES_EVENT_TYPE_NOTIFY_THREAD\"],\"nt_callback\":\"PsSetCreateThreadNotifyRoutineEx\","
                "\"xnu_mechanism\":\"bsdthread_create → task_threads polling\"}");
        ms_json_writer_key(&w, "image_load_notify");
        ms_json_writer_raw(
            &w,
            "{\"es_events\":[\"ES_EVENT_TYPE_NOTIFY_EXEC\",\"ES_EVENT_TYPE_NOTIFY_MMAP\"],\"nt_callback\":"
            "\"PsSetLoadImageNotifyRoutineEx\",\"xnu_mechanism\":\"mmap(PROT_EXEC) → EndpointSecurity NOTIFY_MMAP\"}");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "start")) {
        live = true;
        return strdup("{\"ok\":true,\"running\":true}");
    }
    if (!strcmp(action, "stop")) {
        live = false;
        return strdup("{\"ok\":true,\"running\":false}");
    }
    if (!strcmp(action, "detect-events")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "detected_count");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "events");
        ms_json_writer_array_begin(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "own_pid");
        ms_json_writer_u64(&w, (unsigned long long)getpid());
        ms_json_writer_key(&w, "watch_pid");
        ms_json_writer_null(&w);
        ms_json_writer_key(&w, "watch_tid");
        ms_json_writer_null(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "nt-callback-bridge")) {
        j = parse(body, len);
        s = j ? text(j, "nt_routine", "") : strdup("");
        if (!s || !*s) {
            free(s);
            ms_json_free(j);
            return error_json("nt_routine required");
        }
        free(s);
        ms_json_free(j);
        return strdup("{\"ok\":true}");
    }
    if (!strcmp(action, "seed-demo")) {
        unsigned long long callback_ids[3], event_ids[4];
        int ignored = 200;
        const char* callback_bodies[] = {"{\"callback_type\":\"process_notify\"}",
                                         "{\"callback_type\":\"image_load_notify\"}",
                                         "{\"callback_type\":\"thread_notify\"}"};
        for (size_t i = 0; i < 3; i++) {
            callback_ids[i] = next_cb;
            char* result = ms_es_bridge_json("register-callback", (const unsigned char*)callback_bodies[i],
                                             strlen(callback_bodies[i]), &ignored);
            free(result);
        }
        char event_body[768];
        event_ids[0] = next_evt;
        snprintf(event_body, sizeof(event_body),
                 "{\"parent_pid\":%u,\"child_pid\":9999,\"action\":\"created\",\"image_name\":\"game.exe\",\"command_"
                 "line\":\"game.exe --anti-cheat\"}",
                 (unsigned)getpid());
        char* result =
            ms_es_bridge_json("fire-process-event", (const unsigned char*)event_body, strlen(event_body), &ignored);
        free(result);
        event_ids[1] = next_evt;
        snprintf(event_body, sizeof(event_body),
                 "{\"process_id\":9999,\"action\":\"loaded\",\"image_name\":\"ntdll.dll\",\"image_base\":"
                 "\"0x7ffe00000000\",\"image_size\":2097152}");
        result = ms_es_bridge_json("fire-image-event", (const unsigned char*)event_body, strlen(event_body), &ignored);
        free(result);
        event_ids[2] = next_evt;
        snprintf(
            event_body, sizeof(event_body),
            "{\"process_id\":9999,\"thread_id\":7777,\"action\":\"created\",\"start_address\":\"0x7ffe00001000\"}");
        result = ms_es_bridge_json("fire-thread-event", (const unsigned char*)event_body, strlen(event_body), &ignored);
        free(result);
        event_ids[3] = next_evt;
        snprintf(event_body, sizeof(event_body), "{\"parent_pid\":%u,\"child_pid\":9999,\"action\":\"exited\"}",
                 (unsigned)getpid());
        result =
            ms_es_bridge_json("fire-process-event", (const unsigned char*)event_body, strlen(event_body), &ignored);
        free(result);
        result = ms_es_bridge_json("create-ipc-channel", (const unsigned char*)"{}", 2, &ignored);
        free(result);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "seeded");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "callbacks_registered");
        ms_json_writer_u64(&w, 3);
        ms_json_writer_key(&w, "callback_ids");
        ms_json_writer_array_begin(&w);
        for (size_t i = 0; i < 3; i++)
            ms_json_writer_u64(&w, callback_ids[i]);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "events_fired");
        ms_json_writer_u64(&w, 4);
        ms_json_writer_key(&w, "event_ids");
        ms_json_writer_array_begin(&w);
        for (size_t i = 0; i < 4; i++)
            ms_json_writer_u64(&w, event_ids[i]);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "scenario");
        ms_json_writer_string(
            &w, "Anti-cheat game launch: process created → ntdll loaded → worker thread created → process exited");
        ms_json_writer_key(&w, "own_pid");
        ms_json_writer_u64(&w, (unsigned long long)getpid());
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    return error_json("unknown EndpointSecurity action");
}
