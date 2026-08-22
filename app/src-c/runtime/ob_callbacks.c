#include "metalsharp_backend/ob_callbacks.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct reg {
    unsigned long long id;
    char ops[8][32];
    unsigned op_count;
    unsigned protected_pids[32];
    unsigned protected_count;
    bool pre, post, active;
    unsigned altitude;
    unsigned long long calls, last, registered, strip_access, block_access;
    struct reg* next;
} reg;
typedef struct prot {
    unsigned pid;
    char name[128], level[32], sign[128];
    unsigned long long at, opens, blocked, stripped;
    struct prot* next;
} prot;
typedef struct operation_record {
    unsigned long long id, timestamp, source, target, requested, modified, granted, stripped, return_handle;
    char operation[32], pre_status[24], post_status[24];
    bool blocked;
    unsigned pre_count, post_count, pre_ids[32], post_ids[32];
    struct operation_record* next;
} operation_record;
static reg* regs;
static prot* prots;
static operation_record* operation_records;
static unsigned long long next_reg = 1, next_op = 1;
static unsigned long long now_ms(void) {
    return (unsigned long long)time(NULL) * 1000ULL;
}
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
static ms_json* parse(const unsigned char* b, size_t n) {
    char e[96];
    return ms_json_parse(b ? (const char*)b : "", b ? n : 0, e, sizeof(e));
}
static bool u64(const ms_json* j, const char* k, unsigned long long* out) {
    long long n;
    return ms_json_as_i64(ms_json_object_get(j, k), &n) && n >= 0 && ((*out) = (unsigned long long)n, true);
}
static const char* normalize_operation(const char* operation) {
    if (!strcmp(operation, "open_process") || !strcmp(operation, "NtOpenProcess"))
        return "OpenProcess";
    if (!strcmp(operation, "open_thread") || !strcmp(operation, "NtOpenThread"))
        return "OpenThread";
    if (!strcmp(operation, "duplicate_object") || !strcmp(operation, "NtDuplicateObject"))
        return "DuplicateObject";
    if (!strcmp(operation, "close_handle") || !strcmp(operation, "NtClose"))
        return "CloseHandle";
    return NULL;
}
static bool registration_matches(const reg* r, const char* operation) {
    for (unsigned i = 0; i < r->op_count; i++)
        if (!strcmp(r->ops[i], operation))
            return true;
    return false;
}
static void access_flags(ms_json_writer* w, unsigned long long access) {
    static const struct {
        unsigned long long bit;
        const char* name;
    } flags[] = {{1, "PROCESS_TERMINATE"},
                 {16, "PROCESS_VM_READ"},
                 {32, "PROCESS_VM_WRITE"},
                 {8, "PROCESS_VM_OPERATION"},
                 {1024, "PROCESS_QUERY_INFORMATION"},
                 {2048, "PROCESS_SUSPEND_RESUME"}};
    ms_json_writer_array_begin(w);
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        if (access & flags[i].bit) {
            ms_json_writer_string(w, flags[i].name);
            count++;
        }
    if (access == 0x1fffffULL) {
        ms_json_writer_string(w, "PROCESS_ALL_ACCESS");
        count++;
    }
    if (!count)
        ms_json_writer_string(w, access ? "UNKNOWN" : "NONE");
    ms_json_writer_array_end(w);
}
static void access_record_json(ms_json_writer* w, const operation_record* o, bool pre, bool post, bool log) {
    ms_json_writer_object_begin(w);
    if (log) {
        ms_json_writer_key(w, "id");
        ms_json_writer_u64(w, o->id);
    }
    ms_json_writer_key(w, "operation");
    ms_json_writer_string(w, o->operation);
    if (log) {
        ms_json_writer_key(w, "source_pid");
        ms_json_writer_u64(w, o->source);
        ms_json_writer_key(w, "target_pid");
        ms_json_writer_u64(w, o->target);
        ms_json_writer_key(w, "requested");
        ms_json_writer_u64(w, o->requested);
        ms_json_writer_key(w, "granted");
        ms_json_writer_u64(w, o->granted);
        ms_json_writer_key(w, "stripped");
        ms_json_writer_u64(w, o->stripped);
        ms_json_writer_key(w, "blocked");
        ms_json_writer_bool(w, o->blocked);
    } else if (pre) {
        ms_json_writer_key(w, "operation_id");
        ms_json_writer_u64(w, o->id);
        ms_json_writer_key(w, "source_pid");
        ms_json_writer_u64(w, o->source);
        ms_json_writer_key(w, "target_pid");
        ms_json_writer_u64(w, o->target);
        ms_json_writer_key(w, "requested_access");
        ms_json_writer_u64(w, o->requested);
        ms_json_writer_key(w, "modified_access");
        ms_json_writer_u64(w, o->modified);
        ms_json_writer_key(w, "pre_status");
        ms_json_writer_string(w, o->pre_status);
        ms_json_writer_key(w, "dispatched_to");
        ms_json_writer_array_begin(w);
        for (unsigned i = 0; i < o->pre_count; i++)
            ms_json_writer_u64(w, o->pre_ids[i]);
        ms_json_writer_array_end(w);
    } else if (post) {
        ms_json_writer_key(w, "operation_id");
        ms_json_writer_u64(w, o->id);
        ms_json_writer_key(w, "source_pid");
        ms_json_writer_u64(w, o->source);
        ms_json_writer_key(w, "target_pid");
        ms_json_writer_u64(w, o->target);
        ms_json_writer_key(w, "granted_access");
        ms_json_writer_u64(w, o->granted);
        ms_json_writer_key(w, "post_status");
        ms_json_writer_string(w, o->post_status);
        ms_json_writer_key(w, "return_handle");
        ms_json_writer_u64(w, o->return_handle);
        ms_json_writer_key(w, "dispatched_to");
        ms_json_writer_array_begin(w);
        for (unsigned i = 0; i < o->post_count; i++)
            ms_json_writer_u64(w, o->post_ids[i]);
        ms_json_writer_array_end(w);
    }
    ms_json_writer_key(w, "timestamp");
    ms_json_writer_u64(w, o->timestamp);
    ms_json_writer_object_end(w);
}
static void write_records(ms_json_writer* w, const operation_record* o, bool pre, bool log) {
    if (!o)
        return;
    write_records(w, o->next, pre, log);
    access_record_json(w, o, pre, !pre && !log, log);
}
static char* str(const ms_json* j, const char* k, const char* fallback) {
    char* s = NULL;
    if (!ms_json_as_string(ms_json_object_get(j, k), &s))
        s = strdup(fallback);
    return s;
}
static void registration(ms_json_writer* w, const reg* r) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, r->id);
    ms_json_writer_key(w, "operations");
    ms_json_writer_array_begin(w);
    for (unsigned i = 0; i < r->op_count; i++)
        ms_json_writer_string(w, r->ops[i]);
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "pre_callback");
    ms_json_writer_bool(w, r->pre);
    ms_json_writer_key(w, "post_callback");
    ms_json_writer_bool(w, r->post);
    ms_json_writer_key(w, "active");
    ms_json_writer_bool(w, r->active);
    ms_json_writer_key(w, "altitude");
    ms_json_writer_u64(w, r->altitude);
    ms_json_writer_key(w, "call_count");
    ms_json_writer_u64(w, r->calls);
    ms_json_writer_key(w, "last_fired");
    if (r->last)
        ms_json_writer_u64(w, r->last);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "registered_at");
    ms_json_writer_u64(w, r->registered);
    ms_json_writer_key(w, "protected_pids");
    ms_json_writer_array_begin(w);
    for (unsigned i = 0; i < r->protected_count; i++)
        ms_json_writer_u64(w, r->protected_pids[i]);
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "strip_access_mask");
    ms_json_writer_u64(w, r->strip_access);
    ms_json_writer_key(w, "block_access_mask");
    ms_json_writer_u64(w, r->block_access);
    ms_json_writer_object_end(w);
}
static void write_registrations(ms_json_writer* w, const reg* r) {
    if (!r)
        return;
    write_registrations(w, r->next);
    registration(w, r);
}
static void protected_json(ms_json_writer* w, const prot* p) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "pid");
    ms_json_writer_u64(w, p->pid);
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, p->name);
    ms_json_writer_key(w, "protected_at");
    ms_json_writer_u64(w, p->at);
    ms_json_writer_key(w, "signaturatory");
    ms_json_writer_string(w, p->sign);
    ms_json_writer_key(w, "protection_level");
    ms_json_writer_string(w, p->level);
    ms_json_writer_key(w, "open_attempts");
    ms_json_writer_u64(w, p->opens);
    ms_json_writer_key(w, "blocked_attempts");
    ms_json_writer_u64(w, p->blocked);
    ms_json_writer_key(w, "stripped_attempts");
    ms_json_writer_u64(w, p->stripped);
    ms_json_writer_object_end(w);
}
char* ms_ob_callback_json(const char* action, const unsigned char* body, size_t len, int* status) {
    ms_json* j;
    unsigned long long id, pid, source, target, access;
    char* s;
    reg *r, *prev;
    prot* p;
    ms_json_writer w;
    if (status)
        *status = 200;
    if (!strcmp(action, "register-callback")) {
        j = parse(body, len);
        const ms_json* operation_array = j ? ms_json_object_get(j, "operations") : NULL;
        r = calloc(1, sizeof(*r));
        if (!r) {
            ms_json_free(j);
            return failure("out of memory");
        }
        if (!operation_array) {
            r->op_count = 2;
            snprintf(r->ops[0], sizeof(r->ops[0]), "OpenProcess");
            snprintf(r->ops[1], sizeof(r->ops[1]), "DuplicateObject");
        } else if (ms_json_type_of(operation_array) == MS_JSON_ARRAY) {
            for (size_t i = 0; i < ms_json_array_length(operation_array) && r->op_count < 8; i++) {
                char* operation = NULL;
                if (!ms_json_as_string(ms_json_array_get(operation_array, i), &operation))
                    continue;
                const char* normalized =
                    !strcmp(operation, "open_process") || !strcmp(operation, "NtOpenProcess") ? "OpenProcess"
                    : !strcmp(operation, "open_thread") || !strcmp(operation, "NtOpenThread") ? "OpenThread"
                    : !strcmp(operation, "duplicate_object") || !strcmp(operation, "NtDuplicateObject")
                        ? "DuplicateObject"
                    : !strcmp(operation, "close_handle") || !strcmp(operation, "NtClose") ? "CloseHandle"
                                                                                          : NULL;
                if (normalized)
                    snprintf(r->ops[r->op_count++], sizeof(r->ops[0]), "%s", normalized);
                free(operation);
            }
        }
        if (r->op_count == 0) {
            free(r);
            ms_json_free(j);
            return failure("at least one operation required");
        }
        bool value = true;
        r->pre = j && ms_json_as_bool(ms_json_object_get(j, "pre_callback"), &value) ? value : true;
        r->post = j && ms_json_as_bool(ms_json_object_get(j, "post_callback"), &value) ? value : true;
        r->active = true;
        r->altitude = 1000;
        r->strip_access = 32;
        r->block_access = 0;
        if (j) {
            unsigned long long value_u64;
            if (u64(j, "altitude", &value_u64))
                r->altitude = (unsigned)value_u64;
            if (u64(j, "strip_access_mask", &value_u64))
                r->strip_access = value_u64;
            if (u64(j, "block_access_mask", &value_u64))
                r->block_access = value_u64;
            const ms_json* protected = ms_json_object_get(j, "protected_pids");
            if (protected && ms_json_type_of(protected) == MS_JSON_ARRAY)
                for (size_t i = 0; i < ms_json_array_length(protected) && r->protected_count < 32; i++) {
                    long long protected_pid;
                    if (ms_json_as_i64(ms_json_array_get(protected, i), &protected_pid) && protected_pid >= 0 &&
                        protected_pid <= 0xffffffffLL)
                        r->protected_pids[r->protected_count++] = (unsigned)protected_pid;
                }
        }
        r->id = next_reg++;
        r->registered = now_ms();
        r->next = regs;
        regs = r;
        ms_json_free(j);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "registration_id");
        ms_json_writer_u64(&w, r->id);
        ms_json_writer_key(&w, "registration");
        registration(&w, r);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "unregister-callback")) {
        j = parse(body, len);
        if (!j || !u64(j, "registration_id", &id)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return failure("registration_id required");
        }
        prev = NULL;
        for (r = regs; r; r = r->next) {
            if (r->id == id)
                break;
            prev = r;
        }
        if (!r) {
            ms_json_free(j);
            return failure("registration not found");
        }
        if (prev)
            prev->next = r->next;
        else
            regs = r->next;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "removed");
        registration(&w, r);
        ms_json_writer_object_end(&w);
        free(r);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "list-registrations")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "count");
        {
            size_t c = 0;
            for (r = regs; r; r = r->next)
                c++;
            ms_json_writer_u64(&w, c);
        }
        ms_json_writer_key(&w, "registrations");
        ms_json_writer_array_begin(&w);
        write_registrations(&w, regs);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "protect-process")) {
        j = parse(body, len);
        if (!j || !u64(j, "pid", &pid) || pid > 0xffffffffULL) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return failure("pid (u32) required");
        }
        prot* old_prot = NULL;
        for (prot* q = prots; q; q = q->next) {
            if (q->pid == pid) {
                old_prot = q;
                break;
            }
        }
        if (old_prot) {
            prot* before_prot = NULL;
            for (prot* q = prots; q && q != old_prot; q = q->next)
                before_prot = q;
            if (before_prot)
                before_prot->next = old_prot->next;
            else
                prots = old_prot->next;
            free(old_prot);
        }
        p = calloc(1, sizeof(*p));
        p->pid = (unsigned)pid;
        p->at = now_ms();
        s = str(j, "name", "unknown");
        snprintf(p->name, sizeof(p->name), "%s", s);
        free(s);
        s = str(j, "protection_level", "ppl");
        const char* level = !strcmp(s, "ppl")             ? "PPL"
                            : !strcmp(s, "none")          ? "None"
                            : !strcmp(s, "authenticode")  ? "Authenticode"
                            : !strcmp(s, "antitampering") ? "Antitampering"
                            : !strcmp(s, "microsoft")     ? "Microsoft"
                            : !strcmp(s, "system")        ? "System"
                                                          : "PPL";
        snprintf(p->level, sizeof(p->level), "%s", level);
        free(s);
        s = str(j, "signaturatory", "MetalSharp");
        snprintf(p->sign, sizeof(p->sign), "%s", s);
        free(s);
        p->next = prots;
        prots = p;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "protected");
        protected_json(&w, p);
        ms_json_writer_object_end(&w);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "unprotect-process")) {
        j = parse(body, len);
        if (!j || !u64(j, "pid", &pid)) {
            ms_json_free(j);
            if (status)
                *status = 400;
            return failure("pid (u32) required");
        }
        p = NULL;
        prot* before = NULL;
        for (prot* q = prots; q; q = q->next) {
            if (q->pid == pid) {
                p = q;
                break;
            }
            before = q;
        }
        if (!p) {
            ms_json_free(j);
            return failure("pid was not protected");
        }
        if (before)
            before->next = p->next;
        else
            prots = p->next;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "unprotected");
        protected_json(&w, p);
        ms_json_writer_object_end(&w);
        free(p);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "list-protected")) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "count");
        {
            size_t c = 0;
            for (p = prots; p; p = p->next)
                c++;
            ms_json_writer_u64(&w, c);
        }
        ms_json_writer_key(&w, "processes");
        ms_json_writer_array_begin(&w);
        for (p = prots; p; p = p->next)
            protected_json(&w, p);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "simulate-operation")) {
        j = parse(body, len);
        s = j ? str(j, "operation", "") : NULL;
        const char* op_name = s ? normalize_operation(s) : NULL;
        if (!op_name) {
            free(s);
            ms_json_free(j);
            if (status)
                *status = 400;
            return failure("operation required: open_process, open_thread, duplicate_object, close_handle");
        }
        if (!u64(j, "source_pid", &source)) {
            free(s);
            ms_json_free(j);
            if (status)
                *status = 400;
            return failure("source_pid (u32) required");
        }
        if (!u64(j, "target_pid", &target)) {
            free(s);
            ms_json_free(j);
            if (status)
                *status = 400;
            return failure("target_pid (u32) required");
        }
        if (!strcmp(op_name, "CloseHandle")) {
            free(s);
            ms_json_free(j);
            return strdup("{\"ok\":true,\"operation\":\"close_handle\",\"note\":\"NT ObRegisterCallbacks does not fire "
                          "for CloseHandle — no pre/post callback dispatched\"}");
        }
        if (!u64(j, "requested_access", &access))
            access = 0x1fffffULL;
        id = next_op++;
        operation_record* o = calloc(1, sizeof(*o));
        if (!o) {
            free(s);
            ms_json_free(j);
            return failure("out of memory");
        }
        o->id = id;
        o->source = source;
        o->target = target;
        o->requested = access;
        o->modified = access;
        o->timestamp = now_ms();
        snprintf(o->operation, sizeof(o->operation), "%s", op_name);
        bool is_protected = false;
        for (p = prots; p; p = p->next)
            if (p->pid == target) {
                is_protected = true;
                p->opens++;
            }
        reg* pre_matches[32];
        unsigned pre_match_count = 0;
        for (r = regs; r && pre_match_count < 32; r = r->next)
            if (r->active && r->pre && registration_matches(r, op_name))
                pre_matches[pre_match_count++] = r;
        for (unsigned i = 1; i < pre_match_count; i++) {
            reg* item = pre_matches[i];
            unsigned j = i;
            while (j && pre_matches[j - 1]->altitude > item->altitude) {
                pre_matches[j] = pre_matches[j - 1];
                j--;
            }
            pre_matches[j] = item;
        }
        for (unsigned i = 0; i < pre_match_count; i++) {
            r = pre_matches[i];
            if (o->pre_count < 32)
                o->pre_ids[o->pre_count++] = r->id;
            unsigned long long mask = r->block_access;
            if (is_protected && (o->modified & mask)) {
                o->blocked = true;
                o->modified = 0;
                snprintf(o->pre_status, sizeof(o->pre_status), "Block");
                break;
            }
            unsigned long long stripped = is_protected ? (o->modified & r->strip_access) : 0;
            if (stripped) {
                o->modified &= ~r->strip_access;
                snprintf(o->pre_status, sizeof(o->pre_status), "StripAccess");
            }
            if (!o->pre_status[0])
                snprintf(o->pre_status, sizeof(o->pre_status), "Allow");
        }
        if (!o->pre_status[0])
            snprintf(o->pre_status, sizeof(o->pre_status), "Allow");
        o->granted = o->modified;
        o->stripped = o->requested - o->granted;
        snprintf(o->post_status, sizeof(o->post_status), "%s", o->blocked ? "Denied" : "Success");
        o->return_handle = o->blocked ? 0 : 0xFFFF0000ULL + id;
        for (r = regs; r; r = r->next)
            if (r->active && r->post && registration_matches(r, op_name) && o->post_count < 32)
                o->post_ids[o->post_count++] = r->id;
        for (r = regs; r; r = r->next) {
            bool pre = false, post = false;
            for (unsigned i = 0; i < o->pre_count; i++)
                if (o->pre_ids[i] == r->id)
                    pre = true;
            for (unsigned i = 0; i < o->post_count; i++)
                if (o->post_ids[i] == r->id)
                    post = true;
            if (pre || post) {
                r->calls += (pre ? 1 : 0) + (post ? 1 : 0);
                r->last = o->timestamp;
            }
        }
        if (is_protected)
            for (p = prots; p; p = p->next)
                if (p->pid == target) {
                    if (o->blocked)
                        p->blocked++;
                    if (o->stripped)
                        p->stripped++;
                }
        o->next = operation_records;
        operation_records = o;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "operation_id");
        ms_json_writer_u64(&w, id);
        ms_json_writer_key(&w, "pre");
        access_record_json(&w, o, true, false, false);
        ms_json_writer_key(&w, "post");
        access_record_json(&w, o, false, true, false);
        ms_json_writer_key(&w, "blocked");
        ms_json_writer_bool(&w, o->blocked);
        ms_json_writer_key(&w, "access");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "requested");
        ms_json_writer_u64(&w, o->requested);
        ms_json_writer_key(&w, "requested_flags");
        access_flags(&w, o->requested);
        ms_json_writer_key(&w, "granted");
        ms_json_writer_u64(&w, o->granted);
        ms_json_writer_key(&w, "granted_flags");
        access_flags(&w, o->granted);
        ms_json_writer_key(&w, "stripped");
        ms_json_writer_u64(&w, o->stripped);
        ms_json_writer_key(&w, "stripped_flags");
        access_flags(&w, o->stripped);
        ms_json_writer_object_end(&w);
        ms_json_writer_object_end(&w);
        free(s);
        ms_json_free(j);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "pre-operations") || !strcmp(action, "post-operations") || !strcmp(action, "access-log")) {
        bool pre = !strcmp(action, "pre-operations"), log = !strcmp(action, "access-log");
        const char* key = log ? "logs" : "operations";
        size_t count = 0;
        for (operation_record* o = operation_records; o; o = o->next)
            count++;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "count");
        ms_json_writer_u64(&w, count);
        ms_json_writer_key(&w, key);
        ms_json_writer_array_begin(&w);
        write_records(&w, operation_records, pre, log);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    if (!strcmp(action, "capability-survey"))
        return strdup(
            "{\"ok\":true,\"nt_equivalent\":\"ObRegisterCallbacks / "
            "ObUnRegisterCallbacks\",\"mechanisms\":[{\"availability\":\"kext_required\",\"description\":\"Hooks "
            "task_for_pid() calls — fires when any process requests task port of "
            "another\",\"detail\":\"mac_proc_check_get_task(policy, cred, p) — returns 0 to allow, EPERM to deny. This "
            "IS the handle-open event on "
            "macOS.\",\"id\":\"mac_proc_check_get_task\",\"nt_mapping\":\"ObRegisterCallbacks(OB_OPERATION_HANDLE_"
            "CREATE)\",\"type\":\"MACF policy\"},{\"availability\":\"userspace\",\"description\":\"Hook "
            "NtOpenProcess/NtDuplicateObject in Wine ntdll — intercept before kernel\",\"detail\":\"Since Wine "
            "controls the handle table (Phase 2), it can fire callbacks whenever a handle is created to a protected "
            "process. No kernel needed.\",\"id\":\"wine_handle_callback\",\"nt_mapping\":\"ObRegisterCallbacks "
            "pre/post operation\",\"type\":\"Wine "
            "virtual\"},{\"availability\":\"system_extension\",\"description\":\"ES may see task_for_pid as a mach "
            "trap — investigate NOTIFY_MACH or custom ES client\",\"detail\":\"ES can observe but cannot modify access "
            "in-flight. For detection only, no pre-operation "
            "filtering.\",\"id\":\"endpoint_security_task_for_pid\",\"nt_mapping\":\"Partial — detect but not "
            "filter\",\"type\":\"EndpointSecurity\"},{\"availability\":\"builtin\",\"description\":\"Sandbox profiles "
            "can deny task_for_pid — Apple uses this for App Store\",\"detail\":\"Can block access entirely but cannot "
            "strip individual access rights. Useful as failsafe.\",\"id\":\"sandbox_extension\",\"nt_mapping\":\"Hard "
            "deny (no granularity)\",\"type\":\"macOS "
            "sandbox\"}],\"recommended\":\"wine_handle_callback\",\"rationale\":\"Wine "
            "controls its own handle table. Pre/post callbacks fire on NtOpenProcess/NtDuplicateObject. Can strip "
            "PROCESS_VM_WRITE, block entirely, or allow. No kernel extension "
            "needed.\",\"kernel_enhanced\":\"mac_proc_check_get_task\",\"kernel_rationale\":\"When system extension is "
            "available, MACF hook catches task_for_pid from ANY process (not just Wine). Full NT-equivalent coverage "
            "including non-Wine "
            "attackers.\",\"access_rights\":{\"PROCESS_ALL_ACCESS\":\"0x001FFFFF\",\"PROCESS_QUERY_INFORMATION\":"
            "\"0x00000400\",\"PROCESS_TERMINATE\":\"0x00000001\",\"PROCESS_VM_OPERATION\":\"0x00000008\",\"PROCESS_VM_"
            "READ\":\"0x00000010\",\"PROCESS_VM_WRITE\":\"0x00000020\"}}");
    if (!strcmp(action, "seed-demo")) {
        int ignored = 200;
        char* result;
        unsigned long long reg_id = next_reg, op1 = next_op, op2 = next_op + 1, op3 = next_op + 2;
        result = ms_ob_callback_json("protect-process",
                                     (const unsigned char*)"{\"pid\":5000,\"name\":\"game.exe\",\"protection_level\":"
                                                           "\"ppl\",\"signaturatory\":\"EasyAntiCheat\"}",
                                     strlen("{\"pid\":5000,\"name\":\"game.exe\",\"protection_level\":\"ppl\","
                                            "\"signaturatory\":\"EasyAntiCheat\"}"),
                                     &ignored);
        free(result);
        result = ms_ob_callback_json(
            "register-callback",
            (const unsigned char*)"{\"operations\":[\"open_process\",\"duplicate_object\"],\"pre_callback\":true,"
                                  "\"post_callback\":true,\"strip_access_mask\":524320,\"altitude\":1000}",
            strlen("{\"operations\":[\"open_process\",\"duplicate_object\"],\"pre_callback\":true,\"post_callback\":"
                   "true,\"strip_access_mask\":524320,\"altitude\":1000}"),
            &ignored);
        free(result);
        result = ms_ob_callback_json("simulate-operation",
                                     (const unsigned char*)"{\"operation\":\"open_process\",\"source_pid\":6000,"
                                                           "\"target_pid\":5000,\"requested_access\":2097151}",
                                     strlen("{\"operation\":\"open_process\",\"source_pid\":6000,\"target_pid\":5000,"
                                            "\"requested_access\":2097151}"),
                                     &ignored);
        free(result);
        result = ms_ob_callback_json(
            "simulate-operation",
            (const unsigned char*)"{\"operation\":\"open_process\",\"source_pid\":5000,\"target_pid\":5000,\"requested_"
                                  "access\":16}",
            strlen("{\"operation\":\"open_process\",\"source_pid\":5000,\"target_pid\":5000,\"requested_access\":16}"),
            &ignored);
        free(result);
        result = ms_ob_callback_json(
            "simulate-operation",
            (const unsigned char*)"{\"operation\":\"duplicate_object\",\"source_pid\":6000,\"target_pid\":5000,"
                                  "\"requested_access\":32}",
            strlen(
                "{\"operation\":\"duplicate_object\",\"source_pid\":6000,\"target_pid\":5000,\"requested_access\":32}"),
            &ignored);
        free(result);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "seeded");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "protected_pid");
        ms_json_writer_u64(&w, 5000);
        ms_json_writer_key(&w, "registration_id");
        ms_json_writer_u64(&w, reg_id);
        ms_json_writer_key(&w, "operations_simulated");
        ms_json_writer_u64(&w, 3);
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "scenario");
        ms_json_writer_string(
            &w, "Anti-cheat protection: game.exe (PID 5000) protected. Cheat (PID 6000) tries PROCESS_ALL_ACCESS → "
                "stripped/blocked. Game opens itself → allowed. Cheat tries DuplicateObject with VM_WRITE → stripped.");
        ms_json_writer_key(&w, "operation_results");
        ms_json_writer_array_begin(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_u64(&w, op1);
        ms_json_writer_key(&w, "blocked");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "pre_status");
        ms_json_writer_string(&w, "StripAccess");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_u64(&w, op2);
        ms_json_writer_key(&w, "blocked");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "pre_status");
        ms_json_writer_string(&w, "Allow");
        ms_json_writer_object_end(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "id");
        ms_json_writer_u64(&w, op3);
        ms_json_writer_key(&w, "blocked");
        ms_json_writer_bool(&w, false);
        ms_json_writer_key(&w, "pre_status");
        ms_json_writer_string(&w, "StripAccess");
        ms_json_writer_object_end(&w);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    return failure("unknown object callback action");
}
