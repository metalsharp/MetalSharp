#include "metalsharp_backend/migration.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/setup.h"
#include "metalsharp_backend/steam_actions.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MIGRATION_VERSION "0.61.0"
#define MIGRATION_SCHEMA  5

static const char* const migration_payload_denies[] = {"steamapps",
                                                       "common",
                                                       "downloading",
                                                       "shadercache",
                                                       "compatdata",
                                                       "prefix",
                                                       "prefix-steam",
                                                       "drive_c",
                                                       "dosdevices",
                                                       "Program Files",
                                                       "Program Files (x86)",
                                                       "Steam",
                                                       "runtime",
                                                       "downloads",
                                                       "updates",
                                                       "updater-tools",
                                                       "tmp",
                                                       "Temp",
                                                       "cache",
                                                       "logs",
                                                       "crashes"};
static const char* const migration_setting_names[] = {
    "setup.json",    "steam_config.json",   "bottle.json",      "library.json",   "apps.json",
    "routes.json",   "settings.json",       "preferences.json", "user.reg",       "userdef.reg",
    "system.reg",    "libraryfolders.vdf",  "config.vdf",       "loginusers.vdf", "localconfig.vdf",
    "shortcuts.vdf", "steam_autocloud.vdf", "location.txt"};
static const char* const migration_setting_exts[] = {"json", "toml", "plist", "vdf", "reg", "ini", "cfg", "conf"};
static const char* const migration_steam_deny_names[] = {
    "cache", "common", "compatdata", "crashes", "depotcache", "downloading", "logs", "shadercache", "Temp", "tmp"};
static const char* const migration_steam_exts[] = {"acf",      "cfg",   "conf", "dll",  "ini", "json",
                                                   "manifest", "plist", "reg",  "toml", "vdf"};

typedef struct migration_link {
    char* name;
    char* target;
    struct migration_link* next;
} migration_link;

typedef struct {
    char* temp;
    char* setup;
    char* steam_config;
    migration_link* steam_links;
    migration_link* gptk_links;
} preserved_data;

static const char* const migration_m12_hashes[][2] = {
    {"x86_64-windows/d3d10core.dll", "11c9610770cb0e3f6476d2bde2a3b1afa36a41bd00a2fffc6ea61d2e62c6258d"},
    {"x86_64-windows/d3d11.dll", "98ba9581e10414db0273bf1345b5087ee28de0859fcadfb4a6da09579c2020e9"},
    {"x86_64-windows/d3d12.dll", "cce26811c2ff0ab771a15d90e6c927b9e22567c2311b433de143ad3e4d07dd4f"},
    {"x86_64-windows/dxgi.dll", "628998e1ee632eb7a2d601e4bbeb1e28c05f96193ab5fcd349b1f49faaf6131e"},
    {"x86_64-windows/dxgi_dxmt.dll", "6b7ff46182cd1f0be44227f87fe24e7185de43a028ceb189ac3f2190767f8226"},
    {"x86_64-windows/winemetal.dll", "f6844535ce448e6c525884c8c630298895d7cad97c64eade0f85208a804b9003"},
    {"x86_64-windows/nvapi64.dll", "2eeb618e67c0c2a8d8ff0d84bf45cf69828118c15e894881126e2b94e40d1f83"},
    {"x86_64-windows/nvngx.dll", "cc268b8d89eecef4312a010d25cf77d169c1c68c0875ac1b224d2bc118b921e3"},
    {"x86_64-unix/winemetal.so", "2a635d713446f26eb275eede947574d9c03f3165a74f7828d3f48fadd9ffd519"},
    {"x86_64-unix/libc++.1.dylib", "9bfcf5310f95ebaeddaa55482debbb115a5cb109244dece727a314933dcbcc15"},
    {"x86_64-unix/libc++abi.1.dylib", "b819a65788f8f4e8bc1e67a601e8e3d59c52c14a74910e61f2e7307006340fb4"},
    {"x86_64-unix/libunwind.1.dylib", "105e72335d9e919e32028d151934b97d4b75267528023cb7f22111ac8065de0e"}};
static const char* const migration_vkd3d_hashes[][2] = {
    {"x86_64-windows/d3d12.dll", "ac2b8674798bdbdd21ce1aa48daf1e2657813ecc878b80e2641bf0d2c3f2a43e"},
    {"x86_64-windows/d3d12core.dll", "78ab917a20dbc050ba3d0def8c0241e53c90ded0a036462955108e0ef78022a8"},
    {"x86_64-windows/dxgi.dll", "943dc921530aeba8bc5add09f5a3c5fac7da50e90a84ca2f41f1b87ba532846e"}};
static const char* const migration_dxvk_hashes[][2] = {
    {"x86_64-windows/d3d9.dll", "67f8b1f139c7b4838de535876668c44716cec5dda56a1aa88bab5b820acd72fc"},
    {"x86_64-windows/d3d10core.dll", "d8616fc3c1e13b32562325202655d4ecba972b4043bdf8f0b7350d627b842c26"},
    {"x86_64-windows/d3d11.dll", "e7cf78bdc3722b40f19919ada77cfb535bdb3708934eb6d4c13111f5454b8c74"},
    {"x86_64-windows/dxgi.dll", "1568105bcbbb0a98e6f12f386725e8186483c985a3c95cfe1484cfef125ae63c"},
    {"i386-windows/d3d9.dll", "3bbe4b5aa1445380223ab5ce98f9ea5ad91ab3599e3354b4e91943a017474dbd"},
    {"i386-windows/d3d10core.dll", "a7010f0a1b4eaa54b892c79fbdc01c83b6030770acd6045962fff05c142dfbeb"},
    {"i386-windows/d3d11.dll", "04a6393bff8da791eccc81f6e54012e148ec9f960d465405bf5e0c76f024f063"},
    {"i386-windows/dxgi.dll", "ce7d7235562b534474098e77e4d26742b91807e766693a44dfdd5e50385199df"}};

static char* path_join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (p)
        snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
}
static char* read_text(const char* p) {
    FILE* f = fopen(p, "rb");
    long n;
    char* s;
    size_t got;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || n > 8 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
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
    s[got] = '\0';
    return s;
}
static bool exists(const char* p) {
    struct stat st;
    return p && stat(p, &st) == 0;
}
static char* raw_or(const char* p, const char* fallback) {
    char* s = read_text(p);
    return s ? s : strdup(fallback);
}
static bool json_bool(const ms_json* r, const char* key, bool fallback) {
    bool b;
    return ms_json_as_bool(ms_json_object_get(r, key), &b) ? b : fallback;
}
static unsigned long long json_u64(const ms_json* r, const char* key, unsigned long long fallback) {
    long long n;
    return ms_json_as_i64(ms_json_object_get(r, key), &n) && n >= 0 ? (unsigned long long)n : fallback;
}
static char* json_str(const ms_json* r, const char* key) {
    char* s = NULL;
    (void)ms_json_as_string(ms_json_object_get(r, key), &s);
    return s;
}

static bool file_nonempty_local(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool sha256_matches_local(const char* path, const char* expected) {
    FILE* file = fopen(path, "rb");
    CC_SHA256_CTX context;
    unsigned char buffer[8192], digest[CC_SHA256_DIGEST_LENGTH];
    char actual[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    size_t count;
    if (!file || CC_SHA256_Init(&context) != 1) {
        if (file)
            fclose(file);
        return false;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        CC_SHA256_Update(&context, buffer, (CC_LONG)count);
    if (ferror(file) || CC_SHA256_Final(digest, &context) != 1) {
        fclose(file);
        return false;
    }
    fclose(file);
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    actual[sizeof(actual) - 1] = '\0';
    return !strcmp(actual, expected);
}

static bool hash_set_current(const char* root, const char* const hashes[][2], size_t count) {
    bool current = true;
    for (size_t i = 0; i < count; i++) {
        char* path = path_join(root, hashes[i][0]);
        current = current && path && sha256_matches_local(path, hashes[i][1]);
        free(path);
    }
    return current;
}

static bool directory_local(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool migration_manifest_current(const char* path) {
    char* text = read_text(path);
    char expected[128];
    bool current;
    snprintf(expected, sizeof(expected), "\"version\":\"%s-m12-isolated-surface-v1\"", MIGRATION_VERSION);
    current = text && strstr(text, expected) != NULL;
    free(text);
    return current;
}

static bool migration_moltenvk_current(const char* home) {
    const char* library_hash = "8249d81ebf2d46f82b16ca166c2e5cca5d76d91d0a412cd6d3db1aaa6e8430bf";
    const char* lane_hash = "578ff08cd0d8734619357541771a5abc9c3470ca300030219a971a9e9dbbe466";
    const char* relative[] = {
        "runtime/wine/lib/wine/x86_64-unix/libMoltenVK.dylib", "runtime/wine/lib/wine/x86_64-unix/libMoltenVK.1.dylib",
        "runtime/wine/lib/moltenvk-vkmt/libMoltenVK.dylib", "runtime/wine/lib/moltenvk-vkmt/libMoltenVK.1.dylib"};
    char *lane_icd = path_join(home, "runtime/wine/lib/moltenvk-vkmt/MoltenVK_icd.json"),
         *runtime_icd = path_join(home, "runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json"), *library = NULL,
         *text = NULL;
    bool current = true;
    for (size_t i = 0; i < sizeof(relative) / sizeof(relative[0]); i++) {
        char* path = path_join(home, relative[i]);
        current = current && path && sha256_matches_local(path, library_hash);
        free(path);
    }
    current = current && lane_icd && sha256_matches_local(lane_icd, lane_hash) && runtime_icd;
    library = path_join(home, "runtime/wine/lib/wine/x86_64-unix/libMoltenVK.dylib");
    text = runtime_icd ? read_text(runtime_icd) : NULL;
    if (current && text) {
        char error[128];
        ms_json* value = ms_json_parse(text, strlen(text), error, sizeof(error));
        char *format = NULL, *api = NULL, *path = NULL;
        bool portability = false;
        const ms_json* icd = value ? ms_json_object_get(value, "ICD") : NULL;
        current =
            value && ms_json_type_of(value) == MS_JSON_OBJECT && icd &&
            ms_json_as_string(ms_json_object_get(value, "file_format_version"), &format) && !strcmp(format, "1.0.0") &&
            ms_json_as_string(ms_json_object_get(icd, "api_version"), &api) && !strcmp(api, "1.4.0") &&
            ms_json_as_bool(ms_json_object_get(icd, "is_portability_driver"), &portability) && portability &&
            ms_json_as_string(ms_json_object_get(icd, "library_path"), &path) && library && !strcmp(path, library);
        free(format);
        free(api);
        free(path);
        ms_json_free(value);
    } else {
        current = false;
    }
    free(lane_icd);
    free(runtime_icd);
    free(library);
    free(text);
    return current;
}

static bool copy_file_local(const char* source, const char* destination) {
    FILE *in = NULL, *out = NULL;
    char buffer[1024 * 64];
    size_t n;
    if (!source || !destination)
        return false;
    in = fopen(source, "rb");
    if (!in)
        return false;
    out = fopen(destination, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0 && fwrite(buffer, 1, n, out) == n) {
    }
    if (ferror(in) || ferror(out)) {
        fclose(in);
        fclose(out);
        unlink(destination);
        return false;
    }
    fclose(in);
    fclose(out);
    return true;
}

static bool name_in_list(const char* name, const char* const* list, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!strcasecmp(name, list[i]))
            return true;
    return false;
}

static bool extension_in_list(const char* path, const char* const* list, size_t count) {
    const char* dot = strrchr(path, '.');
    if (!dot || !dot[1])
        return false;
    for (size_t i = 0; i < count; i++)
        if (!strcasecmp(dot + 1, list[i]))
            return true;
    return false;
}

static bool allowed_setting_file(const char* path, bool steam_metadata) {
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (name_in_list(name, migration_payload_denies,
                     sizeof(migration_payload_denies) / sizeof(migration_payload_denies[0])))
        return false;
    if (name_in_list(name, migration_setting_names,
                     sizeof(migration_setting_names) / sizeof(migration_setting_names[0])))
        return true;
    return extension_in_list(path, steam_metadata ? migration_steam_exts : migration_setting_exts,
                             steam_metadata ? sizeof(migration_steam_exts) / sizeof(migration_steam_exts[0])
                                            : sizeof(migration_setting_exts) / sizeof(migration_setting_exts[0]));
}

static void normalize_steam_config(char** data) {
    char error[128];
    ms_json* value;
    char* legacy = NULL;
    char* runtime_key = NULL;
    bool has_runtime_key = false;
    ms_json_writer writer;
    char* normalized;
    if (!data || !*data)
        return;
    value = ms_json_parse(*data, strlen(*data), error, sizeof(error));
    if (!value || ms_json_type_of(value) != MS_JSON_OBJECT) {
        ms_json_free(value);
        return;
    }
    if (ms_json_as_string(ms_json_object_get(value, "steam_api_key"), &runtime_key) && runtime_key && runtime_key[0]) {
        has_runtime_key = true;
        legacy = strdup(runtime_key);
    } else {
        free(runtime_key);
        runtime_key = NULL;
        (void)ms_json_as_string(ms_json_object_get(value, "api_key"), &legacy);
    }
    if (!legacy || !legacy[0]) {
        free(legacy);
        free(runtime_key);
        ms_json_free(value);
        return;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < ms_json_object_length(value); i++) {
        const char* key = ms_json_object_key_at(value, i);
        char* raw = ms_json_stringify(ms_json_object_value_at(value, i));
        if (!key || !raw || (!has_runtime_key && !strcmp(key, "steam_api_key"))) {
            free(raw);
            continue;
        }
        ms_json_writer_key(&writer, key);
        ms_json_writer_raw(&writer, raw);
        free(raw);
    }
    if (!has_runtime_key) {
        ms_json_writer_key(&writer, "steam_api_key");
        ms_json_writer_string(&writer, legacy);
    }
    ms_json_writer_object_end(&writer);
    normalized = ms_json_writer_take(&writer);
    if (normalized) {
        free(*data);
        *data = normalized;
    }
    free(legacy);
    free(runtime_key);
    ms_json_free(value);
}

static bool steam_config_has_api_key(const char* data) {
    char error[128];
    ms_json* value;
    char* key = NULL;
    bool result = false;
    if (!data)
        return false;
    value = ms_json_parse(data, strlen(data), error, sizeof(error));
    if (value && ms_json_type_of(value) == MS_JSON_OBJECT &&
        ms_json_as_string(ms_json_object_get(value, "steam_api_key"), &key))
        result = key && key[0];
    free(key);
    ms_json_free(value);
    return result;
}

/* mode 0 is a complete non-symlink copy, mode 1 is a settings-only
 * filter, and mode 2 is the broader Steam metadata filter. */
static bool copy_tree_filtered(const char* source, const char* destination, int mode) {
    DIR* dir;
    struct dirent* entry;
    if (!directory_local(source))
        return true;
    if (mkdir(destination, 0700) != 0 && errno != EEXIST)
        return false;
    dir = opendir(source);
    if (!dir)
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char *src = NULL, *dst = NULL;
        struct stat st;
        bool keep = true;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        src = path_join(source, entry->d_name);
        dst = path_join(destination, entry->d_name);
        if (!src || !dst || lstat(src, &st) != 0) {
            free(src);
            free(dst);
            closedir(dir);
            return false;
        }
        if (S_ISLNK(st.st_mode)) {
            free(src);
            free(dst);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (mode == 1 && name_in_list(entry->d_name, migration_payload_denies,
                                          sizeof(migration_payload_denies) / sizeof(migration_payload_denies[0])))
                keep = false;
            if (mode == 3 &&
                name_in_list(entry->d_name, (const char* const[]){"downloads", "updates", "updater-tools", "tmp"}, 4))
                keep = false;
            if (mode == 2 && name_in_list(entry->d_name, migration_steam_deny_names,
                                          sizeof(migration_steam_deny_names) / sizeof(migration_steam_deny_names[0])))
                keep = false;
            if (keep && !copy_tree_filtered(src, dst, mode)) {
                free(src);
                free(dst);
                closedir(dir);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            keep = mode == 0 || mode == 3 || allowed_setting_file(src, mode == 2);
            if (keep && !copy_file_local(src, dst)) {
                free(src);
                free(dst);
                closedir(dir);
                return false;
            }
        }
        free(src);
        free(dst);
    }
    closedir(dir);
    return true;
}

static void remove_tree_local(const char* path) {
    DIR* dir;
    struct dirent* entry;
    struct stat st;
    if (!path || lstat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    dir = opendir(path);
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = path_join(path, entry->d_name);
        if (child) {
            remove_tree_local(child);
            free(child);
        }
    }
    closedir(dir);
    rmdir(path);
}

static migration_link* collect_links(const char* prefix) {
    char* dosdevices = path_join(prefix, "dosdevices");
    DIR* dir;
    struct dirent* entry;
    migration_link *head = NULL, *tail = NULL;
    if (!dosdevices || !directory_local(dosdevices)) {
        free(dosdevices);
        return NULL;
    }
    dir = opendir(dosdevices);
    if (!dir) {
        free(dosdevices);
        return NULL;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *link_path, target[PATH_MAX];
        ssize_t n;
        migration_link* item;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !strcmp(entry->d_name, "c:"))
            continue;
        link_path = path_join(dosdevices, entry->d_name);
        n = link_path ? readlink(link_path, target, sizeof(target) - 1) : -1;
        free(link_path);
        if (n <= 0)
            continue;
        target[n] = '\0';
        if (!strcmp(target, "/") || strstr(target, "MetalSharp-arm64") || strstr(target, "MetalSharp-x86_64") ||
            strstr(target, ".dmg") || strstr(target, "MetalSharp-intel"))
            continue;
        item = calloc(1, sizeof(*item));
        if (!item)
            continue;
        item->name = strdup(entry->d_name);
        item->target = strdup(target);
        if (!item->name || !item->target) {
            free(item->name);
            free(item->target);
            free(item);
            continue;
        }
        if (tail)
            tail->next = item;
        else
            head = item;
        tail = item;
    }
    closedir(dir);
    free(dosdevices);
    return head;
}

static void free_links(migration_link* links) {
    while (links) {
        migration_link* next = links->next;
        free(links->name);
        free(links->target);
        free(links);
        links = next;
    }
}

static void restore_links(const char* prefix, migration_link* links) {
    char* dosdevices = path_join(prefix, "dosdevices");
    if (!dosdevices)
        return;
    (void)mkdir(dosdevices, 0700);
    for (migration_link* link = links; link; link = link->next) {
        char* path = path_join(dosdevices, link->name);
        struct stat st;
        if (!path)
            continue;
        if (lstat(path, &st) == 0)
            unlink(path);
        (void)symlink(link->target, path);
        free(path);
    }
    free(dosdevices);
}
static bool runtime_ready(const char* home) {
    const char* required[] = {"runtime/wine/bin/metalsharp-wine",
                              "runtime/host/manifest.json",
                              "runtime/host/HostRuntimeABI.h",
                              "runtime/wine/lib/wine/x86_64-windows/d3d9.dll",
                              "runtime/wine/lib/wine/x86_64-windows/d3d10.dll",
                              "runtime/wine/lib/wine/x86_64-windows/d3d10_1.dll",
                              "runtime/goldberg/x86/steam_api.dll",
                              "runtime/goldberg/x64/steam_api64.dll",
                              "configs/mtsp-rules.toml",
                              "runtime/wine/etc/dxmt.conf",
                              "runtime/wine/lib/dxmt/metalsharp-dxmt-runtime.json",
                              "runtime/wine/lib/dxmt_m12/metalsharp-dxmt-runtime.json",
                              "runtime/wine/lib/moltenvk-vkmt/libMoltenVK.dylib",
                              "runtime/wine/lib/moltenvk-vkmt/MoltenVK_icd.json"};
    bool ok = true;
    char* host_lib = NULL;
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        char* path = path_join(home, required[i]);
        ok = ok && path && (i == 0 ? access(path, X_OK) == 0 : file_nonempty_local(path));
        free(path);
    }
    host_lib = path_join(home, "runtime/host/libmetalsharp_host_runtime.dylib");
    if (!file_nonempty_local(host_lib)) {
        free(host_lib);
        host_lib = path_join(home, "runtime/host/libmetalsharp_host_runtime.so");
    }
    if (!file_nonempty_local(host_lib)) {
        free(host_lib);
        host_lib = path_join(home, "runtime/host/metalsharp_host_runtime.dll");
    }
    ok = ok && file_nonempty_local(host_lib);
    free(host_lib);
    {
        char *unix_dir = path_join(home, "runtime/wine/lib/wine/x86_64-unix"),
             *dxmt_manifest = path_join(home, "runtime/wine/lib/dxmt/metalsharp-dxmt-runtime.json"),
             *m12_manifest = path_join(home, "runtime/wine/lib/dxmt_m12/metalsharp-dxmt-runtime.json");
        ok = ok && directory_local(unix_dir) && migration_manifest_current(dxmt_manifest) &&
             migration_manifest_current(m12_manifest);
        {
            char *m12_root = path_join(home, "runtime/wine/lib/dxmt_m12"), *dxvk_root = path_join(home, "vkd3d/dxvk"),
                 *vkd3d_root = path_join(home, "vkd3d/vkd3d-proton");
            ok = ok && m12_root && dxvk_root && vkd3d_root &&
                 hash_set_current(m12_root, migration_m12_hashes,
                                  sizeof(migration_m12_hashes) / sizeof(migration_m12_hashes[0])) &&
                 hash_set_current(dxvk_root, migration_dxvk_hashes,
                                  sizeof(migration_dxvk_hashes) / sizeof(migration_dxvk_hashes[0])) &&
                 hash_set_current(vkd3d_root, migration_vkd3d_hashes,
                                  sizeof(migration_vkd3d_hashes) / sizeof(migration_vkd3d_hashes[0])) &&
                 migration_moltenvk_current(home);
            free(m12_root);
            free(dxvk_root);
            free(vkd3d_root);
        }
        free(unix_dir);
        free(dxmt_manifest);
        free(m12_manifest);
    }
    return ok;
}

static int compare_versions(const char* left, const char* right) {
    const char *a = left, *b = right;
    while (*a || *b) {
        unsigned long av = 0, bv = 0;
        while (*a && *a != '.' && *a != '-' && *a != '+') {
            if (*a >= '0' && *a <= '9')
                av = av * 10 + (unsigned long)(*a - '0');
            else
                break;
            a++;
        }
        while (*b && *b != '.' && *b != '-' && *b != '+') {
            if (*b >= '0' && *b <= '9')
                bv = bv * 10 + (unsigned long)(*b - '0');
            else
                break;
            b++;
        }
        if (av != bv)
            return av < bv ? -1 : 1;
        while (*a && *a != '.')
            a++;
        while (*b && *b != '.')
            b++;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
        if ((!*a || *a == '-' || *a == '+') && (!*b || *b == '-' || *b == '+'))
            break;
    }
    return 0;
}

static bool preserve_user_data(const char* home, preserved_data* out) {
    char template_path[PATH_MAX];
    char *cache = NULL, *prefix = NULL, *gptk = NULL, *games = NULL, *library = NULL, *bottles = NULL,
         *sharp_prefix = NULL, *epic = NULL, *launcher_games = NULL, *source = NULL, *destination = NULL;
    DIR* dir = NULL;
    struct dirent* entry;
    memset(out, 0, sizeof(*out));
    snprintf(template_path, sizeof(template_path), "/tmp/metalsharp-migration-preserve-%ld-%ld", (long)getpid(),
             (long)time(NULL));
    if (mkdir(template_path, 0700) != 0)
        return false;
    out->temp = strdup(template_path);
    if (!out->temp)
        return false;

    source = path_join(home, "setup.json");
    out->setup = source ? read_text(source) : NULL;
    free(source);
    cache = path_join(home, "cache");
    source = cache ? path_join(cache, "steam_config.json") : NULL;
    if (source)
        out->steam_config = read_text(source);
    if (!out->steam_config) {
        free(source);
        source = path_join(home, ".migration-steam_config.json");
        if (source)
            out->steam_config = read_text(source);
    }
    free(source);
    normalize_steam_config(&out->steam_config);

    prefix = path_join(home, "prefix-steam");
    gptk = path_join(home, "prefix-gptk");
    games = path_join(home, "games");
    library = path_join(home, "sharp-library");
    sharp_prefix = path_join(home, "sharp-prefix");
    epic = path_join(home, "epic");
    launcher_games = path_join(home, "launcher-games");
    bottles = path_join(home, "bottles");
    if (prefix) {
        out->steam_links = collect_links(prefix);
        destination = path_join(out->temp, "prefix-steam");
        if (!copy_tree_filtered(prefix, destination, 2))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (gptk) {
        out->gptk_links = collect_links(gptk);
        destination = path_join(out->temp, "prefix-gptk");
        if (!copy_tree_filtered(gptk, destination, 1))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (games) {
        destination = path_join(out->temp, "games");
        if (!copy_tree_filtered(games, destination, 1))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (library) {
        destination = path_join(out->temp, "sharp-library");
        if (!copy_tree_filtered(library, destination, 1))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (sharp_prefix) {
        destination = path_join(out->temp, "sharp-prefix");
        if (!copy_tree_filtered(sharp_prefix, destination, 0))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (epic) {
        destination = path_join(out->temp, "epic");
        if (!copy_tree_filtered(epic, destination, 1))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (launcher_games) {
        destination = path_join(out->temp, "launcher-games");
        if (!copy_tree_filtered(launcher_games, destination, 1))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (cache) {
        destination = path_join(out->temp, "cache");
        if (!copy_tree_filtered(cache, destination, 3))
            goto fail;
        free(destination);
        destination = NULL;
    }
    if (bottles) {
        destination = path_join(out->temp, "bottles");
        if (!copy_tree_filtered(bottles, destination, 1))
            goto fail;
        free(destination);
        destination = NULL;
        dir = opendir(bottles);
        if (dir) {
            while ((entry = readdir(dir)) != NULL) {
                char *name_prefix, *src_prefix, *dst_prefix;
                if (!strncmp(entry->d_name, "steam_", 6) || !strncmp(entry->d_name, "epic_", 5)) {
                    name_prefix = path_join(bottles, entry->d_name);
                    src_prefix = name_prefix ? path_join(name_prefix, "prefix") : NULL;
                    dst_prefix = src_prefix ? path_join(out->temp, "bottles") : NULL;
                    if (dst_prefix) {
                        char* n = path_join(dst_prefix, entry->d_name);
                        free(dst_prefix);
                        dst_prefix = n;
                    }
                    if (dst_prefix) {
                        char* n = path_join(dst_prefix, "prefix");
                        free(dst_prefix);
                        dst_prefix = n;
                    }
                    int prefix_mode = !strncmp(entry->d_name, "epic_", 5) ? 1 : 2;
                    if (src_prefix && dst_prefix && directory_local(src_prefix) &&
                        !copy_tree_filtered(src_prefix, dst_prefix, prefix_mode)) {
                        free(name_prefix);
                        free(src_prefix);
                        free(dst_prefix);
                        closedir(dir);
                        goto fail;
                    }
                    free(name_prefix);
                    free(src_prefix);
                    free(dst_prefix);
                }
                if (entry->d_name[0] == '.' || strcmp(entry->d_name, "gog-prefix") != 0)
                    continue;
                name_prefix = path_join(bottles, entry->d_name);
                src_prefix = name_prefix ? path_join(name_prefix, "prefix") : NULL;
                destination = path_join(out->temp, "bottles/gog-prefix/prefix");
                if (src_prefix && destination && directory_local(src_prefix) &&
                    !copy_tree_filtered(src_prefix, destination, 0))
                    goto fail;
                free(name_prefix);
                free(src_prefix);
                free(destination);
                destination = NULL;
            }
            closedir(dir);
            dir = NULL;
        }
    }
    free(cache);
    free(prefix);
    free(gptk);
    free(games);
    free(library);
    free(sharp_prefix);
    free(epic);
    free(launcher_games);
    free(bottles);
    return true;
fail:
    if (dir)
        closedir(dir);
    free(destination);
    free(cache);
    free(prefix);
    free(gptk);
    free(games);
    free(library);
    free(sharp_prefix);
    free(epic);
    free(launcher_games);
    free(bottles);
    free(out->setup);
    free(out->steam_config);
    free_links(out->steam_links);
    free_links(out->gptk_links);
    remove_tree_local(out->temp);
    free(out->temp);
    memset(out, 0, sizeof(*out));
    return false;
}

static void free_preserved_data(preserved_data* data) {
    if (!data)
        return;
    free(data->setup);
    free(data->steam_config);
    free_links(data->steam_links);
    free_links(data->gptk_links);
    remove_tree_local(data->temp);
    free(data->temp);
    memset(data, 0, sizeof(*data));
}

static void remove_old_runtime(const char* home) {
    const char* names[] = {"runtime",
                           "configs",
                           "cache",
                           "logs",
                           "shader-cache",
                           "crashes",
                           "SteamSetup.exe",
                           "install_progress.json",
                           "update_progress.json"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char* path = path_join(home, names[i]);
        remove_tree_local(path);
        free(path);
    }
    for (size_t i = 0; i < 5; i++) {
        const char* names2[] = {"runtime", "configs", "cache", "logs", "shader-cache"};
        char* path = path_join(home, names2[i]);
        if (path)
            (void)mkdir(path, 0700);
        free(path);
    }
}

static void write_setup_metadata(const char* home) {
    char* path = path_join(home, "setup.json");
    char* text = path ? read_text(path) : NULL;
    char* config_path = path_join(home, "cache/steam_config.json");
    char* config_text = config_path ? read_text(config_path) : NULL;
    bool api_key = steam_config_has_api_key(config_text);
    char error[128];
    ms_json* value = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
    ms_json_writer writer;
    if (text && (!value || ms_json_type_of(value) != MS_JSON_OBJECT)) {
        ms_json_free(value);
        free(text);
        free(config_text);
        free(config_path);
        free(path);
        return;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    if (value && ms_json_type_of(value) == MS_JSON_OBJECT) {
        for (size_t i = 0; i < ms_json_object_length(value); i++) {
            const char* key = ms_json_object_key_at(value, i);
            char* raw = ms_json_stringify(ms_json_object_value_at(value, i));
            if (!key || !raw || !strcmp(key, "last_migrated_version") || !strcmp(key, "runtime_migration_schema") ||
                (api_key && !strcmp(key, "steamApiKeySet"))) {
                free(raw);
                continue;
            }
            ms_json_writer_key(&writer, key);
            ms_json_writer_raw(&writer, raw);
            free(raw);
        }
    }
    ms_json_writer_key(&writer, "last_migrated_version");
    ms_json_writer_string(&writer, MIGRATION_VERSION);
    ms_json_writer_key(&writer, "runtime_migration_schema");
    ms_json_writer_u64(&writer, MIGRATION_SCHEMA);
    if (api_key) {
        ms_json_writer_key(&writer, "steamApiKeySet");
        ms_json_writer_bool(&writer, true);
    }
    ms_json_writer_object_end(&writer);
    {
        char* output = ms_json_writer_take(&writer);
        if (output && path) {
            (void)mkdir(home, 0700);
            FILE* f = fopen(path, "wb");
            if (f) {
                fputs(output, f);
                fclose(f);
            }
        }
        free(output);
    }
    ms_json_free(value);
    free(text);
    free(config_text);
    free(config_path);
    free(path);
}

static bool migration_metadata_current(const char* home) {
    char* path = path_join(home, "setup.json");
    char* text = path ? read_text(path) : NULL;
    char error[128];
    ms_json* value = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
    char* version = value ? json_str(value, "last_migrated_version") : NULL;
    unsigned long long schema = value ? json_u64(value, "runtime_migration_schema", 0) : 0;
    bool current = version && !strcmp(version, MIGRATION_VERSION) && schema >= MIGRATION_SCHEMA;
    free(version);
    ms_json_free(value);
    free(text);
    free(path);
    return current;
}

static void restore_preserved_data(const char* home, const preserved_data* data) {
    char *src = NULL, *dst = NULL, *compatdata = path_join(home, "compatdata"), *steam_config = NULL;
    remove_tree_local(compatdata);
    free(compatdata);

    src = path_join(data->temp, "prefix-steam");
    dst = path_join(home, "prefix-steam");
    if (src && dst && directory_local(src)) {
        (void)mkdir(dst, 0700);
        (void)copy_tree_filtered(src, dst, 2);
        {
            char* dos = path_join(dst, "dosdevices");
            char* c = dos ? path_join(dos, "c:") : NULL;
            if (dos)
                (void)mkdir(dos, 0700);
            if (c && access(c, F_OK) != 0)
                (void)symlink("../drive_c", c);
            free(dos);
            free(c);
        }
        restore_links(dst, data->steam_links);
    }
    free(src);
    free(dst);
    src = path_join(data->temp, "prefix-gptk");
    dst = path_join(home, "prefix-gptk");
    if (src && dst && directory_local(src)) {
        (void)mkdir(dst, 0700);
        (void)copy_tree_filtered(src, dst, 1);
        restore_links(dst, data->gptk_links);
    }
    free(src);
    free(dst);
    {
        const char* names[] = {"games", "sharp-library", "epic", "launcher-games"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            src = path_join(data->temp, names[i]);
            dst = path_join(home, names[i]);
            if (src && dst && directory_local(src)) {
                (void)mkdir(dst, 0700);
                (void)copy_tree_filtered(src, dst, 1);
            }
            free(src);
            free(dst);
        }
    }
    src = path_join(data->temp, "sharp-prefix");
    dst = path_join(home, "sharp-prefix");
    if (src && dst && directory_local(src)) {
        remove_tree_local(dst);
        (void)copy_tree_filtered(src, dst, 0);
    }
    free(src);
    free(dst);
    src = path_join(data->temp, "bottles");
    dst = path_join(home, "bottles");
    if (src && dst && directory_local(src)) {
        (void)mkdir(dst, 0700);
        (void)copy_tree_filtered(src, dst, 1);
        {
            char* gog_src = path_join(src, "gog-prefix/prefix");
            char* gog_dst = path_join(dst, "gog-prefix/prefix");
            if (gog_src && gog_dst && directory_local(gog_src)) {
                remove_tree_local(gog_dst);
                (void)copy_tree_filtered(gog_src, gog_dst, 0);
            }
            free(gog_src);
            free(gog_dst);
        }
        /* The settings-only bottle copy intentionally skips every prefix.
         * Restore Steam and Epic bottle metadata separately. */
        DIR* dir = opendir(src);
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL) {
                bool steam_bottle = strncmp(entry->d_name, "steam_", 6) == 0;
                bool epic_bottle = strncmp(entry->d_name, "epic_", 5) == 0;
                if (!steam_bottle && !epic_bottle)
                    continue;
                char *steam_src = path_join(src, entry->d_name), *steam_dst = path_join(dst, entry->d_name);
                char* p_src = steam_src ? path_join(steam_src, "prefix") : NULL;
                char* p_dst = steam_dst ? path_join(steam_dst, "prefix") : NULL;
                if (p_src && p_dst && directory_local(p_src)) {
                    (void)mkdir(steam_dst, 0700);
                    (void)copy_tree_filtered(p_src, p_dst, epic_bottle ? 1 : 2);
                }
                free(steam_src);
                free(steam_dst);
                free(p_src);
                free(p_dst);
            }
            closedir(dir);
        }
    }
    free(src);
    free(dst);
    if (data->setup) {
        char* setup = path_join(home, "setup.json");
        FILE* f = setup ? fopen(setup, "wb") : NULL;
        if (f) {
            fputs(data->setup, f);
            fclose(f);
        }
        free(setup);
    }
    if (data->steam_config) {
        steam_config = path_join(home, "cache/steam_config.json");
        if (steam_config) {
            char* slash = strrchr(steam_config, '/');
            if (slash) {
                *slash = '\0';
                (void)mkdir(steam_config, 0700);
                *slash = '/';
            }
            FILE* f = fopen(steam_config, "wb");
            if (f) {
                fputs(data->steam_config, f);
                fclose(f);
            }
        }
        free(steam_config);
    }
}

static void write_migration_report(const char* home, bool preserved, bool restored) {
    char* logs = path_join(home, "logs");
    char* path;
    FILE* f;
    ms_json_writer writer;
    char* payload;
    const char* categories[] = {"setup.json",    "steam_config", "cache", "prefix-steam",   "prefix-gptk", "games",
                                "sharp-library", "sharp-prefix", "epic",  "launcher-games", "bottles"};
    const char* reason = "migration preservation/restoration pass";
    if (logs)
        (void)mkdir(logs, 0700);
    path = logs ? path_join(logs, "migration-report-latest.json") : NULL;
    f = path ? fopen(path, "wb") : NULL;
    if (f) {
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "schema_version");
        ms_json_writer_u64(&writer, 1);
        ms_json_writer_key(&writer, "version");
        ms_json_writer_string(&writer, MIGRATION_VERSION);
        ms_json_writer_key(&writer, "generated_at_unix");
        ms_json_writer_u64(&writer, (unsigned long long)time(NULL));
        ms_json_writer_key(&writer, "entries");
        ms_json_writer_array_begin(&writer);
        for (size_t phase = 0; phase < 2; phase++) {
            for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++) {
                if (phase == 1 && i == 2)
                    continue;
                ms_json_writer_object_begin(&writer);
                ms_json_writer_key(&writer, "phase");
                ms_json_writer_string(&writer, phase == 0 ? "preserve" : "restore");
                ms_json_writer_key(&writer, "outcome");
                ms_json_writer_string(&writer, phase == 0 ? (preserved ? "preserved" : "failed")
                                                          : (restored ? "restored" : "failed"));
                ms_json_writer_key(&writer, "category");
                ms_json_writer_string(&writer, categories[i]);
                ms_json_writer_key(&writer, "path");
                ms_json_writer_null(&writer);
                ms_json_writer_key(&writer, "reason");
                ms_json_writer_string(&writer, reason);
                ms_json_writer_object_end(&writer);
            }
        }
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "phase");
        ms_json_writer_string(&writer, "restore");
        ms_json_writer_key(&writer, "outcome");
        ms_json_writer_string(&writer, "removed");
        ms_json_writer_key(&writer, "category");
        ms_json_writer_string(&writer, "compatdata");
        ms_json_writer_key(&writer, "path");
        ms_json_writer_null(&writer);
        ms_json_writer_key(&writer, "reason");
        ms_json_writer_string(&writer, "compatdata is deprecated and was not restored");
        ms_json_writer_object_end(&writer);
        ms_json_writer_array_end(&writer);
        ms_json_writer_object_end(&writer);
        payload = ms_json_writer_take(&writer);
        if (payload)
            fputs(payload, f);
        free(payload);
        fclose(f);
    }
    free(path);
    free(logs);
}

static bool run_migration_wineboot(const char* home, const char* prefix) {
    char *wine = path_join(home, "runtime/wine/bin/metalsharp-wine"), *runtime = path_join(home, "runtime/wine"),
         *fallback = NULL;
    pid_t pid;
    int status = 0;
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        free(runtime);
        return false;
    }
    fallback = runtime ? path_join(runtime, "lib/wine/x86_64-unix") : NULL;
    if ((pid = fork()) == 0) {
        char* args[] = {wine, "wineboot", "-u", NULL};
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "/usr/bin/true", 1);
        setenv("WINEDLOVERRIDES", "winedbg=d", 1);
        if (fallback)
            setenv("DYLD_FALLBACK_LIBRARY_PATH", fallback, 1);
        execv(wine, args);
        _exit(127);
    }
    if (pid < 0) {
        free(wine);
        free(runtime);
        free(fallback);
        return false;
    }
    for (unsigned i = 0; i < 240; i++) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            free(wine);
            free(runtime);
            free(fallback);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR)
            break;
        usleep(500000);
    }
    (void)kill(pid, SIGTERM);
    (void)waitpid(pid, &status, 0);
    free(wine);
    free(runtime);
    free(fallback);
    return false;
}

static bool migration_command_available(const char* command) {
    const char* dirs[] = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char* path = path_join(dirs[i], command);
        bool available = path && access(path, X_OK) == 0;
        free(path);
        if (available)
            return true;
    }
    return false;
}

static bool ensure_migration_zstd(void) {
    pid_t pid;
    int status = 0;
    if (migration_command_available("unzstd") || migration_command_available("zstd"))
        return true;
    if (!migration_command_available("brew"))
        return false;
    pid = fork();
    if (pid == 0) {
        execlp("brew", "brew", "install", "zstd", (char*)NULL);
        _exit(127);
    }
    if (pid < 0)
        return false;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           (migration_command_available("unzstd") || migration_command_available("zstd"));
}

static void update_existing_wine_prefixes(const char* home) {
    char* prefix = path_join(home, "prefix-steam");
    char* gog = path_join(home, "bottles/gog-prefix/prefix");
    if (prefix && directory_local(prefix))
        (void)run_migration_wineboot(home, prefix);
    if (gog && directory_local(gog) && (!prefix || strcmp(prefix, gog) != 0))
        (void)run_migration_wineboot(home, gog);
    free(prefix);
    free(gog);
}

static bool stop_managed_wine_processes(const char* home) {
    int status = 500;
    char* result = ms_steam_stop_json(home, &status);
    bool stopped = result != NULL && status == 200 && strstr(result, "\"running\":false") != NULL;
    free(result);
    return stopped;
}

static bool steam_library_has_manifest(const char* library) {
    char* steamapps = path_join(library, "steamapps");
    DIR* dir;
    struct dirent* entry;
    bool found = false;
    if (!steamapps)
        return false;
    dir = opendir(steamapps);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            if (!strncmp(entry->d_name, "appmanifest_", 12)) {
                found = true;
                break;
            }
        }
        closedir(dir);
    }
    free(steamapps);
    return found;
}

static void register_external_steam_libraries(const char* home) {
    char *prefix = path_join(home, "prefix-steam"), *dosdevices = NULL, *steamapps = NULL, *folders = NULL;
    DIR* volumes;
    struct dirent* volume;
    char* libraries[64] = {NULL};
    char mapped_drives[64] = {0};
    size_t library_count = 0;
    if (!prefix || !directory_local(prefix))
        goto done;
    dosdevices = path_join(prefix, "dosdevices");
    steamapps = path_join(prefix, "drive_c/Program Files (x86)/Steam/steamapps");
    if (!dosdevices || !steamapps)
        goto done;
    (void)mkdir(dosdevices, 0700);
    (void)mkdir(steamapps, 0700);
    volumes = opendir("/Volumes");
    if (!volumes)
        goto done;
    while ((volume = readdir(volumes)) != NULL && library_count < 64) {
        char *mount, *candidate;
        if (volume->d_name[0] == '.')
            continue;
        mount = path_join("/Volumes", volume->d_name);
        candidate = mount ? path_join(mount, "SteamLibrary") : NULL;
        if (!candidate || !steam_library_has_manifest(candidate)) {
            free(candidate);
            candidate = mount ? path_join(mount, "SteamLibrary/SteamLibrary") : NULL;
        }
        if ((!candidate || !steam_library_has_manifest(candidate)) && mount && steam_library_has_manifest(mount)) {
            free(candidate);
            candidate = strdup(mount);
        }
        if (candidate && steam_library_has_manifest(candidate)) {
            bool duplicate = false;
            for (size_t i = 0; i < library_count; i++)
                duplicate |= !strcmp(libraries[i], candidate);
            if (!duplicate)
                libraries[library_count++] = candidate;
            else
                free(candidate);
        } else {
            free(candidate);
        }
        free(mount);
    }
    closedir(volumes);
    {
        char drive = 'd';
        for (size_t i = 0; i < library_count; i++) {
            char name[4], *link_path;
            while (drive <= 'z') {
                snprintf(name, sizeof(name), "%c:", drive);
                link_path = path_join(dosdevices, name);
                if (link_path && lstat(link_path, &(struct stat){0}) != 0) {
                    if (symlink(libraries[i], link_path) == 0)
                        mapped_drives[i] = drive;
                    free(link_path);
                    drive++;
                    break;
                }
                free(link_path);
                drive++;
            }
            if (drive > 'z')
                break;
        }
    }
    folders = path_join(steamapps, "libraryfolders.vdf");
    if (folders) {
        char* existing = read_text(folders);
        if (existing) {
            size_t length = strlen(existing), capacity = length + library_count * 320 + 2;
            char* output = calloc(1, capacity);
            size_t used = 0;
            int next_index = 1;
            if (output) {
                const char* close = strrchr(existing, '}');
                size_t prefix_length = close ? (size_t)(close - existing) : length;
                memcpy(output, existing, prefix_length);
                used = prefix_length;
                for (size_t i = 0; i < library_count; i++) {
                    char drive = mapped_drives[i] ? mapped_drives[i] : 'z';
                    char path_token[64];
                    path_token[0] = '"';
                    path_token[1] = drive;
                    path_token[2] = ':';
                    path_token[3] = '\\';
                    path_token[4] = '\\';
                    path_token[5] = '"';
                    path_token[6] = '\0';
                    bool present = strstr(existing, path_token) != NULL;
                    if (!present) {
                        char line[32];
                        snprintf(line, sizeof(line), "\"%d\"", next_index);
                        while (strstr(existing, line) != NULL) {
                            next_index++;
                            snprintf(line, sizeof(line), "\"%d\"", next_index);
                        }
                        used += (size_t)snprintf(output + used, capacity - used,
                                                 "\n\t\"%d\"\n\t{\n\t\t\"path\"\t\t\"%c:\\\\\"\n\t}\n", next_index++,
                                                 drive);
                    }
                }
                if (close)
                    output[used++] = '}';
                output[used] = '\0';
                {
                    FILE* file = fopen(folders, "wb");
                    if (file) {
                        fputs(output, file);
                        fclose(file);
                    }
                }
                free(output);
            }
            free(existing);
        }
    }
done:
    for (size_t i = 0; i < library_count; i++)
        free(libraries[i]);
    free(prefix);
    free(dosdevices);
    free(steamapps);
    free(folders);
}
static bool write_migration_progress(const char* home, const char* state, unsigned step, const char* message,
                                     const char* error) {
    char* path = path_join(home, "migrate_progress.json");
    FILE* f;
    ms_json_writer writer;
    char* payload;
    if (!path)
        return false;
    f = fopen(path, "wb");
    if (!f) {
        free(path);
        return false;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "status");
    ms_json_writer_string(&writer, state);
    ms_json_writer_key(&writer, "step");
    ms_json_writer_u64(&writer, step);
    ms_json_writer_key(&writer, "total");
    ms_json_writer_u64(&writer, 8);
    ms_json_writer_key(&writer, "message");
    ms_json_writer_string(&writer, message);
    ms_json_writer_key(&writer, "error");
    if (error)
        ms_json_writer_string(&writer, error);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "version");
    ms_json_writer_string(&writer, MIGRATION_VERSION);
    ms_json_writer_object_end(&writer);
    payload = ms_json_writer_take(&writer);
    if (payload)
        fputs(payload, f);
    free(payload);
    fclose(f);
    free(path);
    return true;
}

static char* error_json(const char* s) {
    ms_json_writer w;
    char* out;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, s);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}

char* ms_migration_progress_json(const char* home) {
    char *p = path_join(home, "migrate_progress.json"), *out;
    if (!p)
        return NULL;
    out = raw_or(p,
                 "{\"status\":\"idle\",\"step\":0,\"total\":0,\"message\":\"\",\"error\":null,\"version\":\"0.61.0\"}");
    free(p);
    return out;
}
char* ms_migration_report_json(const char* home) {
    char *p = path_join(home, "logs/migration-report-latest.json"), *out;
    if (!p)
        return NULL;
    out = raw_or(p, "{\"schema_version\":1,\"status\":\"idle\",\"version\":\"0.61.0\",\"entries\":[],\"summary\":\"No "
                    "migration has run yet.\"}");
    free(p);
    return out;
}

char* ms_migration_check_json(const char* home) {
    char *setup_path = path_join(home, "setup.json"), *marker_path = path_join(home, ".post-update-migration"),
         *setup_text, *marker_text;
    ms_json *setup = NULL, *marker = NULL;
    char e[128];
    char* prefix_path = path_join(home, "prefix-steam");
    bool marker_needed = false, marker_mismatch = false, ready;
    char *legacy, *target = NULL;
    unsigned long long schema;
    ms_json_writer w;
    char* out;
    if (!exists(home) || !exists(setup_path)) {
        free(setup_path);
        free(marker_path);
        free(prefix_path);
        return strdup("{\"ok\":true,\"needed\":false,\"reason\":\"fresh_install\"}");
    }
    setup_text = read_text(setup_path);
    if (!setup_text) {
        free(setup_path);
        free(marker_path);
        free(prefix_path);
        return strdup("{\"ok\":true,\"needed\":false,\"reason\":\"cannot_read_setup\"}");
    }
    setup = ms_json_parse(setup_text, strlen(setup_text), e, sizeof(e));
    free(setup_text);
    if (!setup || ms_json_type_of(setup) != MS_JSON_OBJECT) {
        ms_json_free(setup);
        free(setup_path);
        free(marker_path);
        free(prefix_path);
        return strdup("{\"ok\":true,\"needed\":false,\"reason\":\"cannot_parse_setup\"}");
    }
    marker_text = read_text(marker_path);
    if (marker_text) {
        marker = ms_json_parse(marker_text, strlen(marker_text), e, sizeof(e));
        if (marker && ms_json_type_of(marker) == MS_JSON_OBJECT) {
            marker_needed = json_bool(marker, "needed", false);
            target = json_str(marker, "target_version");
        }
    }
    schema = json_u64(setup, "runtime_migration_schema", 0);
    legacy = json_str(setup, "last_migrated_version");
    ready = runtime_ready(home);
    marker_mismatch = target != NULL && compare_versions(target, MIGRATION_VERSION) > 0;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "needed");
    ms_json_writer_bool(&w, marker_needed || (!ready && (json_bool(setup, "completed", false) || exists(prefix_path))));
    ms_json_writer_key(&w, "current_version");
    ms_json_writer_string(&w, legacy ? legacy : "0.0.0");
    ms_json_writer_key(&w, "target_version");
    ms_json_writer_string(&w, MIGRATION_VERSION);
    ms_json_writer_key(&w, "current_schema");
    ms_json_writer_u64(&w, schema);
    ms_json_writer_key(&w, "target_schema");
    ms_json_writer_u64(&w, MIGRATION_SCHEMA);
    ms_json_writer_key(&w, "post_update_target_version");
    if (target)
        ms_json_writer_string(&w, target);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "running_version");
    ms_json_writer_string(&w, MIGRATION_VERSION);
    ms_json_writer_key(&w, "update_target_satisfied");
    ms_json_writer_bool(&w, !marker_mismatch);
    ms_json_writer_key(&w, "reason");
    if (marker_mismatch)
        ms_json_writer_string(&w, "post_update_target_version_mismatch");
    else if (marker_needed && !ready)
        ms_json_writer_string(&w, "post_update_marker_and_runtime_repair");
    else if (marker_needed)
        ms_json_writer_string(&w, "post_update_marker");
    else if (!ready && (json_bool(setup, "completed", false) || exists(prefix_path)))
        ms_json_writer_string(&w, "runtime_bundle_update_required");
    else if (schema >= MIGRATION_SCHEMA)
        ms_json_writer_string(&w, "up_to_date");
    else
        ms_json_writer_string(&w, "runtime_schema_already_satisfied");
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(legacy);
    free(target);
    free(setup_path);
    free(marker_path);
    free(prefix_path);
    ms_json_free(setup);
    ms_json_free(marker);
    return out;
}

typedef struct {
    char* home;
    char* lock_path;
} migration_job;

static void* migration_worker(void* opaque) {
    migration_job* job = (migration_job*)opaque;
    preserved_data preserved;
    int install_status = 200;
    char* install_progress_path = path_join(job->home, "install_progress.json");
    char* install_result;
    if (install_progress_path) {
        unlink(install_progress_path);
        free(install_progress_path);
    }
    (void)write_migration_progress(job->home, "running", 1,
                                   "Stopping managed Wine processes and ensuring extract tools are available...", NULL);
    if (!stop_managed_wine_processes(job->home)) {
        (void)write_migration_progress(job->home, "error", 1, "Could not stop managed Wine processes",
                                       "managed_wine_shutdown_failed");
        unlink(job->lock_path);
        free(job->home);
        free(job->lock_path);
        free(job);
        return NULL;
    }
    if (!ensure_migration_zstd()) {
        (void)write_migration_progress(job->home, "error", 1, "Failed to install zstd", "zstd_unavailable");
        unlink(job->lock_path);
        free(job->home);
        free(job->lock_path);
        free(job);
        return NULL;
    }
    (void)write_migration_progress(job->home, "running", 2,
                                   "Preserving user preferences, Steam API key, and bottle settings...", NULL);
    if (!preserve_user_data(job->home, &preserved)) {
        (void)write_migration_progress(job->home, "error", 2,
                                       "Could not safely preserve user data; no files were removed",
                                       "migration_preserve_failed");
        unlink(job->lock_path);
        free(job->home);
        free(job->lock_path);
        free(job);
        return NULL;
    }
    (void)write_migration_progress(job->home, "running", 3, "Cleaning stale runtime state...", NULL);
    remove_old_runtime(job->home);
    (void)write_migration_progress(job->home, "running", 4, "Installing update...", NULL);
    install_result = ms_setup_install_all_json(job->home, &install_status);
    if (install_result == NULL || strstr(install_result, "\"ok\":false") != NULL) {
        free(install_result);
        restore_preserved_data(job->home, &preserved);
        write_migration_report(job->home, true, true);
        free_preserved_data(&preserved);
        (void)write_migration_progress(job->home, "error", 8,
                                       "Runtime install incomplete — re-run setup wizard after restart",
                                       "runtime_install_incomplete");
        unlink(job->lock_path);
        free(job->home);
        free(job->lock_path);
        free(job);
        return NULL;
    }
    free(install_result);
    for (unsigned attempts = 0;; attempts++) {
        char* install_progress = path_join(job->home, "install_progress.json");
        char* text = install_progress ? read_text(install_progress) : NULL;
        bool complete = text && strstr(text, "\"status\":\"complete\"") != NULL;
        bool failed = (text && strstr(text, "\"status\":\"error\"") != NULL) || attempts >= 600;
        free(text);
        free(install_progress);
        if (failed) {
            restore_preserved_data(job->home, &preserved);
            write_migration_report(job->home, true, true);
            free_preserved_data(&preserved);
            (void)write_migration_progress(job->home, "error", 8,
                                           "Runtime install incomplete — re-run setup wizard after restart",
                                           "runtime_install_incomplete");
            unlink(job->lock_path);
            free(job->home);
            free(job->lock_path);
            free(job);
            return NULL;
        }
        if (complete) {
            (void)write_migration_progress(job->home, "running", 5, "Restoring preserved user data...", NULL);
            restore_preserved_data(job->home, &preserved);
            write_migration_report(job->home, true, true);
            free_preserved_data(&preserved);
            (void)write_migration_progress(job->home, "running", 6,
                                           "Updating Wine prefixes and registering external Steam libraries...", NULL);
            update_existing_wine_prefixes(job->home);
            if (!stop_managed_wine_processes(job->home)) {
                (void)write_migration_progress(job->home, "error", 6,
                                               "Could not stop Wine processes started while updating prefixes",
                                               "managed_wine_shutdown_failed");
                unlink(job->lock_path);
                free(job->home);
                free(job->lock_path);
                free(job);
                return NULL;
            }
            register_external_steam_libraries(job->home);
            {
                char* crash = path_join(job->home, "prefix-steam/drive_c/Program Files (x86)/Steam/.crash");
                if (crash)
                    unlink(crash);
                free(crash);
            }
            if (!runtime_ready(job->home)) {
                (void)write_migration_progress(job->home, "error", 7,
                                               "Update verification failed: runtime bundle is incomplete",
                                               "runtime_bundle_incomplete");
                unlink(job->lock_path);
                free(job->home);
                free(job->lock_path);
                free(job);
                return NULL;
            }
            write_setup_metadata(job->home);
            if (!migration_metadata_current(job->home)) {
                (void)write_migration_progress(job->home, "error", 7,
                                               "Update verification failed: migration metadata was not saved",
                                               "migration_metadata_not_saved");
                unlink(job->lock_path);
                free(job->home);
                free(job->lock_path);
                free(job);
                return NULL;
            }
            {
                char* marker = path_join(job->home, ".post-update-migration");
                char* backup = path_join(job->home, ".migration-steam_config.json");
                if (marker)
                    unlink(marker);
                if (backup)
                    unlink(backup);
                free(marker);
                free(backup);
            }
            (void)write_migration_progress(job->home, "running", 7, "Verifying MetalSharp update...", NULL);
            (void)write_migration_progress(job->home, "complete", 8, "MetalSharp is updated and ready.", NULL);
            unlink(job->lock_path);
            free(job->home);
            free(job->lock_path);
            free(job);
            return NULL;
        }
        sleep(1);
    }
}

char* ms_migration_start_json(const char* home) {
    char *setup_path = path_join(home, "setup.json"), *progress = path_join(home, "migrate_progress.json"),
         *lock_path = path_join(home, "migration.lock"), *marker_path = path_join(home, ".post-update-migration");
    char* marker_text = marker_path ? read_text(marker_path) : NULL;
    char marker_error[128];
    ms_json* marker =
        marker_text ? ms_json_parse(marker_text, strlen(marker_text), marker_error, sizeof(marker_error)) : NULL;
    char* target = marker ? json_str(marker, "target_version") : NULL;
    bool marker_needed = marker ? json_bool(marker, "needed", false) : false;
    int lock_fd;
    ms_json_writer w;
    char* out;
    bool ready = runtime_ready(home);
    if (target && compare_versions(target, MIGRATION_VERSION) > 0) {
        char message[256];
        snprintf(message, sizeof(message),
                 "Update handoff targeted MetalSharp v%s, but the running app is v%s. Relaunch the installed update "
                 "and retry migration.",
                 target, MIGRATION_VERSION);
        free(setup_path);
        free(progress);
        free(lock_path);
        free(marker_path);
        free(marker_text);
        free(target);
        ms_json_free(marker);
        return error_json(message);
    }
    if (!setup_path || !progress || !lock_path) {
        free(setup_path);
        free(progress);
        free(lock_path);
        free(marker_path);
        free(marker_text);
        free(target);
        ms_json_free(marker);
        return error_json("failed to start migration");
    }
    lock_fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (lock_fd < 0 && errno == EEXIST) {
        char* lock_text = read_text(lock_path);
        long owner = lock_text ? strtol(lock_text, NULL, 10) : 0;
        free(lock_text);
        if (owner <= 0 || (kill((pid_t)owner, 0) != 0 && errno == ESRCH)) {
            unlink(lock_path);
            lock_fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        }
    }
    if (lock_fd < 0) {
        free(setup_path);
        free(progress);
        free(lock_path);
        return error_json("migration already in progress");
    }
    dprintf(lock_fd, "%ld\n", (long)getpid());
    close(lock_fd);
    if (ready && !marker_needed) {
        write_setup_metadata(home);
        if (marker_path)
            unlink(marker_path);
        (void)write_migration_progress(home, "complete", 1, "Runtime already ready; app update complete.", NULL);
        unlink(lock_path);
    } else {
        migration_job* job = (migration_job*)calloc(1, sizeof(*job));
        pthread_t thread;
        (void)write_migration_progress(home, "running", 0, "Starting MetalSharp migration...", NULL);
        if (!job) {
            (void)write_migration_progress(home, "error", 8,
                                           "Runtime install incomplete — re-run setup wizard after restart",
                                           "runtime_install_incomplete");
            unlink(lock_path);
        } else {
            job->home = strdup(home);
            job->lock_path = strdup(lock_path);
            if (!job->home || !job->lock_path || pthread_create(&thread, NULL, migration_worker, job) != 0) {
                free(job->home);
                free(job->lock_path);
                free(job);
                (void)write_migration_progress(home, "error", 8,
                                               "Runtime install incomplete — re-run setup wizard after restart",
                                               "runtime_install_incomplete");
                unlink(lock_path);
            } else {
                pthread_detach(thread);
            }
        }
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    free(setup_path);
    free(progress);
    free(lock_path);
    free(marker_path);
    free(marker_text);
    free(target);
    ms_json_free(marker);
    return out;
}
