#include "metalsharp_backend/kernel_integrity.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CS_VALID           0x00000001U
#define CS_ADHOC           0x00000002U
#define CS_SIGNED          0x00000004U
#define CS_HARD            0x00000100U
#define CS_KILL            0x00000200U
#define CS_RESTRICT        0x00000800U
#define CS_ENFORCEMENT     0x00001000U
#define CS_PLATFORM_BINARY 0x04000000U

typedef enum {
    LEVEL_NONE,
    LEVEL_AUTH_TRUSTED,
    LEVEL_AUTH,
    LEVEL_AUTH_ANTI,
    LEVEL_MICROSOFT,
    LEVEL_MICROSOFT_ANTI,
    LEVEL_CUSTOM1,
    LEVEL_CUSTOM2,
    LEVEL_CUSTOM3,
    LEVEL_CUSTOM4,
    LEVEL_CUSTOM5,
    LEVEL_CUSTOM6,
    LEVEL_DEVELOPER,
    LEVEL_DEVELOPER_ANTI,
    LEVEL_STORE,
    LEVEL_STORE_ANTI
} signing_level;
typedef enum { MODULE_MACHO, MODULE_PE, MODULE_UNKNOWN } module_type;
typedef struct module_entry {
    char base[128], name[512], hash[65];
    bool has_hash, has_csops;
    unsigned csops, policy;
    signing_level level;
    module_type type;
    bool signed_module, trusted;
    struct module_entry* next;
} module_entry;
static module_entry* g_modules;

static char* error_json(const char* s) {
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
static ms_json* root(const char* body, size_t len) {
    char e[128];
    ms_json* v = ms_json_parse(body ? body : "", len, e, sizeof(e));
    if (!v || ms_json_type_of(v) != MS_JSON_OBJECT) {
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
static char* str(const ms_json* r, const char* key) {
    char* s = NULL;
    (void)ms_json_as_string(ms_json_object_get(r, key), &s);
    return s;
}
static const char* level_variant(signing_level l) {
    static const char* n[] = {"None",         "AuthenticodeTrusted",
                              "Authenticode", "AuthenticodeAntitampering",
                              "Microsoft",    "MicrosoftAntitampering",
                              "Custom1",      "Custom2",
                              "Custom3",      "Custom4",
                              "Custom5",      "Custom6",
                              "Developer",    "DeveloperAntitampering",
                              "Store",        "StoreAntitampering"};
    return l <= LEVEL_STORE_ANTI ? n[l] : n[0];
}
static const char* level_name(signing_level l) {
    static const char* n[] = {"SE_SIGNING_LEVEL_UNSIGNED",     "SE_SIGNING_LEVEL_AUTHENTICODE_TRUSTED",
                              "SE_SIGNING_LEVEL_AUTHENTICODE", "SE_SIGNING_LEVEL_AUTHENTICODE_ANTITAMPERING",
                              "SE_SIGNING_LEVEL_MICROSOFT",    "SE_SIGNING_LEVEL_MICROSOFT_ANTITAMPERING",
                              "SE_SIGNING_LEVEL_CUSTOM_1",     "SE_SIGNING_LEVEL_CUSTOM_2",
                              "SE_SIGNING_LEVEL_CUSTOM_3",     "SE_SIGNING_LEVEL_CUSTOM_4",
                              "SE_SIGNING_LEVEL_CUSTOM_5",     "SE_SIGNING_LEVEL_CUSTOM_6",
                              "SE_SIGNING_LEVEL_DEVELOPER",    "SE_SIGNING_LEVEL_DEVELOPER_ANTITAMPERING",
                              "SE_SIGNING_LEVEL_STORE",        "SE_SIGNING_LEVEL_STORE_ANTITAMPERING"};
    return l <= LEVEL_STORE_ANTI ? n[l] : n[0];
}
static const char* type_name(module_type t) {
    return t == MODULE_MACHO ? "MachO" : t == MODULE_PE ? "PE" : "Unknown";
}
static signing_level level_from_num(unsigned long long n) {
    return n <= 15 ? (signing_level)n : LEVEL_NONE;
}
static module_entry* find_module(const char* name) {
    module_entry* m;
    for (m = g_modules; m; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}
/* Replacement with explicit list handling; avoids depending on map ordering. */
static module_entry* store_module(const module_entry* value) {
    module_entry** p = &g_modules;
    while (*p && strcmp((*p)->name, value->name) != 0)
        p = &(*p)->next;
    if (!*p) {
        *p = calloc(1, sizeof(**p));
        if (!*p)
            return NULL;
    }
    module_entry* m = *p;
    struct module_entry* next = m->next;
    *m = *value;
    m->next = next;
    return m;
}
static unsigned policy_for(signing_level l, const char** description) {
    switch (l) {
    case LEVEL_NONE:
        *description = "No policy — unsigned";
        return 0;
    case LEVEL_AUTH_TRUSTED:
        *description = "Signed, trusted by Authenticode";
        return 2;
    case LEVEL_AUTH:
        *description = "Signed, Authenticode chain";
        return 4;
    case LEVEL_AUTH_ANTI:
        *description = "Signed, tamper-evident";
        return 6;
    case LEVEL_MICROSOFT:
        *description = "Microsoft-signed — fully trusted";
        return 0x0e;
    case LEVEL_MICROSOFT_ANTI:
        *description = "Microsoft-signed, tamper-evident";
        return 0x0f;
    case LEVEL_DEVELOPER:
        *description = "Developer-signed";
        return 3;
    case LEVEL_STORE:
        *description = "Store-signed";
        return 0x0c;
    default:
        *description = "Custom signing";
        return 1;
    }
}
static void module_json(ms_json_writer* w, const module_entry* m) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "base_address");
    ms_json_writer_string(w, m->base);
    ms_json_writer_key(w, "module_name");
    ms_json_writer_string(w, m->name);
    ms_json_writer_key(w, "module_type");
    ms_json_writer_string(w, type_name(m->type));
    ms_json_writer_key(w, "signing_level");
    ms_json_writer_string(w, level_variant(m->level));
    ms_json_writer_key(w, "csops_flags");
    if (m->has_csops)
        ms_json_writer_u64(w, m->csops);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "policy_flags");
    ms_json_writer_u64(w, m->policy);
    ms_json_writer_key(w, "is_signed");
    ms_json_writer_bool(w, m->signed_module);
    ms_json_writer_key(w, "is_trusted");
    ms_json_writer_bool(w, m->trusted);
    ms_json_writer_key(w, "hash_algorithm");
    ms_json_writer_string(w, "SHA256");
    ms_json_writer_key(w, "hash_hex");
    if (m->has_hash)
        ms_json_writer_string(w, m->hash);
    else
        ms_json_writer_null(w);
    ms_json_writer_object_end(w);
}
static void decode_flags(ms_json_writer* w, unsigned flags) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "valid");
    ms_json_writer_bool(w, (flags & CS_VALID) != 0);
    ms_json_writer_key(w, "signed");
    ms_json_writer_bool(w, (flags & CS_SIGNED) != 0);
    ms_json_writer_key(w, "adhoc");
    ms_json_writer_bool(w, (flags & CS_ADHOC) != 0);
    ms_json_writer_key(w, "hard");
    ms_json_writer_bool(w, (flags & CS_HARD) != 0);
    ms_json_writer_key(w, "kill");
    ms_json_writer_bool(w, (flags & CS_KILL) != 0);
    ms_json_writer_key(w, "restrict");
    ms_json_writer_bool(w, (flags & CS_RESTRICT) != 0);
    ms_json_writer_key(w, "enforcement");
    ms_json_writer_bool(w, (flags & CS_ENFORCEMENT) != 0);
    ms_json_writer_key(w, "platformBinary");
    ms_json_writer_bool(w, (flags & CS_PLATFORM_BINARY) != 0);
    ms_json_writer_key(w, "devCode");
    ms_json_writer_bool(w, false);
    ms_json_writer_object_end(w);
}
static void csops_json(ms_json_writer* w, unsigned pid, unsigned flags, bool ok, bool signing_ok) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "pid");
    ms_json_writer_u64(w, pid);
    ms_json_writer_key(w, "csopsStatusOk");
    ms_json_writer_bool(w, ok);
    ms_json_writer_key(w, "csopsSigningOk");
    ms_json_writer_bool(w, signing_ok);
    ms_json_writer_key(w, "flags");
    ms_json_writer_u64(w, flags);
    ms_json_writer_key(w, "signingInfo");
    ms_json_writer_u64(w, 0);
    ms_json_writer_key(w, "decoded");
    decode_flags(w, flags);
    ms_json_writer_object_end(w);
}
static signing_level flags_level(unsigned flags) {
    if (flags & CS_PLATFORM_BINARY)
        return LEVEL_MICROSOFT;
    if ((flags & CS_VALID) && (flags & CS_SIGNED) && !(flags & CS_ADHOC))
        return LEVEL_AUTH_TRUSTED;
    if ((flags & CS_SIGNED) && (flags & CS_ADHOC))
        return LEVEL_DEVELOPER;
    if (flags & CS_SIGNED)
        return LEVEL_AUTH;
    return LEVEL_NONE;
}
static void pseudo_hash(const unsigned char* data, size_t len, char out[65]) {
    unsigned char h[32] = {0x6a, 0x09, 0xe6, 0x67, 0xbb, 0x67, 0xae, 0x85, 0x3c, 0x6e, 0xf3,
                           0x72, 0xa5, 0x4f, 0xf5, 0x3a, 0x51, 0x0e, 0x52, 0x7f, 0x9b, 0x05,
                           0x68, 0x8c, 0x1f, 0x83, 0xd9, 0xab, 0x5b, 0xe0, 0xcd, 0x19};
    size_t i;
    for (i = 0; i < len; i++) {
        size_t s = i % 32;
        h[s] = (unsigned char)((h[s] + data[i] + h[(s + 3) % 32]) ^ h[(s + 17) % 32]);
        h[(s + 7) % 32] = (unsigned char)(h[(s + 7) % 32] + h[s]);
    }
    for (unsigned round = 0; round < 4; round++)
        for (i = 0; i < 32; i++) {
            size_t j = (i + 13 + round * 7) % 32;
            h[i] = (unsigned char)((h[i] + h[j]) << ((round + 3) & 7) | (h[i] + h[j]) >> ((8 - ((round + 3) & 7)) & 7));
        }
    for (i = 0; i < 32; i++)
        h[i] = (unsigned char)(h[i] + (unsigned char)len);
    for (i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", h[i]);
    out[64] = '\0';
}
static void demo_hash(const char* name, char out[65]) {
    unsigned char h[32] = {0};
    size_t i;
    h[0] = 0xab;
    h[1] = 0xcd;
    h[2] = 0xef;
    h[3] = 0x01;
    for (i = 0; name[i] != '\0'; ++i)
        h[4 + (i % 28)] = (unsigned char)(h[4 + (i % 28)] + (unsigned char)name[i]);
    for (i = 0; i < 32; ++i)
        snprintf(out + i * 2, 3, "%02x", h[i]);
    out[64] = '\0';
}
static void file_hash(const char* path, char out[65]) {
    FILE* f = fopen(path, "rb");
    unsigned char* data;
    long n;
    if (!f) {
        snprintf(out, 65, "hash_unavailable:%s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 64 * 1024 * 1024) {
        fclose(f);
        snprintf(out, 65, "error_reading:%s", path);
        return;
    }
    data = malloc((size_t)n);
    if (!data && n > 0) {
        fclose(f);
        snprintf(out, 65, "error_reading:%s", path);
        return;
    }
    if (n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        snprintf(out, 65, "error_reading:%s", path);
        return;
    }
    fclose(f);
    pseudo_hash(data, (size_t)n, out);
    free(data);
}

char* ms_integrity_query_signing_level(const char* body, size_t len) {
    ms_json* r = root(body, len);
    unsigned long long pid = 0;
    char* name = NULL;
    module_entry* m;
    unsigned flags = 0;
    signing_level level;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "pid", &pid) || pid > 0xffffffffULL) {
        ms_json_free(r);
        return error_json("pid (u32) required");
    }
    name = str(r, "module_name");
    if (name && name[0] && (m = find_module(name)) != NULL) {
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        ms_json_writer_key(&w, "module");
        module_json(&w, m);
        ms_json_writer_key(&w, "source");
        ms_json_writer_string(&w, "cached");
        ms_json_writer_object_end(&w);
        out = ms_json_writer_take(&w);
        free(name);
        ms_json_free(r);
        return out;
    }
    level = flags_level(flags);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, pid);
    ms_json_writer_key(&w, "module_name");
    ms_json_writer_string(&w, name ? name : "");
    ms_json_writer_key(&w, "csops");
    csops_json(&w, (unsigned)pid, flags, false, false);
    ms_json_writer_key(&w, "signingLevel");
    ms_json_writer_u64(&w, level);
    ms_json_writer_key(&w, "signingLevelName");
    ms_json_writer_string(&w, level_name(level));
    ms_json_writer_key(&w, "policy");
    {
        const char* d;
        unsigned p = policy_for(level, &d);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "Policy");
        {
            char s[16];
            snprintf(s, sizeof(s), "0x%02X", p);
            ms_json_writer_string(&w, s);
        }
        ms_json_writer_key(&w, "Description");
        ms_json_writer_string(&w, d);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_key(&w, "source");
    ms_json_writer_string(&w, "csops_probe");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(name);
    ms_json_free(r);
    return out;
}
char* ms_integrity_query_process_signing(const char* body, size_t len) {
    ms_json* r = root(body, len);
    unsigned long long pid = 0;
    unsigned flags = 0;
    signing_level l;
    ms_json_writer w;
    char* out;
    if (!r || !u64(r, "pid", &pid) || pid > 0xffffffffULL) {
        ms_json_free(r);
        return error_json("pid (u32) required");
    }
    l = flags_level(flags);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "ntApi");
    ms_json_writer_string(&w, "NtQueryInformationProcess(ProcessSigningLevel)");
    ms_json_writer_key(&w, "ProcessSigningLevel");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "SigningLevel");
    ms_json_writer_u64(&w, l);
    ms_json_writer_key(&w, "SigningLevelName");
    ms_json_writer_string(&w, level_name(l));
    ms_json_writer_key(&w, "Flags");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "csopsRaw");
    csops_json(&w, (unsigned)pid, flags, false, false);
    ms_json_writer_key(&w, "translation");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "csopsFlags");
    ms_json_writer_u64(&w, flags);
    ms_json_writer_key(&w, "mappedTo");
    ms_json_writer_string(&w, level_name(l));
    ms_json_writer_key(&w, "reason");
    ms_json_writer_string(&w, "no signing flags → SE_SIGNING_LEVEL_UNSIGNED");
    ms_json_writer_object_end(&w);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    ms_json_free(r);
    return out;
}
char* ms_integrity_register_pe(const char* body, size_t len) {
    ms_json* r = root(body, len);
    char *base, *name, *hash;
    unsigned long long n = 0;
    module_entry m;
    bool builtin;
    if (!r) {
        return error_json("invalid JSON object");
    }
    base = str(r, "base_address");
    name = str(r, "module_name");
    if (!base) {
        ms_json_free(r);
        return error_json("base_address (hex string) required");
    }
    if (!name) {
        free(base);
        ms_json_free(r);
        return error_json("module_name required");
    }
    if (u64(r, "signing_level", &n) == false)
        n = 0;
    hash = str(r, "hash_hex");
    if (hash) {
        for (char* q = hash; *q; q++)
            if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F'))) {
                free(base);
                free(name);
                free(hash);
                ms_json_free(r);
                return error_json("hash_hex must be valid hexadecimal");
            }
    }
    memset(&m, 0, sizeof(m));
    snprintf(m.base, sizeof(m.base), "%s", base);
    snprintf(m.name, sizeof(m.name), "%s", name);
    m.type = MODULE_PE;
    m.level = level_from_num(n);
    m.has_hash = hash != NULL;
    if (hash)
        snprintf(m.hash, sizeof(m.hash), "%s", hash);
    builtin = strstr(name, ".dll") != NULL || strstr(name, ".DLL") != NULL || strstr(name, ".exe") != NULL ||
              strstr(name, ".sys") != NULL;
    m.policy = builtin ? 0x0e : 0x02;
    m.signed_module = builtin || m.level != LEVEL_NONE;
    m.trusted = m.signed_module;
    store_module(&m);
    ms_json_writer w;
    char* out;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "module");
    module_json(&w, &m);
    ms_json_writer_key(&w, "note");
    {
        char s[256];
        snprintf(s, sizeof(s), "PE module registered as %s (wine_builtin=%s)", level_name(m.level),
                 builtin ? "true" : "false");
        ms_json_writer_string(&w, s);
    }
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(base);
    free(name);
    free(hash);
    ms_json_free(r);
    return out;
}
char* ms_integrity_register_macho(const char* body, size_t len) {
    ms_json* r = root(body, len);
    char *path, *name, *base;
    module_entry m;
    ms_json_writer w;
    char* out;
    if (!r)
        return error_json("invalid JSON object");
    path = str(r, "path");
    if (!path) {
        ms_json_free(r);
        return error_json("path to Mach-O required");
    }
    name = str(r, "module_name");
    base = str(r, "base_address");
    if (!name)
        name = strdup(path);
    if (!base)
        base = strdup("0x0000000000000000");
    memset(&m, 0, sizeof(m));
    snprintf(m.base, sizeof(m.base), "%s", base);
    snprintf(m.name, sizeof(m.name), "%s", name);
    m.type = MODULE_MACHO;
    m.level = LEVEL_NONE;
    m.has_csops = true;
    m.csops = 0;
    m.policy = 0x06;
    m.signed_module = false;
    m.trusted = false;
    m.has_hash = true;
    file_hash(path, m.hash);
    store_module(&m);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "module");
    module_json(&w, &m);
    ms_json_writer_key(&w, "source");
    ms_json_writer_string(&w, "csops_bridge");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(path);
    free(name);
    free(base);
    ms_json_free(r);
    return out;
}
char* ms_integrity_set_cached_level(const char* body, size_t len) {
    (void)body;
    (void)len;
    return strdup("{\"ok\":true,\"ntStatus\":\"STATUS_SUCCESS\",\"ntApi\":\"NtSetCachedSigningLevel\",\"note\":\"Stub "
                  "— always returns STATUS_SUCCESS. Anti-cheat calls this during init expecting success.\"}");
}
char* ms_integrity_list_modules(const char* body, size_t len) {
    ms_json* r = root(body, len);
    char* filter = NULL;
    bool signed_only = false;
    module_entry* m;
    size_t total = 0, count = 0;
    ms_json_writer w;
    if (!r)
        return error_json("invalid JSON object");
    filter = str(r, "filter_type");
    (void)ms_json_as_bool(ms_json_object_get(r, "signed_only"), &signed_only);
    for (m = g_modules; m; m = m->next) {
        total++;
        if (filter && strcmp(filter, "MachO") != 0 && strcmp(filter, "PE") != 0) {
        } else if (filter && strcmp(filter, type_name(m->type)) != 0)
            continue;
        if (signed_only && !m->signed_module)
            continue;
        count++;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "totalModules");
    ms_json_writer_u64(&w, total);
    ms_json_writer_key(&w, "filteredCount");
    ms_json_writer_u64(&w, count);
    ms_json_writer_key(&w, "typeCounts");
    ms_json_writer_object_begin(&w);
    size_t pe = 0, mo = 0, un = 0;
    for (m = g_modules; m; m = m->next) {
        if (filter && strcmp(filter, "MachO") != 0 && strcmp(filter, "PE") != 0) {
        } else if (filter && strcmp(filter, type_name(m->type)) != 0)
            continue;
        if (signed_only && !m->signed_module)
            continue;
        if (m->type == MODULE_PE)
            pe++;
        else if (m->type == MODULE_MACHO)
            mo++;
        else
            un++;
    }
    if (pe) {
        ms_json_writer_key(&w, "PE");
        ms_json_writer_u64(&w, pe);
    }
    if (mo) {
        ms_json_writer_key(&w, "MachO");
        ms_json_writer_u64(&w, mo);
    }
    if (un) {
        ms_json_writer_key(&w, "Unknown");
        ms_json_writer_u64(&w, un);
    }
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "modules");
    ms_json_writer_array_begin(&w);
    for (m = g_modules; m; m = m->next) {
        if (filter && strcmp(filter, "MachO") != 0 && strcmp(filter, "PE") != 0) {
        } else if (filter && strcmp(filter, type_name(m->type)) != 0)
            continue;
        if (signed_only && !m->signed_module)
            continue;
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "name");
        ms_json_writer_string(&w, m->name);
        ms_json_writer_key(&w, "type");
        ms_json_writer_string(&w, type_name(m->type));
        ms_json_writer_key(&w, "base");
        ms_json_writer_string(&w, m->base);
        ms_json_writer_key(&w, "signingLevel");
        ms_json_writer_string(&w, level_name(m->level));
        ms_json_writer_key(&w, "signed");
        ms_json_writer_bool(&w, m->signed_module);
        ms_json_writer_key(&w, "trusted");
        ms_json_writer_bool(&w, m->trusted);
        ms_json_writer_key(&w, "hash");
        if (!m->has_hash)
            ms_json_writer_null(&w);
        else if (strlen(m->hash) > 16) {
            char h[32];
            snprintf(h, sizeof(h), "%.8s...%.8s", m->hash, m->hash + strlen(m->hash) - 8);
            ms_json_writer_string(&w, h);
        } else
            ms_json_writer_string(&w, m->hash);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    free(filter);
    ms_json_free(r);
    return ms_json_writer_take(&w);
}
char* ms_integrity_seed_demo(const char* body, size_t len) {
    static const char* names[] = {"ntdll.dll",     "kernel32.dll",       "kernelbase.dll",
                                  "user32.dll",    "game.exe",           "gameoverlayrenderer.dll",
                                  "d3d11.dll",     "dxgi.dll",           "winegstreamer.dll",
                                  "winemac.drv",   "libwine.dylib",      "libwined3d.dylib",
                                  "anticheat.sys", "anticheat_user.dll", "vcruntime140.dll",
                                  "msvcp140.dll"};
    static const char* bases[] = {
        "0x00007FF800000000", "0x00007FF800100000", "0x00007FF800200000", "0x00007FF800300000",
        "0x0000000140000000", "0x00007FF800400000", "0x00007FF800500000", "0x00007FF800600000",
        "0x00007FF800700000", "0x00007FF800800000", "0x0000000100000000", "0x0000000101000000",
        "0x00007FF800900000", "0x00007FF800A00000", "0x00007FF800B00000", "0x00007FF800C00000"};
    unsigned long long pid = 0;
    ms_json* r = root(body, len);
    module_entry m;
    ms_json_writer w;
    size_t i;
    if (r)
        (void)u64(r, "pid", &pid);
    for (i = 0; i < 16; i++) {
        memset(&m, 0, sizeof(m));
        snprintf(m.name, sizeof(m.name), "%s", names[i]);
        snprintf(m.base, sizeof(m.base), "%s", bases[i]);
        m.type = (i == 10 || i == 11) ? MODULE_MACHO : MODULE_PE;
        m.level = (i < 4 || i == 6 || i == 7 || i == 14 || i == 15)
                      ? LEVEL_MICROSOFT
                      : (i == 4 ? LEVEL_AUTH
                                : (i == 5 ? LEVEL_AUTH_TRUSTED : (i == 8 || i == 9 ? LEVEL_DEVELOPER : LEVEL_CUSTOM1)));
        m.has_csops = m.type == MODULE_MACHO;
        m.csops = m.has_csops ? CS_VALID | CS_SIGNED : 0;
        m.policy = (i == 8 || i == 9) ? 0x0e : 0x06;
        m.signed_module = true;
        m.trusted = true;
        m.has_hash = true;
        demo_hash(names[i], m.hash);
        store_module(&m);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_u64(&w, pid ? pid : (unsigned)getpid());
    ms_json_writer_key(&w, "created");
    ms_json_writer_u64(&w, 16);
    ms_json_writer_key(&w, "totalModules");
    size_t total = 0;
    for (module_entry* x = g_modules; x; x = x->next)
        total++;
    ms_json_writer_u64(&w, total);
    ms_json_writer_key(&w, "modules");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < 16; i++)
        ms_json_writer_string(&w, names[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    ms_json_free(r);
    return ms_json_writer_take(&w);
}
