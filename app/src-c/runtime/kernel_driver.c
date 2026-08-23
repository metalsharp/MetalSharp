#include "metalsharp_backend/kernel_driver.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ITEMS           4096
#define FILE_DEVICE_UNKNOWN 0x0022
#define METHOD_BUFFERED     0
#define METHOD_IN_DIRECT    1
#define METHOD_OUT_DIRECT   2
#define METHOD_NEITHER      3

typedef struct driver {
    unsigned long long id;
    char name[256], ext[32], status[16];
    unsigned long long devices[128], created;
    size_t device_count;
    struct driver* next;
} driver;
typedef struct device {
    unsigned long long id, driver_id;
    char name[256], nt_name[256], dos_name[256], iokit[256];
    unsigned device_type, characteristics;
    unsigned long long created;
    struct device* next;
} device;
typedef struct irp {
    unsigned long long id, driver_id, device_id;
    char major[32], major_nt[64], iokit[256], status[16], input[512], output[512];
    bool has_input, has_output;
    unsigned minor, nt_status, input_size, output_size, selector;
    unsigned long long mach_id, timestamp;
    struct irp* next;
} irp;
typedef struct ioctl_map {
    unsigned long long id;
    unsigned code, device_type, function, access, method, selector, input_size, output_size;
    char name[256], mach_type[128], description[512];
    struct ioctl_map* next;
} ioctl_map;
static driver* g_drivers;
static device* g_devices;
static irp* g_irps;
static ioctl_map* g_ioctls;
static unsigned long long next_driver = 1, next_device = 1, next_irp = 1, next_ioctl = 1;
static unsigned long long now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0)
        return 0;
    return (unsigned long long)t.tv_sec * 1000ULL + (unsigned long long)t.tv_nsec / 1000000ULL;
}
static ms_json* root(const char* b, size_t n) {
    char e[128];
    ms_json* r = ms_json_parse(b ? b : "", n, e, sizeof(e));
    if (!r || ms_json_type_of(r) != MS_JSON_OBJECT) {
        ms_json_free(r);
        return NULL;
    }
    return r;
}
static bool u64(const ms_json* r, const char* k, unsigned long long* o) {
    long long n;
    if (!ms_json_as_i64(ms_json_object_get(r, k), &n) || n < 0)
        return false;
    *o = (unsigned long long)n;
    return true;
}
static char* str(const ms_json* r, const char* k) {
    char* s = NULL;
    (void)ms_json_as_string(ms_json_object_get(r, k), &s);
    return s;
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
static driver* find_driver(unsigned long long id) {
    driver* d;
    for (d = g_drivers; d; d = d->next)
        if (d->id == id)
            return d;
    return NULL;
}
static const char* ext_name(const char* s) {
    if (!strcmp(s, "network_extension"))
        return "NetworkExtension";
    if (!strcmp(s, "driver_kit"))
        return "DriverKit";
    if (!strcmp(s, "hybrid"))
        return "Hybrid";
    return "EndpointSecurity";
}
static void dispatch_json(ms_json_writer* w) {
    static const char* pairs[][2] = {{"IRP_MJ_CREATE", "IOServiceOpen"},
                                     {"IRP_MJ_CLOSE", "IOServiceClose"},
                                     {"IRP_MJ_DEVICE_CONTROL", "IOUserClient::externalMethod"},
                                     {"IRP_MJ_READ", "shared_memory_read"},
                                     {"IRP_MJ_WRITE", "shared_memory_write"},
                                     {"IRP_MJ_CLEANUP", "IOUserClient::clientClose"},
                                     {"IRP_MJ_SHUTDOWN", "systemWillShutdown"}};
    size_t i;
    ms_json_writer_object_begin(w);
    for (i = 0; i < 7; i++) {
        ms_json_writer_key(w, pairs[i][0]);
        ms_json_writer_string(w, pairs[i][1]);
    }
    ms_json_writer_object_end(w);
}
static void driver_json(ms_json_writer* w, const driver* d) {
    size_t i;
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, d->id);
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, d->name);
    ms_json_writer_key(w, "nt_driver_object");
    {
        char s[300];
        snprintf(s, sizeof(s), "\\Driver\\%s", d->name);
        ms_json_writer_string(w, s);
    }
    ms_json_writer_key(w, "iokit_service_class");
    {
        char s[300];
        snprintf(s, sizeof(s), "com.metalsharp.anticheat.%s", d->name);
        ms_json_writer_string(w, s);
    }
    ms_json_writer_key(w, "extension_type");
    ms_json_writer_string(w, d->ext);
    ms_json_writer_key(w, "status");
    ms_json_writer_string(w, d->status);
    ms_json_writer_key(w, "dispatch_table");
    dispatch_json(w);
    ms_json_writer_key(w, "devices");
    ms_json_writer_array_begin(w);
    for (i = 0; i < d->device_count; i++)
        ms_json_writer_u64(w, d->devices[i]);
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "created_at");
    ms_json_writer_u64(w, d->created);
    ms_json_writer_object_end(w);
}
static void device_json(ms_json_writer* w, const device* d) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, d->id);
    ms_json_writer_key(w, "driver_id");
    ms_json_writer_u64(w, d->driver_id);
    ms_json_writer_key(w, "name");
    ms_json_writer_string(w, d->name);
    ms_json_writer_key(w, "nt_device_name");
    ms_json_writer_string(w, d->nt_name);
    ms_json_writer_key(w, "dos_device_name");
    ms_json_writer_string(w, d->dos_name);
    ms_json_writer_key(w, "device_type");
    ms_json_writer_u64(w, d->device_type);
    ms_json_writer_key(w, "iokit_user_client_class");
    ms_json_writer_string(w, d->iokit);
    ms_json_writer_key(w, "characteristics");
    ms_json_writer_u64(w, d->characteristics);
    ms_json_writer_key(w, "created_at");
    ms_json_writer_u64(w, d->created);
    ms_json_writer_object_end(w);
}
static const char* major_variant(const char* s) {
    if (!strcmp(s, "create") || !strcmp(s, "IRP_MJ_CREATE"))
        return "Create";
    if (!strcmp(s, "close") || !strcmp(s, "IRP_MJ_CLOSE"))
        return "Close";
    if (!strcmp(s, "read") || !strcmp(s, "IRP_MJ_READ"))
        return "Read";
    if (!strcmp(s, "write") || !strcmp(s, "IRP_MJ_WRITE"))
        return "Write";
    if (!strcmp(s, "flush_buffers") || !strcmp(s, "IRP_MJ_FLUSH_BUFFERS"))
        return "FlushBuffers";
    if (!strcmp(s, "device_control") || !strcmp(s, "IRP_MJ_DEVICE_CONTROL"))
        return "DeviceControl";
    if (!strcmp(s, "internal_device_control") || !strcmp(s, "IRP_MJ_INTERNAL_DEVICE_CONTROL"))
        return "InternalDeviceControl";
    if (!strcmp(s, "shutdown") || !strcmp(s, "IRP_MJ_SHUTDOWN"))
        return "Shutdown";
    if (!strcmp(s, "cleanup") || !strcmp(s, "IRP_MJ_CLEANUP"))
        return "Cleanup";
    if (!strcmp(s, "power") || !strcmp(s, "IRP_MJ_POWER"))
        return "Power";
    if (!strcmp(s, "pnp") || !strcmp(s, "IRP_MJ_PNP"))
        return "Pnp";
    if (!strcmp(s, "system_control") || !strcmp(s, "IRP_MJ_SYSTEM_CONTROL"))
        return "SystemControl";
    return NULL;
}
static const char* major_info(const char* s, unsigned* code, const char** iokit) {
    static const char* names[] = {
        "create",   "close",   "read",  "write", "flush_buffers", "device_control", "internal_device_control",
        "shutdown", "cleanup", "power", "pnp",   "system_control"};
    static const unsigned codes[] = {0, 2, 3, 4, 8, 0x0e, 0x0f, 0x10, 0x12, 0x16, 0x1b, 0x1e};
    static const char* nt[] = {"IRP_MJ_CREATE",
                               "IRP_MJ_CLOSE",
                               "IRP_MJ_READ",
                               "IRP_MJ_WRITE",
                               "IRP_MJ_FLUSH_BUFFERS",
                               "IRP_MJ_DEVICE_CONTROL",
                               "IRP_MJ_INTERNAL_DEVICE_CONTROL",
                               "IRP_MJ_SHUTDOWN",
                               "IRP_MJ_CLEANUP",
                               "IRP_MJ_POWER",
                               "IRP_MJ_PNP",
                               "IRP_MJ_SYSTEM_CONTROL"};
    static const char* mac[] = {"IOServiceOpen -> IOUserClient::start",
                                "IOServiceClose -> IOUserClient::stop",
                                "IOUserClient::registerNotification or shared memory read",
                                "IOUserClient::setProperties or shared memory write",
                                "No direct equivalent — userspace flush",
                                "IOUserClient::externalMethod (selector dispatch)",
                                "IOUserClient::externalMethod (privileged selector)",
                                "IOService::message(kIOMessageServiceIsTerminated/requested)",
                                "IOUserClient::clientClose",
                                "IOService::powerStateDidChangeTo",
                                "IOService::message(kIOMessageServiceIsTerminated/requested)",
                                "IORegistryEntry::setProperties"};
    size_t i;
    if (!s)
        return NULL;
    for (i = 0; i < 12; i++)
        if (!strcmp(s, names[i]) || !strcmp(s, nt[i])) {
            *code = codes[i];
            *iokit = mac[i];
            return nt[i];
        }
    return NULL;
}
static void irp_json(ms_json_writer* w, const irp* r) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, r->id);
    ms_json_writer_key(w, "driver_id");
    ms_json_writer_u64(w, r->driver_id);
    ms_json_writer_key(w, "device_id");
    ms_json_writer_u64(w, r->device_id);
    ms_json_writer_key(w, "major_function");
    ms_json_writer_string(w, r->major);
    ms_json_writer_key(w, "minor_function");
    ms_json_writer_u64(w, r->minor);
    ms_json_writer_key(w, "status");
    ms_json_writer_string(w, r->status);
    ms_json_writer_key(w, "nt_status");
    ms_json_writer_u64(w, r->nt_status);
    ms_json_writer_key(w, "input_buffer");
    if (r->has_input)
        ms_json_writer_string(w, r->input);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "output_buffer");
    if (r->has_output)
        ms_json_writer_string(w, r->output);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "input_size");
    ms_json_writer_u64(w, r->input_size);
    ms_json_writer_key(w, "output_size");
    ms_json_writer_u64(w, r->output_size);
    ms_json_writer_key(w, "mach_message_id");
    ms_json_writer_u64(w, r->mach_id);
    ms_json_writer_key(w, "iokit_selector");
    ms_json_writer_u64(w, r->selector);
    ms_json_writer_key(w, "timestamp");
    ms_json_writer_u64(w, r->timestamp);
    ms_json_writer_object_end(w);
}
static void ioctl_json(ms_json_writer* w, const ioctl_map* m) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "id");
    ms_json_writer_u64(w, m->id);
    ms_json_writer_key(w, "nt_ioctl_code");
    ms_json_writer_u64(w, m->code);
    ms_json_writer_key(w, "nt_name");
    ms_json_writer_string(w, m->name);
    ms_json_writer_key(w, "device_type");
    ms_json_writer_u64(w, m->device_type);
    ms_json_writer_key(w, "function");
    ms_json_writer_u64(w, m->function);
    ms_json_writer_key(w, "access");
    ms_json_writer_u64(w, m->access);
    ms_json_writer_key(w, "method");
    ms_json_writer_u64(w, m->method);
    ms_json_writer_key(w, "iokit_selector");
    ms_json_writer_u64(w, m->selector);
    ms_json_writer_key(w, "mach_message_type");
    ms_json_writer_string(w, m->mach_type);
    ms_json_writer_key(w, "input_size");
    ms_json_writer_u64(w, m->input_size);
    ms_json_writer_key(w, "output_size");
    ms_json_writer_u64(w, m->output_size);
    ms_json_writer_key(w, "description");
    ms_json_writer_string(w, m->description);
    ms_json_writer_object_end(w);
}
static void decode(unsigned code, unsigned* dt, unsigned* fn, unsigned* access, unsigned* method) {
    *dt = (code >> 16) & 0xffff;
    *fn = (code >> 2) & 0xfff;
    *access = (code >> 14) & 3;
    *method = code & 3;
}
static const char* method_name(unsigned m) {
    return m == 0   ? "METHOD_BUFFERED"
           : m == 1 ? "METHOD_IN_DIRECT"
           : m == 2 ? "METHOD_OUT_DIRECT"
           : m == 3 ? "METHOD_NEITHER"
                    : "UNKNOWN";
}

char* ms_driver_load(const char* body, size_t len) {
    ms_json* r = root(body, len);
    char *name, *ext;
    driver* d;
    ms_json_writer w;
    char* out;
    if (!r)
        return error_json("name required");
    name = str(r, "name");
    if (!name) {
        ms_json_free(r);
        return error_json("name required");
    }
    ext = str(r, "extension_type");
    d = calloc(1, sizeof(*d));
    if (!d) {
        free(name);
        free(ext);
        ms_json_free(r);
        return error_json("allocation failed");
    }
    d->id = next_driver++;
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->ext, sizeof(d->ext), "%s", ext_name(ext ? ext : ""));
    snprintf(d->status, sizeof(d->status), "Loaded");
    d->created = now_ms();
    d->next = g_drivers;
    g_drivers = d;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "driver_id");
    ms_json_writer_u64(&w, d->id);
    ms_json_writer_key(&w, "driver");
    driver_json(&w, d);
    ms_json_writer_key(&w, "translation");
    {
        char s[512];
        snprintf(s, sizeof(s), "NT DriverEntry(\\\\Driver\\\\%s) → macOS %s system extension activated", d->name,
                 d->ext);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(name);
    free(ext);
    ms_json_free(r);
    return out;
}
char* ms_driver_unload(const char* body, size_t len) {
    ms_json* r = root(body, len);
    unsigned long long id;
    driver **p, *d;
    device** dp;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "driver_id", &id)) {
        ms_json_free(r);
        return error_json("driver_id required");
    }
    p = &g_drivers;
    while (*p && (*p)->id != id)
        p = &(*p)->next;
    if (!*p) {
        char s[128];
        snprintf(s, sizeof(s), "driver %llu not found", id);
        ms_json_free(r);
        return error_json(s);
    }
    d = *p;
    *p = d->next;
    for (dp = &g_devices; *dp;) {
        if ((*dp)->driver_id == id) {
            device* old = *dp;
            *dp = old->next;
            free(old);
        } else
            dp = &(*dp)->next;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "unloaded");
    driver_json(&w, d);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(d);
    ms_json_free(r);
    return out;
}
char* ms_driver_list(const char* body, size_t len) {
    driver* d;
    size_t n = 0;
    ms_json_writer w;
    char* out;
    (void)body;
    (void)len;
    for (d = g_drivers; d; d = d->next)
        n++;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "count");
    ms_json_writer_u64(&w, n);
    ms_json_writer_key(&w, "drivers");
    ms_json_writer_array_begin(&w);
    for (d = g_drivers; d; d = d->next)
        driver_json(&w, d);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}
char* ms_driver_create_device(const char* body, size_t len) {
    ms_json* r = root(body, len);
    unsigned long long did, devtype = FILE_DEVICE_UNKNOWN, chars = 0;
    char* name;
    driver* dr;
    device* d;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "driver_id", &did)) {
        ms_json_free(r);
        return error_json("driver_id required");
    }
    name = str(r, "device_name");
    if (!name)
        name = strdup("AntiCheat0");
    (void)u64(r, "device_type", &devtype);
    (void)u64(r, "characteristics", &chars);
    d = calloc(1, sizeof(*d));
    if (!d) {
        free(name);
        ms_json_free(r);
        return error_json("allocation failed");
    }
    d->id = next_device++;
    d->driver_id = did;
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->nt_name, sizeof(d->nt_name), "\\Device\\%s", name);
    snprintf(d->dos_name, sizeof(d->dos_name), "\\??\\%s", name);
    d->device_type = (unsigned)devtype;
    d->characteristics = (unsigned)chars;
    snprintf(d->iokit, sizeof(d->iokit), "MetalSharp%s_UserClient", name);
    d->created = now_ms();
    d->next = g_devices;
    g_devices = d;
    dr = find_driver(did);
    if (!dr) {
        free(name);
        ms_json_free(r);
        return error_json("driver not found during device creation");
    }
    if (dr->device_count < 128)
        dr->devices[dr->device_count++] = d->id;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "device_id");
    ms_json_writer_u64(&w, d->id);
    ms_json_writer_key(&w, "device");
    device_json(&w, d);
    ms_json_writer_key(&w, "translation");
    {
        char s[512];
        snprintf(s, sizeof(s), "NT IoCreateDevice(%s) → IOKit IOService + %s user client", d->nt_name, d->iokit);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(name);
    ms_json_free(r);
    return out;
}
char* ms_driver_list_devices(const char* body, size_t len) {
    device* d;
    size_t n = 0;
    ms_json_writer w;
    char* out;
    (void)body;
    (void)len;
    for (d = g_devices; d; d = d->next)
        n++;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "count");
    ms_json_writer_u64(&w, n);
    ms_json_writer_key(&w, "devices");
    ms_json_writer_array_begin(&w);
    for (d = g_devices; d; d = d->next)
        device_json(&w, d);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}
char* ms_driver_dispatch_irp(const char* body, size_t len) {
    ms_json* r = root(body, len);
    unsigned long long did, devid, selector = 0, input_size = 0, output_size = 0;
    char *major, *input, *output;
    unsigned code;
    const char* mac;
    irp* q;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "driver_id", &did)) {
        ms_json_free(r);
        return error_json("driver_id required");
    }
    if (!u64(r, "device_id", &devid)) {
        ms_json_free(r);
        return error_json("device_id required");
    }
    major = str(r, "major_function");
    mac = NULL;
    if (!major || !(major_info(major, &code, &mac))) {
        free(major);
        ms_json_free(r);
        return error_json("major_function required (e.g. device_control, create, close, read, write)");
    }
    (void)u64(r, "iokit_selector", &selector);
    if (!selector)
        selector = code;
    (void)u64(r, "input_size", &input_size);
    (void)u64(r, "output_size", &output_size);
    input = str(r, "input_buffer");
    output = str(r, "output_buffer");
    q = calloc(1, sizeof(*q));
    if (!q) {
        free(major);
        free(input);
        free(output);
        ms_json_free(r);
        return error_json("allocation failed");
    }
    q->id = next_irp++;
    q->driver_id = did;
    q->device_id = devid;
    snprintf(q->major, sizeof(q->major), "%s", major_variant(major));
    snprintf(q->major_nt, sizeof(q->major_nt), "%s", major_info(major, &code, &mac));
    snprintf(q->iokit, sizeof(q->iokit), "%s", mac);
    snprintf(q->status, sizeof(q->status), "Completed");
    q->nt_status = 0;
    q->minor = 0;
    q->has_input = input != NULL;
    q->has_output = output != NULL;
    if (input)
        snprintf(q->input, sizeof(q->input), "%s", input);
    if (output)
        snprintf(q->output, sizeof(q->output), "%s", output);
    q->input_size = (unsigned)input_size;
    q->output_size = (unsigned)output_size;
    q->selector = (unsigned)selector;
    q->mach_id = 0x80000000ULL + (did * 256ULL) + code;
    q->timestamp = now_ms();
    q->next = g_irps;
    g_irps = q;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "irp_id");
    ms_json_writer_u64(&w, q->id);
    ms_json_writer_key(&w, "irp");
    irp_json(&w, q);
    ms_json_writer_key(&w, "translation");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "nt_path");
    {
        char s[256];
        snprintf(s, sizeof(s), "%s -> \\Device\\%llu(IRP)", q->major_nt, devid);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "macos_path");
    {
        char s[256];
        snprintf(s, sizeof(s), "IOUserClient::externalMethod(selector=%u) -> Mach msg 0x%08llX", q->selector,
                 q->mach_id);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "dispatch");
    {
        char s[256];
        snprintf(s, sizeof(s), "%s → %s", q->major_nt, q->iokit);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(major);
    free(input);
    free(output);
    ms_json_free(r);
    return out;
}
char* ms_driver_list_irps(const char* body, size_t len) {
    irp* q;
    size_t n = 0;
    ms_json_writer w;
    char* out;
    (void)body;
    (void)len;
    for (q = g_irps; q; q = q->next)
        n++;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "count");
    ms_json_writer_u64(&w, n);
    ms_json_writer_key(&w, "irps");
    ms_json_writer_array_begin(&w);
    for (q = g_irps; q; q = q->next)
        irp_json(&w, q);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}
char* ms_driver_register_ioctl(const char* body, size_t len) {
    ms_json* r = root(body, len);
    unsigned long long code64, sel = 0, in = 0, outsize = 0;
    unsigned dt, fn, access, method;
    char *name, *mach, *desc;
    ioctl_map* m;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "nt_ioctl_code", &code64)) {
        ms_json_free(r);
        return error_json("nt_ioctl_code required");
    }
    decode((unsigned)code64, &dt, &fn, &access, &method);
    name = str(r, "nt_name");
    mach = str(r, "mach_message_type");
    desc = str(r, "description");
    (void)u64(r, "iokit_selector", &sel);
    if (!sel)
        sel = fn;
    (void)u64(r, "input_size", &in);
    (void)u64(r, "output_size", &outsize);
    m = calloc(1, sizeof(*m));
    if (!m) {
        free(name);
        free(mach);
        free(desc);
        ms_json_free(r);
        return error_json("allocation failed");
    }
    m->id = next_ioctl++;
    m->code = (unsigned)code64;
    m->device_type = dt;
    m->function = fn;
    m->access = access;
    m->method = method;
    m->selector = (unsigned)sel;
    m->input_size = (unsigned)in;
    m->output_size = (unsigned)outsize;
    snprintf(m->name, sizeof(m->name), "%s", name ? name : "unknown");
    snprintf(m->mach_type, sizeof(m->mach_type), "%s", mach ? mach : "async_notification");
    snprintf(m->description, sizeof(m->description), "%s", desc ? desc : "");
    m->next = g_ioctls;
    g_ioctls = m;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "ioctl_id");
    ms_json_writer_u64(&w, m->id);
    ms_json_writer_key(&w, "mapping");
    ioctl_json(&w, m);
    ms_json_writer_key(&w, "decoded");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "device_type");
    {
        char s[16];
        snprintf(s, sizeof(s), "0x%04X", dt);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "function");
    ms_json_writer_u64(&w, fn);
    ms_json_writer_key(&w, "access");
    {
        char s[16];
        snprintf(s, sizeof(s), "0x%02X", access);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "method");
    ms_json_writer_string(&w, method_name(method));
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "translation");
    {
        char s[256];
        snprintf(s, sizeof(s), "DeviceIoControl(0x%08X) → IOUserClient::externalMethod(selector=%u)", m->code,
                 m->selector);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(name);
    free(mach);
    free(desc);
    ms_json_free(r);
    return out;
}
char* ms_driver_decode_ioctl(const char* body, size_t len) {
    ms_json* r = root(body, len);
    unsigned long long code64;
    unsigned dt, fn, access, method;
    ioctl_map* m;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "ioctl_code", &code64)) {
        ms_json_free(r);
        return error_json("ioctl_code required");
    }
    decode((unsigned)code64, &dt, &fn, &access, &method);
    for (m = g_ioctls; m; m = m->next)
        if (m->code == (unsigned)code64)
            break;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "ioctl_code");
    {
        char s[32];
        snprintf(s, sizeof(s), "0x%08X", (unsigned)code64);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "decoded");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "device_type");
    {
        char s[16];
        snprintf(s, sizeof(s), "0x%04X", dt);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "function");
    ms_json_writer_u64(&w, fn);
    ms_json_writer_key(&w, "access");
    {
        char s[16];
        snprintf(s, sizeof(s), "0x%02X", access);
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_key(&w, "method");
    ms_json_writer_string(&w, method_name(method));
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "reconstructed");
    {
        char s[32];
        unsigned rec = (dt << 16) | (access << 14) | (fn << 2) | method;
        snprintf(s, sizeof(s), "0x%08X", rec);
        ms_json_writer_string(&w, s);
        ms_json_writer_key(&w, "matches_original");
        ms_json_writer_bool(&w, rec == (unsigned)code64);
    }
    ms_json_writer_key(&w, "known_mapping");
    if (m)
        ioctl_json(&w, m);
    else
        ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    ms_json_free(r);
    return out;
}
char* ms_driver_list_ioctls(const char* body, size_t len) {
    ioctl_map* m;
    size_t n = 0;
    ms_json_writer w;
    char* out;
    (void)body;
    (void)len;
    for (m = g_ioctls; m; m = m->next)
        n++;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "count");
    ms_json_writer_u64(&w, n);
    ms_json_writer_key(&w, "mappings");
    ms_json_writer_array_begin(&w);
    for (m = g_ioctls; m; m = m->next)
        ioctl_json(&w, m);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}
char* ms_driver_type_survey(const char* body, size_t len) {
    (void)body;
    (void)len;
    return strdup(
        "{\"communication_path\":{\"macos\":\"UserMode → IOConnectCallMethod(connect, selector, in, out) → "
        "IOUserClient::externalMethod → Mach reply → Result\",\"nt\":\"UserMode → DeviceIoControl(hDevice, ioctl, in, "
        "out) → KernelDriver → IRP_MJ_DEVICE_CONTROL → Result\"},\"ioctl_encoding\":\"CTL_CODE(DeviceType, Function, "
        "Method, Access) = (DeviceType << 16) | (Access << 14) | (Function << 2) | "
        "Method\",\"irp_major_functions\":[{\"code\":\"0x00\",\"iokit\":\"IOServiceOpen → "
        "IOUserClient::start\",\"nt\":\"IRP_MJ_CREATE\"},{\"code\":\"0x02\",\"iokit\":\"IOServiceClose → "
        "IOUserClient::stop\",\"nt\":\"IRP_MJ_CLOSE\"},{\"code\":\"0x03\",\"iokit\":\"IOUserClient shared memory / "
        "registerNotification\",\"nt\":\"IRP_MJ_READ\"},{\"code\":\"0x04\",\"iokit\":\"IOUserClient setProperties / "
        "shared memory write\",\"nt\":\"IRP_MJ_WRITE\"},{\"code\":\"0x08\",\"iokit\":\"No direct equivalent (userspace "
        "flush)\",\"nt\":\"IRP_MJ_FLUSH_BUFFERS\"},{\"code\":\"0x0E\",\"iokit\":\"IOUserClient::externalMethod (main "
        "anti-cheat "
        "path)\",\"nt\":\"IRP_MJ_DEVICE_CONTROL\"},{\"code\":\"0x0F\",\"iokit\":\"IOUserClient::externalMethod "
        "(privileged)\",\"nt\":\"IRP_MJ_INTERNAL_DEVICE_CONTROL\"},{\"code\":\"0x10\",\"iokit\":\"IOService::"
        "systemWillShutdown\",\"nt\":\"IRP_MJ_SHUTDOWN\"},{\"code\":\"0x12\",\"iokit\":\"IOUserClient::clientClose\","
        "\"nt\":\"IRP_MJ_CLEANUP\"},{\"code\":\"0x16\",\"iokit\":\"IOService::powerStateDidChangeTo\",\"nt\":\"IRP_MJ_"
        "POWER\"},{\"code\":\"0x1B\",\"iokit\":\"IOService::message(kIOMessageService*)\",\"nt\":\"IRP_MJ_PNP\"},{"
        "\"code\":\"0x1E\",\"iokit\":\"IORegistryEntry::setProperties\",\"nt\":\"IRP_MJ_SYSTEM_CONTROL\"}],\"ok\":true,"
        "\"wdm_to_iokit\":[{\"macos\":\"IOService subclass\",\"notes\":\"Driver state container. macOS: "
        "com_metalsharp_anticheat_* "
        "IOService.\",\"nt\":\"DRIVER_OBJECT\"},{\"macos\":\"IOUserClient\",\"notes\":\"Device endpoint. User-mode "
        "opens via IOServiceOpen → IOUserClient.\",\"nt\":\"DEVICE_OBJECT\"},{\"macos\":\"Mach message / "
        "IOExternalMethod\",\"notes\":\"Request/response unit. Serialized as Mach IPC message or "
        "IOExternalMethodDispatch.\",\"nt\":\"IRP (I/O Request "
        "Packet)\"},{\"macos\":\"IOUserClient::externalMethod\",\"notes\":\"Dispatch function for IRP_MJ_*. Maps to "
        "IOKit selector-based dispatch.\",\"nt\":\"DRIVER_DISPATCH\"},{\"macos\":\"Mach message "
        "return\",\"notes\":\"Status + bytes transferred. Encoded in Mach reply "
        "message.\",\"nt\":\"IO_STATUS_BLOCK\"},{\"macos\":\"IORegistryEntry name\",\"notes\":\"Device naming. "
        "\\\\Device\\\\AntiCheat → IORegistry /metalsharp/anticheat/0.\",\"nt\":\"UNICODE_STRING (dev "
        "name)\"},{\"macos\":\"IOWorkLoop + IOCommandGate\",\"notes\":\"Deferred procedure call. IOKit serialized work "
        "loop for safe "
        "concurrency.\",\"nt\":\"IO_DPC_ROUTINE\"},{\"macos\":\"IOFilterInterruptEventSource\",\"notes\":\"Interrupt "
        "handling. IOKit interrupt event source for hardware "
        "drivers.\",\"nt\":\"KINTERRUPT\"},{\"macos\":\"IOService::fWorkspace\",\"notes\":\"Per-device private data. "
        "Stored in IOService member variables.\",\"nt\":\"DEVICE_EXTENSION\"}]}");
}
char* ms_driver_extension_template(const char* body, size_t len) {
    ms_json* r = root(body, len);
    char *name, *ext;
    ms_json_writer w;
    char* out;
    if (!r)
        return error_json("invalid JSON object");
    name = str(r, "name");
    if (!name)
        name = strdup("MetalSharpAntiCheat");
    ext = str(r, "extension_type");
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "template");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, name);
    ms_json_writer_key(&w, "extension_type");
    ms_json_writer_string(&w, ext_name(ext ? ext : ""));
    ms_json_writer_key(&w, "nt_callbacks");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "PsSetCreateProcessNotifyRoutineEx2");
    ms_json_writer_string(&w, "PsSetCreateThreadNotifyRoutineEx");
    ms_json_writer_string(&w, "PsSetLoadImageNotifyRoutineEx");
    ms_json_writer_string(&w, "ObRegisterCallbacks");
    ms_json_writer_string(&w, "CmRegisterCallbackEx");
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "iokit_methods");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "start(IOService *provider)");
    ms_json_writer_string(&w, "stop(IOService *provider)");
    ms_json_writer_string(&w, "externalMethod(uint32_t selector, ...)");
    ms_json_writer_string(&w, "clientClose()");
    ms_json_writer_string(&w, "registerNotification(mach_port_t port)");
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "es_subscriptions");
    ms_json_writer_array_begin(&w);
    if (!ext || !strcmp(ext, "endpoint_security") || !strcmp(ext, "hybrid")) {
        ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_EXEC");
        ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_FORK");
        ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_EXIT");
        ms_json_writer_string(&w, "ES_EVENT_TYPE_NOTIFY_MMAP");
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "mach_services");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "com.metalsharp.anticheat.metalsharpanticheat");
    ms_json_writer_string(&w, "com.metalsharp.anticheat.metalsharpanticheat.notifications");
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "scaffold");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "entry_point");
    ms_json_writer_string(&w, "DriverEntry equivalent: MetalSharpAntiCheat::start()");
    ms_json_writer_key(&w, "dispatch");
    ms_json_writer_string(&w, "IRP_MJ_DEVICE_CONTROL → MetalSharpAntiCheat::externalMethod()");
    ms_json_writer_key(&w, "subscriptions");
    ms_json_writer_u64(&w, 4);
    ms_json_writer_key(&w, "mach_services");
    ms_json_writer_u64(&w, 2);
    ms_json_writer_key(&w, "files");
    ms_json_writer_array_begin(&w);
    ms_json_writer_string(&w, "MetalSharpAntiCheat.cpp — IOService subclass + IOUserClient");
    ms_json_writer_string(&w, "MetalSharpAntiCheatInfo.plist — Extension descriptor");
    ms_json_writer_string(&w, "MetalSharpAntiCheat.entitlements — com.apple.developer.endpoint-security.client");
    ms_json_writer_string(&w, "Makefile — meson build for MetalSharp integration");
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(name);
    free(ext);
    ms_json_free(r);
    return out;
}
char* ms_driver_seed_demo(const char* body, size_t len) {
    (void)body;
    (void)len;
    driver* d = calloc(1, sizeof(*d));
    device* dv = calloc(1, sizeof(*dv));
    size_t i;
    unsigned codes[] = {2293760, 2293764, 2293768};
    if (!d || !dv) {
        free(d);
        free(dv);
        return error_json("allocation failed");
    }
    d->id = next_driver++;
    snprintf(d->name, sizeof(d->name), "EasyAntiCheat");
    snprintf(d->ext, sizeof(d->ext), "EndpointSecurity");
    snprintf(d->status, sizeof(d->status), "Loaded");
    d->created = now_ms();
    d->next = g_drivers;
    g_drivers = d;
    dv->id = next_device++;
    dv->driver_id = d->id;
    snprintf(dv->name, sizeof(dv->name), "EasyAntiCheat0");
    snprintf(dv->nt_name, sizeof(dv->nt_name), "\\Device\\EasyAntiCheat0");
    snprintf(dv->dos_name, sizeof(dv->dos_name), "\\??\\EasyAntiCheat0");
    dv->device_type = FILE_DEVICE_UNKNOWN;
    snprintf(dv->iokit, sizeof(dv->iokit), "MetalSharpEasyAntiCheat0_UserClient");
    dv->created = now_ms();
    dv->next = g_devices;
    g_devices = dv;
    d->devices[d->device_count++] = dv->id;
    for (i = 0; i < 3; i++) {
        ioctl_map* m = calloc(1, sizeof(*m));
        m->id = next_ioctl++;
        m->code = codes[i];
        m->device_type = 34;
        m->function = (codes[i] >> 2) & 0xfff;
        m->method = codes[i] & 3;
        m->selector = (unsigned)(i + 1);
        snprintf(m->name, sizeof(m->name), "IOCTL_EAC_%s",
                 i == 0   ? "PROCESS_EVENT"
                 : i == 1 ? "IMAGE_EVENT"
                          : "MEMORY_SCAN");
        m->next = g_ioctls;
        g_ioctls = m;
    }
    ms_json_writer w;
    char* out;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "seeded");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "driver_id");
    ms_json_writer_u64(&w, d->id);
    ms_json_writer_key(&w, "device_id");
    ms_json_writer_u64(&w, dv->id);
    ms_json_writer_key(&w, "ioctls_registered");
    ms_json_writer_u64(&w, 3);
    ms_json_writer_key(&w, "irps_dispatched");
    ms_json_writer_u64(&w, 3);
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "irp_ids");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < 3; i++) {
        irp* q = calloc(1, sizeof(*q));
        q->id = next_irp++;
        q->driver_id = d->id;
        q->device_id = dv->id;
        snprintf(q->major, sizeof(q->major), "DeviceControl");
        snprintf(q->status, sizeof(q->status), "Completed");
        q->next = g_irps;
        g_irps = q;
        ms_json_writer_u64(&w, q->id);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "scenario");
    ms_json_writer_string(&w, "EAC-style anti-cheat: load driver → create device → register IOCTL handlers → open → "
                              "send process event IOCTL → close");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}
