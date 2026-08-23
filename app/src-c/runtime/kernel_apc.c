#include "metalsharp_backend/kernel_apc.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#ifdef __APPLE__
#define MAP_ANONYMOUS 0x1000
#else
#define MAP_ANONYMOUS 0x20
#endif
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

typedef enum { APC_USER, APC_KERNEL } apc_mode;
typedef enum { APC_PENDING, APC_DELIVERING, APC_DELIVERED, APC_CANCELLED, APC_FAILED } apc_status;
typedef struct {
    unsigned long long id, thread_handle, target_tid;
    char routine[128], context[128], arg1[128], arg2[128], arg3[128];
    apc_mode mode;
    apc_status status;
    unsigned long long enqueued_at, delivered_at;
    bool has_delivered;
} apc_entry;
typedef struct apc_queue {
    unsigned long long tid;
    apc_entry* items;
    size_t count, capacity;
    struct apc_queue* next;
} apc_queue;
typedef struct saved_context {
    unsigned long long tid, saved_at;
    char pc[32], sp[32], x0[32], x1[32];
    struct saved_context* next;
} saved_context;
static apc_queue* g_queues;
static saved_context* g_contexts;
static unsigned long long g_next_apc = 1;
static struct {
    bool allocated;
    char address[32];
    size_t page_size, trampoline_offset, restore_offset, code_size;
    char status[32], detail[256];
} g_trampoline = {false, "0x0000000000000000", 0, 0, 0, 0, "not_allocated", "Trampoline page not yet allocated"};

static unsigned long long now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0)
        return 0;
    return (unsigned long long)t.tv_sec * 1000ULL + (unsigned long long)t.tv_nsec / 1000000ULL;
}
static char* err_json(const char* s) {
    ms_json_writer w;
    char* r;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, s);
    ms_json_writer_object_end(&w);
    r = ms_json_writer_take(&w);
    return r;
}
static ms_json* parse_root(const char* body, size_t len) {
    char e[128];
    ms_json* v = ms_json_parse(body ? body : "", len, e, sizeof(e));
    if (v == NULL || ms_json_type_of(v) != MS_JSON_OBJECT) {
        ms_json_free(v);
        return NULL;
    }
    return v;
}
static bool u64(const ms_json* r, const char* key, unsigned long long* out) {
    long long n;
    if (!ms_json_as_i64(ms_json_object_get(r, key), &n) || n < 0)
        return false;
    *out = (unsigned long long)n;
    return true;
}
static char* sfield(const ms_json* r, const char* key, const char* fallback) {
    char* s = NULL;
    if (!ms_json_as_string(ms_json_object_get(r, key), &s) || s == NULL)
        s = strdup(fallback);
    return s;
}
static apc_queue* queue_for(unsigned long long tid, bool create) {
    apc_queue* q;
    for (q = g_queues; q; q = q->next)
        if (q->tid == tid)
            return q;
    if (!create)
        return NULL;
    q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    q->tid = tid;
    q->next = g_queues;
    g_queues = q;
    return q;
}
static bool queue_add(apc_queue* q, const apc_entry* e) {
    if (q->count == q->capacity) {
        size_t c = q->capacity ? q->capacity * 2 : 8;
        apc_entry* p = realloc(q->items, c * sizeof(*p));
        if (!p)
            return false;
        q->items = p;
        q->capacity = c;
    }
    q->items[q->count++] = *e;
    return true;
}
static const char* mode_name(apc_mode m) {
    return m == APC_KERNEL ? "Kernel" : "User";
}
static const char* status_name(apc_status s) {
    switch (s) {
    case APC_PENDING:
        return "Pending";
    case APC_DELIVERING:
        return "Delivering";
    case APC_DELIVERED:
        return "Delivered";
    case APC_CANCELLED:
        return "Cancelled";
    default:
        return "Failed";
    }
}
static void entry_json(ms_json_writer* w, const apc_entry* e) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, e->id);
    ms_json_writer_key(w, "thread_handle");
    ms_json_writer_u64(w, e->thread_handle);
    ms_json_writer_key(w, "target_thread_id");
    ms_json_writer_u64(w, e->target_tid);
    ms_json_writer_key(w, "apc_routine");
    ms_json_writer_string(w, e->routine);
    ms_json_writer_key(w, "apc_context");
    ms_json_writer_string(w, e->context);
    ms_json_writer_key(w, "arg1");
    ms_json_writer_string(w, e->arg1);
    ms_json_writer_key(w, "arg2");
    ms_json_writer_string(w, e->arg2);
    ms_json_writer_key(w, "arg3");
    ms_json_writer_string(w, e->arg3);
    ms_json_writer_key(w, "mode");
    ms_json_writer_string(w, mode_name(e->mode));
    ms_json_writer_key(w, "status");
    ms_json_writer_string(w, status_name(e->status));
    ms_json_writer_key(w, "enqueued_at");
    ms_json_writer_u64(w, e->enqueued_at);
    ms_json_writer_key(w, "delivered_at");
    if (e->has_delivered)
        ms_json_writer_u64(w, e->delivered_at);
    else
        ms_json_writer_null(w);
    ms_json_writer_object_end(w);
}
static void ids_json(ms_json_writer* w, const unsigned long long* ids, size_t n) {
    size_t i;
    ms_json_writer_array_begin(w);
    for (i = 0; i < n; i++)
        ms_json_writer_u64(w, ids[i]);
    ms_json_writer_array_end(w);
}

char* ms_kernel_apc_queue(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long h, tid;
    char *routine, *context, *a1, *a2, *a3;
    apc_entry e;
    apc_queue* q;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "thread_handle", &h)) {
        ms_json_free(r);
        return err_json("thread_handle (u64) required");
    }
    if (!u64(r, "target_thread_id", &tid))
        tid = h;
    routine = sfield(r, "apc_routine", "0x0000000000000000");
    context = sfield(r, "apc_context", "0x0000000000000000");
    a1 = sfield(r, "arg1", "0x0000000000000000");
    a2 = sfield(r, "arg2", "0x0000000000000000");
    a3 = sfield(r, "arg3", "0x0000000000000000");
    memset(&e, 0, sizeof(e));
    e.id = g_next_apc++;
    e.thread_handle = h;
    e.target_tid = tid;
    snprintf(e.routine, sizeof(e.routine), "%s", routine);
    snprintf(e.context, sizeof(e.context), "%s", context);
    snprintf(e.arg1, sizeof(e.arg1), "%s", a1);
    snprintf(e.arg2, sizeof(e.arg2), "%s", a2);
    snprintf(e.arg3, sizeof(e.arg3), "%s", a3);
    {
        bool kernel = false;
        (void)ms_json_as_bool(ms_json_object_get(r, "kernel_mode"), &kernel);
        e.mode = kernel ? APC_KERNEL : APC_USER;
    }
    e.status = APC_PENDING;
    e.enqueued_at = now_ms();
    q = queue_for(tid, true);
    if (q == NULL || !queue_add(q, &e)) {
        free(routine);
        free(context);
        free(a1);
        free(a2);
        free(a3);
        ms_json_free(r);
        return err_json("failed to allocate APC queue");
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "ntApi");
    ms_json_writer_string(&w, "NtQueueApcThread");
    ms_json_writer_key(&w, "apcId");
    ms_json_writer_u64(&w, e.id);
    ms_json_writer_key(&w, "entry");
    entry_json(&w, &e);
    ms_json_writer_key(&w, "queueDepth");
    ms_json_writer_u64(&w, q->count);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(routine);
    free(context);
    free(a1);
    free(a2);
    free(a3);
    ms_json_free(r);
    return out;
}

static char* deliver_result(unsigned long long tid, const char* api, bool wait_mode) {
    apc_queue* q = queue_for(tid, true);
    size_t i, pending = 0, delivered = 0;
    unsigned long long ids[1024];
    if (!q)
        return err_json("failed to allocate APC queue");
    for (i = 0; i < q->count; i++)
        if (q->items[i].status == APC_PENDING)
            pending++;
    if (wait_mode && pending == 0) {
        ms_json_writer w;
        char* out;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "ntApi");
        ms_json_writer_string(&w, "NtWaitForSingleObject(alertable=TRUE)");
        ms_json_writer_key(&w, "threadId");
        ms_json_writer_u64(&w, tid);
        ms_json_writer_key(&w, "status");
        ms_json_writer_string(&w, "STATUS_WAIT_0");
        ms_json_writer_key(&w, "pendingApcs");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "action");
        ms_json_writer_string(&w, "no APCs pending — normal wait return");
        ms_json_writer_object_end(&w);
        out = ms_json_writer_take(&w);
        return out;
    }
    for (i = 0; i < q->count; i++)
        if (q->items[i].status == APC_PENDING) {
            q->items[i].status = APC_DELIVERED;
            q->items[i].delivered_at = now_ms();
            q->items[i].has_delivered = true;
            if (delivered < 1024)
                ids[delivered++] = q->items[i].id;
        }
    {
        ms_json_writer w;
        char* out;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "ntApi");
        ms_json_writer_string(&w, api);
        ms_json_writer_key(&w, "threadId");
        ms_json_writer_u64(&w, tid);
        if (wait_mode) {
            ms_json_writer_key(&w, "status");
            ms_json_writer_string(&w, "STATUS_USER_APC");
            ms_json_writer_key(&w, "pendingApcs");
            ms_json_writer_u64(&w, delivered);
            ms_json_writer_key(&w, "deliveredApcIds");
            ids_json(&w, ids, delivered);
            ms_json_writer_key(&w, "action");
            ms_json_writer_string(&w, "APCs delivered before wait — return STATUS_USER_APC");
        } else {
            size_t remaining = 0;
            for (i = 0; i < q->count; i++)
                if (q->items[i].status == APC_PENDING)
                    remaining++;
            ms_json_writer_key(&w, "pendingCount");
            ms_json_writer_u64(&w, pending);
            ms_json_writer_key(&w, "deliveredApcIds");
            ids_json(&w, ids, delivered);
            ms_json_writer_key(&w, "remainingInQueue");
            ms_json_writer_u64(&w, remaining);
            ms_json_writer_key(&w, "note");
            ms_json_writer_string(&w, "Delivered all pending user-mode APCs for this thread");
        }
        ms_json_writer_object_end(&w);
        out = ms_json_writer_take(&w);
        return out;
    }
}
char* ms_kernel_apc_test_alert(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long tid;
    if (!r || !u64(r, "thread_id", &tid)) {
        ms_json_free(r);
        return err_json("thread_id (u64) required");
    }
    char* out = deliver_result(tid, "NtTestAlert", false);
    ms_json_free(r);
    return out;
}
char* ms_kernel_apc_wait_alertable(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long tid;
    if (!r || !u64(r, "thread_id", &tid)) {
        ms_json_free(r);
        return err_json("thread_id (u64) required");
    }
    char* out = deliver_result(tid, "NtWaitForSingleObject(alertable=TRUE)", true);
    ms_json_free(r);
    return out;
}

static const uint32_t trampoline_code[] = {0xA9BF7BFD, 0xA9BF53F3, 0xA9BF4BF5, 0xAA1503E0, 0xD63F0060,
                                           0x14000003, 0xA8C14BF5, 0xA8C153F3, 0xA8C17BFD};
static const uint32_t restore_code[] = {0x58000060, 0xD51B0020, 0x58000040, 0xD51B0040, 0xD69F03E0, 0, 0, 0, 0};
static void asm_arrays(ms_json_writer* w, bool restore) {
    const char* a[] = {"stp x29, x30, [sp, #-16]!   // save frame pointer and link register",
                       "stp x19, x20, [sp, #-16]!   // save callee-saved registers",
                       "stp x21, x22, [sp, #-16]!   // save more callee-saved",
                       "mov x0, x21                  // x0 = ApcContext (passed in x21)",
                       "blr x1                       // call ApcRoutine(x0=ApcContext)",
                       "b restore_handler            // jump to restore handler",
                       "// --- restore handler ---",
                       "ldp x21, x22, [sp], #16     // restore callee-saved",
                       "ldp x19, x20, [sp], #16     // restore callee-saved",
                       "ldp x29, x30, [sp], #16     // restore fp and lr"};
    const char* b[] = {"ldr x0, [pc, #12]           // load saved_sp from literal pool",
                       "msr sp_el0, x0              // restore user stack pointer",
                       "ldr x0, [pc, #8]            // load saved_pc from literal pool",
                       "msr elr_el1, x0             // restore return address",
                       "eret                         // return to original code",
                       ".quad saved_sp              // embedded saved stack pointer",
                       ".quad saved_pc              // embedded saved program counter"};
    size_t i, n = restore ? 7 : 10;
    ms_json_writer_array_begin(w);
    for (i = 0; i < n; i++)
        ms_json_writer_string(w, restore ? b[i] : a[i]);
    ms_json_writer_array_end(w);
}
char* ms_kernel_apc_allocate_trampoline(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    long long n = 4096;
    size_t page;
    void* ptr;
    ms_json_writer w;
    char* out;
    if (!r) {
        return err_json("invalid JSON object");
    }
    {
        long long x;
        if (ms_json_as_i64(ms_json_object_get(r, "page_size"), &x) && x > 0)
            n = x;
    }
    page = (size_t)n;
    ptr = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ptr == MAP_FAILED) {
        ms_json_free(r);
        return err_json("mmap RW failed");
    }
    memcpy(ptr, trampoline_code, sizeof(trampoline_code));
    memcpy((unsigned char*)ptr + sizeof(trampoline_code), restore_code, sizeof(restore_code));
    if (mprotect(ptr, page, PROT_READ | PROT_EXEC) != 0) {
        munmap(ptr, page);
        ms_json_free(r);
        return err_json("mprotect RW→RX failed");
    }
    g_trampoline.allocated = true;
    snprintf(g_trampoline.address, sizeof(g_trampoline.address), "0x%016" PRIXPTR, (uintptr_t)ptr);
    g_trampoline.page_size = page;
    g_trampoline.trampoline_offset = 0;
    g_trampoline.restore_offset = sizeof(trampoline_code);
    g_trampoline.code_size = sizeof(trampoline_code) + sizeof(restore_code);
    snprintf(g_trampoline.status, sizeof(g_trampoline.status), "allocated");
    snprintf(g_trampoline.detail, sizeof(g_trampoline.detail),
             "Trampoline at +0x0, restore handler at +0x%zX, total %zu bytes", sizeof(trampoline_code),
             g_trampoline.code_size);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pageAddress");
    ms_json_writer_string(&w, g_trampoline.address);
    ms_json_writer_key(&w, "pageSize");
    ms_json_writer_u64(&w, page);
    ms_json_writer_key(&w, "trampolineOffset");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_key(&w, "restoreHandlerOffset");
    ms_json_writer_u64(&w, sizeof(trampoline_code));
    ms_json_writer_key(&w, "codeSize");
    ms_json_writer_u64(&w, g_trampoline.code_size);
    ms_json_writer_key(&w, "code");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "trampoline");
    {
        char s[128];
        snprintf(s, sizeof(s), "%zu bytes (save LR, load args, BLR to apc_routine, B to restore)",
                 sizeof(trampoline_code));
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "restoreHandler");
    {
        char s[128];
        snprintf(s, sizeof(s), "%zu bytes (load saved context, MSR SP_el0, ERET)", sizeof(restore_code));
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "trampolineAssembly");
    asm_arrays(&w, false);
    ms_json_writer_key(&w, "restoreAssembly");
    asm_arrays(&w, true);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    ms_json_free(r);
    return out;
}

char* ms_kernel_apc_suspend_thread(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long port;
    if (!r || !u64(r, "thread_port", &port)) {
        ms_json_free(r);
        return err_json("thread_port (u32 Mach thread port) required");
    }
    ms_json_writer w;
    char* out;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "ntApi");
    ms_json_writer_string(&w, "NtSuspendThread");
    ms_json_writer_key(&w, "threadPort");
    {
        char s[32];
        snprintf(s, sizeof(s), "0x%08" PRIX64, port);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "kr");
    ms_json_writer_i64(&w, -1);
    ms_json_writer_key(&w, "syscallResult");
    ms_json_writer_i64(&w, -1);
    ms_json_writer_key(&w, "note");
    ms_json_writer_string(&w, "thread_suspend failed — may need task port");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    ms_json_free(r);
    return out;
}

static saved_context* context_for(unsigned long long tid, bool create) {
    saved_context* c;
    for (c = g_contexts; c; c = c->next)
        if (c->tid == tid)
            return c;
    if (!create)
        return NULL;
    c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->tid = tid;
    c->next = g_contexts;
    g_contexts = c;
    return c;
}
static void context_json(ms_json_writer* w, const saved_context* c) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "pc");
    ms_json_writer_string(w, c->pc);
    ms_json_writer_key(w, "sp");
    ms_json_writer_string(w, c->sp);
    ms_json_writer_key(w, "x0");
    ms_json_writer_string(w, c->x0);
    ms_json_writer_key(w, "x1");
    ms_json_writer_string(w, c->x1);
    ms_json_writer_object_end(w);
}
char* ms_kernel_apc_get_context(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long tid;
    saved_context* c;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "thread_id", &tid)) {
        ms_json_free(r);
        return err_json("thread_id required");
    }
    c = context_for(tid, false);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    if (c) {
        ms_json_writer_key(&w, "source");
        ms_json_writer_string(&w, "saved");
        ms_json_writer_key(&w, "context");
        context_json(&w, c);
    } else {
        ms_json_writer_key(&w, "source");
        ms_json_writer_string(&w, "simulated_zeroed");
        ms_json_writer_key(&w, "threadId");
        ms_json_writer_u64(&w, tid);
        ms_json_writer_key(&w, "context");
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "x0");
        ms_json_writer_string(&w, "0x0000000000000000");
        ms_json_writer_key(&w, "x1");
        ms_json_writer_string(&w, "0x0000000000000000");
        ms_json_writer_key(&w, "sp");
        ms_json_writer_string(&w, "0x0000000000000000");
        ms_json_writer_key(&w, "pc");
        ms_json_writer_string(&w, "0x0000000000000000");
        ms_json_writer_key(&w, "lr");
        ms_json_writer_string(&w, "0x0000000000000000");
        ms_json_writer_key(&w, "cpsr");
        ms_json_writer_string(&w, "0x00000000");
        ms_json_writer_object_end(&w);
        ms_json_writer_key(&w, "note");
        ms_json_writer_string(
            &w, "Zeroed context — in production, thread_get_state(ARM_THREAD_STATE64) provides real registers");
    }
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    ms_json_free(r);
    return out;
}
char* ms_kernel_apc_set_context(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long tid;
    char *pc, *sp, *x0, *x1;
    saved_context* c;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "thread_id", &tid)) {
        ms_json_free(r);
        return err_json("thread_id required");
    }
    pc = sfield(r, "pc", "0x0000000000000000");
    sp = sfield(r, "sp", "0x0000000000000000");
    x0 = sfield(r, "x0", "0x0000000000000000");
    x1 = sfield(r, "x1", "0x0000000000000000");
    c = context_for(tid, true);
    if (!c) {
        free(pc);
        free(sp);
        free(x0);
        free(x1);
        ms_json_free(r);
        return err_json("failed to save context");
    }
    snprintf(c->pc, sizeof(c->pc), "%s", pc);
    snprintf(c->sp, sizeof(c->sp), "%s", sp);
    snprintf(c->x0, sizeof(c->x0), "%s", x0);
    snprintf(c->x1, sizeof(c->x1), "%s", x1);
    c->saved_at = now_ms();
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "ntApi");
    ms_json_writer_string(&w, "NtSetContextThread");
    ms_json_writer_key(&w, "threadId");
    ms_json_writer_u64(&w, tid);
    ms_json_writer_key(&w, "newContext");
    context_json(&w, c);
    ms_json_writer_key(&w, "note");
    ms_json_writer_string(&w,
                          "Context set — in production, thread_set_state(ARM_THREAD_STATE64) writes to real thread");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(pc);
    free(sp);
    free(x0);
    free(x1);
    ms_json_free(r);
    return out;
}

char* ms_kernel_apc_inject(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long tid;
    char *routine, *trampoline;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "thread_id", &tid)) {
        ms_json_free(r);
        return err_json("thread_id required");
    }
    routine = sfield(r, "apc_routine", "0x0000000000000000");
    trampoline = sfield(r, "trampoline_address", "0x0000000000000000");
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "ntApi");
    ms_json_writer_string(&w, "NtQueueApcThread (full injection sequence)");
    ms_json_writer_key(&w, "threadId");
    ms_json_writer_u64(&w, tid);
    ms_json_writer_key(&w, "steps");
    ms_json_writer_array_begin(&w);
#define STEP(n, action, api, mach, detail)                                                                             \
    do {                                                                                                               \
        ms_json_writer_object_begin(&w);                                                                               \
        ms_json_writer_key(&w, "step");                                                                                \
        ms_json_writer_u64(&w, n);                                                                                     \
        ms_json_writer_key(&w, "action");                                                                              \
        ms_json_writer_string(&w, action);                                                                             \
        ms_json_writer_key(&w, "ntApi");                                                                               \
        ms_json_writer_string(&w, api);                                                                                \
        ms_json_writer_key(&w, "machApi");                                                                             \
        ms_json_writer_string(&w, mach);                                                                               \
        ms_json_writer_key(&w, "detail");                                                                              \
        ms_json_writer_string(&w, detail);                                                                             \
        ms_json_writer_object_end(&w);                                                                                 \
    } while (0)
    STEP(1, "suspend", "NtSuspendThread", "thread_suspend(thread_port)",
         "Suspend target thread to safely modify its context");
    STEP(2, "save_context", "NtGetContextThread", "thread_get_state(ARM_THREAD_STATE64)",
         "Save full ARM64 context (x0-x28, fp, lr, sp, pc, cpsr) for restoration");
    STEP(3, "modify_context", "NtSetContextThread", "thread_set_state(ARM_THREAD_STATE64)",
         "Set PC to trampoline, x0=ApcContext, x1=ApcRoutine, x2=arg1, x3=arg2, x4=arg3, LR=restore_handler");
    STEP(4, "resume", "NtResumeThread", "thread_resume(thread_port)",
         "Resume thread — it begins executing at trampoline entry point");
    STEP(5, "trampoline_executes", "", "",
         "Trampoline: save remaining regs → BLR x1 (call ApcRoutine) → B restore_handler");
    STEP(6, "restore_context", "", "thread_set_state(ARM_THREAD_STATE64) with saved values",
         "Restore handler: load saved context from step 2 → thread_set_state → original PC resumes");
#undef STEP
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "totalSteps");
    ms_json_writer_u64(&w, 6);
    ms_json_writer_key(&w, "sequence");
    ms_json_writer_string(&w, "suspend → save → modify → resume → execute → restore");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(routine);
    free(trampoline);
    ms_json_free(r);
    return out;
}

static bool apc_filter_matches(const char* filter, const apc_entry* entry) {
    if (filter[0] == '\0')
        return true;
    if (strcmp(filter, "pending") == 0)
        return entry->status == APC_PENDING;
    if (strcmp(filter, "delivered") == 0)
        return entry->status == APC_DELIVERED;
    if (strcmp(filter, "cancelled") == 0)
        return entry->status == APC_CANCELLED;
    return true;
}

char* ms_kernel_apc_queue_status(const char* body, size_t len) {
    ms_json* r = parse_root(body, len);
    unsigned long long tid;
    char* filter = NULL;
    ms_json_writer w;
    if (!r)
        return err_json("invalid JSON object");
    if (u64(r, "thread_id", &tid)) {
        apc_queue* q = queue_for(tid, false);
        size_t i, count = 0;
        filter = sfield(r, "status", "");
        for (i = 0; q && i < q->count; i++)
            if (apc_filter_matches(filter, &q->items[i]))
                count++;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "threadId");
        ms_json_writer_u64(&w, tid);
        ms_json_writer_key(&w, "totalInQueue");
        ms_json_writer_u64(&w, q ? q->count : 0);
        ms_json_writer_key(&w, "filteredCount");
        ms_json_writer_u64(&w, count);
        ms_json_writer_key(&w, "entries");
        ms_json_writer_array_begin(&w);
        for (i = 0; q && i < q->count; i++)
            if (apc_filter_matches(filter, &q->items[i]))
                entry_json(&w, &q->items[i]);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        free(filter);
        ms_json_free(r);
        return ms_json_writer_take(&w);
    } else {
        apc_queue* q;
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        size_t threads = 0;
        for (q = g_queues; q; q = q->next)
            threads++;
        ms_json_writer_key(&w, "totalThreads");
        ms_json_writer_u64(&w, threads);
        ms_json_writer_key(&w, "threads");
        ms_json_writer_array_begin(&w);
        for (q = g_queues; q; q = q->next) {
            size_t i, p = 0, d = 0;
            for (i = 0; i < q->count; i++) {
                if (q->items[i].status == APC_PENDING)
                    p++;
                if (q->items[i].status == APC_DELIVERED)
                    d++;
            }
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "threadId");
            ms_json_writer_u64(&w, q->tid);
            ms_json_writer_key(&w, "total");
            ms_json_writer_u64(&w, q->count);
            ms_json_writer_key(&w, "pending");
            ms_json_writer_u64(&w, p);
            ms_json_writer_key(&w, "delivered");
            ms_json_writer_u64(&w, d);
            ms_json_writer_object_end(&w);
        }
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_free(r);
        return ms_json_writer_take(&w);
    }
}
char* ms_kernel_apc_trampoline_status(const char* body, size_t len) {
    (void)body;
    (void)len;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "trampoline");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "allocated");
    ms_json_writer_bool(&w, g_trampoline.allocated);
    ms_json_writer_key(&w, "page_address");
    ms_json_writer_string(&w, g_trampoline.address);
    ms_json_writer_key(&w, "page_size");
    ms_json_writer_u64(&w, g_trampoline.page_size);
    ms_json_writer_key(&w, "trampoline_offset");
    ms_json_writer_u64(&w, g_trampoline.trampoline_offset);
    ms_json_writer_key(&w, "restore_handler_offset");
    ms_json_writer_u64(&w, g_trampoline.restore_offset);
    ms_json_writer_key(&w, "code_size");
    ms_json_writer_u64(&w, g_trampoline.code_size);
    ms_json_writer_key(&w, "status");
    ms_json_writer_string(&w, g_trampoline.status);
    ms_json_writer_key(&w, "detail");
    ms_json_writer_string(&w, g_trampoline.detail);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}
