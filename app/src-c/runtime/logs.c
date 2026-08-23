#include "metalsharp_backend/logs.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char* path;
    time_t modified;
} log_entry;

typedef struct {
    log_entry* entries;
    size_t length;
    size_t capacity;
} log_list;

typedef struct {
    char* file;
    char* name;
    char* source;
    char* pipeline;
    char* timestamp;
    unsigned long long size;
    time_t modified;
} crash_report;

typedef struct {
    crash_report* entries;
    size_t length;
    size_t capacity;
} crash_list;

static char* join_path(const char* left, const char* right) {
    size_t a = strlen(left), b = strlen(right);
    bool slash = a > 0 && left[a - 1] != '/';
    char* path = (char*)malloc(a + b + (slash ? 2 : 1));
    if (path != NULL)
        (void)snprintf(path, a + b + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return path;
}

static bool has_log_extension(const char* name) {
    size_t length = strlen(name);
    return length >= 4 && strcmp(name + length - 4, ".log") == 0;
}

static void free_log_list(log_list* list) {
    size_t i;
    for (i = 0; i < list->length; ++i)
        free(list->entries[i].path);
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

static bool add_log(log_list* list, const char* path, time_t modified) {
    log_entry* grown;
    if (list->length == list->capacity) {
        size_t next = list->capacity == 0 ? 16 : list->capacity * 2;
        grown = (log_entry*)realloc(list->entries, next * sizeof(*grown));
        if (grown == NULL)
            return false;
        list->entries = grown;
        list->capacity = next;
    }
    list->entries[list->length].path = strdup(path);
    if (list->entries[list->length].path == NULL)
        return false;
    list->entries[list->length++].modified = modified;
    return true;
}

static void collect_logs(const char* root, const char* path, unsigned depth, log_list* list) {
    struct stat st;
    DIR* dir;
    struct dirent* entry;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISREG(st.st_mode)) {
        if (depth <= 2 && has_log_extension(path))
            (void)add_log(list, path, st.st_mtime);
        return;
    }
    if (!S_ISDIR(st.st_mode) || depth >= 2 || (depth > 0 && strcmp(path, root) == 0))
        return;
    dir = opendir(path);
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        child = join_path(path, entry->d_name);
        if (child != NULL) {
            collect_logs(root, child, depth + 1, list);
            free(child);
        }
    }
    closedir(dir);
}

static int newest_first(const void* left, const void* right) {
    const log_entry* a = (const log_entry*)left;
    const log_entry* b = (const log_entry*)right;
    if (a->modified < b->modified)
        return 1;
    if (a->modified > b->modified)
        return -1;
    return strcmp(a->path, b->path);
}

static char* read_log(const char* path, size_t* length_out) {
    FILE* file = fopen(path, "rb");
    long size;
    char* data;
    size_t length;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL)
            fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || (unsigned long long)size > 16ULL * 1024ULL * 1024ULL || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (char*)malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    length = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[length] = '\0';
    if (length_out != NULL)
        *length_out = length;
    return data;
}

static void write_file_lines(ms_json_writer* writer, const char* path, size_t after, size_t* total_out) {
    char* text;
    size_t length;
    size_t* starts = NULL;
    size_t count = 0, capacity = 0, i, start = 0;
    text = read_log(path, &length);
    if (text == NULL) {
        *total_out = 0;
        ms_json_writer_array_begin(writer);
        ms_json_writer_array_end(writer);
        return;
    }
    for (i = 0; i <= length; ++i) {
        if (i == length || text[i] == '\n') {
            if (i > start || i < length) {
                if (count == capacity) {
                    size_t next = capacity == 0 ? 64 : capacity * 2;
                    size_t* grown = (size_t*)realloc(starts, next * sizeof(*grown));
                    if (grown == NULL) {
                        free(starts);
                        free(text);
                        *total_out = 0;
                        ms_json_writer_array_begin(writer);
                        ms_json_writer_array_end(writer);
                        return;
                    }
                    starts = grown;
                    capacity = next;
                }
                starts[count++] = start;
            }
            start = i + 1;
        }
    }
    *total_out = count;
    ms_json_writer_array_begin(writer);
    if (after < count) {
        size_t begin = after;
        size_t first = count > 500 ? count - 500 : 0;
        if (begin < first)
            begin = first;
        for (i = begin; i < count; ++i) {
            size_t end = (i + 1 < count) ? starts[i + 1] : length;
            size_t line_length = end - starts[i];
            char* line;
            if (line_length > 0 && text[starts[i] + line_length - 1] == '\n')
                line_length--;
            if (line_length > 0 && text[starts[i] + line_length - 1] == '\r')
                line_length--;
            line = (char*)malloc(line_length + 1);
            if (line == NULL)
                continue;
            memcpy(line, text + starts[i], line_length);
            line[line_length] = '\0';
            ms_json_writer_string(writer, line);
            free(line);
        }
    }
    ms_json_writer_array_end(writer);
    free(starts);
    free(text);
}

char* ms_logs_json(const char* metalsharp_home) {
    char* root = join_path(metalsharp_home, "logs");
    log_list list = {0};
    ms_json_writer writer;
    char* result;
    size_t i, limit;
    if (root == NULL)
        return NULL;
    collect_logs(root, root, 0, &list);
    qsort(list.entries, list.length, sizeof(*list.entries), newest_first);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "logs");
    ms_json_writer_array_begin(&writer);
    limit = list.length < 8 ? list.length : 8;
    for (i = 0; i < limit; ++i) {
        char* text = read_log(list.entries[i].path, NULL);
        const char* relative = list.entries[i].path;
        if (strncmp(relative, root, strlen(root)) == 0 && relative[strlen(root)] == '/')
            relative += strlen(root) + 1;
        if (text == NULL)
            continue;
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "name");
        ms_json_writer_string(&writer, relative);
        ms_json_writer_key(&writer, "lines");
        {
            size_t total;
            write_file_lines(&writer, list.entries[i].path, 0, &total);
        }
        ms_json_writer_object_end(&writer);
        free(text);
    }
    if (limit == 0) {
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "name");
        ms_json_writer_string(&writer, "app.log");
        ms_json_writer_key(&writer, "lines");
        ms_json_writer_array_begin(&writer);
        ms_json_writer_string(&writer, "No logs yet. Logs will appear here as you use MetalSharp.");
        ms_json_writer_array_end(&writer);
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free_log_list(&list);
    free(root);
    return result;
}

static size_t query_number(const char* query, const char* key) {
    char needle[64];
    const char* value;
    char* end;
    unsigned long long number;
    (void)snprintf(needle, sizeof(needle), "%s=", key);
    value = query == NULL ? NULL : strstr(query, needle);
    if (value == NULL)
        return 0;
    value += strlen(needle);
    number = strtoull(value, &end, 10);
    return end == value ? 0 : (size_t)number;
}

static char* today_log_path(const char* home) {
    time_t now = time(NULL);
    struct tm local;
    char date[32];
    char* root;
    if (localtime_r(&now, &local) == NULL || strftime(date, sizeof(date), "%Y-%m-%d.log", &local) == 0)
        return NULL;
    root = join_path(home, "logs");
    if (root == NULL)
        return NULL;
    {
        char* path = join_path(root, date);
        free(root);
        return path;
    }
}

static void log_slug(const char* input, char* output, size_t capacity) {
    size_t i = 0;
    if (!input || capacity == 0)
        return;
    while (*input && i + 1 < capacity) {
        unsigned char ch = (unsigned char)*input++;
        output[i++] = (char)(isalnum(ch) ? tolower(ch) : '-');
    }
    output[i] = '\0';
}

void ms_log_event(const char* metalsharp_home, const char* message) {
    char* log_dir;
    char* path;
    FILE* file;
    time_t now;
    struct tm local;
    char stamp[64];
    if (!metalsharp_home || !message)
        return;
    log_dir = join_path(metalsharp_home, "logs");
    if (!log_dir)
        return;
    (void)mkdir(log_dir, 0755);
    free(log_dir);
    path = today_log_path(metalsharp_home);
    if (!path)
        return;
    file = fopen(path, "a");
    if (!file) {
        free(path);
        return;
    }
    now = time(NULL);
    if (localtime_r(&now, &local) && strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %Z", &local) > 0)
        (void)fprintf(file, "[%s] %s\n", stamp, message);
    fclose(file);
    free(path);
}

void ms_issue_log(const char* metalsharp_home, const char* kind, const char* subject, const char* summary) {
    static unsigned sequence;
    char* log_dir;
    char path[2048], stamp[64], kind_slug[96], subject_slug[96];
    struct timespec now;
    struct tm local;
    FILE* file;
    const char* safe_kind = kind ? kind : "unknown";
    const char* safe_subject = subject ? subject : "unknown";
    const char* safe_summary = summary ? summary : "unknown";
    if (!metalsharp_home)
        return;
    log_dir = join_path(metalsharp_home, "logs");
    if (!log_dir)
        return;
    (void)mkdir(log_dir, 0755);
    log_slug(safe_kind, kind_slug, sizeof(kind_slug));
    log_slug(safe_subject, subject_slug, sizeof(subject_slug));
    (void)clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    snprintf(path, sizeof(path), "%s/issue-%s-%ld-%09ld-%u-%s-%s.log", log_dir, stamp, (long)getpid(), now.tv_nsec,
             sequence++, kind_slug, subject_slug);
    file = fopen(path, "w");
    if (file) {
        char human[64];
        strftime(human, sizeof(human), "%Y-%m-%d %H:%M:%S %Z", &local);
        fprintf(file, "timestamp: %s\nkind: %s\nsubject: %s\nsummary: %s\n", human, safe_kind, safe_subject,
                safe_summary);
        fclose(file);
    }
    free(log_dir);
    {
        char message[512];
        snprintf(message, sizeof(message), "[ISSUE] %s | %s | %s", safe_kind, safe_subject, safe_summary);
        ms_log_event(metalsharp_home, message);
    }
}

char* ms_logs_stream_json(const char* metalsharp_home, const char* query) {
    char* path = today_log_path(metalsharp_home);
    size_t after = query_number(query, "after");
    size_t total = 0;
    ms_json_writer writer;
    char* result;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "total");
    {
        /* write_file_lines emits the array after total is written; collect its count first. */
        char* text = path == NULL ? NULL : read_log(path, NULL);
        if (text != NULL) {
            size_t i;
            total = 1;
            for (i = 0; text[i] != '\0'; ++i)
                if (text[i] == '\n' && text[i + 1] != '\0')
                    total++;
            free(text);
        }
    }
    ms_json_writer_u64(&writer, total);
    ms_json_writer_key(&writer, "lines");
    if (path == NULL) {
        ms_json_writer_array_begin(&writer);
        ms_json_writer_array_end(&writer);
    } else
        write_file_lines(&writer, path, after, &total);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(path);
    return result;
}

static bool crash_name(const char* name) {
    char lower[PATH_MAX];
    size_t length = strlen(name);
    if (length >= sizeof(lower))
        length = sizeof(lower) - 1;
    for (size_t i = 0; i < length; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[length] = '\0';
    return strstr(lower, "crash") != NULL || strstr(lower, ".dmp") != NULL || strstr(lower, ".mdmp") != NULL ||
           strstr(lower, "crashdump") != NULL || strstr(lower, "crash_report") != NULL;
}

static void format_report_time(time_t modified, char* output, size_t capacity) {
    struct tm local;
    if (localtime_r(&modified, &local) == NULL || strftime(output, capacity, "%Y-%m-%d %H:%M:%S %Z", &local) == 0)
        snprintf(output, capacity, "%lld", (long long)modified);
}

static bool add_crash_report(crash_list* list, const char* file, const char* source, const char* pipeline,
                             const struct stat* info) {
    crash_report* entry;
    char timestamp[64];
    if (list->length == list->capacity) {
        size_t next = list->capacity == 0 ? 32 : list->capacity * 2;
        crash_report* grown = realloc(list->entries, next * sizeof(*grown));
        if (!grown)
            return false;
        list->entries = grown;
        list->capacity = next;
    }
    format_report_time(info->st_mtime, timestamp, sizeof(timestamp));
    entry = &list->entries[list->length];
    memset(entry, 0, sizeof(*entry));
    entry->file = strdup(file);
    entry->name = strdup(strrchr(file, '/') ? strrchr(file, '/') + 1 : file);
    entry->source = strdup(source);
    entry->pipeline = strdup(pipeline);
    entry->timestamp = strdup(timestamp);
    entry->size = (unsigned long long)(info->st_size < 0 ? 0 : info->st_size);
    entry->modified = info->st_mtime;
    if (!entry->file || !entry->name || !entry->source || !entry->pipeline || !entry->timestamp) {
        free(entry->file);
        free(entry->name);
        free(entry->source);
        free(entry->pipeline);
        free(entry->timestamp);
        memset(entry, 0, sizeof(*entry));
        return false;
    }
    list->length++;
    return true;
}

static void free_crash_list(crash_list* list) {
    for (size_t i = 0; i < list->length; i++) {
        free(list->entries[i].file);
        free(list->entries[i].name);
        free(list->entries[i].source);
        free(list->entries[i].pipeline);
        free(list->entries[i].timestamp);
    }
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

static int newest_crash_first(const void* left, const void* right) {
    const crash_report* a = left;
    const crash_report* b = right;
    if (a->modified < b->modified)
        return 1;
    if (a->modified > b->modified)
        return -1;
    return strcmp(a->file, b->file);
}

static bool mkdirs(const char* path) {
    char* copy = strdup(path);
    if (!copy)
        return false;
    for (char* p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static void persist_crash_report(const char* home, const crash_report* report) {
    char* root = join_path(home, "logs/crash-reports");
    char source[96], name[128], timestamp[128], path[PATH_MAX];
    FILE* file;
    if (!root || !mkdirs(root)) {
        free(root);
        return;
    }
    log_slug(report->source, source, sizeof(source));
    log_slug(report->name, name, sizeof(name));
    log_slug(report->timestamp, timestamp, sizeof(timestamp));
    snprintf(path, sizeof(path), "%s/crash-%s-%s-%s.log", root, source, name, timestamp);
    if (access(path, F_OK) != 0 && (file = fopen(path, "w")) != NULL) {
        fprintf(file, "timestamp: %s\ncrash_timestamp: %s\nsource: %s\nfile: %s\nsize_bytes: %llu\n", report->timestamp,
                report->timestamp, report->source, report->file, report->size);
        fclose(file);
    }
    free(root);
}

static void scan_crash_directory(const char* home, const char* path, const char* source, const char* pipeline,
                                 unsigned depth, crash_list* reports) {
    DIR* dir;
    struct dirent* entry;
    if (depth > 2 || !path || !(dir = opendir(path)))
        return;
    while ((entry = readdir(dir)) != NULL) {
        char* child;
        struct stat info;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        child = join_path(path, entry->d_name);
        if (!child || lstat(child, &info) != 0) {
            free(child);
            continue;
        }
        if (S_ISREG(info.st_mode) && crash_name(entry->d_name) && add_crash_report(reports, child, source, pipeline, &info))
            persist_crash_report(home, &reports->entries[reports->length - 1]);
        if (S_ISDIR(info.st_mode))
            scan_crash_directory(home, child, source, pipeline, depth + 1, reports);
        free(child);
    }
    closedir(dir);
}

static char* bottle_pipeline_label(const char* home, const char* bottle_id) {
    char* bottle = join_path(home, "bottles");
    char* path;
    FILE* file;
    char raw[1024 * 1024];
    size_t length;
    char error[96];
    ms_json* json;
    char* pipeline = NULL;
    char* result;
    if (!bottle)
        return strdup("System");
    path = join_path(bottle, bottle_id);
    free(bottle);
    bottle = path ? join_path(path, "bottle.json") : NULL;
    free(path);
    file = bottle ? fopen(bottle, "rb") : NULL;
    if (!file) {
        free(bottle);
        return strdup("System");
    }
    length = fread(raw, 1, sizeof(raw) - 1, file);
    fclose(file);
    raw[length] = '\0';
    json = ms_json_parse(raw, length, error, sizeof(error));
    if (json) {
        (void)ms_json_as_string(ms_json_object_get(json, "preferred_pipeline"), &pipeline);
        if (!pipeline)
            (void)ms_json_as_string(ms_json_object_get(json, "runtime_profile"), &pipeline);
    }
    result = pipeline ? strdup(pipeline) : strdup("System");
    free(pipeline);
    ms_json_free(json);
    free(bottle);
    return result;
}

char* ms_crash_reports_json(const char* metalsharp_home) {
    crash_list reports = {0};
    char* root = join_path(metalsharp_home, "games");
    char* bottles = join_path(metalsharp_home, "bottles");
    char* prefix = join_path(metalsharp_home, "prefix-steam/drive_c");
    ms_json_writer writer;
    char* result;
    if (root) {
        DIR* dir = opendir(root);
        struct dirent* entry;
        if (dir) {
            while ((entry = readdir(dir)) != NULL) {
                char* game = join_path(root, entry->d_name);
                if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") && game)
                    scan_crash_directory(metalsharp_home, game, entry->d_name, "Other", 0, &reports);
                free(game);
            }
            closedir(dir);
        }
    }
    if (bottles) {
        DIR* dir = opendir(bottles);
        struct dirent* entry;
        if (dir) {
            while ((entry = readdir(dir)) != NULL) {
                char* logs = join_path(bottles, entry->d_name);
                char* log_dir = logs ? join_path(logs, "logs") : NULL;
                char* pipeline = NULL;
                if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") && log_dir) {
                    pipeline = bottle_pipeline_label(metalsharp_home, entry->d_name);
                    scan_crash_directory(metalsharp_home, log_dir, entry->d_name, pipeline ? pipeline : "System", 0, &reports);
                }
                free(pipeline);
                free(log_dir);
                free(logs);
            }
            closedir(dir);
        }
    }
    if (prefix) {
        char* dumps = join_path(prefix, "Program Files (x86)/Steam/dumps");
        char* local = join_path(prefix, "users/steamuser/AppData/Local/CrashDumps");
        char* program = join_path(prefix, "ProgramData/CrashDumps");
        scan_crash_directory(metalsharp_home, dumps, "steam-dumps", "System", 0, &reports);
        scan_crash_directory(metalsharp_home, local, "system", "System", 0, &reports);
        scan_crash_directory(metalsharp_home, program, "system", "System", 0, &reports);
        free(dumps);
        free(local);
        free(program);
    }
    qsort(reports.entries, reports.length, sizeof(*reports.entries), newest_crash_first);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "reports");
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < reports.length; i++) {
        crash_report* report = &reports.entries[i];
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "file");
        ms_json_writer_string(&writer, report->file);
        ms_json_writer_key(&writer, "name");
        ms_json_writer_string(&writer, report->name);
        ms_json_writer_key(&writer, "source");
        ms_json_writer_string(&writer, report->source);
        ms_json_writer_key(&writer, "pipeline");
        ms_json_writer_string(&writer, report->pipeline);
        ms_json_writer_key(&writer, "timestamp");
        ms_json_writer_string(&writer, report->timestamp);
        ms_json_writer_key(&writer, "size_bytes");
        ms_json_writer_u64(&writer, report->size);
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free_crash_list(&reports);
    free(root);
    free(bottles);
    free(prefix);
    return result;
}
