#include "metalsharp_backend/kernel_handles.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_OBJECT_NAME 2048

typedef enum {
    OT_PROCESS,
    OT_THREAD,
    OT_FILE,
    OT_DEVICE,
    OT_KEY,
    OT_EVENT,
    OT_MUTANT,
    OT_SEMAPHORE,
    OT_TIMER,
    OT_PORT,
    OT_IO_COMPLETION,
    OT_SECTION,
    OT_DIRECTORY,
    OT_SYMBOLIC_LINK,
    OT_TOKEN,
    OT_JOB,
    OT_DEBUG,
    OT_KEYED_EVENT,
    OT_WAITABLE_PORT,
    OT_WORKER_FACTORY,
    OT_PROFILE,
    OT_TRANSACTION,
    OT_UNKNOWN
} object_type;

typedef enum { BK_FD, BK_MACH, BK_VIRTUAL } backend_kind;

typedef struct {
    unsigned long long handle;
    object_type type;
    unsigned access;
    char name[MAX_OBJECT_NAME];
    backend_kind backend;
    int fd;
    unsigned mach_port;
    char right[32];
    char virtual_name[MAX_OBJECT_NAME];
    unsigned long long created_at;
    unsigned pid;
} handle_entry;

typedef struct handle_table {
    unsigned pid;
    handle_entry* entries;
    size_t count;
    size_t capacity;
    struct handle_table* next;
} handle_table;

static handle_table* g_tables;
static unsigned long long g_next_handle = 0x100;

static unsigned long long now_millis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static const char* type_name(object_type type) {
    static const char* names[] = {
        "Process", "Thread",     "File",         "Device",        "Key",       "Event",        "Mutant", "Semaphore",
        "Timer",   "Port",       "IoCompletion", "Section",       "Directory", "SymbolicLink", "Token",  "Job",
        "Debug",   "KeyedEvent", "WaitablePort", "WorkerFactory", "Profile",   "Transaction",  "Unknown"};
    return type <= OT_UNKNOWN ? names[type] : "Unknown";
}

static bool parse_type(const char* name, object_type* out) {
    object_type t;
    if (name == NULL)
        return false;
    for (t = OT_PROCESS; t <= OT_UNKNOWN; ++t) {
        if (strcmp(name, type_name(t)) == 0) {
            if (out != NULL)
                *out = t;
            return t != OT_UNKNOWN;
        }
    }
    return false;
}

static unsigned type_number(object_type type) {
    return type == OT_UNKNOWN ? 0xffU : (unsigned)type + 1U;
}

static handle_table* find_table(unsigned pid, bool create) {
    handle_table* table;
    for (table = g_tables; table != NULL; table = table->next)
        if (table->pid == pid)
            return table;
    if (!create)
        return NULL;
    table = (handle_table*)calloc(1, sizeof(*table));
    if (table == NULL)
        return NULL;
    table->pid = pid;
    table->next = g_tables;
    g_tables = table;
    return table;
}

static bool add_entry(handle_table* table, const handle_entry* entry) {
    handle_entry* next;
    if (table->count == table->capacity) {
        size_t capacity = table->capacity == 0 ? 16 : table->capacity * 2;
        next = (handle_entry*)realloc(table->entries, capacity * sizeof(*next));
        if (next == NULL)
            return false;
        table->entries = next;
        table->capacity = capacity;
    }
    table->entries[table->count++] = *entry;
    return true;
}

static long parse_object(const char* body, size_t length, ms_json** root, char* error, size_t error_size) {
    *root = ms_json_parse(body == NULL ? "" : body, length, error, error_size);
    if (*root == NULL || ms_json_type_of(*root) != MS_JSON_OBJECT) {
        ms_json_free(*root);
        *root = NULL;
        return -1;
    }
    return 0;
}

static bool u32_field(const ms_json* root, const char* name, unsigned* out) {
    long long value;
    if (!ms_json_as_i64(ms_json_object_get(root, name), &value) || value < 0 || value > 0xffffffffLL)
        return false;
    *out = (unsigned)value;
    return true;
}

static bool u64_field(const ms_json* root, const char* name, unsigned long long* out) {
    long long value;
    if (ms_json_as_i64(ms_json_object_get(root, name), &value) && value >= 0) {
        *out = (unsigned long long)value;
        return true;
    }
    {
        char* text = NULL;
        const ms_json* value_node = ms_json_object_get(root, name);
        if (ms_json_as_string(value_node, &text) && text != NULL) {
            char* end = NULL;
            unsigned long long parsed;
            parsed = strtoull(text, &end, 16);
            if (end != NULL && *end == '\0') {
                *out = parsed;
                free(text);
                return true;
            }
            free(text);
        }
    }
    return false;
}

static char* string_field(const ms_json* root, const char* name) {
    char* value = NULL;
    (void)ms_json_as_string(ms_json_object_get(root, name), &value);
    return value;
}

static void entry_json(ms_json_writer* w, const handle_entry* e) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "handle");
    ms_json_writer_u64(w, e->handle);
    ms_json_writer_key(w, "object_type");
    ms_json_writer_string(w, type_name(e->type));
    ms_json_writer_key(w, "access_mask");
    ms_json_writer_u64(w, e->access);
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, e->name);
    ms_json_writer_key(w, "backend");
    if (e->backend == BK_FD) {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "Fd");
        ms_json_writer_i64(w, e->fd);
        ms_json_writer_object_end(w);
    } else if (e->backend == BK_MACH) {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "MachPort");
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "name");
        ms_json_writer_u64(w, e->mach_port);
        ms_json_writer_key(w, "right");
        ms_json_writer_string(w, e->right);
        ms_json_writer_object_end(w);
        ms_json_writer_object_end(w);
    } else {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "Virtual");
        ms_json_writer_string(w, e->virtual_name);
        ms_json_writer_object_end(w);
    }
    ms_json_writer_key(w, "created_at");
    ms_json_writer_u64(w, e->created_at);
    ms_json_writer_key(w, "process_id");
    ms_json_writer_u64(w, e->pid);
    ms_json_writer_object_end(w);
}

static void entry_summary_json(ms_json_writer* w, const handle_entry* e) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "Handle");
    {
        char text[32];
        snprintf(text, sizeof(text), "0x%08" PRIX64, e->handle);
        ms_json_writer_string(w, text);
    }
    ms_json_writer_key(w, "TypeName");
    ms_json_writer_string(w, type_name(e->type));
    ms_json_writer_key(w, "Name");
    ms_json_writer_string(w, e->name);
    ms_json_writer_key(w, "GrantedAccess");
    {
        char text[32];
        snprintf(text, sizeof(text), "0x%08X", e->access);
        ms_json_writer_string(w, text);
    }
    ms_json_writer_key(w, "Backend");
    if (e->backend == BK_FD) {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "Fd");
        ms_json_writer_i64(w, e->fd);
        ms_json_writer_object_end(w);
    } else if (e->backend == BK_MACH) {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "MachPort");
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "name");
        ms_json_writer_u64(w, e->mach_port);
        ms_json_writer_key(w, "right");
        ms_json_writer_string(w, e->right);
        ms_json_writer_object_end(w);
        ms_json_writer_object_end(w);
    } else {
        ms_json_writer_object_begin(w);
        ms_json_writer_key(w, "Virtual");
        ms_json_writer_string(w, e->virtual_name);
        ms_json_writer_object_end(w);
    }
    ms_json_writer_object_end(w);
}

static char* error_json(const char* message) {
    ms_json_writer w;
    char* out;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}

static char* finish_writer(ms_json_writer* w) {
    ms_json_writer_object_end(w);
    return ms_json_writer_take(w);
}

char* ms_kernel_handle_create(const char* body, size_t length) {
    char error[128], *type_text = NULL, *name = NULL, *backend_text = NULL;
    ms_json* root = NULL;
    unsigned pid, access = 0x001f01ffU;
    object_type type;
    handle_entry e;
    handle_table* table;
    ms_json_writer w;
    char hex[32];
    if (parse_object(body, length, &root, error, sizeof(error)) != 0 || !u32_field(root, "pid", &pid)) {
        ms_json_free(root);
        return error_json("pid (u32) required");
    }
    type_text = string_field(root, "object_type");
    if (type_text == NULL) {
        ms_json_free(root);
        return error_json("object_type string required");
    }
    if (!parse_type(type_text, &type)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown object_type '%s'", type_text);
        free(type_text);
        ms_json_free(root);
        return error_json(msg);
    }
    {
        long long n;
        if (ms_json_as_i64(ms_json_object_get(root, "access_mask"), &n) && n >= 0)
            access = (unsigned)n;
    }
    name = string_field(root, "name");
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.access = access;
    e.pid = pid;
    e.created_at = now_millis();
    e.handle = g_next_handle;
    g_next_handle += 4;
    if (name != NULL)
        snprintf(e.name, sizeof(e.name), "%s", name);
    else
        e.name[0] = '\0';
    e.backend = BK_VIRTUAL;
    snprintf(e.virtual_name, sizeof(e.virtual_name), "auto");
    {
        const ms_json* b = ms_json_object_get(root, "backend");
        if (b != NULL && ms_json_type_of(b) != MS_JSON_OBJECT) {
            free(type_text);
            free(name);
            ms_json_free(root);
            return error_json("backend must be an object: {fd: i32} or {mach_port: u32, right: str} or {virtual: str}");
        }
        if (b != NULL) {
            long long fd;
            unsigned port;
            char* right;
            if (ms_json_as_i64(ms_json_object_get(b, "fd"), &fd)) {
                e.backend = BK_FD;
                e.fd = (int)fd;
            } else if (u32_field(b, "mach_port", &port)) {
                e.backend = BK_MACH;
                e.mach_port = port;
                right = string_field(b, "right");
                snprintf(e.right, sizeof(e.right), "%s", right == NULL ? "send" : right);
                free(right);
            } else {
                backend_text = string_field(b, "virtual");
                if (backend_text != NULL) {
                    e.backend = BK_VIRTUAL;
                    snprintf(e.virtual_name, sizeof(e.virtual_name), "%s", backend_text);
                }
                free(backend_text);
            }
        }
    }
    table = find_table(pid, true);
    if (table == NULL || !add_entry(table, &e)) {
        free(type_text);
        free(name);
        ms_json_free(root);
        return error_json("failed to allocate handle table");
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "handle");
    snprintf(hex, sizeof(hex), "0x%08" PRIX64, e.handle);
    ms_json_writer_string(&w, hex);
    ms_json_writer_key(&w, "handle_raw");
    ms_json_writer_u64(&w, e.handle);
    ms_json_writer_key(&w, "entry");
    entry_json(&w, &e);
    free(type_text);
    free(name);
    ms_json_free(root);
    return finish_writer(&w);
}

static bool get_handle(const ms_json* root, const char* field, unsigned long long* out) {
    return u64_field(root, field, out);
}

char* ms_kernel_handle_close(const char* body, size_t length) {
    char error[128], *hex;
    ms_json* root = NULL;
    unsigned pid;
    unsigned long long handle;
    handle_table* table;
    size_t i;
    bool removed = false;
    ms_json_writer w;
    if (parse_object(body, length, &root, error, sizeof(error)) != 0 || !u32_field(root, "pid", &pid)) {
        ms_json_free(root);
        return error_json("pid (u32) required");
    }
    if (!get_handle(root, "handle", &handle)) {
        ms_json_free(root);
        return error_json("handle (u64 or hex string) required");
    }
    table = find_table(pid, false);
    if (table == NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "no handle table for pid %u", pid);
        ms_json_free(root);
        return error_json(msg);
    }
    for (i = 0; i < table->count; ++i)
        if (table->entries[i].handle == handle) {
            table->entries[i] = table->entries[--table->count];
            removed = true;
            break;
        }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, removed);
    ms_json_writer_key(&w, "handle");
    hex = (char*)malloc(32);
    if (hex != NULL) {
        snprintf(hex, 32, "0x%08" PRIX64, handle);
        ms_json_writer_string(&w, hex);
        free(hex);
    } else
        ms_json_writer_string(&w, "");
    ms_json_writer_key(&w, "removed");
    ms_json_writer_bool(&w, removed);
    ms_json_writer_object_end(&w);
    ms_json_free(root);
    return ms_json_writer_take(&w);
}

char* ms_kernel_handle_duplicate(const char* body, size_t length) {
    char error[128], *hex1, *hex2;
    ms_json* root = NULL;
    unsigned pid, access = 0;
    unsigned long long source, options = 0;
    handle_table* table;
    handle_entry e;
    size_t i;
    ms_json_writer w;
    if (parse_object(body, length, &root, error, sizeof(error)) != 0 || !u32_field(root, "pid", &pid)) {
        ms_json_free(root);
        return error_json("pid (u32) required");
    }
    if (!u64_field(root, "source_handle", &source)) {
        ms_json_free(root);
        return error_json("source_handle (u64) required");
    }
    {
        long long n;
        if (ms_json_as_i64(ms_json_object_get(root, "access_mask"), &n) && n >= 0)
            access = (unsigned)n;
        if (ms_json_as_i64(ms_json_object_get(root, "options"), &n) && n >= 0)
            options = (unsigned)n;
    }
    table = find_table(pid, false);
    if (table == NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "no handle table for pid %u", pid);
        ms_json_free(root);
        return error_json(msg);
    }
    for (i = 0; i < table->count; ++i)
        if (table->entries[i].handle == source)
            break;
    if (i == table->count) {
        char msg[128];
        snprintf(msg, sizeof(msg), "source handle 0x%08" PRIX64 " not found", source);
        ms_json_free(root);
        return error_json(msg);
    }
    e = table->entries[i];
    e.handle = g_next_handle;
    g_next_handle += 4;
    e.access = access;
    e.created_at = now_millis();
    if (!add_entry(table, &e)) {
        ms_json_free(root);
        return error_json("failed to allocate duplicate handle");
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "source_handle");
    hex1 = malloc(32);
    snprintf(hex1, 32, "0x%08" PRIX64, source);
    ms_json_writer_string(&w, hex1);
    free(hex1);
    ms_json_writer_key(&w, "new_handle");
    hex2 = malloc(32);
    snprintf(hex2, 32, "0x%08" PRIX64, e.handle);
    ms_json_writer_string(&w, hex2);
    free(hex2);
    ms_json_writer_key(&w, "new_handle_raw");
    ms_json_writer_u64(&w, e.handle);
    ms_json_writer_key(&w, "entry");
    entry_json(&w, &e);
    ms_json_writer_key(&w, "options");
    ms_json_writer_u64(&w, options);
    ms_json_writer_object_end(&w);
    ms_json_free(root);
    return ms_json_writer_take(&w);
}

char* ms_kernel_handle_query(const char* body, size_t length) {
    char error[128];
    ms_json* root = NULL;
    unsigned pid;
    unsigned long long handle;
    handle_table* table;
    size_t i;
    ms_json_writer w;
    char hex[32], granted[32];
    if (parse_object(body, length, &root, error, sizeof(error)) != 0 || !u32_field(root, "pid", &pid)) {
        ms_json_free(root);
        return error_json("pid (u32) required");
    }
    if (!u64_field(root, "handle", &handle)) {
        ms_json_free(root);
        return error_json("handle (u64) required");
    }
    table = find_table(pid, false);
    if (table == NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "no handle table for pid %u", pid);
        ms_json_free(root);
        return error_json(msg);
    }
    for (i = 0; i < table->count; ++i)
        if (table->entries[i].handle == handle)
            break;
    if (i == table->count) {
        char msg[128];
        snprintf(msg, sizeof(msg), "handle 0x%08" PRIX64 " not found in pid %u", handle, pid);
        ms_json_free(root);
        return error_json(msg);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "handle");
    snprintf(hex, sizeof(hex), "0x%08" PRIX64, handle);
    ms_json_writer_string(&w, hex);
    ms_json_writer_key(&w, "TypeName");
    ms_json_writer_string(&w, type_name(table->entries[i].type));
    ms_json_writer_key(&w, "HandleCount");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "PointerCount");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "Name");
    ms_json_writer_string(&w, table->entries[i].name);
    ms_json_writer_key(&w, "GrantedAccess");
    snprintf(granted, sizeof(granted), "0x%08X", table->entries[i].access);
    ms_json_writer_string(&w, granted);
    ms_json_writer_key(&w, "entry");
    entry_json(&w, &table->entries[i]);
    ms_json_writer_object_end(&w);
    ms_json_free(root);
    return ms_json_writer_take(&w);
}

static void type_counts_json(ms_json_writer* w, const handle_table* table) {
    object_type t;
    ms_json_writer_object_begin(w);
    for (t = OT_PROCESS; t <= OT_UNKNOWN; ++t) {
        size_t count = 0, i;
        for (i = 0; i < table->count; ++i)
            if (table->entries[i].type == t)
                count++;
        if (count > 0) {
            ms_json_writer_key(w, type_name(t));
            ms_json_writer_u64(w, count);
        }
    }
    ms_json_writer_object_end(w);
}

char* ms_kernel_handle_enumerate(const char* body, size_t length) {
    char error[128], *filter = NULL;
    ms_json* root = NULL;
    unsigned pid;
    handle_table* table;
    object_type filter_type;
    bool filtered = false;
    size_t i, count = 0;
    ms_json_writer w;
    if (parse_object(body, length, &root, error, sizeof(error)) != 0 || !u32_field(root, "pid", &pid)) {
        ms_json_free(root);
        return error_json("pid (u32) required");
    }
    filter = string_field(root, "filter_type");
    if (filter != NULL) {
        if (!parse_type(filter, &filter_type)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "unknown filter_type '%s'", filter);
            free(filter);
            ms_json_free(root);
            return error_json(msg);
        }
        filtered = true;
    }
    table = find_table(pid, false);
    if (table == NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "no handle table for pid %u", pid);
        free(filter);
        ms_json_free(root);
        return error_json(msg);
    }
    for (i = 0; i < table->count; ++i)
        if (!filtered || table->entries[i].type == filter_type)
            count++;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, pid);
    ms_json_writer_key(&w, "handleCount");
    ms_json_writer_u64(&w, count);
    ms_json_writer_key(&w, "typeCounts");
    type_counts_json(&w, table);
    ms_json_writer_key(&w, "handles");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < table->count; ++i)
        if (!filtered || table->entries[i].type == filter_type)
            entry_summary_json(&w, &table->entries[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    free(filter);
    ms_json_free(root);
    return ms_json_writer_take(&w);
}

char* ms_kernel_handle_system_info(const char* body, size_t length) {
    char error[128];
    ms_json* root = NULL;
    unsigned pid;
    handle_table* table;
    size_t i;
    ms_json_writer w;
    char hex[32], pointer[32], access[32];
    if (parse_object(body, length, &root, error, sizeof(error)) != 0 || !u32_field(root, "pid", &pid)) {
        ms_json_free(root);
        return error_json("pid (u32) required");
    }
    table = find_table(pid, false);
    if (table == NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "no handle table for pid %u", pid);
        ms_json_free(root);
        return error_json(msg);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "SystemHandleInformation");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "NumberOfHandles");
    ms_json_writer_u64(&w, table->count);
    ms_json_writer_key(&w, "Handles");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < table->count; ++i) {
        const handle_entry* e = &table->entries[i];
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ProcessId");
        ms_json_writer_u64(&w, e->pid);
        ms_json_writer_key(&w, "Handle");
        snprintf(hex, sizeof(hex), "0x%08" PRIX64, e->handle);
        ms_json_writer_string(&w, hex);
        ms_json_writer_key(&w, "ObjectTypeNumber");
        ms_json_writer_u64(&w, type_number(e->type));
        ms_json_writer_key(&w, "Flags");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_key(&w, "Pointer");
        snprintf(pointer, sizeof(pointer), "0x%016" PRIX64, e->handle * 31ULL);
        ms_json_writer_string(&w, pointer);
        ms_json_writer_key(&w, "GrantedAccess");
        snprintf(access, sizeof(access), "0x%08X", e->access);
        ms_json_writer_string(&w, access);
        ms_json_writer_key(&w, "TypeName");
        ms_json_writer_string(&w, type_name(e->type));
        ms_json_writer_key(&w, "Name");
        ms_json_writer_string(&w, e->name);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_free(root);
    return ms_json_writer_take(&w);
}

static void table_summary(ms_json_writer* w, const handle_table* t) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "pid");
    ms_json_writer_u64(w, t->pid);
    ms_json_writer_key(w, "handleCount");
    ms_json_writer_u64(w, t->count);
    ms_json_writer_key(w, "typeCounts");
    type_counts_json(w, t);
    ms_json_writer_object_end(w);
}

char* ms_kernel_handle_table_status(const char* body, size_t length) {
    char error[128];
    ms_json* root = NULL;
    unsigned pid;
    bool has_pid = false;
    handle_table* table;
    ms_json_writer w;
    size_t i;
    unsigned total = 0;
    if (parse_object(body, length, &root, error, sizeof(error)) != 0)
        return error_json("invalid JSON object");
    has_pid = u32_field(root, "pid", &pid);
    if (has_pid) {
        table = find_table(pid, false);
        if (table == NULL) {
            char msg[128];
            snprintf(msg, sizeof(msg), "no handle table for pid %u", pid);
            ms_json_free(root);
            return error_json(msg);
        }
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "pid");
        ms_json_writer_u64(&w, pid);
        ms_json_writer_key(&w, "handleCount");
        ms_json_writer_u64(&w, table->count);
        ms_json_writer_key(&w, "typeCounts");
        type_counts_json(&w, table);
        ms_json_writer_key(&w, "entries");
        ms_json_writer_array_begin(&w);
        for (i = 0; i < table->count; ++i)
            entry_summary_json(&w, &table->entries[i]);
        ms_json_writer_array_end(&w);
        ms_json_writer_object_end(&w);
        ms_json_free(root);
        return ms_json_writer_take(&w);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "totalTables");
    for (table = g_tables; table != NULL; table = table->next)
        total++;
    ms_json_writer_u64(&w, total);
    ms_json_writer_key(&w, "pids");
    ms_json_writer_array_begin(&w);
    for (table = g_tables; table != NULL; table = table->next)
        ms_json_writer_u64(&w, table->pid);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "tables");
    ms_json_writer_array_begin(&w);
    for (table = g_tables; table != NULL; table = table->next)
        table_summary(&w, table);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_free(root);
    return ms_json_writer_take(&w);
}

char* ms_kernel_handle_seed_demo(const char* body, size_t length) {
    static const struct {
        object_type type;
        unsigned access;
        const char* name;
        backend_kind backend;
        int fd;
        unsigned port;
        const char* virtual_name;
    } demo[] = {
        {OT_PROCESS, 0x001f0fff, "\\\\??\\\\explorer.exe", BK_FD, 0, 0, ""},
        {OT_THREAD, 0x001f0fff, "", BK_VIRTUAL, 0, 0, "main thread"},
        {OT_FILE, 0x00100001, "\\\\Device\\\\HarddiskVolume3\\\\Windows\\\\System32\\\\ntdll.dll", BK_FD, 3, 0, ""},
        {OT_FILE, 0x00100001, "\\\\Device\\\\HarddiskVolume3\\\\Windows\\\\System32\\\\kernel32.dll", BK_FD, 4, 0, ""},
        {OT_FILE, 0x00100001, "\\\\Device\\\\HarddiskVolume3\\\\game\\\\game.exe", BK_FD, 5, 0, ""},
        {OT_SECTION, 5, "\\\\Windows\\\\System32\\\\ntdll.dll", BK_FD, 6, 0, ""},
        {OT_EVENT, 0x001f0003, "\\\\BaseNamedObjects\\\\Global\\\\GameInitComplete", BK_VIRTUAL, 0, 0, "event"},
        {OT_MUTANT, 0x001f0001, "\\\\BaseNamedObjects\\\\Global\\\\GameMutex", BK_VIRTUAL, 0, 0, "mutex"},
        {OT_KEY, 0x00020019, "\\\\REGISTRY\\\\MACHINE\\\\SOFTWARE\\\\Game", BK_VIRTUAL, 0, 0, "registry"},
        {OT_KEY, 0x00020019, "\\\\REGISTRY\\\\MACHINE\\\\SYSTEM\\\\CurrentControlSet\\\\Services", BK_VIRTUAL, 0, 0,
         "registry"},
        {OT_TOKEN, 0x00020008, "", BK_VIRTUAL, 0, 0, "primary token"},
        {OT_PORT, 0x000f003f, "\\\\RPC Control\\\\epmapper", BK_MACH, 0, 0x1307, ""},
        {OT_DEVICE, 0x000f003f, "\\\\Device\\\\Afd", BK_FD, 7, 0, ""},
        {OT_IO_COMPLETION, 0x001f0003, "", BK_FD, 8, 0, ""},
        {OT_SEMAPHORE, 0x001f0003, "\\\\BaseNamedObjects\\\\Global\\\\MaxConnections", BK_VIRTUAL, 0, 0, "semaphore"},
        {OT_TIMER, 0x001f0003, "", BK_MACH, 0, 0x1401, ""},
        {OT_DEBUG, 0x000f003f, "", BK_VIRTUAL, 0, 0, "debug object"},
        {OT_JOB, 0x001f0fff, "\\\\BaseNamedObjects\\\\GameJob", BK_VIRTUAL, 0, 0, "coalition"},
        {OT_DIRECTORY, 0x00020001, "\\\\??\\\\", BK_VIRTUAL, 0, 0, "object directory"},
        {OT_SYMBOLIC_LINK, 0x00020001, "\\\\??\\\\C:", BK_VIRTUAL, 0, 0, "symlink to prefix"}};
    char error[128];
    ms_json* root = NULL;
    unsigned pid, count = 25;
    handle_table* table;
    size_t i, limit;
    ms_json_writer w;
    if (parse_object(body, length, &root, error, sizeof(error)) != 0 || !u32_field(root, "pid", &pid)) {
        ms_json_free(root);
        return error_json("pid (u32) required");
    }
    {
        long long n;
        if (ms_json_as_i64(ms_json_object_get(root, "count"), &n) && n >= 0)
            count = (unsigned)n;
    }
    table = find_table(pid, true);
    if (table == NULL) {
        ms_json_free(root);
        return error_json("failed to allocate handle table");
    }
    limit = count < sizeof(demo) / sizeof(demo[0]) ? count : sizeof(demo) / sizeof(demo[0]);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, pid);
    ms_json_writer_key(&w, "created");
    ms_json_writer_u64(&w, limit);
    ms_json_writer_key(&w, "totalHandles");
    ms_json_writer_u64(&w, table->count + limit);
    ms_json_writer_key(&w, "typeCounts"); /* Emit counts after constructing the entries below. */
    for (i = 0; i < limit; ++i) {
        handle_entry e;
        memset(&e, 0, sizeof(e));
        e.handle = g_next_handle;
        g_next_handle += 4;
        e.type = demo[i].type;
        e.access = demo[i].access;
        e.pid = pid;
        e.created_at = now_millis();
        snprintf(e.name, sizeof(e.name), "%s", demo[i].name);
        e.backend = demo[i].backend;
        e.fd = demo[i].fd;
        e.mach_port = demo[i].port;
        snprintf(e.right, sizeof(e.right), "send");
        snprintf(e.virtual_name, sizeof(e.virtual_name), "%s", demo[i].virtual_name);
        (void)add_entry(table, &e);
    }
    /* Rebuild the response writer because typeCounts must include the new entries. */
    ms_json_writer_dispose(&w);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, pid);
    ms_json_writer_key(&w, "created");
    ms_json_writer_u64(&w, limit);
    ms_json_writer_key(&w, "totalHandles");
    ms_json_writer_u64(&w, table->count);
    ms_json_writer_key(&w, "typeCounts");
    type_counts_json(&w, table);
    ms_json_writer_key(&w, "handles");
    ms_json_writer_array_begin(&w);
    for (i = table->count - limit; i < table->count; ++i) {
        char text[128];
        snprintf(text, sizeof(text), "0x%08" PRIX64 " %s %s", table->entries[i].handle,
                 type_name(table->entries[i].type), table->entries[i].name);
        ms_json_writer_string(&w, text);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_free(root);
    return ms_json_writer_take(&w);
}
