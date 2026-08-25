#include "metalsharp_backend/diagnostics.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char base_flavor[16];
    bool steamworks_net, csteamworks, faudio, fmod, openal, xinput;
    bool managed_dir, x86_mono, native_mono;
    char evidence[24][96];
    size_t evidence_count;
} fna_signals;

static bool contains_ci(const char* text, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0)
        return true;
    for (; *text; text++) {
        size_t i;
        for (i = 0; i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i]); i++)
            ;
        if (i == n)
            return true;
    }
    return false;
}

static void ensure_diagnostic_directory(const char* path) {
    char copy[2048];
    char* cursor;
    if (!path || strlen(path) >= sizeof(copy))
        return;
    snprintf(copy, sizeof(copy), "%s", path);
    for (cursor = copy + 1; *cursor; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            (void)mkdir(copy, 0755);
            *cursor = '/';
        }
    }
    (void)mkdir(copy, 0755);
}

static void evidence(fna_signals* s, const char* value) {
    size_t i;
    if (!value || s->evidence_count >= sizeof(s->evidence) / sizeof(s->evidence[0]))
        return;
    for (i = 0; i < s->evidence_count; i++)
        if (!strcmp(s->evidence[i], value))
            return;
    snprintf(s->evidence[s->evidence_count++], sizeof(s->evidence[0]), "%s", value);
}

static char* query_value(const char* query, const char* key) {
    const char* p = query;
    size_t key_len = strlen(key);
    char* out;
    size_t n = 0, i;
    if (!query)
        return NULL;
    while (*p) {
        if ((p == query || p[-1] == '&') && !strncmp(p, key, key_len) && p[key_len] == '=')
            break;
        p = strchr(p, '&');
        if (!p)
            return NULL;
        p++;
    }
    p += key_len + 1;
    while (p[n] && p[n] != '&')
        n++;
    out = malloc(n + 1);
    if (!out)
        return NULL;
    {
        size_t j = 0;
        for (i = 0; i < n; i++) {
            if (p[i] == '%' && i + 2 < n && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
                char hex[3] = {p[i + 1], p[i + 2], 0};
                out[j++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else {
                out[j++] = p[i] == '+' ? ' ' : p[i];
            }
        }
        out[j] = 0;
    }
    return out;
}

static void detect_fna(const char* game_dir, fna_signals* s) {
    DIR* root;
    struct dirent* entry;
    memset(s, 0, sizeof(*s));
    snprintf(s->base_flavor, sizeof(s->base_flavor), "unknown");
    root = game_dir ? opendir(game_dir) : NULL;
    if (root) {
        while ((entry = readdir(root)) != NULL) {
            char* data;
            struct stat st;
            if (entry->d_name[0] == '.')
                continue;
            data = malloc(strlen(game_dir) + strlen(entry->d_name) + 10);
            if (!data)
                continue;
            snprintf(data, strlen(game_dir) + strlen(entry->d_name) + 10, "%s/%s", game_dir, entry->d_name);
            if (stat(data, &st) != 0) {
                free(data);
                continue;
            }
            if (S_ISDIR(st.st_mode) && strlen(entry->d_name) > 5 &&
                !strcasecmp(entry->d_name + strlen(entry->d_name) - 5, "_data")) {
                char* managed = malloc(strlen(data) + 9);
                DIR* md;
                struct dirent* dll;
                if (!managed) {
                    free(data);
                    continue;
                }
                snprintf(managed, strlen(data) + 9, "%s/Managed", data);
                md = opendir(managed);
                if (md) {
                    s->managed_dir = true;
                    while ((dll = readdir(md)) != NULL) {
                        const char* name = dll->d_name;
                        if (name[0] == '.' || !contains_ci(name, ".dll"))
                            continue;
                        if (!strcasecmp(name, "FNA.dll")) {
                            snprintf(s->base_flavor, sizeof(s->base_flavor), "fna");
                            evidence(s, "FNA.dll");
                        } else if (!strncasecmp(name, "MonoGame", 8) || contains_ci(name, "mg.framework")) {
                            if (!strcmp(s->base_flavor, "unknown"))
                                snprintf(s->base_flavor, sizeof(s->base_flavor), "monogame");
                        } else if (!strncasecmp(name, "Microsoft.Xna.Framework", 23) &&
                                   !strcmp(s->base_flavor, "unknown")) {
                            snprintf(s->base_flavor, sizeof(s->base_flavor), "xna");
                        }
                        if (!strcasecmp(name, "Steamworks.NET.dll")) {
                            s->steamworks_net = true;
                            evidence(s, name);
                        }
                        if (!strcasecmp(name, "CSteamworks.dll")) {
                            s->csteamworks = true;
                            evidence(s, name);
                        }
                        if (contains_ci(name, "faudio")) {
                            s->faudio = true;
                            evidence(s, "FAudio");
                        }
                        if (contains_ci(name, "fmod")) {
                            s->fmod = true;
                            evidence(s, "FMOD");
                        }
                        if (contains_ci(name, "openal")) {
                            s->openal = true;
                            evidence(s, "OpenAL");
                        }
                        if (contains_ci(name, "xinput")) {
                            s->xinput = true;
                            evidence(s, "XInput");
                        }
                    }
                    closedir(md);
                }
                free(managed);
            }
            if (S_ISDIR(st.st_mode) && !strcasecmp(entry->d_name, "x86")) {
                s->x86_mono = true;
                evidence(s, "x86/");
            }
            if (S_ISDIR(st.st_mode) && strlen(entry->d_name) > 4 &&
                !strcasecmp(entry->d_name + strlen(entry->d_name) - 4, ".app")) {
                s->native_mono = true;
                evidence(s, "arm64 macOS executable");
            }
            free(data);
        }
        closedir(root);
    }
    if (game_dir) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/FNA.dll", game_dir);
        if (access(path, F_OK) == 0 && !strcmp(s->base_flavor, "unknown")) {
            snprintf(s->base_flavor, sizeof(s->base_flavor), "fna");
            evidence(s, "FNA.dll");
        }
        snprintf(path, sizeof(path), "%s/libFAudio.dylib", game_dir);
        if (access(path, F_OK) == 0)
            s->faudio = true;
        snprintf(path, sizeof(path), "%s/libfmod.dylib", game_dir);
        if (access(path, F_OK) == 0)
            s->fmod = true;
        snprintf(path, sizeof(path), "%s/libopenal.dylib", game_dir);
        if (access(path, F_OK) == 0)
            s->openal = true;
    }
    if (!s->native_mono && strcmp(s->base_flavor, "unknown"))
        s->x86_mono = true;
}

static void write_signals(ms_json_writer* w, const fna_signals* s) {
    size_t i;
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "base_flavor");
    ms_json_writer_string(w, s->base_flavor);
    ms_json_writer_key(w, "uses_steamworks_net");
    ms_json_writer_bool(w, s->steamworks_net);
    ms_json_writer_key(w, "uses_csteamworks");
    ms_json_writer_bool(w, s->csteamworks);
    ms_json_writer_key(w, "uses_faudio");
    ms_json_writer_bool(w, s->faudio);
    ms_json_writer_key(w, "uses_fmod");
    ms_json_writer_bool(w, s->fmod);
    ms_json_writer_key(w, "uses_openal");
    ms_json_writer_bool(w, s->openal);
    ms_json_writer_key(w, "uses_xinput");
    ms_json_writer_bool(w, s->xinput);
    ms_json_writer_key(w, "has_managed_dir");
    ms_json_writer_bool(w, s->managed_dir);
    ms_json_writer_key(w, "indicates_x86_mono");
    ms_json_writer_bool(w, s->x86_mono);
    ms_json_writer_key(w, "indicates_native_mono");
    ms_json_writer_bool(w, s->native_mono);
    ms_json_writer_key(w, "evidence_files");
    ms_json_writer_array_begin(w);
    for (i = 0; i < s->evidence_count; i++)
        ms_json_writer_string(w, s->evidence[i]);
    ms_json_writer_array_end(w);
    ms_json_writer_object_end(w);
}

static char* sha256_file(const char* path) {
    FILE* file = fopen(path, "rb");
    CC_SHA256_CTX context;
    unsigned char buffer[8192], digest[CC_SHA256_DIGEST_LENGTH];
    size_t count;
    bool read_error;
    char* output;
    size_t i;
    if (!file || CC_SHA256_Init(&context) != 1) {
        if (file)
            fclose(file);
        return NULL;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        CC_SHA256_Update(&context, buffer, (CC_LONG)count);
    read_error = ferror(file) != 0;
    fclose(file);
    if (read_error || CC_SHA256_Final(digest, &context) != 1)
        return NULL;
    output = malloc(CC_SHA256_DIGEST_LENGTH * 2 + 1);
    if (!output)
        return NULL;
    for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(output + i * 2, 3, "%02x", digest[i]);
    output[CC_SHA256_DIGEST_LENGTH * 2] = 0;
    return output;
}

static char* read_file_text(const char* path) {
    FILE* file = fopen(path, "rb");
    long length;
    size_t got;
    char* text;
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file)
            fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)length + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    got = fread(text, 1, (size_t)length, file);
    fclose(file);
    text[got] = 0;
    return text;
}

static void artifact_group(ms_json_writer* w, const char* home, const char* label, const char* const* unix_names,
                           size_t unix_count, const char* const* pe_names, size_t pe_count, bool* all_present) {
    size_t i;
    char path[2048];
    const char* subdirs[] = {"x86_64-unix", "x86_64-windows"};
    const char* const* lists[] = {unix_names, pe_names};
    size_t counts[] = {unix_count, pe_count};
    size_t group;
    bool group_present = true;
    ms_json_writer_key(w, label);
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "entries");
    ms_json_writer_array_begin(w);
    for (group = 0; group < 2; group++) {
        for (i = 0; i < counts[group]; i++) {
            struct stat st;
            bool present;
            char* hash;
            snprintf(path, sizeof(path), "%s/runtime/wine/lib/%s/%s/%s", home, label, subdirs[group], lists[group][i]);
            present = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
            if (!present) {
                group_present = false;
                *all_present = false;
            }
            ms_json_writer_object_begin(w);
            ms_json_writer_key(w, "label");
            ms_json_writer_string(w, label);
            ms_json_writer_key(w, "subdir");
            ms_json_writer_string(w, subdirs[group]);
            ms_json_writer_key(w, "filename");
            ms_json_writer_string(w, lists[group][i]);
            ms_json_writer_key(w, "path");
            ms_json_writer_string(w, path);
            ms_json_writer_key(w, "present");
            ms_json_writer_bool(w, present);
            hash = present ? sha256_file(path) : NULL;
            ms_json_writer_key(w, "sha256");
            if (hash)
                ms_json_writer_string(w, hash);
            else
                ms_json_writer_null(w);
            ms_json_writer_key(w, "size_bytes");
            if (present)
                ms_json_writer_u64(w, (unsigned long long)st.st_size);
            else
                ms_json_writer_null(w);
            ms_json_writer_object_end(w);
            free(hash);
        }
    }
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "all_present");
    ms_json_writer_bool(w, group_present);
    ms_json_writer_object_end(w);
}

static char* runtime_artifact_report(int* status) {
    static const char* const unix_m11[] = {"winemetal.so"};
    static const char* const unix_m12[] = {"winemetal.so", "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib"};
    static const char* const pe[] = {"d3d10core.dll", "d3d11.dll",     "d3d12.dll",   "dxgi.dll",
                                     "dxgi_dxmt.dll", "winemetal.dll", "nvapi64.dll", "nvngx.dll"};
    const char* home = getenv("METALSHARP_HOME");
    ms_json_writer w;
    bool all_present = true;
    if (!home || !*home)
        home = getenv("HOME");
    if (!home || !*home) {
        if (status)
            *status = 200;
        return strdup("{\"ok\":false,\"error\":\"home directory could not be resolved\"}");
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_u64(&w, 1);
    artifact_group(&w, home, "dxmt", unix_m11, 1, pe, sizeof(pe) / sizeof(pe[0]), &all_present);
    artifact_group(&w, home, "dxmt_m12", unix_m12, sizeof(unix_m12) / sizeof(unix_m12[0]), pe,
                   sizeof(pe) / sizeof(pe[0]), &all_present);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, all_present);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static char* contract_error(const char* message) {
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

static bool sqlite_capture(const char* database, const char* sql, char* output, size_t output_size) {
    int pipes[2];
    pid_t pid;
    int status;
    size_t used = 0;
    if (!output || output_size < 2 || pipe(pipes) != 0)
        return false;
    pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return false;
    }
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execl("/usr/bin/sqlite3", "sqlite3", "-batch", database, sql, (char*)NULL);
        execl("/opt/homebrew/bin/sqlite3", "sqlite3", "-batch", database, sql, (char*)NULL);
        _exit(127);
    }
    close(pipes[1]);
    while (used + 1 < output_size) {
        ssize_t got = read(pipes[0], output + used, output_size - used - 1);
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    close(pipes[0]);
    waitpid(pid, &status, 0);
    output[used] = 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static unsigned long long sqlite_entry_count(const char* database) {
    char tables[8192], *line, *save = NULL;
    unsigned long long total = 0;
    if (!sqlite_capture(database, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'cache_%';", tables,
                        sizeof(tables)))
        return 0;
    for (line = strtok_r(tables, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char sql[256], count[128];
        bool safe = !strncmp(line, "cache_", 6);
        for (const char* p = line + 6; safe && *p; p++)
            if (!(isalnum((unsigned char)*p) || *p == '_'))
                safe = false;
        if (!safe)
            continue;
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\";", line);
        if (sqlite_capture(database, sql, count, sizeof(count)))
            total += strtoull(count, NULL, 10);
    }
    return total;
}

static void cache_dir_summary(ms_json_writer* w, const char* path) {
    DIR* dir = opendir(path);
    struct dirent* entry;
    unsigned long long total_size = 0;
    unsigned long long newest = 0, oldest = 0, total_entries = 0;
    size_t db_count = 0;

    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "path");
    ms_json_writer_string(w, path);
    ms_json_writer_key(w, "exists");
    ms_json_writer_bool(w, dir != NULL);
    ms_json_writer_key(w, "db_files");
    ms_json_writer_array_begin(w);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            size_t n = strlen(entry->d_name);
            char full[2048];
            struct stat st;
            if (n < 3 || strcmp(entry->d_name + n - 3, ".db") != 0)
                continue;
            snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            db_count++;
            total_size += (unsigned long long)st.st_size;
            if (newest == 0 || (unsigned long long)st.st_mtime > newest)
                newest = (unsigned long long)st.st_mtime;
            if (oldest == 0 || (unsigned long long)st.st_mtime < oldest)
                oldest = (unsigned long long)st.st_mtime;
            ms_json_writer_object_begin(w);
            ms_json_writer_key(w, "name");
            ms_json_writer_string(w, entry->d_name);
            ms_json_writer_key(w, "path");
            ms_json_writer_string(w, full);
            ms_json_writer_key(w, "size_bytes");
            ms_json_writer_u64(w, (unsigned long long)st.st_size);
            ms_json_writer_key(w, "mtime_unix");
            ms_json_writer_u64(w, (unsigned long long)st.st_mtime);
            unsigned long long entries = sqlite_entry_count(full);
            total_entries += entries;
            ms_json_writer_key(w, "entry_count");
            ms_json_writer_u64(w, entries);
            ms_json_writer_object_end(w);
        }
        closedir(dir);
    }
    ms_json_writer_array_end(w);
    ms_json_writer_key(w, "total_entries");
    ms_json_writer_u64(w, total_entries);
    ms_json_writer_key(w, "newest_mtime_unix");
    if (newest)
        ms_json_writer_u64(w, newest);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "oldest_mtime_unix");
    if (oldest)
        ms_json_writer_u64(w, oldest);
    else
        ms_json_writer_null(w);
    ms_json_writer_object_end(w);
    (void)db_count;
    (void)total_size;
}

static char* cache_doctor_report(const char* query, int* status) {
    char* app = query_value(query, "appid");
    char* requested = query_value(query, "pipeline");
    unsigned long appid = app ? strtoul(app, NULL, 10) : 0;
    const char* home = getenv("METALSHARP_HOME");
    const char* pipeline = "auto";
    char shader[2048], pipeline_cache[2048];
    ms_json_writer w;
    if (status)
        *status = 200;
    if (!home || !*home)
        home = getenv("HOME");
    if (!home)
        home = "";
    if (requested && *requested) {
        if (!strcasecmp(requested, "m12"))
            pipeline = "m12";
        else if (!strcasecmp(requested, "m13"))
            pipeline = "m13";
        else if (!strcasecmp(requested, "m11"))
            pipeline = "m11";
        else if (!strcasecmp(requested, "m10"))
            pipeline = "m10";
        else if (!strcasecmp(requested, "m9"))
            pipeline = "m9";
        else if (!strcasecmp(requested, "vkd3d"))
            pipeline = "vkd3d";
    }
    if (!strcmp(pipeline, "auto") || !strcmp(pipeline, "vkd3d")) {
        snprintf(shader, sizeof(shader), "%s/shader-cache/%lu", home, appid);
        snprintf(pipeline_cache, sizeof(pipeline_cache), "%s/pipeline-cache/%lu", home, appid);
    } else {
        snprintf(shader, sizeof(shader), "%s/shader-cache/%s/%lu", home, pipeline, appid);
        snprintf(pipeline_cache, sizeof(pipeline_cache), "%s/pipeline-cache/%s/%lu", home, pipeline, appid);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "cache_family");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "shader_cache");
    cache_dir_summary(&w, shader);
    ms_json_writer_key(&w, "pipeline_cache");
    cache_dir_summary(&w, pipeline_cache);
    ms_json_writer_key(&w, "runtime_artifact_hash");
    ms_json_writer_null(&w);
    ms_json_writer_key(&w, "stale_warning");
    ms_json_writer_null(&w);
    ms_json_writer_object_end(&w);
    free(app);
    free(requested);
    return ms_json_writer_take(&w);
}

typedef struct {
    char* raw;
    unsigned long long recorded_at;
} pso_entry;

static int pso_entry_compare(const void* left, const void* right) {
    const pso_entry* a = left;
    const pso_entry* b = right;
    return a->recorded_at < b->recorded_at ? 1 : a->recorded_at > b->recorded_at ? -1 : 0;
}

static char* pso_manifest_report(const char* query, int* status) {
    char* app = query_value(query, "appid");
    char* requested = query_value(query, "pipeline");
    char* limit_value = query_value(query, "limit");
    unsigned long appid = app ? strtoul(app, NULL, 10) : 0;
    unsigned long limit = limit_value ? strtoul(limit_value, NULL, 10) : 20;
    const char* home = getenv("METALSHARP_HOME");
    const char* pipeline = "vkd3d";
    if (requested) {
        if (!strcasecmp(requested, "m12"))
            pipeline = "m12";
        else if (!strcasecmp(requested, "m13"))
            pipeline = "m13";
        else if (!strcasecmp(requested, "m11"))
            pipeline = "m11";
        else if (!strcasecmp(requested, "m10"))
            pipeline = "m10";
        else if (!strcasecmp(requested, "m9"))
            pipeline = "m9";
    }
    char directory[2048];
    DIR* dir;
    struct dirent* entry;
    ms_json_writer w;
    pso_entry entries[256];
    size_t entry_count = 0, count = 0;

    if (status)
        *status = 200;
    if (limit > 200)
        limit = 200;
    if (!home || !*home)
        home = getenv("HOME");
    if (!home)
        home = "";
    if (!strcmp(pipeline, "vkd3d"))
        snprintf(directory, sizeof(directory), "%s/pipeline-cache/%lu", home, appid);
    else
        snprintf(directory, sizeof(directory), "%s/pipeline-cache/%s/%lu", home, pipeline, appid);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "manifests");
    ms_json_writer_array_begin(&w);
    dir = opendir(directory);
    if (dir) {
        while (entry_count < 256 && (entry = readdir(dir)) != NULL) {
            size_t n = strlen(entry->d_name);
            char path[2048];
            char* raw;
            char parse_error[96];
            ms_json* parsed;
            if (n < 9 || strncmp(entry->d_name, "pso-", 4) != 0 || strcmp(entry->d_name + n - 5, ".json") != 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            raw = read_file_text(path);
            parsed = raw ? ms_json_parse(raw, strlen(raw), parse_error, sizeof(parse_error)) : NULL;
            if (parsed && ms_json_type_of(parsed) == MS_JSON_OBJECT) {
                long long recorded = 0;
                ms_json_as_i64(ms_json_object_get(parsed, "recorded_at_unix"), &recorded);
                entries[entry_count].raw = raw;
                entries[entry_count].recorded_at = recorded > 0 ? (unsigned long long)recorded : 0;
                entry_count++;
                raw = NULL;
            }
            ms_json_free(parsed);
            free(raw);
        }
        closedir(dir);
    }
    qsort(entries, entry_count, sizeof(entries[0]), pso_entry_compare);
    for (count = 0; count < entry_count && count < limit; count++)
        ms_json_writer_raw(&w, entries[count].raw);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "count");
    ms_json_writer_u64(&w, count);
    ms_json_writer_object_end(&w);
    free(app);
    free(requested);
    free(limit_value);
    for (size_t i = 0; i < entry_count; i++)
        free(entries[i].raw);
    return ms_json_writer_take(&w);
}

typedef struct {
    const char* subpath;
    const char* filename;
    bool optional;
} launch_artifact;

static char* launch_preferred_pipeline(const char* home, unsigned long appid) {
    char path[2048], *raw = NULL, *preferred = NULL, error[96];
    FILE* file;
    long length;
    ms_json* parsed;
    snprintf(path, sizeof(path), "%s/bottles/steam_%lu/bottle.json", home, appid);
    file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file)
            fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    raw = malloc((size_t)length + 1);
    if (!raw || fread(raw, 1, (size_t)length, file) != (size_t)length) {
        free(raw);
        fclose(file);
        return NULL;
    }
    fclose(file);
    raw[length] = 0;
    parsed = ms_json_parse(raw, (size_t)length, error, sizeof(error));
    if (parsed && ms_json_type_of(parsed) == MS_JSON_OBJECT) {
        char* value = NULL;
        if (ms_json_as_string(ms_json_object_get(parsed, "preferred_pipeline"), &value) && value && *value)
            preferred = value;
        else
            free(value);
    }
    ms_json_free(parsed);
    free(raw);
    return preferred;
}

static void write_manifest_value(ms_json_writer* w, const ms_json* object, const char* key) {
    const ms_json* value = ms_json_object_get(object, key);
    char* raw = value ? ms_json_stringify(value) : NULL;
    if (raw) {
        ms_json_writer_raw(w, raw);
        free(raw);
    } else
        ms_json_writer_null(w);
}

static void write_staged_dll_hashes(ms_json_writer* w, const char* home, unsigned long appid) {
    char path[2048], error[96];
    char* raw = NULL;
    ms_json* manifest = NULL;
    snprintf(path, sizeof(path), "%s/games/%lu/.metalsharp/injections.json", home, appid);
    raw = read_file_text(path);
    if (raw)
        manifest = ms_json_parse(raw, strlen(raw), error, sizeof(error));
    ms_json_writer_array_begin(w);
    if (manifest && ms_json_type_of(manifest) == MS_JSON_OBJECT) {
        const ms_json* dlls = ms_json_object_get(manifest, "dlls");
        if (dlls && ms_json_type_of(dlls) == MS_JSON_ARRAY)
            for (size_t i = 0; i < ms_json_array_length(dlls); i++) {
                const ms_json* dll = ms_json_array_get(dlls, i);
                char *filename = NULL, *dest = NULL, *source = NULL;
                ms_json_as_string(ms_json_object_get(dll, "filename"), &filename);
                ms_json_as_string(ms_json_object_get(dll, "dest_path"), &dest);
                ms_json_as_string(ms_json_object_get(dll, "source_path"), &source);
                if (!filename || !dest) {
                    free(filename);
                    free(dest);
                    free(source);
                    continue;
                }
                char* hash = access(dest, F_OK) == 0 ? sha256_file(dest) : NULL;
                char* source_hash = source && access(source, F_OK) == 0 ? sha256_file(source) : NULL;
                ms_json_writer_object_begin(w);
                ms_json_writer_key(w, "filename");
                ms_json_writer_string(w, filename);
                ms_json_writer_key(w, "dest_path");
                ms_json_writer_string(w, dest);
                ms_json_writer_key(w, "present");
                ms_json_writer_bool(w, hash != NULL);
                ms_json_writer_key(w, "sha256");
                if (hash)
                    ms_json_writer_string(w, hash);
                else
                    ms_json_writer_null(w);
                ms_json_writer_key(w, "source_sha256");
                if (source_hash)
                    ms_json_writer_string(w, source_hash);
                else
                    ms_json_writer_null(w);
                ms_json_writer_key(w, "matches_source");
                if (hash && source_hash)
                    ms_json_writer_bool(w, !strcmp(hash, source_hash));
                else
                    ms_json_writer_null(w);
                ms_json_writer_key(w, "manifest_pipeline");
                write_manifest_value(w, manifest, "pipeline");
                ms_json_writer_key(w, "manifest_pipeline_name");
                write_manifest_value(w, manifest, "pipeline_name");
                ms_json_writer_key(w, "manifest_updated_at_unix");
                write_manifest_value(w, manifest, "updated_at_unix");
                ms_json_writer_object_end(w);
                free(filename);
                free(dest);
                free(source);
                free(hash);
                free(source_hash);
            }
    }
    ms_json_writer_array_end(w);
    ms_json_free(manifest);
    free(raw);
}

static char* launch_bundle_hash(const char* home) {
    const char* suffixes[] = {"runtime/bundle-hash.txt", "runtime/wine/bundle-hash.txt", "bundle-hash.txt"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char path[2048];
        char* raw;
        snprintf(path, sizeof(path), "%s/%s", home, suffixes[i]);
        raw = read_file_text(path);
        if (raw) {
            char* start = raw;
            while (*start && isspace((unsigned char)*start))
                start++;
            char* end = start + strlen(start);
            while (end > start && isspace((unsigned char)end[-1]))
                *--end = 0;
            if (*start) {
                char* result = strdup(start);
                free(raw);
                return result;
            }
            free(raw);
        }
    }
    return NULL;
}

static char* launch_diagnostic_report(const char* query, int* status) {
    static const launch_artifact m12_artifacts[] = {{"lib/dxmt_m12/x86_64-windows", "d3d12.dll", false},
                                                    {"lib/dxmt_m12/x86_64-windows", "d3d11.dll", false},
                                                    {"lib/dxmt_m12/x86_64-windows", "dxgi.dll", false},
                                                    {"lib/dxmt_m12/x86_64-windows", "dxgi_dxmt.dll", false},
                                                    {"lib/dxmt_m12/x86_64-windows", "d3d10core.dll", false},
                                                    {"lib/dxmt_m12/x86_64-windows", "winemetal.dll", false},
                                                    {"lib/dxmt_m12/x86_64-windows", "nvapi64.dll", true},
                                                    {"lib/dxmt_m12/x86_64-windows", "nvngx.dll", true}};
    static const launch_artifact vkd3d_artifacts[] = {
        {"vkd3d-proton/x86_64-windows", "d3d12.dll", false}, {"vkd3d-proton/x86_64-windows", "d3d12core.dll", false},
        {"dxvk/x86_64-windows", "d3d11.dll", false},         {"dxvk/x86_64-windows", "d3d10core.dll", false},
        {"dxvk/x86_64-windows", "d3d9.dll", false},          {"dxvk/x86_64-windows", "dxgi.dll", false}};
    static const launch_artifact m11_artifacts[] = {
        {"lib/dxmt/x86_64-windows", "d3d11.dll", false},
        {"lib/dxmt/x86_64-windows", "dxgi.dll", false},
        {"lib/dxmt/x86_64-windows", "dxgi_dxmt.dll", false},
        {"lib/dxmt/x86_64-windows", "d3d10core.dll", false},
        {"lib/dxmt/x86_64-windows", "winemetal.dll", false},
        {"lib/dxmt/x86_64-windows", "nvapi64.dll", true},
        {"lib/dxmt/x86_64-windows", "nvngx.dll", true},
        {"lib/metalsharp/x86_64-windows", "metalsharp_ntdll_hook.dll", false}};
    static const launch_artifact m13_artifacts[] = {{NULL, NULL, true}};
    char* app = query_value(query, "appid");
    char* requested = query_value(query, "pipeline");
    unsigned long appid = app ? strtoul(app, NULL, 10) : 0;
    const char* home = getenv("METALSHARP_HOME");
    char* preferred = NULL;
    const char* pipeline = requested && *requested ? requested : "vkd3d";
    const launch_artifact* artifacts = vkd3d_artifacts;
    size_t artifact_count = sizeof(vkd3d_artifacts) / sizeof(vkd3d_artifacts[0]);
    const char* pipeline_name = "VKD3D";
    const char* backend = "vulkan";
    const char* graphics_backend = "vulkan";
    if (!strcasecmp(pipeline, "dxmt") || !strcasecmp(pipeline, "auto"))
        pipeline = "vkd3d";
    if (!strcasecmp(pipeline, "m12")) {
        pipeline = "m12";
        artifacts = m12_artifacts;
        artifact_count = sizeof(m12_artifacts) / sizeof(m12_artifacts[0]);
        pipeline_name = "M12";
        backend = "dxmt";
        graphics_backend = "dxmt";
    } else if (!strcasecmp(pipeline, "m13")) {
        pipeline = "m13";
        artifacts = m13_artifacts;
        artifact_count = 0;
        pipeline_name = "M13";
        backend = "gptk";
        graphics_backend = "gptk";
    } else if (!strcasecmp(pipeline, "m11") || !strcasecmp(pipeline, "m11_32")) {
        pipeline = !strcasecmp(pipeline, "m11_32") ? "m11_32" : "m11";
        artifacts = m11_artifacts;
        artifact_count = sizeof(m11_artifacts) / sizeof(m11_artifacts[0]);
        pipeline_name = !strcmp(pipeline, "m11_32") ? "M11(32)" : "M11";
        backend = "dxmt";
        graphics_backend = "dxmt";
    } else if (!strcasecmp(pipeline, "m10") || !strcasecmp(pipeline, "m10_32")) {
        pipeline = !strcasecmp(pipeline, "m10_32") ? "m10_32" : "m10";
        artifacts = m11_artifacts;
        artifact_count = sizeof(m11_artifacts) / sizeof(m11_artifacts[0]);
        pipeline_name = !strcmp(pipeline, "m10_32") ? "M10(32)" : "M10";
        backend = "dxmt";
        graphics_backend = "dxmt";
    } else if (!strcasecmp(pipeline, "m9")) {
        pipeline = "m9";
        artifacts = m13_artifacts;
        artifact_count = 0;
        pipeline_name = "M9";
        backend = "dxmt";
        graphics_backend = "dxmt";
    } else if (!strcasecmp(pipeline, "d3dmetal")) {
        pipeline = "d3dmetal";
        artifacts = m13_artifacts;
        artifact_count = 0;
        pipeline_name = "D3DMetal";
        backend = "gptk";
        graphics_backend = "gptk";
    } else {
        pipeline = "vkd3d";
    }
    char root[2048], prefix[2048], cache[2048], game_path[2048];
    char* bundle_hash = NULL;
    const char* cache_names[2] = {NULL, NULL};
    size_t cache_count = 0;
    bool missing = false;
    ms_json_writer w;
    size_t i;

    if (status)
        *status = 200;
    if (!home || !*home)
        home = getenv("HOME");
    if (!home)
        home = "";
    preferred = launch_preferred_pipeline(home, appid);
    if ((!requested || !*requested || !strcasecmp(requested, "dxmt") || !strcasecmp(requested, "auto")) && preferred) {
        free(requested);
        requested = preferred;
        preferred = NULL;
    }
    free(preferred);
    snprintf(root, sizeof(root), "%s/runtime/wine", home);
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(game_path, sizeof(game_path), "%s/games/%lu", home, appid);
    bundle_hash = launch_bundle_hash(home);
    if (!strcmp(pipeline, "m12")) {
        cache_names[cache_count++] = "m12";
        cache_names[cache_count++] = "dxmt-metal12";
    } else if (!strcmp(pipeline, "m9") || !strcmp(pipeline, "m10") || !strcmp(pipeline, "m10_32") ||
               !strcmp(pipeline, "m11") || !strcmp(pipeline, "m11_32")) {
        cache_names[cache_count++] = pipeline;
        cache_names[cache_count++] = "dxmt-metal";
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "metalsharp_version");
    ms_json_writer_string(&w, MS_BACKEND_VERSION);
    ms_json_writer_key(&w, "generated_at_unix");
    ms_json_writer_u64(&w, (unsigned long long)time(NULL));
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "pipeline_name");
    ms_json_writer_string(&w, pipeline_name);
    ms_json_writer_key(&w, "runtime_profile");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "wine_binary_path");
    {
        char wine[2048];
        snprintf(wine, sizeof(wine), "%s/bin/metalsharp-wine", root);
        if (access(wine, F_OK) != 0)
            snprintf(wine, sizeof(wine), "%s/bin/wine", root);
        ms_json_writer_string(&w, wine);
    }
    ms_json_writer_key(&w, "wine_binary_exists");
    {
        char wine[2048];
        snprintf(wine, sizeof(wine), "%s/bin/metalsharp-wine", root);
        bool exists = access(wine, F_OK) == 0;
        if (!exists) {
            snprintf(wine, sizeof(wine), "%s/bin/wine", root);
            exists = access(wine, F_OK) == 0;
        }
        ms_json_writer_bool(&w, exists);
    }
    ms_json_writer_key(&w, "wine_library_env");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "key");
    ms_json_writer_string(&w, "DYLD_FALLBACK_LIBRARY_PATH");
    ms_json_writer_key(&w, "value");
    {
        char value[4096];
        snprintf(value, sizeof(value), "%s/lib:%s/lib/wine/x86_64-unix", root, root);
        ms_json_writer_string(&w, value);
    }
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "prefix_path");
    ms_json_writer_string(&w, prefix);
    ms_json_writer_key(&w, "prefix_exists");
    ms_json_writer_bool(&w, access(prefix, F_OK) == 0);
    ms_json_writer_key(&w, "game_install_path");
    if (access(game_path, F_OK) == 0)
        ms_json_writer_string(&w, game_path);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "bundle_hash");
    if (bundle_hash)
        ms_json_writer_string(&w, bundle_hash);
    else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "artifact_sources");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < artifact_count; i++) {
        char path[2048];
        struct stat st;
        bool present, optional = artifacts[i].optional;
        char* hash;
        snprintf(path, sizeof(path), "%s/%s/%s", root, artifacts[i].subpath, artifacts[i].filename);
        present = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
        if (!present && !optional)
            missing = true;
        hash = present ? sha256_file(path) : NULL;
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "source_subpath");
        ms_json_writer_string(&w, artifacts[i].subpath);
        ms_json_writer_key(&w, "filename");
        ms_json_writer_string(&w, artifacts[i].filename);
        ms_json_writer_key(&w, "dest_filename");
        ms_json_writer_null(&w);
        ms_json_writer_key(&w, "source_path");
        ms_json_writer_string(&w, path);
        ms_json_writer_key(&w, "present");
        ms_json_writer_bool(&w, present);
        ms_json_writer_key(&w, "optional");
        ms_json_writer_bool(&w, optional);
        ms_json_writer_key(&w, "sha256");
        if (hash)
            ms_json_writer_string(&w, hash);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "size_bytes");
        if (present)
            ms_json_writer_u64(&w, (unsigned long long)st.st_size);
        else
            ms_json_writer_null(&w);
        ms_json_writer_object_end(&w);
        free(hash);
    }
    ms_json_writer_array_end(&w);
    if (missing) {
        ms_json_writer_key(&w, "missing_artifacts");
        ms_json_writer_array_begin(&w);
        for (i = 0; i < artifact_count; i++) {
            char path[2048];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s/%s", root, artifacts[i].subpath, artifacts[i].filename);
            if (artifacts[i].optional || (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0))
                continue;
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "filename");
            ms_json_writer_string(&w, artifacts[i].filename);
            ms_json_writer_key(&w, "source_subpath");
            ms_json_writer_string(&w, artifacts[i].subpath);
            ms_json_writer_key(&w, "source_path");
            ms_json_writer_string(&w, path);
            ms_json_writer_object_end(&w);
        }
        ms_json_writer_array_end(&w);
    } else {
        ms_json_writer_key(&w, "backend");
        ms_json_writer_string(&w, backend);
        ms_json_writer_key(&w, "graphics_backend");
        ms_json_writer_string(&w, graphics_backend);
        ms_json_writer_key(&w, "staged_dll_hashes");
        write_staged_dll_hashes(&w, home, appid);
    }
    ms_json_writer_key(&w, "cache_directories");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < cache_count; i++) {
        snprintf(cache, sizeof(cache), "%s/shader-cache/%s/%lu", home, cache_names[i], appid);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "path");
        ms_json_writer_string(&w, cache);
        ms_json_writer_key(&w, "exists");
        ms_json_writer_bool(&w, access(cache, F_OK) == 0);
        ms_json_writer_key(&w, "entry_count");
        ms_json_writer_u64(&w, 0);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, !missing);
    if (missing) {
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, "required runtime artifacts are missing");
    }
    ms_json_writer_object_end(&w);
    free(app);
    free(requested);
    free(bundle_hash);
    return ms_json_writer_take(&w);
}

static char* launch_timing_report(const char* query, int* status) {
    char* app = query_value(query, "appid");
    unsigned long appid = app ? strtoul(app, NULL, 10) : 0;
    const char* home = getenv("METALSHARP_HOME");
    char path[2048];
    char* timing = NULL;
    ms_json_writer w;
    if (status)
        *status = 200;
    if (!home || !*home)
        home = getenv("HOME");
    if (!home)
        home = "";
    snprintf(path, sizeof(path), "%s/bottles/steam_%lu/logs/launch-timing-latest.json", home, appid);
    timing = read_file_text(path);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, timing != NULL);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, "bottle_id");
    {
        char bottle[64];
        snprintf(bottle, sizeof(bottle), "steam_%lu", appid);
        ms_json_writer_string(&w, bottle);
    }
    if (timing) {
        char parse_error[96];
        ms_json* parsed = ms_json_parse(timing, strlen(timing), parse_error, sizeof(parse_error));
        ms_json_writer_key(&w, "timing");
        if (parsed && ms_json_type_of(parsed) == MS_JSON_OBJECT)
            ms_json_writer_raw(&w, timing);
        else {
            ms_json_writer_null(&w);
            free(timing);
            timing = NULL;
        }
        ms_json_free(parsed);
    }
    if (!timing) {
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, "no launch timing recorded for this bottle yet");
    }
    ms_json_writer_object_end(&w);
    free(app);
    free(timing);
    return ms_json_writer_take(&w);
}

static char* wineboot_state_report(const char* query, int* status) {
    char* app = query_value(query, "appid");
    char* verifying_value = query_value(query, "verifying");
    unsigned long appid = app ? strtoul(app, NULL, 10) : 0;
    const char* home = getenv("METALSHARP_HOME");
    char prefix[2048], state[32];
    ms_json_writer w;
    if (status)
        *status = 200;
    if (!home || !*home)
        home = getenv("HOME");
    if (!home)
        home = "";
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    if (verifying_value && (!strcasecmp(verifying_value, "true") || !strcmp(verifying_value, "1")))
        snprintf(state, sizeof(state), "verifying");
    else if (access(prefix, F_OK) != 0)
        snprintf(state, sizeof(state), "prefix_missing");
    else
        snprintf(state, sizeof(state), "idle");
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, "prefix_path");
    ms_json_writer_string(&w, prefix);
    ms_json_writer_key(&w, "wineboot_state");
    ms_json_writer_string(&w, state);
    ms_json_writer_key(&w, "is_prefix_updating");
    ms_json_writer_bool(&w, !strcmp(state, "prefix_updating"));
    ms_json_writer_key(&w, "is_verifying");
    ms_json_writer_bool(&w, !strcmp(state, "verifying"));
    ms_json_writer_object_end(&w);
    free(app);
    free(verifying_value);
    return ms_json_writer_take(&w);
}

typedef struct {
    unsigned index, reg_space, reg;
    char kind[24], visibility[24];
} contract_param;

typedef struct {
    unsigned param, base, count, reg_space;
    char kind[16];
} contract_range;

typedef struct {
    unsigned index, reg, reg_space;
    char visibility[24];
} contract_sampler;

static void contract_violation(ms_json_writer* w, bool* bad, bool has_param, unsigned param, const char* kind,
                               const char* detail) {
    *bad = true;
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "root_parameter_index");
    if (has_param)
        ms_json_writer_u64(w, param);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "kind");
    ms_json_writer_string(w, kind);
    ms_json_writer_key(w, "detail");
    ms_json_writer_string(w, detail);
    ms_json_writer_object_end(w);
}

static bool json_string_is(const ms_json* value, const char* expected) {
    char* text = NULL;
    bool match = ms_json_as_string(value, &text) && text && !strcmp(text, expected);
    free(text);
    return match;
}

static const char* range_debug_name(const char* kind) {
    if (!strcmp(kind, "cbv"))
        return "Cbv";
    if (!strcmp(kind, "srv"))
        return "Srv";
    if (!strcmp(kind, "uav"))
        return "Uav";
    if (!strcmp(kind, "sampler"))
        return "Sampler";
    return kind;
}

static char* binding_contract_report(const ms_json* request, int* status) {
    const ms_json* manifest = ms_json_object_get(request, "root_signature");
    const ms_json* parameters = ms_json_object_get(manifest, "parameters");
    const ms_json* samplers = ms_json_object_get(manifest, "static_samplers");
    const ms_json* reflection = ms_json_object_get(request, "reflection");
    contract_param params[256];
    contract_range ranges[512];
    contract_sampler static_samplers[256];
    size_t param_count = 0, range_count = 0, sampler_count = 0, i;
    bool bad = false;
    ms_json_writer w;
    if (status)
        *status = 200;
    long long schema_version = 0;
    if (!manifest || ms_json_type_of(manifest) != MS_JSON_OBJECT || !parameters ||
        ms_json_type_of(parameters) != MS_JSON_ARRAY || !samplers || ms_json_type_of(samplers) != MS_JSON_ARRAY) {
        if (status)
            *status = 400;
        return contract_error("invalid root signature manifest: missing required fields");
    }
    if (!ms_json_as_i64(ms_json_object_get(manifest, "schema_version"), &schema_version) || schema_version < 0 ||
        (!json_string_is(ms_json_object_get(manifest, "version"), "v1_0") &&
         !json_string_is(ms_json_object_get(manifest, "version"), "v1_1")) ||
        (!json_string_is(ms_json_object_get(manifest, "null_descriptor_policy"), "allowed") &&
         !json_string_is(ms_json_object_get(manifest, "null_descriptor_policy"), "rejected") &&
         !json_string_is(ms_json_object_get(manifest, "null_descriptor_policy"), "unspecified"))) {
        if (status)
            *status = 400;
        return contract_error("invalid root signature manifest: invalid schema, version, or null descriptor policy");
    }
    for (i = 0; i < ms_json_array_length(parameters) && param_count < 256; i++) {
        const ms_json* item = ms_json_array_get(parameters, i);
        long long value;
        char* text = NULL;
        params[param_count].index = (unsigned)i;
        if (ms_json_as_i64(ms_json_object_get(item, "index"), &value) && value >= 0)
            params[param_count].index = (unsigned)value;
        ms_json_as_string(ms_json_object_get(item, "kind"), &text);
        snprintf(params[param_count].kind, sizeof(params[param_count].kind), "%s", text ? text : "");
        free(text);
        text = NULL;
        ms_json_as_string(ms_json_object_get(item, "visibility"), &text);
        snprintf(params[param_count].visibility, sizeof(params[param_count].visibility), "%s", text ? text : "");
        free(text);
        params[param_count].reg_space = 0;
        params[param_count].reg = 0;
        if (ms_json_as_i64(ms_json_object_get(item, "register_space"), &value) && value >= 0)
            params[param_count].reg_space = (unsigned)value;
        if (ms_json_as_i64(ms_json_object_get(item, "register"), &value) && value >= 0)
            params[param_count].reg = (unsigned)value;
        if (params[param_count].index != i) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "root parameter at position %zu has index %u; indices must be dense from 0", i,
                     params[param_count].index);
            /* emitted after the report object is opened below */
        }
        const ms_json* table = ms_json_object_get(item, "descriptor_table_ranges");
        if (table && ms_json_type_of(table) == MS_JSON_ARRAY) {
            size_t j;
            for (j = 0; j < ms_json_array_length(table) && range_count < 512; j++) {
                const ms_json* range = ms_json_array_get(table, j);
                contract_range* out = &ranges[range_count++];
                out->param = (unsigned)i;
                out->base = 0;
                out->count = 0;
                out->reg_space = 0;
                text = NULL;
                ms_json_as_string(ms_json_object_get(range, "kind"), &text);
                snprintf(out->kind, sizeof(out->kind), "%s", text ? text : "");
                free(text);
                if (ms_json_as_i64(ms_json_object_get(range, "base_register"), &value) && value >= 0)
                    out->base = (unsigned)value;
                if (ms_json_as_i64(ms_json_object_get(range, "count"), &value) && value >= 0)
                    out->count = (unsigned)value;
                if (ms_json_as_i64(ms_json_object_get(range, "register_space"), &value) && value >= 0)
                    out->reg_space = (unsigned)value;
            }
        }
        param_count++;
    }
    for (i = 0; i < ms_json_array_length(samplers) && sampler_count < 256; i++) {
        const ms_json* item = ms_json_array_get(samplers, i);
        long long value;
        char* text = NULL;
        static_samplers[sampler_count].index = (unsigned)i;
        static_samplers[sampler_count].reg = 0;
        static_samplers[sampler_count].reg_space = 0;
        if (ms_json_as_i64(ms_json_object_get(item, "index"), &value) && value >= 0)
            static_samplers[sampler_count].index = (unsigned)value;
        if (ms_json_as_i64(ms_json_object_get(item, "register"), &value) && value >= 0)
            static_samplers[sampler_count].reg = (unsigned)value;
        if (ms_json_as_i64(ms_json_object_get(item, "register_space"), &value) && value >= 0)
            static_samplers[sampler_count].reg_space = (unsigned)value;
        text = NULL;
        ms_json_as_string(ms_json_object_get(item, "visibility"), &text);
        snprintf(static_samplers[sampler_count].visibility, sizeof(static_samplers[sampler_count].visibility), "%s",
                 text ? text : "");
        free(text);
        sampler_count++;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "limits");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "max_direct_buffers");
    ms_json_writer_u64(&w, 31);
    ms_json_writer_key(&w, "max_direct_textures");
    ms_json_writer_u64(&w, 8);
    ms_json_writer_key(&w, "max_direct_samplers");
    ms_json_writer_u64(&w, 16);
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "violations");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < param_count; i++)
        if (params[i].index != i) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "root parameter at position %zu has index %u; indices must be dense from 0", i, params[i].index);
            contract_violation(&w, &bad, true, params[i].index, "sparse_root_parameter_index", detail);
        }
    for (i = 0; i < range_count; i++) {
        contract_range* r = &ranges[i];
        unsigned limit = !strcmp(r->kind, "cbv") ? 31 : !strcmp(r->kind, "sampler") ? 16 : 8;
        unsigned long long high = (unsigned long long)r->base + r->count;
        char detail[220];
        if (r->count == 0xffffffffU) {
            snprintf(detail, sizeof(detail),
                     "root parameter %u space %u %s range is unbounded (count = UINT_MAX); unbounded descriptor ranges "
                     "require proven probe support before they may be advertised",
                     r->param, r->reg_space, range_debug_name(r->kind));
            contract_violation(&w, &bad, true, r->param, "unbounded_range_unsupported", detail);
        } else if (high > limit) {
            snprintf(
                detail, sizeof(detail),
                "root parameter %u space %u %s range covers registers %u..=%llu; exceeds Metal direct-binding limit %u",
                r->param, r->reg_space, range_debug_name(r->kind), r->base, high - 1, limit);
            contract_violation(&w, &bad, true, r->param,
                               !strcmp(r->kind, "cbv")       ? "direct_buffer_limit"
                               : !strcmp(r->kind, "sampler") ? "direct_sampler_limit"
                                                             : "direct_texture_limit",
                               detail);
        }
        for (size_t j = 0; j < i; j++)
            if (ranges[j].reg_space == r->reg_space && !strcmp(ranges[j].kind, r->kind) &&
                (unsigned long long)ranges[j].base + ranges[j].count > r->base) {
                snprintf(detail, sizeof(detail), "root parameter %u space %u %s range overlaps an earlier range",
                         r->param, r->reg_space, range_debug_name(r->kind));
                contract_violation(&w, &bad, true, r->param, "overlapping_table_range", detail);
                break;
            }
    }
    for (i = 0; i < sampler_count; i++)
        for (size_t j = 0; j < param_count; j++)
            if (static_samplers[i].reg_space == params[j].reg_space && static_samplers[i].reg == params[j].reg &&
                (!strcmp(params[j].kind, "cbv") || !strcmp(params[j].kind, "srv") || !strcmp(params[j].kind, "uav") ||
                 !strcmp(params[j].kind, "constants"))) {
                char detail[180];
                snprintf(
                    detail, sizeof(detail), "static sampler #%u space %u register %u clashes with root parameter %u",
                    static_samplers[i].index, static_samplers[i].reg_space, static_samplers[i].reg, params[j].index);
                contract_violation(&w, &bad, true, params[j].index, "static_sampler_register_clash", detail);
            }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "reflection_abi_mismatches");
    ms_json_writer_array_begin(&w);
    if (reflection && ms_json_type_of(reflection) == MS_JSON_ARRAY)
        for (i = 0; i < ms_json_array_length(reflection); i++) {
            const ms_json* item = ms_json_array_get(reflection, i);
            long long reg = 0, space = 0, count = 0;
            char *shader = NULL, *kind = NULL, *visibility = NULL;
            bool covered = false;
            ms_json_as_string(ms_json_object_get(item, "shader"), &shader);
            ms_json_as_string(ms_json_object_get(item, "kind"), &kind);
            ms_json_as_string(ms_json_object_get(item, "visibility"), &visibility);
            ms_json_as_i64(ms_json_object_get(item, "register"), &reg);
            ms_json_as_i64(ms_json_object_get(item, "register_space"), &space);
            ms_json_as_i64(ms_json_object_get(item, "count"), &count);
            for (size_t j = 0; j < range_count; j++)
                if (!strcmp(ranges[j].kind, kind ? kind : "") && ranges[j].reg_space == (unsigned)space &&
                    (!strcmp(params[ranges[j].param].visibility, "all") ||
                     !strcmp(params[ranges[j].param].visibility, visibility ? visibility : "")) &&
                    ranges[j].base <= (unsigned)reg && ranges[j].count != 0xffffffffU &&
                    (unsigned long long)ranges[j].base + ranges[j].count > (unsigned)reg)
                    covered = true;
            for (size_t j = 0; j < sampler_count; j++)
                if (!strcmp(kind ? kind : "", "sampler") && static_samplers[j].reg_space == (unsigned)space &&
                    static_samplers[j].reg == (unsigned)reg &&
                    (!strcmp(static_samplers[j].visibility, "all") ||
                     !strcmp(static_samplers[j].visibility, visibility ? visibility : "")))
                    covered = true;
            if (!covered) {
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "shader");
                ms_json_writer_string(&w, shader ? shader : "");
                ms_json_writer_key(&w, "kind");
                ms_json_writer_string(&w, kind ? kind : "");
                ms_json_writer_key(&w, "register_space");
                ms_json_writer_u64(&w, space >= 0 ? (unsigned long long)space : 0);
                ms_json_writer_key(&w, "register");
                ms_json_writer_u64(&w, reg >= 0 ? (unsigned long long)reg : 0);
                ms_json_writer_key(&w, "detail");
                {
                    char detail[260];
                    snprintf(detail, sizeof(detail),
                             "shader %s declares %s space %lld register %lld but no root parameter covers it",
                             shader ? shader : "", range_debug_name(kind ? kind : ""), space, reg);
                    ms_json_writer_string(&w, detail);
                }
                ms_json_writer_object_end(&w);
                bad = true;
            }
            free(shader);
            free(kind);
            free(visibility);
            (void)count;
        }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, !bad);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

typedef struct {
    char resource[128], state[40];
} replay_resource;

typedef struct {
    char resource[128], before[40], after[40];
} replay_pending;

typedef struct {
    size_t op_index, target_count;
    char targets[32][128], depth[128];
    bool has_depth;
    unsigned sample_count;
} replay_boundary;

static bool replay_write_state(const char* state) {
    return !strcmp(state, "render_target") || !strcmp(state, "unordered_access") || !strcmp(state, "depth_write") ||
           !strcmp(state, "stream_out") || !strcmp(state, "copy_dest") || !strcmp(state, "resolve_dest");
}

static bool replay_read_state(const char* state) {
    return !replay_write_state(state) && strcmp(state, "present") && strcmp(state, "common");
}

static void replay_violation(ms_json_writer* w, bool* bad, size_t op_index, const char* kind, const char* detail) {
    *bad = true;
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "op_index");
    ms_json_writer_u64(w, op_index);
    ms_json_writer_key(w, "kind");
    ms_json_writer_string(w, kind);
    ms_json_writer_key(w, "detail");
    ms_json_writer_string(w, detail);
    ms_json_writer_object_end(w);
}

static const char* replay_state_for(const replay_resource* resources, size_t count, const char* resource) {
    for (size_t i = count; i-- > 0;)
        if (!strcmp(resources[i].resource, resource))
            return resources[i].state;
    return "common";
}

static bool replay_target_in_pass(char targets[][128], size_t count, const char* target) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(targets[i], target))
            return true;
    return false;
}

static void replay_required_state(ms_json_writer* w, bool* bad, size_t op_index, const replay_resource* resources,
                                  size_t resource_count, const replay_pending* pending, size_t pending_count,
                                  const char* resource, const char* required, const char* kind) {
    for (size_t i = 0; i < pending_count; i++)
        if (!strcmp(pending[i].resource, resource)) {
            char detail[180];
            snprintf(detail, sizeof(detail), "resource %s was used while a split barrier is pending", resource);
            replay_violation(w, bad, op_index, "unfinished_split_barrier", detail);
            return;
        }
    const char* state = replay_state_for(resources, resource_count, resource);
    if (strcmp(state, "common") && strcmp(state, required)) {
        char detail[220];
        snprintf(detail, sizeof(detail), "resource %s in state %s but %s is required", resource, state, required);
        replay_violation(w, bad, op_index, kind, detail);
    }
}

static bool replay_known_operation(const char* name) {
    static const char* const names[] = {"reset",
                                        "resource_barrier",
                                        "begin_render_pass",
                                        "end_render_pass",
                                        "clear_render_target_view",
                                        "draw",
                                        "draw_indexed",
                                        "dispatch",
                                        "copy_resource",
                                        "copy_buffer_region",
                                        "copy_texture_region",
                                        "resolve_subresource",
                                        "present",
                                        "close",
                                        "execute"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return true;
    return false;
}

static char* command_replay_report(const ms_json* request, int* status) {
    const ms_json* trace = ms_json_object_get(request, "trace");
    replay_resource resources[256];
    replay_pending pending[256];
    replay_boundary boundaries[256];
    char active_targets[32][128], active_depth[128];
    size_t active_target_count = 0;
    bool active_has_depth = false;
    size_t resource_count = 0, pending_count = 0, boundary_count = 0, i;
    unsigned long long transitions = 0, write_to_read = 0, read_to_write = 0, split = 0, unfinished = 0, passes = 0;
    bool render_open = false, bad = false;
    ms_json_writer w;
    if (status)
        *status = 200;
    for (i = 0; i < ms_json_array_length(trace); i++) {
        char* name = NULL;
        ms_json_as_string(ms_json_object_get(ms_json_array_get(trace, i), "op"), &name);
        if (!name || !replay_known_operation(name)) {
            free(name);
            if (status)
                *status = 400;
            return contract_error("invalid command trace: unknown or missing operation");
        }
        free(name);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "violations");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < ms_json_array_length(trace); i++) {
        const ms_json* op = ms_json_array_get(trace, i);
        char* name = NULL;
        ms_json_as_string(ms_json_object_get(op, "op"), &name);
        if (!name) {
            free(name);
            continue;
        }
        if (!strcmp(name, "begin_render_pass")) {
            if (render_open) {
                replay_violation(&w, &bad, i, "render_pass_not_closed",
                                 "a new render pass began while a render pass was still open");
            }
            render_open = true;
            passes++;
            active_target_count = 0;
            active_has_depth = false;
            const ms_json* targets = ms_json_object_get(op, "render_targets");
            if (targets && ms_json_type_of(targets) == MS_JSON_ARRAY)
                for (size_t j = 0; j < ms_json_array_length(targets) && active_target_count < 32; j++) {
                    char* target = NULL;
                    ms_json_as_string(ms_json_array_get(targets, j), &target);
                    if (target) {
                        snprintf(active_targets[active_target_count++], sizeof(active_targets[0]), "%s", target);
                        free(target);
                    }
                }
            char* depth = NULL;
            ms_json_as_string(ms_json_object_get(op, "depth_target"), &depth);
            if (depth) {
                snprintf(active_depth, sizeof(active_depth), "%s", depth);
                active_has_depth = true;
                free(depth);
            }
            if (boundary_count < 256) {
                replay_boundary* boundary = &boundaries[boundary_count++];
                boundary->op_index = i;
                boundary->target_count = active_target_count;
                boundary->has_depth = active_has_depth;
                boundary->sample_count = 0;
                for (size_t j = 0; j < active_target_count; j++)
                    snprintf(boundary->targets[j], sizeof(boundary->targets[0]), "%s", active_targets[j]);
                if (active_has_depth)
                    snprintf(boundary->depth, sizeof(boundary->depth), "%s", active_depth);
                long long sample = 0;
                if (ms_json_as_i64(ms_json_object_get(op, "sample_count"), &sample) && sample >= 0)
                    boundary->sample_count = (unsigned)sample;
            }
        } else if (!strcmp(name, "end_render_pass")) {
            render_open = false;
        } else if (!strcmp(name, "reset") && render_open) {
            char detail[160];
            snprintf(detail, sizeof(detail), "command list reset at op %zu while a render pass is open", i);
            replay_violation(&w, &bad, i, "reset_inside_render_pass", detail);
        } else if (!strcmp(name, "resource_barrier")) {
            const ms_json* barriers = ms_json_object_get(op, "barriers");
            if (barriers && ms_json_type_of(barriers) == MS_JSON_ARRAY)
                for (size_t j = 0; j < ms_json_array_length(barriers); j++) {
                    const ms_json* barrier = ms_json_array_get(barriers, j);
                    char *resource = NULL, *before = NULL, *after = NULL;
                    bool split_begin = false;
                    ms_json_as_string(ms_json_object_get(barrier, "resource"), &resource);
                    ms_json_as_string(ms_json_object_get(barrier, "before"), &before);
                    ms_json_as_string(ms_json_object_get(barrier, "after"), &after);
                    ms_json_as_bool(ms_json_object_get(barrier, "split_begin"), &split_begin);
                    if (!resource || !before || !after) {
                        free(resource);
                        free(before);
                        free(after);
                        continue;
                    }
                    transitions++;
                    if (split_begin) {
                        if (pending_count < 256) {
                            snprintf(pending[pending_count].resource, sizeof(pending[0].resource), "%s", resource);
                            snprintf(pending[pending_count].before, sizeof(pending[0].before), "%s", before);
                            snprintf(pending[pending_count].after, sizeof(pending[0].after), "%s", after);
                            pending_count++;
                        }
                        split++;
                    } else {
                        for (size_t k = 0; k < pending_count; k++)
                            if (!strcmp(pending[k].resource, resource)) {
                                if (strcmp(pending[k].before, before)) {
                                    char detail[240];
                                    snprintf(detail, sizeof(detail),
                                             "resource %s had a pending split barrier (begin %s) but a non-split "
                                             "barrier transitioned from %s",
                                             resource, pending[k].before, before);
                                    replay_violation(&w, &bad, i, "unfinished_split_barrier", detail);
                                }
                                memmove(&pending[k], &pending[k + 1], (pending_count - k - 1) * sizeof(pending[0]));
                                pending_count--;
                                break;
                            }
                        const char* old = "common";
                        for (size_t k = resource_count; k-- > 0;)
                            if (!strcmp(resources[k].resource, resource)) {
                                old = resources[k].state;
                                break;
                            }
                        if (replay_write_state(old) && replay_read_state(after))
                            write_to_read++;
                        if (replay_read_state(old) && replay_write_state(after))
                            read_to_write++;
                        size_t slot = resource_count < 256 ? resource_count++ : 255;
                        snprintf(resources[slot].resource, sizeof(resources[slot].resource), "%s", resource);
                        snprintf(resources[slot].state, sizeof(resources[slot].state), "%s", after);
                    }
                    free(resource);
                    free(before);
                    free(after);
                }
        } else if (!strcmp(name, "clear_render_target_view")) {
            char* target = NULL;
            ms_json_as_string(ms_json_object_get(op, "target"), &target);
            if (target) {
                replay_required_state(&w, &bad, i, resources, resource_count, pending, pending_count, target,
                                      "render_target", "invalid_resource_state_for_use");
                if (render_open && !replay_target_in_pass(active_targets, active_target_count, target))
                    replay_violation(&w, &bad, i, "render_target_set_changed_without_boundary",
                                     "ClearRenderTargetView was recorded outside the active render-pass set");
            }
            free(target);
        } else if (!strcmp(name, "draw") || !strcmp(name, "draw_indexed")) {
            for (size_t j = 0; j < active_target_count; j++)
                replay_required_state(&w, &bad, i, resources, resource_count, pending, pending_count, active_targets[j],
                                      "render_target", "invalid_resource_state_for_use");
            if (active_has_depth) {
                const char* state = replay_state_for(resources, resource_count, active_depth);
                if (strcmp(state, "common") && strcmp(state, "depth_write") && strcmp(state, "depth_read")) {
                    char detail[200];
                    snprintf(detail, sizeof(detail),
                             "draw with depth target %s in state %s (expected depth_write/depth_read)", active_depth,
                             state);
                    replay_violation(&w, &bad, i, "invalid_resource_state_for_use", detail);
                }
            }
        } else if (!strcmp(name, "copy_resource") || !strcmp(name, "copy_buffer_region") ||
                   !strcmp(name, "copy_texture_region")) {
            char *dst = NULL, *src = NULL;
            ms_json_as_string(ms_json_object_get(op, "dst"), &dst);
            ms_json_as_string(ms_json_object_get(op, "src"), &src);
            if (dst)
                replay_required_state(&w, &bad, i, resources, resource_count, pending, pending_count, dst, "copy_dest",
                                      "copy_state_mismatch");
            if (src) {
                const char* state = replay_state_for(resources, resource_count, src);
                if (strcmp(state, "common") && strcmp(state, "copy_source") && strcmp(state, "generic_read")) {
                    char detail[200];
                    snprintf(detail, sizeof(detail), "resource %s in state %s but copy_source is required", src, state);
                    replay_violation(&w, &bad, i, "copy_state_mismatch", detail);
                }
            }
            free(dst);
            free(src);
        } else if (!strcmp(name, "resolve_subresource")) {
            char *dst = NULL, *src = NULL;
            ms_json_as_string(ms_json_object_get(op, "dst"), &dst);
            ms_json_as_string(ms_json_object_get(op, "src"), &src);
            if (dst)
                replay_required_state(&w, &bad, i, resources, resource_count, pending, pending_count, dst,
                                      "resolve_dest", "copy_state_mismatch");
            if (src)
                replay_required_state(&w, &bad, i, resources, resource_count, pending, pending_count, src,
                                      "resolve_source", "copy_state_mismatch");
            free(dst);
            free(src);
        } else if (!strcmp(name, "present")) {
            char* buffer = NULL;
            ms_json_as_string(ms_json_object_get(op, "back_buffer"), &buffer);
            const char* state = "common";
            for (size_t k = resource_count; k-- > 0;)
                if (buffer && !strcmp(resources[k].resource, buffer)) {
                    state = resources[k].state;
                    break;
                }
            if (strcmp(state, "present"))
                replay_violation(&w, &bad, i, "present_not_in_present_state",
                                 "present submitted while the back buffer was not in Present state");
            free(buffer);
        }
        free(name);
    }
    for (i = 0; i < pending_count; i++) {
        char detail[160];
        snprintf(detail, sizeof(detail), "resource %s split barrier (begin %s -> end %s) was never ended",
                 pending[i].resource, pending[i].before, pending[i].after);
        replay_violation(&w, &bad, ms_json_array_length(trace), "unfinished_split_barrier", detail);
        unfinished++;
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "encoder_boundaries");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < boundary_count; i++) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "at_op_index");
        ms_json_writer_u64(&w, boundaries[i].op_index);
        ms_json_writer_key(&w, "render_targets");
        ms_json_writer_array_begin(&w);
        for (size_t j = 0; j < boundaries[i].target_count; j++)
            ms_json_writer_string(&w, boundaries[i].targets[j]);
        ms_json_writer_array_end(&w);
        ms_json_writer_key(&w, "depth_target");
        if (boundaries[i].has_depth)
            ms_json_writer_string(&w, boundaries[i].depth);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "sample_count");
        ms_json_writer_u64(&w, boundaries[i].sample_count);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "visibility_summary");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "total_transitions");
    ms_json_writer_u64(&w, transitions);
    ms_json_writer_key(&w, "write_to_read_transitions");
    ms_json_writer_u64(&w, write_to_read);
    ms_json_writer_key(&w, "read_to_write_transitions");
    ms_json_writer_u64(&w, read_to_write);
    ms_json_writer_key(&w, "split_barriers");
    ms_json_writer_u64(&w, split);
    ms_json_writer_key(&w, "unfinished_split_barriers");
    ms_json_writer_u64(&w, unfinished);
    ms_json_writer_key(&w, "render_passes");
    ms_json_writer_u64(&w, passes);
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, !bad);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static char* pipeline_diagnostic(const char* kind, const char* query, int* status) {
    static const char* const m12_unix[] = {"winemetal.so", "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib"};
    static const char* const pe[] = {"d3d12.dll",     "d3d11.dll",     "dxgi.dll",    "dxgi_dxmt.dll",
                                     "d3d10core.dll", "winemetal.dll", "nvapi64.dll", "nvngx.dll"};
    static const char* const vkd3d_pe[] = {"d3d12.dll",     "d3d12core.dll", "d3d11.dll",
                                           "d3d10core.dll", "d3d9.dll",      "dxgi.dll"};
    char* app = query_value(query, "appid");
    char* requested = query_value(query, "pipeline");
    unsigned long appid = app ? strtoul(app, NULL, 10) : 0;
    const char* pipeline = !strcmp(kind, "m12-dry-run") ? "m12" : "vkd3d";
    if (requested) {
        if (!strcasecmp(requested, "m12"))
            pipeline = "m12";
        else if (!strcasecmp(requested, "m11"))
            pipeline = "m11";
        else if (!strcasecmp(requested, "vkd3d"))
            pipeline = "vkd3d";
        else if (!strcasecmp(requested, "m10"))
            pipeline = "m10";
        else if (!strcasecmp(requested, "m9"))
            pipeline = "m9";
    }
    const char* pipeline_name = !strcmp(pipeline, "m12")     ? "M12"
                                : !strcmp(pipeline, "vkd3d") ? "VKD3D"
                                : !strcmp(pipeline, "m10")   ? "M10"
                                : !strcmp(pipeline, "m9")    ? "M9"
                                                             : "M11";
    const char* home = getenv("METALSHARP_HOME");
    char root[2048], lane_root[2048];
    ms_json_writer w;
    bool all_present = true;
    size_t i;
    const char* const* deploy_pe = !strcmp(pipeline, "vkd3d") ? vkd3d_pe : pe;
    size_t deploy_count =
        !strcmp(pipeline, "vkd3d") ? sizeof(vkd3d_pe) / sizeof(vkd3d_pe[0]) : sizeof(pe) / sizeof(pe[0]);
    const char* deploy_subpath = !strcmp(pipeline, "m12")     ? "lib/dxmt_m12/x86_64-windows"
                                 : !strcmp(pipeline, "vkd3d") ? "vkd3d-proton/x86_64-windows"
                                                              : "lib/dxmt/x86_64-windows";
    (void)kind;
    if (status)
        *status = 200;
    if (!home || !*home)
        home = getenv("HOME");
    if (!home)
        home = "";
    snprintf(root, sizeof(root), "%s/runtime/wine", home);
    snprintf(lane_root, sizeof(lane_root), "%s/vkd3d", home);
    {
        char cache_dir[2048];
        snprintf(cache_dir, sizeof(cache_dir), "%s/shader-cache/%s/%lu", home, pipeline, appid);
        ensure_diagnostic_directory(cache_dir);
        snprintf(cache_dir, sizeof(cache_dir), "%s/pipeline-cache/%s/%lu", home, pipeline, appid);
        ensure_diagnostic_directory(cache_dir);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "schema_version");
    ms_json_writer_u64(&w, 1);
    ms_json_writer_key(&w, "dry_run");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "appid");
    ms_json_writer_u64(&w, appid);
    ms_json_writer_key(&w, "pipeline");
    ms_json_writer_string(&w, pipeline);
    ms_json_writer_key(&w, "pipeline_name");
    ms_json_writer_string(&w, pipeline_name);
    ms_json_writer_key(&w, "runtime_root");
    ms_json_writer_string(&w, root);
    ms_json_writer_key(&w, "windows_dll_dir");
    {
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", !strcmp(pipeline, "vkd3d") ? lane_root : root,
                 !strcmp(pipeline, "m12")     ? "lib/dxmt_m12/x86_64-windows"
                 : !strcmp(pipeline, "vkd3d") ? "vkd3d-proton/x86_64-windows"
                                              : "lib/dxmt/x86_64-windows");
        ms_json_writer_string(&w, path);
    }
    ms_json_writer_key(&w, "windows_dll_dir_exists");
    {
        char path[2048];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", !strcmp(pipeline, "vkd3d") ? lane_root : root,
                 !strcmp(pipeline, "m12")     ? "lib/dxmt_m12/x86_64-windows"
                 : !strcmp(pipeline, "vkd3d") ? "vkd3d-proton/x86_64-windows"
                                              : "lib/dxmt/x86_64-windows");
        ms_json_writer_bool(&w, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    }
    ms_json_writer_key(&w, "unix_lib_dir");
    if (!strcmp(pipeline, "m12")) {
        char path[2048];
        snprintf(path, sizeof(path), "%s/lib/dxmt_m12/x86_64-unix", root);
        ms_json_writer_string(&w, path);
    } else
        ms_json_writer_null(&w);
    ms_json_writer_key(&w, "unix_lib_dir_exists");
    if (!strcmp(pipeline, "m12")) {
        char path[2048];
        struct stat st;
        snprintf(path, sizeof(path), "%s/lib/dxmt_m12/x86_64-unix", root);
        ms_json_writer_bool(&w, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    } else {
        ms_json_writer_null(&w);
    }
    ms_json_writer_key(&w, "unix_sidecars");
    ms_json_writer_array_begin(&w);
    if (!strcmp(pipeline, "m12")) {
        for (i = 0; i < sizeof(m12_unix) / sizeof(m12_unix[0]); i++) {
            char path[2048];
            struct stat st;
            char* hash;
            snprintf(path, sizeof(path), "%s/lib/dxmt_m12/x86_64-unix/%s", root, m12_unix[i]);
            bool present = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
            if (!present)
                all_present = false;
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "filename");
            ms_json_writer_string(&w, m12_unix[i]);
            ms_json_writer_key(&w, "path");
            ms_json_writer_string(&w, path);
            ms_json_writer_key(&w, "present");
            ms_json_writer_bool(&w, present);
            hash = present ? sha256_file(path) : NULL;
            ms_json_writer_key(&w, "sha256");
            if (hash)
                ms_json_writer_string(&w, hash);
            else
                ms_json_writer_null(&w);
            ms_json_writer_object_end(&w);
            free(hash);
        }
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "deploy_dlls");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < deploy_count; i++) {
        char path[2048];
        struct stat st;
        char* hash;
        const char* source = !strcmp(pipeline, "vkd3d") && i >= 2 ? "dxvk/x86_64-windows" : deploy_subpath;
        snprintf(path, sizeof(path), "%s/%s/%s", !strcmp(pipeline, "vkd3d") ? lane_root : root, source,
                 deploy_pe[i]);
        bool present = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
        bool optional =
            strcmp(pipeline, "m12") && (!strncmp(deploy_pe[i], "nvapi", 5) || !strncmp(deploy_pe[i], "nvngx", 5));
        if (!present && !optional)
            all_present = false;
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "filename");
        ms_json_writer_string(&w, deploy_pe[i]);
        ms_json_writer_key(&w, "source_subpath");
        ms_json_writer_string(&w, source);
        ms_json_writer_key(&w, "source_path");
        ms_json_writer_string(&w, path);
        ms_json_writer_key(&w, "present");
        ms_json_writer_bool(&w, present);
        ms_json_writer_key(&w, "optional");
        ms_json_writer_bool(&w, optional);
        hash = present ? sha256_file(path) : NULL;
        ms_json_writer_key(&w, "sha256");
        if (hash)
            ms_json_writer_string(&w, hash);
        else
            ms_json_writer_null(&w);
        ms_json_writer_key(&w, "size_bytes");
        if (present)
            ms_json_writer_u64(&w, (unsigned long long)st.st_size);
        else
            ms_json_writer_null(&w);
        ms_json_writer_object_end(&w);
        free(hash);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "env_pairs");
    ms_json_writer_array_begin(&w);
    {
        char value[4096], unix_path[2048], fallback_unix_path[4096], windows_path[4096], shader_path[2048],
            pipeline_path[2048], summary[4096];
        const char* user_home = getenv("HOME");
        if (!user_home || !*user_home)
            user_home = home;
        if (!strcmp(pipeline, "m12")) {
            snprintf(unix_path, sizeof(unix_path), "%s/lib/dxmt_m12/x86_64-unix:%s/lib/wine/x86_64-unix", root,
                     root);
            snprintf(fallback_unix_path, sizeof(fallback_unix_path), "%s", unix_path);
            snprintf(windows_path, sizeof(windows_path), "%s/lib/dxmt_m12/x86_64-windows", root);
        } else if (!strcmp(pipeline, "vkd3d")) {
            snprintf(unix_path, sizeof(unix_path), "%s/lib/wine/x86_64-unix", root);
            snprintf(fallback_unix_path, sizeof(fallback_unix_path), "%s", unix_path);
            snprintf(windows_path, sizeof(windows_path),
                     "%s/vkd3d-proton/x86_64-windows:%s/dxvk/x86_64-windows:%s/lib/wine/x86_64-windows",
                     lane_root, lane_root, root);
        } else {
            snprintf(unix_path, sizeof(unix_path), "%s/lib/wine/x86_64-unix", root);
            snprintf(fallback_unix_path, sizeof(fallback_unix_path), "%s", unix_path);
            snprintf(windows_path, sizeof(windows_path), "%s/lib/dxmt/x86_64-windows", root);
        }
        snprintf(shader_path, sizeof(shader_path), "%s/shader-cache/%s/%lu/", user_home, pipeline, appid);
        snprintf(pipeline_path, sizeof(pipeline_path), "%s/pipeline-cache/%s/%lu/", user_home, pipeline, appid);
        snprintf(summary, sizeof(summary), "shader=%s;pipeline=%s", shader_path, pipeline_path);
#define ENV_PAIR(k, v)                                                                                                 \
    do {                                                                                                               \
        ms_json_writer_object_begin(&w);                                                                               \
        ms_json_writer_key(&w, "key");                                                                                 \
        ms_json_writer_string(&w, (k));                                                                                \
        ms_json_writer_key(&w, "value");                                                                               \
        ms_json_writer_string(&w, (v));                                                                                \
        ms_json_writer_object_end(&w);                                                                                 \
    } while (0)
        snprintf(value, sizeof(value), "%lu", appid);
        ENV_PAIR("SteamAppId", value);
        ENV_PAIR("SteamGameId", value);
        ENV_PAIR("SteamOverlayGameId", value);
        ENV_PAIR("DYLD_LIBRARY_PATH", unix_path);
        ENV_PAIR("DYLD_FALLBACK_LIBRARY_PATH", fallback_unix_path);
        ENV_PAIR("WINEDLLOVERRIDES",
                 !strcmp(pipeline, "m12")
                     ? "winemetal,d3d12,dxgi,dxgi_dxmt,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"
                     : "d3d12,d3d12core,d3d11,d3d10core,dxgi,d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d");
        ENV_PAIR("WINEDLLPATH", windows_path);
        if (!strcmp(pipeline, "m12")) {
            snprintf(value, sizeof(value), "%s/etc/dxmt.conf", root);
            ENV_PAIR("DXMT_CONFIG_FILE", value);
            ENV_PAIR("DXMT_WINEMETAL_UNIXLIB", "winemetal.so");
        } else if (!strcmp(pipeline, "vkd3d")) {
            snprintf(value, sizeof(value), "%s/etc/vulkan/icd.d/MoltenVK_icd.json", root);
            ENV_PAIR("VK_ICD_FILENAMES", value);
            ENV_PAIR("VK_DRIVER_FILES", value);
        }
        ENV_PAIR("MS_GRAPHICS_BACKEND", !strcmp(pipeline, "m12") ? "dxmt" : "vulkan");
        ENV_PAIR("WINEMSYNC", "1");
        ENV_PAIR("METALSHARP_SHADER_CACHE_PATH", shader_path);
        ENV_PAIR("METALSHARP_PIPELINE_CACHE_PATH", pipeline_path);
        ENV_PAIR("METALSHARP_CACHE_SUMMARY", summary);
        ENV_PAIR("MTL_SHADER_CACHE_DIR", shader_path);
        if (!strcmp(pipeline, "m12")) {
            ENV_PAIR("DXMT_SHADER_CACHE_PATH", shader_path);
            ENV_PAIR("DXMT_PIPELINE_CACHE_PATH", pipeline_path);
        } else if (!strcmp(pipeline, "vkd3d")) {
            ENV_PAIR("DXVK_STATE_CACHE_PATH", shader_path);
            snprintf(value, sizeof(value), "%s/pipeline-cache/%s/%lu", user_home, pipeline, appid);
            ENV_PAIR("DXVK_LOG_PATH", value);
            ENV_PAIR("DXVK_LOG_LEVEL", "info");
            ENV_PAIR("VKD3D_DEBUG", "info");
            ENV_PAIR("VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT", "1");
            ENV_PAIR("MVK_PRESENT_MODE", "1");
            ENV_PAIR("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1");
            ENV_PAIR("MVK_CONFIG_RESUME_LOST_DEVICE", "1");
        }
        if (!strcmp(pipeline, "m12")) {
            ENV_PAIR("DXMT_METALFX_SPATIAL_SWAPCHAIN", "1");
            ENV_PAIR("DXMT_METALFX_SPATIAL", "1");
            ENV_PAIR("DXMT_METALFX_TEMPORAL", "1");
            ENV_PAIR("DXMT_ASYNC_PIPELINE_COMPILE", "1");
            ENV_PAIR("DXMT_D3D12_PSO_WORKERS", "6");
            ENV_PAIR("DXMT_CONFIG",
                     "d3d11.metalSpatialUpscaleFactor=1.43;d3d11.preferredMaxFrameRate=60;dxmt.shaderMetalVersion=310");
        }
#undef ENV_PAIR
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "env_keys_present");
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "WINEDLLOVERRIDES");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "DXMT_SHADER_CACHE_PATH");
    ms_json_writer_bool(&w, !strcmp(pipeline, "m12"));
    ms_json_writer_key(&w, "DYLD_FALLBACK_LIBRARY_PATH");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "SteamAppId");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "DXMT_WINEMETAL_UNIXLIB");
    ms_json_writer_bool(&w, !strcmp(pipeline, "m12"));
    ms_json_writer_object_end(&w);
    ms_json_writer_key(&w, "missing");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < deploy_count; i++) {
        char path[2048];
        struct stat st;
        bool optional =
            strcmp(pipeline, "m12") && (!strncmp(deploy_pe[i], "nvapi", 5) || !strncmp(deploy_pe[i], "nvngx", 5));
        const char* source = !strcmp(pipeline, "vkd3d") && i >= 2 ? "dxvk/x86_64-windows" : deploy_subpath;
        snprintf(path, sizeof(path), "%s/%s/%s", !strcmp(pipeline, "vkd3d") ? lane_root : root, source,
                 deploy_pe[i]);
        if (!optional && (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)) {
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "filename");
            ms_json_writer_string(&w, deploy_pe[i]);
            ms_json_writer_key(&w, "source_subpath");
            ms_json_writer_string(&w, source);
            ms_json_writer_key(&w, "source_path");
            ms_json_writer_string(&w, path);
            ms_json_writer_object_end(&w);
        }
    }
    if (!strcmp(pipeline, "m12")) {
        for (i = 0; i < sizeof(m12_unix) / sizeof(m12_unix[0]); i++) {
            char path[2048];
            struct stat st;
            snprintf(path, sizeof(path), "%s/lib/dxmt_m12/x86_64-unix/%s", root, m12_unix[i]);
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
                ms_json_writer_object_begin(&w);
                ms_json_writer_key(&w, "filename");
                ms_json_writer_string(&w, m12_unix[i]);
                ms_json_writer_key(&w, "source_path");
                ms_json_writer_string(&w, path);
                ms_json_writer_key(&w, "category");
                ms_json_writer_string(&w, "unix_sidecar");
                ms_json_writer_object_end(&w);
            }
        }
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, all_present);
    ms_json_writer_object_end(&w);
    {
        char* result = ms_json_writer_take(&w);
        free(app);
        free(requested);
        return result;
    }
}

static char* fna_diagnostic(const char* kind, const char* query, int* status) {
    char* dir = query_value(query, "gameDir");
    char* app = query_value(query, "appid");
    unsigned long appid = app ? strtoul(app, NULL, 10) : 0;
    fna_signals signals;
    ms_json_writer w;
    char* result;
    bool pinned = appid == 105600 || appid == 504230 || appid == 413150;
    const char* method = appid == 413150 ? "xna_fna_arm64" : "xna_fna_x86";
    const char* config = appid == 105600   ? "generic-fna-mono.config"
                         : appid == 504230 ? "celeste-x86-mono.config"
                         : appid == 413150 ? "stardew-mono.config"
                                           : "generic-fna-mono.config";
    struct stat dir_stat;
    bool dir_ok = dir && *dir && stat(dir, &dir_stat) == 0 && S_ISDIR(dir_stat.st_mode);
    detect_fna(dir, &signals);
    if (!strcmp(kind, "fna-signals")) {
        if (!dir_ok) {
            if (status)
                *status = 400;
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, false);
            ms_json_writer_key(&w, "error");
            ms_json_writer_string(&w, "gameDir is not a directory");
            ms_json_writer_key(&w, "gameDir");
            ms_json_writer_string(&w, dir ? dir : "");
            ms_json_writer_object_end(&w);
            result = ms_json_writer_take(&w);
            free(dir);
            free(app);
            return result;
        }
        ms_json_writer_init(&w);
        write_signals(&w, &signals);
        result = ms_json_writer_take(&w);
        free(dir);
        free(app);
        return result;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    if (!strcmp(kind, "fna-classify")) {
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, appid);
        ms_json_writer_key(&w, "pinned");
        ms_json_writer_bool(&w, pinned);
    }
    if (!strcmp(kind, "fna-explain")) {
        ms_json_writer_key(&w, "schema_version");
        ms_json_writer_u64(&w, 1);
    }
    if (!strcmp(kind, "fna-explain")) {
        ms_json_writer_key(&w, "appid");
        ms_json_writer_u64(&w, appid);
        ms_json_writer_key(&w, "pinned");
        ms_json_writer_bool(&w, pinned);
        ms_json_writer_key(&w, "mono_arch");
        ms_json_writer_string(&w, appid == 413150 ? "native" : "x86");
        ms_json_writer_key(&w, "method_label");
        ms_json_writer_string(&w, method);
        ms_json_writer_key(&w, "mono_config");
        ms_json_writer_string(&w, config);
    }
    if (!strcmp(kind, "fna-classify")) {
        const char* recommendation =
            pinned || strcmp(signals.base_flavor, "unknown") ? "conservative_fna_setup" : "not_fna";
        ms_json_writer_key(&w, "recommendation");
        ms_json_writer_string(&w, recommendation);
    }
    ms_json_writer_key(&w, "signals");
    write_signals(&w, &signals);
    ms_json_writer_key(&w, "rationale");
    ms_json_writer_array_begin(&w);
    if (!strcmp(signals.base_flavor, "unknown")) {
        ms_json_writer_string(&w, "no FNA/MonoGame/XNA assemblies detected; do not force this lane");
    } else if (pinned) {
        char rationale[160];
        snprintf(rationale, sizeof(rationale), "appid %lu is pinned to the %s lane with the %s mono config", appid,
                 method, config);
        ms_json_writer_string(&w, rationale);
        if (!strcmp(kind, "fna-explain")) {
            char detected[96];
            snprintf(detected, sizeof(detected), "detected %s flavor confirms the FNA/XNA family", signals.base_flavor);
            ms_json_writer_string(&w, detected);
        }
    } else {
        char detected[96];
        snprintf(detected, sizeof(detected), "detected %s flavor", signals.base_flavor);
        ms_json_writer_string(&w, detected);
        if (signals.native_mono)
            ms_json_writer_string(&w, "native arm64 macOS executable present; native Mono lane is plausible");
        else if (signals.x86_mono)
            ms_json_writer_string(&w, "no native arm64 executable / x86 indicator present; x86 Mono lane is plausible");
        if (signals.steamworks_net || signals.csteamworks)
            ms_json_writer_string(
                &w, "Steamworks.NET/CSteamworks detected; staging must keep Steam identity shims reversible");
        ms_json_writer_string(&w, "conservative: stage only reversible shims, preserve originals, offer Wine fallback");
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    result = ms_json_writer_take(&w);
    free(dir);
    free(app);
    return result;
}
char* ms_diagnostics_json(const char* kind, const char* query, const unsigned char* body, size_t len, int* status) {
    ms_json_writer w;
    char* o;
    (void)query;
    (void)body;
    (void)len;
    if (status)
        *status = 200;
    if (!strcmp(kind, "launcher-evidence")) {
        char error[96];
        char* id = NULL;
        char* family = NULL;
        ms_json* request = ms_json_parse((const char*)body, len, error, sizeof(error));
        if (request) {
            ms_json_as_string(ms_json_object_get(request, "id"), &id);
            ms_json_as_string(ms_json_object_get(request, "family"), &family);
        }
        if ((!id || !*id) && (!family || !*family)) {
            free(id);
            free(family);
            ms_json_free(request);
            return contract_error("id or family required");
        }
        if (id && *id) {
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, false);
            ms_json_writer_key(&w, "id");
            ms_json_writer_string(&w, id);
            ms_json_writer_key(&w, "error");
            ms_json_writer_string(&w, "bottle not found");
            ms_json_writer_object_end(&w);
            o = ms_json_writer_take(&w);
        } else {
            char message[256];
            snprintf(message, sizeof(message), "no bottle found for launcher family %s", family);
            o = contract_error(message);
        }
        free(id);
        free(family);
        ms_json_free(request);
        return o;
    }
    if (!strcmp(kind, "fna-signals") || !strcmp(kind, "fna-explain") || !strcmp(kind, "fna-classify"))
        return fna_diagnostic(kind, query, status);
    if (!strcmp(kind, "runtime-artifacts"))
        return runtime_artifact_report(status);
    if (!strcmp(kind, "cache-doctor"))
        return cache_doctor_report(query, status);
    if (!strcmp(kind, "pso-manifests"))
        return pso_manifest_report(query, status);
    if (!strcmp(kind, "launch"))
        return launch_diagnostic_report(query, status);
    if (!strcmp(kind, "launch-timing"))
        return launch_timing_report(query, status);
    if (!strcmp(kind, "wineboot-state"))
        return wineboot_state_report(query, status);
    if (!strcmp(kind, "m12-dry-run") || !strcmp(kind, "pipeline-dry-run"))
        return pipeline_diagnostic(kind, query, status);
    if (!strcmp(kind, "binding-contract") || !strcmp(kind, "command-replay")) {
        char parse_error[128];
        ms_json* request_json = ms_json_parse((const char*)body, len, parse_error, sizeof(parse_error));
        if (!strcmp(kind, "command-replay") && (!request_json || !ms_json_object_get(request_json, "trace"))) {
            ms_json_free(request_json);
            request_json = ms_json_parse("{\"trace\":[]}", 12, parse_error, sizeof(parse_error));
        }
        const ms_json* value =
            request_json
                ? ms_json_object_get(request_json, !strcmp(kind, "binding-contract") ? "root_signature" : "trace")
                : NULL;
        bool valid_shape =
            value && ((strcmp(kind, "binding-contract") == 0 && ms_json_type_of(value) == MS_JSON_OBJECT) ||
                      (strcmp(kind, "command-replay") == 0 && ms_json_type_of(value) == MS_JSON_ARRAY));
        if (!valid_shape) {
            const char* prefix =
                !strcmp(kind, "binding-contract") ? "invalid root signature manifest" : "invalid command trace";
            char message[256];
            const char* detail = parse_error[0] ? parse_error : "expected JSON value";
            if (!strcmp(kind, "binding-contract") && request_json && !value)
                detail = "invalid type: null, expected struct RootSignatureManifest";
            snprintf(message, sizeof(message), "%s: %s", prefix, detail);
            ms_json_free(request_json);
            if (status)
                *status = 400;
            return contract_error(message);
        }
        if (!strcmp(kind, "binding-contract")) {
            char* result = binding_contract_report(request_json, status);
            ms_json_free(request_json);
            return result;
        }
        if (!strcmp(kind, "command-replay")) {
            char* result = command_replay_report(request_json, status);
            ms_json_free(request_json);
            return result;
        }
        ms_json_free(request_json);
        ms_json_writer_init(&w);
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "schema_version");
        ms_json_writer_u64(&w, 1);
        ms_json_writer_key(&w, "ok");
        ms_json_writer_bool(&w, true);
        if (!strcmp(kind, "binding-contract")) {
            ms_json_writer_key(&w, "limits");
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "max_direct_buffers");
            ms_json_writer_u64(&w, 31);
            ms_json_writer_key(&w, "max_direct_textures");
            ms_json_writer_u64(&w, 8);
            ms_json_writer_key(&w, "max_direct_samplers");
            ms_json_writer_u64(&w, 16);
            ms_json_writer_object_end(&w);
            ms_json_writer_key(&w, "violations");
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
            ms_json_writer_key(&w, "reflection_abi_mismatches");
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
        } else {
            ms_json_writer_key(&w, "violations");
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
            ms_json_writer_key(&w, "encoder_boundaries");
            ms_json_writer_array_begin(&w);
            ms_json_writer_array_end(&w);
            ms_json_writer_key(&w, "visibility_summary");
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "total_transitions");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "write_to_read_transitions");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "read_to_write_transitions");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "split_barriers");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "unfinished_split_barriers");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_key(&w, "render_passes");
            ms_json_writer_u64(&w, 0);
            ms_json_writer_object_end(&w);
        }
        ms_json_writer_object_end(&w);
        return ms_json_writer_take(&w);
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "kind");
    ms_json_writer_string(&w, kind);
    ms_json_writer_key(&w, "ready");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "issues");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "artifacts");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
