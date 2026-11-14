/* Games packed as their own apps (packed_app.h). */
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/packed_app.h"

#include "metalsharp_backend/battlenet.h"
#include "metalsharp_backend/epic.h"
#include "metalsharp_backend/gog.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/logs.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/rockstar.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/store_launcher.h"
#include "metalsharp_backend/ubisoft.h"
#include "metalsharp_backend/wine_runtime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PACKED_APPS_RECORD  "config/packed-apps.json"
#define PACKED_APP_LAUNCHER "tools/metalsharp-game-app"
#define PACKED_APP_ICON     "AppIcon"

static const char* const KINDS[] = {"steam", "ubisoft", "rockstar", "battlenet", "gog", "epic"};

static char* error_json(const char* message) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static char* fail(int* status, int code, const char* message) {
    if (status)
        *status = code;
    return error_json(message);
}

static bool valid_kind(const char* kind) {
    for (size_t i = 0; kind && i < sizeof(KINDS) / sizeof(KINDS[0]); i++)
        if (!strcmp(kind, KINDS[i]))
            return true;
    return false;
}

/* Every store's ids fit [A-Za-z0-9_.-] (a Steam appid is digits). */
static bool valid_id(const char* kind, const char* id) {
    size_t length = id ? strlen(id) : 0;
    if (length == 0 || length > 128 || id[0] == '.')
        return false;
    for (const char* p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '-')
            return false;
    if (!strcmp(kind, "steam")) {
        char* end = NULL;
        unsigned long long value;
        errno = 0;
        value = strtoull(id, &end, 10);
        return errno == 0 && end && *end == '\0' && value > 0 && value <= 0xffffffffULL;
    }
    return true;
}

static char* join(const char* a, const char* b) {
    size_t length = strlen(a) + strlen(b) + 2;
    char* out = malloc(length);
    if (out)
        snprintf(out, length, "%s/%s", a, b);
    return out;
}

static bool is_dir(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool mkdir_p(const char* path) {
    char buffer[PATH_MAX];
    if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer))
        return false;
    for (char* p = buffer + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

static char* read_text(const char* path, size_t max_size) {
    FILE* file = fopen(path, "rb");
    char* data;
    size_t length;
    if (!file)
        return NULL;
    data = malloc(max_size + 1);
    length = data ? fread(data, 1, max_size, file) : 0;
    fclose(file);
    if (!data)
        return NULL;
    data[length] = '\0';
    return data;
}

static bool write_text_atomic(const char* path, const char* text) {
    char temp[PATH_MAX];
    FILE* file;
    bool ok;
    if (snprintf(temp, sizeof(temp), "%s.tmp-%ld", path, (long)getpid()) >= (int)sizeof(temp))
        return false;
    file = fopen(temp, "wb");
    if (!file)
        return false;
    ok = fputs(text, file) >= 0;
    ok = fclose(file) == 0 && ok;
    if (!ok || rename(temp, path) != 0) {
        unlink(temp);
        return false;
    }
    return true;
}

/* Removes a tree without following links (as rm -rf does). */
static bool remove_tree(const char* path) {
    struct stat st;
    DIR* dir;
    struct dirent* entry;
    bool ok = true;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (!dir)
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = join(path, entry->d_name);
        if (!child || !remove_tree(child))
            ok = false;
        free(child);
    }
    closedir(dir);
    return rmdir(path) == 0 && ok;
}

static bool copy_file(const char* source, const char* destination, mode_t mode) {
    int in = open(source, O_RDONLY);
    int out = in >= 0 ? open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode) : -1;
    char buffer[65536];
    ssize_t count;
    bool ok = in >= 0 && out >= 0;
    while (ok && (count = read(in, buffer, sizeof(buffer))) != 0) {
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        for (ssize_t done = 0; done < count;) {
            ssize_t written = write(out, buffer + done, (size_t)(count - done));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                ok = false;
                break;
            }
            done += written;
        }
    }
    if (in >= 0)
        close(in);
    if (out >= 0 && close(out) != 0)
        ok = false;
    if (ok)
        ok = chmod(destination, mode) == 0;
    return ok;
}

/* Runs a tool to completion with its output discarded. */
static bool run_tool(char* const argv[]) {
    int status = 0;
    pid_t pid, waited;
    if (access(argv[0], X_OK) != 0)
        return false;
    pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0)
        return false;
    do
        waited = waitpid(pid, &status, 0);
    while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---------------------------------------------------------------- requests */

typedef struct {
    char kind[16];
    char id[129];
} game_ref;

static bool ref_set(game_ref* ref, const char* kind, const char* id) {
    if (!valid_kind(kind) || !valid_id(kind, id))
        return false;
    snprintf(ref->kind, sizeof(ref->kind), "%s", kind);
    snprintf(ref->id, sizeof(ref->id), "%s", id);
    return true;
}

/* {kind,id} from a JSON body; id may be a number (a Steam appid). */
static bool ref_from_json(const ms_json* json, game_ref* ref) {
    char *kind = NULL, *id = NULL;
    char number[32];
    bool ok;
    long long value;
    if (!json)
        return false;
    (void)ms_json_as_string(ms_json_object_get(json, "kind"), &kind);
    if (!ms_json_as_string(ms_json_object_get(json, "id"), &id) &&
        ms_json_as_i64(ms_json_object_get(json, "id"), &value) && value > 0) {
        snprintf(number, sizeof(number), "%lld", value);
        id = strdup(number);
    }
    ok = kind && id && ref_set(ref, kind, id);
    free(kind);
    free(id);
    return ok;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* name's value in a query string, %-decoded. */
static bool query_value(const char* query, const char* name, char* out, size_t out_size) {
    size_t name_length = strlen(name);
    for (const char* p = query; p && *p;) {
        const char* end = strchr(p, '&');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length > name_length && !strncmp(p, name, name_length) && p[name_length] == '=') {
            size_t n = 0;
            for (const char* v = p + name_length + 1; v < p + length; v++) {
                char c = *v;
                if (c == '%' && v + 2 < p + length && hex_value(v[1]) >= 0 && hex_value(v[2]) >= 0) {
                    c = (char)(hex_value(v[1]) * 16 + hex_value(v[2]));
                    v += 2;
                } else if (c == '+')
                    c = ' ';
                if (n + 1 >= out_size)
                    return false;
                out[n++] = c;
            }
            out[n] = '\0';
            return true;
        }
        p = end ? end + 1 : NULL;
    }
    return false;
}

static bool request_ref(const char* query, const char* body, size_t length, game_ref* ref) {
    char kind[32], id[160];
    if (query && query_value(query, "kind", kind, sizeof(kind)) && query_value(query, "id", id, sizeof(id)))
        return ref_set(ref, kind, id);
    if (body && length) {
        ms_json* json = ms_json_parse(body, length, NULL, 0);
        bool ok = ref_from_json(json, ref);
        ms_json_free(json);
        return ok;
    }
    return false;
}

/* --------------------------------------------------------------- the bundle */

static const char* apps_dir(void) {
    const char* configured = getenv("METALSHARP_PACKED_APPS_DIR");
    return configured && configured[0] ? configured : "/Applications";
}

static char* launcher_binary(void) {
    const char* configured = getenv("METALSHARP_GAME_APP_LAUNCHER");
    if (configured && configured[0])
        return access(configured, X_OK) == 0 ? strdup(configured) : NULL;
    return ms_app_resources_path(PACKED_APP_LAUNCHER);
}

/* The game's name as a file name: no '/' or ':', no leading dot, no control characters. */
static void file_name_for(const char* name, const game_ref* ref, char* out, size_t out_size) {
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)name; p && *p && n + 1 < out_size && n < 200; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7f)
            continue;
        if (c == '/' || c == ':')
            c = '-';
        if (n == 0 && (c == '.' || c == ' '))
            continue;
        out[n++] = (char)c;
    }
    /* A UTF-8 sequence cut at the length limit is dropped. */
    while (n > 0 && ((unsigned char)out[n - 1] & 0xc0) == 0x80) {
        size_t start = n - 1;
        while (start > 0 && ((unsigned char)out[start] & 0xc0) == 0x80)
            start--;
        unsigned char lead = (unsigned char)out[start];
        size_t want = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (n - start == want)
            break;
        n = start;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.'))
        n--;
    out[n] = '\0';
    if (n == 0)
        snprintf(out, out_size, "Game %s", ref->id);
}

static void bundle_id_for(const game_ref* ref, char* out, size_t out_size) {
    size_t n = (size_t)snprintf(out, out_size, "com.metalsharp.game.%s.", ref->kind);
    for (const char* p = ref->id; *p && n + 1 < out_size; p++)
        out[n++] = isalnum((unsigned char)*p) || *p == '.' || *p == '-' ? *p : '-';
    out[n] = '\0';
}

static char* xml_escape(const char* text) {
    size_t length = 1;
    char* out;
    char* q;
    for (const char* p = text; *p; p++)
        length += *p == '&' ? 5 : (*p == '<' || *p == '>') ? 4 : (*p == '"' || *p == '\'') ? 6 : 1;
    out = malloc(length);
    if (!out)
        return NULL;
    q = out;
    for (const char* p = text; *p; p++) {
        const char* replacement = *p == '&'    ? "&amp;"
                                  : *p == '<'  ? "&lt;"
                                  : *p == '>'  ? "&gt;"
                                  : *p == '"'  ? "&quot;"
                                  : *p == '\'' ? "&apos;"
                                               : NULL;
        if (replacement) {
            strcpy(q, replacement);
            q += strlen(replacement);
        } else
            *q++ = *p;
    }
    *q = '\0';
    return out;
}

/* A <string> value after <key>key</key> in an XML property list, unescaped only as written here. */
static bool plist_string(const char* text, const char* key, char* out, size_t out_size) {
    char marker[128];
    const char* p;
    const char* end;
    snprintf(marker, sizeof(marker), "<key>%s</key>", key);
    p = text ? strstr(text, marker) : NULL;
    if (!p)
        return false;
    p += strlen(marker);
    while (isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "<string>", 8))
        return false;
    p += 8;
    end = strstr(p, "</string>");
    if (!end || (size_t)(end - p) >= out_size)
        return false;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return true;
}

typedef enum { BUNDLE_MISSING, BUNDLE_OURS, BUNDLE_OTHER_GAME, BUNDLE_FOREIGN } bundle_owner;

/* Whether path is a bundle MetalSharp made, and for which game. */
static bundle_owner bundle_owner_of(const char* path, const game_ref* ref) {
    struct stat st;
    char plist[PATH_MAX], kind[32], id[160];
    char* text;
    bundle_owner owner = BUNDLE_FOREIGN;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? BUNDLE_MISSING : BUNDLE_FOREIGN;
    if (!S_ISDIR(st.st_mode))
        return BUNDLE_FOREIGN;
    snprintf(plist, sizeof(plist), "%s/Contents/Info.plist", path);
    text = read_text(plist, 256 * 1024);
    if (text && plist_string(text, "MetalSharpGameKind", kind, sizeof(kind)) &&
        plist_string(text, "MetalSharpGameId", id, sizeof(id)) && valid_kind(kind))
        owner = !strcmp(kind, ref->kind) && !strcmp(id, ref->id) ? BUNDLE_OURS : BUNDLE_OTHER_GAME;
    free(text);
    return owner;
}

static bool write_info_plist(const char* path, const game_ref* ref, const char* name, const char* executable,
                             bool has_icon) {
    char bundle_id[256];
    char* escaped_name = xml_escape(name);
    char* escaped_executable = xml_escape(executable);
    FILE* file;
    bool ok;
    if (!escaped_name || !escaped_executable) {
        free(escaped_name);
        free(escaped_executable);
        return false;
    }
    bundle_id_for(ref, bundle_id, sizeof(bundle_id));
    file = fopen(path, "wb");
    ok = file != NULL;
    if (file) {
        ok = fprintf(file,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                     "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                     "<plist version=\"1.0\">\n"
                     "<dict>\n"
                     "  <key>CFBundleExecutable</key><string>%s</string>\n"
                     "  <key>CFBundleIdentifier</key><string>%s</string>\n"
                     "  <key>CFBundleName</key><string>%s</string>\n"
                     "  <key>CFBundleDisplayName</key><string>%s</string>\n"
                     "  <key>CFBundlePackageType</key><string>APPL</string>\n"
                     "  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
                     "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
                     "  <key>CFBundleVersion</key><string>1</string>\n"
                     "%s"
                     "  <key>LSMinimumSystemVersion</key><string>13.0</string>\n"
                     "  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>\n"
                     "  <key>NSHighResolutionCapable</key><true/>\n"
                     "  <key>NSPrincipalClass</key><string>NSApplication</string>\n"
                     "  <key>MetalSharpGameKind</key><string>%s</string>\n"
                     "  <key>MetalSharpGameId</key><string>%s</string>\n"
                     "  <key>MetalSharpGameName</key><string>%s</string>\n"
                     "</dict>\n"
                     "</plist>\n",
                     escaped_executable, bundle_id, escaped_name, escaped_name,
                     has_icon ? "  <key>CFBundleIconFile</key><string>" PACKED_APP_ICON "</string>\n" : "", ref->kind,
                     ref->id, escaped_name) > 0;
        ok = fclose(file) == 0 && ok;
    }
    free(escaped_name);
    free(escaped_executable);
    return ok;
}

/* image as a 512-point square .icns (padded, never cropped), with sips. */
static bool icns_from_image(const char* image, const char* work_dir, const char* icns) {
    char scaled[PATH_MAX], square[PATH_MAX];
    char* const sips = "/usr/bin/sips";
    bool ok;
    snprintf(scaled, sizeof(scaled), "%s/icon-scaled.png", work_dir);
    snprintf(square, sizeof(square), "%s/icon-square.png", work_dir);
    {
        char* const argv[] = {sips, "-s", "format", "png", "-Z", "512", (char*)image, "--out", scaled, NULL};
        ok = run_tool(argv);
    }
    if (ok) {
        char* const argv[] = {sips, "--padToHeightWidth", "512", "512", scaled, "--out", square, NULL};
        ok = run_tool(argv);
    }
    if (ok) {
        char* const argv[] = {sips, "-s", "format", "icns", square, "--out", (char*)icns, NULL};
        ok = run_tool(argv) && is_file(icns);
    }
    unlink(scaled);
    unlink(square);
    if (!ok)
        unlink(icns);
    return ok;
}

/* The executable's own icon (wrestool and icotool), extracted afresh into the artwork cache. */
static char* executable_icon(const char* home, const game_ref* ref, const char* executable) {
    char key_text[160], cached[PATH_MAX];
    unsigned key;
    if (!executable || !is_file(executable))
        return NULL;
    snprintf(key_text, sizeof(key_text), "packed-app:%s:%s", ref->kind, ref->id);
    /* Bit 28 alone: apart from Steam's appids and the stores' (bits 29 to 31). */
    key = 0x10000000u | (ms_store_fnv1a(key_text) & 0x0fffffffu);
    snprintf(cached, sizeof(cached), "%s/cache/ubisoft-connect/artwork/%u.png", home, key);
    unlink(cached);
    return ms_steam_extract_executable_icon(home, executable, key);
}

/* ------------------------------------------------------------------ records */

static char* record_path(const char* home) {
    return join(home, PACKED_APPS_RECORD);
}

static ms_json* load_records(const char* home) {
    char* path = record_path(home);
    char* text = path ? read_text(path, 4 * 1024 * 1024) : NULL;
    ms_json* json = text ? ms_json_parse(text, strlen(text), NULL, 0) : NULL;
    free(path);
    free(text);
    return json;
}

static const ms_json* records_array(const ms_json* json) {
    const ms_json* apps = json ? ms_json_object_get(json, "apps") : NULL;
    return apps && ms_json_type_of(apps) == MS_JSON_ARRAY ? apps : NULL;
}

static bool record_matches(const ms_json* entry, const game_ref* ref) {
    char *kind = NULL, *id = NULL;
    bool match = ms_json_as_string(ms_json_object_get(entry, "kind"), &kind) &&
                 ms_json_as_string(ms_json_object_get(entry, "id"), &id) && !strcmp(kind, ref->kind) &&
                 !strcmp(id, ref->id);
    free(kind);
    free(id);
    return match;
}

/* The recorded bundle path of a game, or NULL. */
static char* record_find(const char* home, const game_ref* ref) {
    ms_json* json = load_records(home);
    const ms_json* apps = records_array(json);
    char* path = NULL;
    for (size_t i = 0; apps && i < ms_json_array_length(apps) && !path; i++)
        if (record_matches(ms_json_array_get(apps, i), ref))
            (void)ms_json_as_string(ms_json_object_get(ms_json_array_get(apps, i), "path"), &path);
    ms_json_free(json);
    return path;
}

/* Rewrites the record without ref's entry, then with (name, path) for it when path is given. */
static bool record_store(const char* home, const game_ref* ref, const char* name, const char* path) {
    ms_json* json = load_records(home);
    const ms_json* apps = records_array(json);
    char* file = record_path(home);
    char* config = join(home, "config");
    ms_json_writer w;
    char* text;
    bool ok;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "apps");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; apps && i < ms_json_array_length(apps); i++) {
        const ms_json* entry = ms_json_array_get(apps, i);
        char* raw;
        if (ms_json_type_of(entry) != MS_JSON_OBJECT || record_matches(entry, ref))
            continue;
        raw = ms_json_stringify(entry);
        if (raw)
            ms_json_writer_raw(&w, raw);
        free(raw);
    }
    if (path) {
        char bundle_id[256];
        bundle_id_for(ref, bundle_id, sizeof(bundle_id));
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "kind");
        ms_json_writer_string(&w, ref->kind);
        ms_json_writer_key(&w, "id");
        ms_json_writer_string(&w, ref->id);
        ms_json_writer_key(&w, "name");
        ms_json_writer_string(&w, name ? name : "");
        ms_json_writer_key(&w, "path");
        ms_json_writer_string(&w, path);
        ms_json_writer_key(&w, "bundleId");
        ms_json_writer_string(&w, bundle_id);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    text = ms_json_writer_take(&w);
    ok = text && file && config && mkdir_p(config) && write_text_atomic(file, text);
    free(text);
    free(file);
    free(config);
    ms_json_free(json);
    return ok;
}

static char* packed_result(int* status, bool packed, const char* path, bool has_icon) {
    ms_json_writer w;
    if (status)
        *status = 200;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "packed");
    ms_json_writer_bool(&w, packed);
    ms_json_writer_key(&w, "path");
    if (packed && path)
        ms_json_writer_string(&w, path);
    else
        ms_json_writer_null(&w);
    if (has_icon) {
        ms_json_writer_key(&w, "icon");
        ms_json_writer_bool(&w, true);
    }
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

/* ------------------------------------------------------------------- create */

char* ms_packed_app_create_json(const char* home, const char* body, size_t length, int* status) {
    ms_json* json = body && length ? ms_json_parse(body, length, NULL, 0) : NULL;
    game_ref ref;
    char *name = NULL, *icon_path = NULL, *executable_path = NULL, *launcher = NULL, *previous = NULL;
    char *extracted_icon = NULL, *result = NULL;
    char file_name[256], bundle[PATH_MAX], temp[PATH_MAX], contents[PATH_MAX], macos[PATH_MAX], resources[PATH_MAX],
        target[PATH_MAX], icns[PATH_MAX], plist[PATH_MAX], message[PATH_MAX + 160];
    const char* dir = apps_dir();
    bool has_icon = false, temp_made = false;
    if (!ref_from_json(json, &ref)) {
        result = fail(status, 400, "A game kind and id are required");
        goto done;
    }
    (void)ms_json_as_string(ms_json_object_get(json, "name"), &name);
    (void)ms_json_as_string(ms_json_object_get(json, "iconPath"), &icon_path);
    (void)ms_json_as_string(ms_json_object_get(json, "executablePath"), &executable_path);
    if (!name || !name[0] || strlen(name) > 512) {
        result = fail(status, 400, "The game's name is required");
        goto done;
    }
    if (!(launcher = launcher_binary())) {
        result = fail(status, 500, "This MetalSharp build has no game app launcher (" PACKED_APP_LAUNCHER ")");
        goto done;
    }
    if (!is_dir(dir)) {
        snprintf(message, sizeof(message), "%s is not a folder", dir);
        result = fail(status, 500, message);
        goto done;
    }
    file_name_for(name, &ref, file_name, sizeof(file_name));
    if (snprintf(bundle, sizeof(bundle), "%s/%s.app", dir, file_name) >= (int)sizeof(bundle) ||
        snprintf(temp, sizeof(temp), "%s/.%s.app.metalsharp-%ld", dir, file_name, (long)getpid()) >=
            (int)sizeof(temp)) {
        result = fail(status, 400, "The game's name is too long for an app");
        goto done;
    }
    switch (bundle_owner_of(bundle, &ref)) {
    case BUNDLE_MISSING:
    case BUNDLE_OURS:
        break;
    case BUNDLE_OTHER_GAME:
        snprintf(message, sizeof(message), "%s already holds another game's MetalSharp app", bundle);
        result = fail(status, 409, message);
        goto done;
    case BUNDLE_FOREIGN:
        snprintf(message, sizeof(message), "%s already exists and was not made by MetalSharp", bundle);
        result = fail(status, 409, message);
        goto done;
    }
    snprintf(contents, sizeof(contents), "%s/Contents", temp);
    snprintf(macos, sizeof(macos), "%s/MacOS", contents);
    snprintf(resources, sizeof(resources), "%s/Resources", contents);
    snprintf(target, sizeof(target), "%s/%s", macos, file_name);
    snprintf(icns, sizeof(icns), "%s/" PACKED_APP_ICON ".icns", resources);
    snprintf(plist, sizeof(plist), "%s/Info.plist", contents);
    (void)remove_tree(temp);
    temp_made = true;
    if (!mkdir_p(macos) || !mkdir_p(resources)) {
        snprintf(message, sizeof(message), "Could not create an app in %s: %s", dir, strerror(errno));
        result = fail(status, 500, message);
        goto done;
    }
    if (!copy_file(launcher, target, 0755)) {
        result = fail(status, 500, "Could not copy the game app launcher into the app");
        goto done;
    }
    extracted_icon = executable_icon(home, &ref, executable_path);
    has_icon = (extracted_icon && icns_from_image(extracted_icon, resources, icns)) ||
               (icon_path && is_file(icon_path) && icns_from_image(icon_path, resources, icns));
    if (!write_info_plist(plist, &ref, name, file_name, has_icon)) {
        result = fail(status, 500, "Could not write the app's Info.plist");
        goto done;
    }
    {
        char* const argv[] = {"/usr/bin/codesign", "--force", "--sign", "-", temp, NULL};
        if (!run_tool(argv))
            ms_log_event(home, "Packed game app: ad-hoc signing failed; the app is left unsigned");
    }
    /* A bundle this game had elsewhere (another name) goes, as does the one being replaced. */
    previous = record_find(home, &ref);
    if (previous && strcmp(previous, bundle) && bundle_owner_of(previous, &ref) == BUNDLE_OURS)
        (void)remove_tree(previous);
    if (bundle_owner_of(bundle, &ref) == BUNDLE_OURS && !remove_tree(bundle)) {
        snprintf(message, sizeof(message), "Could not replace %s", bundle);
        result = fail(status, 500, message);
        goto done;
    }
    if (rename(temp, bundle) != 0) {
        snprintf(message, sizeof(message), "Could not move the app into %s: %s", dir, strerror(errno));
        result = fail(status, 500, message);
        goto done;
    }
    temp_made = false;
    if (!record_store(home, &ref, name, bundle))
        ms_log_event(home, "Packed game app: could not record it in " PACKED_APPS_RECORD);
    snprintf(message, sizeof(message), "Packed %s as %s", name, bundle);
    ms_log_event(home, message);
    result = packed_result(status, true, bundle, has_icon);
done:
    if (temp_made)
        (void)remove_tree(temp);
    ms_json_free(json);
    free(name);
    free(icon_path);
    free(executable_path);
    free(launcher);
    free(previous);
    free(extracted_icon);
    return result;
}

char* ms_packed_app_status_json(const char* home, const char* query, const char* body, size_t length, int* status) {
    game_ref ref;
    char* path;
    char* result;
    if (!request_ref(query, body, length, &ref))
        return fail(status, 400, "A game kind and id are required");
    path = record_find(home, &ref);
    result = packed_result(status, path && bundle_owner_of(path, &ref) == BUNDLE_OURS, path, false);
    free(path);
    return result;
}

char* ms_packed_app_remove_json(const char* home, const char* body, size_t length, int* status) {
    game_ref ref;
    char* path;
    char message[PATH_MAX + 96];
    if (!request_ref(NULL, body, length, &ref))
        return fail(status, 400, "A game kind and id are required");
    path = record_find(home, &ref);
    if (path) {
        switch (bundle_owner_of(path, &ref)) {
        case BUNDLE_OURS:
            if (!remove_tree(path)) {
                snprintf(message, sizeof(message), "Could not remove %s", path);
                free(path);
                return fail(status, 500, message);
            }
            break;
        case BUNDLE_MISSING:
            break;
        case BUNDLE_OTHER_GAME:
        case BUNDLE_FOREIGN:
            /* Something else now stands there: it stays, and the record goes. */
            break;
        }
    }
    free(path);
    if (!record_store(home, &ref, NULL, NULL))
        return fail(status, 500, "Could not update " PACKED_APPS_RECORD);
    return packed_result(status, false, NULL, false);
}

/* ------------------------------------------------------- launching a game */

/* bottles/<source>_<appid>/bottle.json's preferred_pipeline, or NULL. */
static char* saved_store_pipeline(const char* home, const char* source, unsigned appid) {
    char relative[96];
    char* path;
    char* text;
    ms_json* json;
    char* pipeline = NULL;
    snprintf(relative, sizeof(relative), "bottles/%s_%u/bottle.json", source, appid);
    path = join(home, relative);
    text = path ? read_text(path, 65536) : NULL;
    json = text ? ms_json_parse(text, strlen(text), NULL, 0) : NULL;
    if (json)
        (void)ms_json_as_string(ms_json_object_get(json, "preferred_pipeline"), &pipeline);
    if (pipeline && !ms_store_canonical_pipeline(pipeline)) {
        free(pipeline);
        pipeline = NULL;
    }
    free(path);
    free(text);
    ms_json_free(json);
    return pipeline;
}

/* The GOG engine MetalSharp's app saved for a game (<home>/gpui-gog-engines.json), or "auto". */
static char* saved_gog_engine(const char* home, const char* product_id) {
    char* path = join(home, "gpui-gog-engines.json");
    char* text = path ? read_text(path, 1024 * 1024) : NULL;
    ms_json* json = text ? ms_json_parse(text, strlen(text), NULL, 0) : NULL;
    char* engine = NULL;
    if (json)
        (void)ms_json_as_string(ms_json_object_get(json, product_id), &engine);
    if (!engine || !engine[0]) {
        free(engine);
        engine = strdup("auto");
    }
    free(path);
    free(text);
    ms_json_free(json);
    return engine;
}

static char* json_object_2(const char* key1, const char* value1, const char* key2, const char* value2,
                           unsigned long long appid) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    if (appid) {
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, appid);
    }
    if (key1) {
        ms_json_writer_key(&w, key1);
        ms_json_writer_string(&w, value1);
    }
    if (key2) {
        ms_json_writer_key(&w, key2);
        if (value2)
            ms_json_writer_string(&w, value2);
        else
            ms_json_writer_null(&w);
    }
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

bool ms_packed_app_plan_for(const char* home, const char* kind, const char* id, ms_packed_app_plan* plan) {
    game_ref ref;
    memset(plan, 0, sizeof(*plan));
    if (!ref_set(&ref, kind, id))
        return false;
    snprintf(plan->kind, sizeof(plan->kind), "%s", ref.kind);
    snprintf(plan->id, sizeof(plan->id), "%s", ref.id);
    if (!strcmp(kind, "steam")) {
        plan->appid = (unsigned)strtoul(id, NULL, 10);
        plan->start_steam = true;
        /* "auto": the game's saved route (bottles/steam_<appid>), as the library's Play has it. */
        plan->launch_route = "/steam/launch-game";
        plan->launch_body = json_object_2("launchMethod", "auto", NULL, NULL, plan->appid);
        plan->stop_route = "/kill";
        plan->stop_body = json_object_2(NULL, NULL, NULL, NULL, plan->appid);
        plan->launcher_stop_route = "/steam/stop";
    } else if (!strcmp(kind, "ubisoft") || !strcmp(kind, "rockstar") || !strcmp(kind, "battlenet")) {
        char key[32];
        char* pipeline;
        plan->appid = !strcmp(kind, "ubisoft")    ? ms_ubisoft_appid(id)
                      : !strcmp(kind, "rockstar") ? ms_rockstar_appid(id)
                                                  : ms_battlenet_appid(id);
        snprintf(key, sizeof(key), "%s_id", kind);
        /* The launch routes default to the store's route; the game's own is passed. */
        pipeline = saved_store_pipeline(home, kind, plan->appid);
        plan->launch_route = !strcmp(kind, "ubisoft")    ? "/ubisoft/launch-game"
                             : !strcmp(kind, "rockstar") ? "/rockstar/launch-game"
                                                         : "/battlenet/launch-game";
        plan->launch_body = json_object_2(key, id, "pipeline", pipeline, plan->appid);
        free(pipeline);
        plan->stop_route = "/kill";
        plan->stop_body = json_object_2(NULL, NULL, NULL, NULL, plan->appid);
        plan->launcher_stop_route = !strcmp(kind, "ubisoft")    ? "/ubisoft/stop"
                                    : !strcmp(kind, "rockstar") ? "/rockstar/stop"
                                                                : "/battlenet/stop";
    } else if (!strcmp(kind, "gog")) {
        char* engine = saved_gog_engine(home, id);
        plan->launch_route = "/sharp-library/gog/play";
        plan->launch_body = json_object_2("productId", id, "engine", engine, 0);
        free(engine);
        plan->stop_route = "/sharp-library/gog/stop";
        plan->stop_body = json_object_2("productId", id, NULL, NULL, 0);
    } else {
        plan->launch_route = "/sharp-library/epic/play";
        plan->launch_body = json_object_2("appName", id, NULL, NULL, 0);
        plan->stop_route = "/sharp-library/epic/stop";
        plan->stop_body = json_object_2("appName", id, NULL, NULL, 0);
    }
    if (!plan->launch_body || !plan->stop_body) {
        ms_packed_app_plan_free(plan);
        return false;
    }
    return true;
}

void ms_packed_app_plan_free(ms_packed_app_plan* plan) {
    if (!plan)
        return;
    free(plan->launch_body);
    free(plan->stop_body);
    plan->launch_body = NULL;
    plan->stop_body = NULL;
}

/* Whether a result's "ok" is true. */
static bool result_ok(const char* result) {
    ms_json* json = result ? ms_json_parse(result, strlen(result), NULL, 0) : NULL;
    bool ok = false;
    if (json)
        (void)ms_json_as_bool(ms_json_object_get(json, "ok"), &ok);
    ms_json_free(json);
    return ok;
}

/* One of the plan's routes, as backend.c dispatches it. */
static char* call_route(const char* home, const char* route, const char* body, int* status) {
    size_t length = body ? strlen(body) : 0;
    char* result = NULL;
    int code = 500;
    if (!strcmp(route, "/steam/launch-game"))
        result = ms_steam_launch_game_json(home, body, length, &code);
    else if (!strcmp(route, "/ubisoft/launch-game"))
        result = ms_ubisoft_launch_game_json(home, body, length, &code);
    else if (!strcmp(route, "/rockstar/launch-game"))
        result = ms_rockstar_launch_game_json(home, body, length, &code);
    else if (!strcmp(route, "/battlenet/launch-game"))
        result = ms_battlenet_launch_game_json(home, body, length, &code);
    else if (!strcmp(route, "/sharp-library/gog/play") || !strcmp(route, "/sharp-library/gog/stop")) {
        result = ms_gog_action_json(home, strrchr(route, '/') + 1, (const unsigned char*)body, length);
        code = result_ok(result) ? 200 : 500;
    } else if (!strcmp(route, "/sharp-library/epic/play")) {
        result = ms_epic_launch_json(home, (const unsigned char*)body, length);
        code = result_ok(result) ? 200 : 500;
    } else if (!strcmp(route, "/sharp-library/epic/stop")) {
        result = ms_epic_stop_json(home, (const unsigned char*)body, length);
        code = result_ok(result) ? 200 : 500;
    } else if (!strcmp(route, "/kill"))
        result = ms_process_kill_json(home, body, length, &code);
    else if (!strcmp(route, "/steam/stop"))
        result = ms_steam_stop_json(home, &code);
    else if (!strcmp(route, "/ubisoft/stop"))
        result = ms_ubisoft_stop_json(home, &code);
    else if (!strcmp(route, "/rockstar/stop"))
        result = ms_rockstar_stop_json(home, &code);
    else if (!strcmp(route, "/battlenet/stop"))
        result = ms_battlenet_stop_json(home, &code);
    if (!result) {
        code = 500;
        result = error_json("Out of memory");
    }
    if (status)
        *status = code;
    return result;
}

char* ms_packed_app_launch_json(const char* home, const char* body, size_t length, int* status) {
    game_ref ref;
    ms_packed_app_plan plan;
    char* result;
    char message[256];
    if (!request_ref(NULL, body, length, &ref))
        return fail(status, 400, "A game kind and id are required");
    if (!ms_packed_app_plan_for(home, ref.kind, ref.id, &plan))
        return fail(status, 500, "Out of memory");
    snprintf(message, sizeof(message), "Packed game app: launching %s %s", ref.kind, ref.id);
    ms_log_event(home, message);
    if (plan.start_steam && (result = ms_steam_ensure_running_json(home, status)) != NULL) {
        ms_packed_app_plan_free(&plan);
        return result;
    }
    result = call_route(home, plan.launch_route, plan.launch_body, status);
    ms_packed_app_plan_free(&plan);
    return result;
}

/* Whether GET /game/running lists appid. */
static bool appid_running(const char* home, unsigned appid) {
    char* text = ms_process_running_json(home);
    ms_json* json = text ? ms_json_parse(text, strlen(text), NULL, 0) : NULL;
    const ms_json* list = json ? ms_json_object_get(json, "running") : NULL;
    bool running = false;
    for (size_t i = 0; list && i < ms_json_array_length(list) && !running; i++) {
        long long value = 0;
        running = ms_json_as_i64(ms_json_object_get(ms_json_array_get(list, i), "appid"), &value) &&
                  (unsigned long long)value == appid;
    }
    ms_json_free(json);
    free(text);
    return running;
}

static bool epic_running(const char* home, const char* app_name) {
    char* text = ms_epic_running_json(home);
    ms_json* json = text ? ms_json_parse(text, strlen(text), NULL, 0) : NULL;
    const ms_json* list = json ? ms_json_object_get(json, "running") : NULL;
    bool running = false;
    for (size_t i = 0; list && i < ms_json_array_length(list) && !running; i++) {
        char* name = NULL;
        running = ms_json_as_string(ms_json_array_get(list, i), &name) && !strcmp(name, app_name);
        free(name);
    }
    ms_json_free(json);
    free(text);
    return running;
}

char* ms_packed_app_running_json(const char* home, const char* query, const char* body, size_t length, int* status) {
    game_ref ref;
    ms_packed_app_plan plan;
    bool running;
    ms_json_writer w;
    if (!request_ref(query, body, length, &ref))
        return fail(status, 400, "A game kind and id are required");
    if (!ms_packed_app_plan_for(home, ref.kind, ref.id, &plan))
        return fail(status, 500, "Out of memory");
    running = !strcmp(ref.kind, "gog")    ? ms_gog_game_running(home, ref.id)
              : !strcmp(ref.kind, "epic") ? epic_running(home, ref.id)
                                          : appid_running(home, plan.appid);
    ms_packed_app_plan_free(&plan);
    if (status)
        *status = 200;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, running);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_packed_app_stop_json(const char* home, const char* body, size_t length, int* status) {
    game_ref ref;
    ms_packed_app_plan plan;
    char *game_result, *launcher_result = NULL;
    int game_status = 500, launcher_status = 200;
    ms_json_writer w;
    char message[256];
    if (!request_ref(NULL, body, length, &ref))
        return fail(status, 400, "A game kind and id are required");
    if (!ms_packed_app_plan_for(home, ref.kind, ref.id, &plan))
        return fail(status, 500, "Out of memory");
    snprintf(message, sizeof(message), "Packed game app: stopping %s %s", ref.kind, ref.id);
    ms_log_event(home, message);
    /* The game may be gone already (closed from inside it): its launcher still stops. */
    game_result = call_route(home, plan.stop_route, plan.stop_body, &game_status);
    if (plan.launcher_stop_route)
        launcher_result = call_route(home, plan.launcher_stop_route, NULL, &launcher_status);
    ms_packed_app_plan_free(&plan);
    if (status)
        *status = launcher_status < 400 ? 200 : launcher_status;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, launcher_status < 400);
    ms_json_writer_key(&w, "gameStopped");
    ms_json_writer_bool(&w, game_status < 400 && result_ok(game_result));
    ms_json_writer_key(&w, "launcherStopped");
    ms_json_writer_bool(&w, launcher_result != NULL && launcher_status < 400);
    if (launcher_status >= 400) {
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, "Could not stop the game's launcher");
    }
    ms_json_writer_object_end(&w);
    free(game_result);
    free(launcher_result);
    return ms_json_writer_take(&w);
}
