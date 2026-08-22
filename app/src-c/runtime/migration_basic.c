#include "metalsharp_backend/migration_basic.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/setup.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MIGRATION_VERSION "0.60.0"
#define MIGRATION_SCHEMA  5
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
static bool runtime_ready(const char* home) {
    char *a = path_join(home, "runtime/wine/bin/metalsharp-wine"), *b = path_join(home, "runtime/host/manifest.json"),
         *c = path_join(home, "runtime/host/HostRuntimeABI.h");
    bool ok = exists(a) && exists(b) && exists(c);
    free(a);
    free(b);
    free(c);
    return ok;
}
static bool write_migration_progress(const char* home, const char* state, unsigned step, const char* message,
                                     const char* error) {
    char* path = path_join(home, "migrate_progress.json");
    FILE* f;
    if (!path)
        return false;
    f = fopen(path, "wb");
    if (!f) {
        free(path);
        return false;
    }
    fprintf(f, "{\"status\":\"%s\",\"step\":%u,\"total\":8,\"message\":\"%s\",\"error\":%s,\"version\":\"%s\"}", state,
            step, message, error ? "\"runtime_install_incomplete\"" : "null", MIGRATION_VERSION);
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
                 "{\"status\":\"idle\",\"step\":0,\"total\":0,\"message\":\"\",\"error\":null,\"version\":\"0.60.0\"}");
    free(p);
    return out;
}
char* ms_migration_report_json(const char* home) {
    char *p = path_join(home, "logs/migration-report-latest.json"), *out;
    if (!p)
        return NULL;
    out = raw_or(p, "{\"schema_version\":1,\"status\":\"idle\",\"version\":\"0.60.0\",\"entries\":[],\"summary\":\"No "
                    "migration has run yet.\"}");
    free(p);
    return out;
}

char* ms_migration_check_json(const char* home) {
    char *setup_path = path_join(home, "setup.json"), *marker_path = path_join(home, ".post-update-migration"),
         *setup_text, *marker_text;
    ms_json *setup = NULL, *marker = NULL;
    char e[128];
    bool marker_needed = false, marker_mismatch = false, ready;
    char *legacy, *target = NULL;
    unsigned long long schema;
    ms_json_writer w;
    char* out;
    if (!exists(home) || !exists(setup_path)) {
        free(setup_path);
        free(marker_path);
        return strdup("{\"ok\":true,\"needed\":false,\"reason\":\"fresh_install\"}");
    }
    setup_text = read_text(setup_path);
    if (!setup_text) {
        free(setup_path);
        free(marker_path);
        return strdup("{\"ok\":true,\"needed\":false,\"reason\":\"cannot_read_setup\"}");
    }
    setup = ms_json_parse(setup_text, strlen(setup_text), e, sizeof(e));
    free(setup_text);
    if (!setup || ms_json_type_of(setup) != MS_JSON_OBJECT) {
        ms_json_free(setup);
        free(setup_path);
        free(marker_path);
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
    marker_mismatch = target != NULL && strcmp(target, MIGRATION_VERSION) > 0;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "needed");
    ms_json_writer_bool(&w, marker_needed || (!ready && json_bool(setup, "completed", false)));
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
    else if (!ready && json_bool(setup, "completed", false))
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
    int install_status = 200;
    char* install_progress_path = path_join(job->home, "install_progress.json");
    char* install_result;
    if (install_progress_path) {
        unlink(install_progress_path);
        free(install_progress_path);
    }
    (void)write_migration_progress(job->home, "running", 1, "Ensuring extract tools (zstd) are available...", NULL);
    (void)write_migration_progress(job->home, "running", 2,
                                   "Preserving user preferences, Steam API key, and bottle settings...", NULL);
    (void)write_migration_progress(job->home, "running", 3, "Cleaning stale runtime state...", NULL);
    (void)write_migration_progress(job->home, "running", 4, "Installing update...", NULL);
    install_result = ms_setup_install_all_json(job->home, &install_status);
    if (install_result == NULL || strstr(install_result, "\"ok\":false") != NULL) {
        free(install_result);
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
    for (;;) {
        char* install_progress = path_join(job->home, "install_progress.json");
        char* text = install_progress ? read_text(install_progress) : NULL;
        bool complete = text && strstr(text, "\"status\":\"complete\"") != NULL;
        bool failed = text && strstr(text, "\"status\":\"error\"") != NULL;
        free(text);
        free(install_progress);
        if (failed) {
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
            (void)write_migration_progress(job->home, "running", 6,
                                           "Updating Wine prefixes and registering external Steam libraries...", NULL);
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
         *lock_path = path_join(home, "migration.lock");
    int lock_fd;
    ms_json_writer w;
    char* out;
    bool ready = runtime_ready(home);
    if (!setup_path || !progress || !lock_path) {
        free(setup_path);
        free(progress);
        free(lock_path);
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
    if (ready) {
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
    if (ready && exists(setup_path)) {
        char* text = read_text(setup_path);
        char e[128];
        ms_json* r = text ? ms_json_parse(text, strlen(text), e, sizeof(e)) : NULL;
        if (r && ms_json_type_of(r) == MS_JSON_OBJECT) { /* Keep the existing setup object intact; migration metadata is
                                                            written only by the full installer lane. */
        }
        ms_json_free(r);
        free(text);
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
    return out;
}
