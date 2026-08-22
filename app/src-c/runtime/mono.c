#include "metalsharp_backend/mono.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool g_downloading;
static char* join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    int s = x && a[x - 1] != '/' ? 1 : 0;
    char* p = malloc(x + y + s + 1);
    if (p)
        snprintf(p, x + y + s + 1, "%s%s%s", a, s ? "/" : "", b);
    return p;
}
static const char* prefix_kind(const char* body, size_t len) {
    char er[64];
    ms_json* j = ms_json_parse(body ? body : "", body ? len : 0, er, sizeof(er));
    char* s = NULL;
    const ms_json* v = j ? ms_json_object_get(j, "prefix") : NULL;
    if (v)
        ms_json_as_string(v, &s);
    ms_json_free(j);
    if (s && (!strcmp(s, "gog") || !strcmp(s, "steam"))) {
        const char* out = !strcmp(s, "steam") ? "steam" : "gog";
        free(s);
        return out;
    }
    free(s);
    return "gog";
}
static char* status(const char* home, const char* kind) {
    char* p = join(home, !strcmp(kind, "steam") ? "prefix-steam" : "bottles/gog-prefix/prefix");
    char* mono = p ? join(p, "drive_c/windows/mono") : NULL;
    bool installed = mono && access(mono, F_OK) == 0;
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "prefixKind");
    ms_json_writer_string(&w, kind);
    ms_json_writer_key(&w, "latestVersion");
    ms_json_writer_string(&w, "11.2.0");
    ms_json_writer_key(&w, "installedVersion");
    if (installed)
        ms_json_writer_string(&w, "11.2.0");
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, installed);
    ms_json_writer_key(&w, "upToDate");
    ms_json_writer_bool(&w, installed);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "stalled");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "pid");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "startedAt");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "elapsedSeconds");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "logPath");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "targetVersion");
    ms_json_writer_string(&w, "11.2.0");
    ms_json_writer_key(&w, "lastError");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "msiCached");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "downloading");
    ms_json_writer_bool(&w, g_downloading);
    ms_json_writer_key(&w, "downloadBytes");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_key(&w, "downloadTotal");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_key(&w, "downloadError");
    ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(p);
    free(mono);
    return o;
}
char* ms_mono_status_json(const char* home, const char* kind) {
    return status(home, !strcmp(kind, "steam") ? "steam" : "gog");
}
char* ms_mono_install_json(const char* home, const char* body, size_t len) {
    const char* kind = prefix_kind(body, len);
    g_downloading = true;
    char* s = status(home, kind);
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "downloading");
    ms_json_writer_bool(&w, g_downloading);
    ms_json_writer_key(&w, "status");
    ms_json_writer_raw(&w, s ? s : "{}");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(s);
    return o;
}
char* ms_mono_reset_json(const char* home, const char* body, size_t len) {
    const char* kind = prefix_kind(body, len);
    char* s = status(home, kind);
    char* prefix_path = join(home, !strcmp(kind, "steam") ? "prefix-steam" : "bottles/gog-prefix/prefix");
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "killedProcesses");
    ms_json_writer_u64(&w, 0);
    ms_json_writer_key(&w, "prefix");
    ms_json_writer_string(&w, prefix_path ? prefix_path : "");
    ms_json_writer_key(&w, "status");
    ms_json_writer_raw(&w, s ? s : "{}");
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(prefix_path);
    free(s);
    return o;
}
