/* Per-launch game logs (#834); see launch_log.h. */
#define _DARWIN_C_SOURCE
#include "metalsharp_backend/launch_log.h"
#include "metalsharp_backend/logs.h"
#include "metalsharp_backend/wine_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MS_BACKEND_VERSION
#define MS_BACKEND_VERSION "dev"
#endif

/* The most of a log that is grouped; a longer one keeps its end, where the problem is. */
#define GROUP_MOST_BYTES (32u << 20)
/* Distinct lines remembered while grouping; past that, lines are kept as they are. */
#define GROUP_MOST_DISTINCT 4096u
/* Slots of the table that finds a line's distinct line: a power of two, at least twice GROUP_MOST_DISTINCT. */
#define GROUP_SLOTS 16384u

static bool make_directory(const char* path) {
    char partial[PATH_MAX];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(partial))
        return false;
    memcpy(partial, path, length + 1);
    for (char* at = partial + 1; *at; at++) {
        if (*at != '/')
            continue;
        *at = '\0';
        if (mkdir(partial, 0755) != 0 && errno != EEXIST)
            return false;
        *at = '/';
    }
    return mkdir(partial, 0755) == 0 || errno == EEXIST;
}

/* A route name for a file name: letters, digits, dashes and underscores. */
static void file_word(const char* input, char* output, size_t capacity) {
    size_t i = 0;
    for (; input && *input && i + 1 < capacity; input++)
        output[i++] = isalnum((unsigned char)*input) || *input == '_' ? *input : '-';
    output[i] = '\0';
    if (i == 0 && capacity > 1)
        snprintf(output, capacity, "game");
}

static void sysctl_text(const char* name, char* out, size_t out_size) {
    size_t length = out_size;
    if (out_size == 0)
        return;
    if (sysctlbyname(name, out, &length, NULL, 0) != 0)
        out[0] = '\0';
    else
        out[length < out_size ? length : out_size - 1] = '\0';
}

/* The first line of a file, trimmed; empty when there is none. */
static void first_line(const char* path, char* out, size_t out_size) {
    FILE* file = fopen(path, "r");
    out[0] = '\0';
    if (!file)
        return;
    if (fgets(out, (int)out_size, file)) {
        size_t length = strlen(out);
        while (length && isspace((unsigned char)out[length - 1]))
            out[--length] = '\0';
    }
    fclose(file);
}

/* The value of `name` under the registry key `key` (as Wine writes it: [Software\\Wine]) in a .reg file. */
static bool reg_value(const char* file_path, const char* key, const char* name, char* out, size_t out_size) {
    FILE* file = fopen(file_path, "r");
    char line[4096], quoted[256];
    bool in_key = false, found = false;
    size_t key_length = strlen(key);
    if (!file)
        return false;
    snprintf(quoted, sizeof(quoted), "\"%s\"=", name);
    while (!found && fgets(line, sizeof(line), file)) {
        if (line[0] == '[') {
            in_key = !strncmp(line + 1, key, key_length) && line[1 + key_length] == ']';
            continue;
        }
        if (in_key && !strncmp(line, quoted, strlen(quoted))) {
            char* value = line + strlen(quoted);
            size_t length = strlen(value);
            while (length && isspace((unsigned char)value[length - 1]))
                value[--length] = '\0';
            if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
                value[length - 1] = '\0';
                value++;
            }
            snprintf(out, out_size, "%s", value);
            found = true;
        }
    }
    fclose(file);
    return found;
}

void ms_launch_log_prefix_settings(const char* prefix, char* windows, size_t windows_size, char* dpi, size_t dpi_size) {
    char system_reg[PATH_MAX], user_reg[PATH_MAX], product[256] = "", build[64] = "", pixels[64] = "";
    if (windows_size)
        windows[0] = '\0';
    if (dpi_size)
        dpi[0] = '\0';
    if (!prefix)
        return;
    snprintf(system_reg, sizeof(system_reg), "%s/system.reg", prefix);
    snprintf(user_reg, sizeof(user_reg), "%s/user.reg", prefix);
    (void)reg_value(system_reg, "Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion", "ProductName", product,
                    sizeof(product));
    (void)reg_value(system_reg, "Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion", "CurrentBuild", build,
                    sizeof(build));
    if (product[0] && windows_size)
        snprintf(windows, windows_size, build[0] ? "%s (build %s)" : "%s", product, build);
    if (reg_value(user_reg, "Control Panel\\\\Desktop", "LogPixels", pixels, sizeof(pixels)) &&
        !strncmp(pixels, "dword:", 6) && dpi_size) {
        unsigned long value = strtoul(pixels + 6, NULL, 16);
        if (value)
            snprintf(dpi, dpi_size, "%lu (%lu %%)", value, value * 100 / 96);
    }
}

/* logs/launches/last/<appid>: the route and end of a game's last launch, read by its next one. */
static void last_launch_path(const char* home, unsigned appid, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/logs/launches/last/%u", home, appid);
}

static void append_text(const char* path, const char* text) {
    FILE* file = fopen(path, "a");
    if (!file)
        return;
    fputs(text, file);
    fclose(file);
}

bool ms_launch_log_begin(const char* home, unsigned appid, const char* route, const char* runtime, char* path,
                         size_t path_size) {
    char directory[PATH_MAX], stamp[64], word[64], chip[256], os_version[64], os_build[64], text[8192];
    char windows[256], dpi[64], prefix[PATH_MAX], last_path[PATH_MAX], last[1024] = "";
    uint64_t memory = 0;
    size_t memory_size = sizeof(memory);
    time_t now = time(NULL);
    struct tm local;
    FILE* file;
    int length;
    if (!home || !path || path_size == 0)
        return false;
    path[0] = '\0';
    snprintf(directory, sizeof(directory), "%s/logs/launches", home);
    if (!make_directory(directory) || !localtime_r(&now, &local) ||
        !strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local))
        return false;
    file_word(route, word, sizeof(word));
    length = snprintf(path, path_size, "%s/%s-%u-%s.log", directory, stamp, appid, word);
    if (length <= 0 || (size_t)length >= path_size || !(file = fopen(path, "w"))) {
        path[0] = '\0';
        return false;
    }
    sysctl_text("machdep.cpu.brand_string", chip, sizeof(chip));
    sysctl_text("kern.osproductversion", os_version, sizeof(os_version));
    sysctl_text("kern.osversion", os_build, sizeof(os_build));
    if (sysctlbyname("hw.memsize", &memory, &memory_size, NULL, 0) != 0)
        memory = 0;
    (void)!strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %Z", &local);
    fprintf(file, "MetalSharp launch log: appid %u, route %s, %s runtime, %s\n", appid, route ? route : "?",
            runtime ? runtime : "?", stamp);
    fprintf(file, "Mac: %s, macOS %s (%s), %llu GB memory\n", chip[0] ? chip : "unknown chip",
            os_version[0] ? os_version : "?", os_build[0] ? os_build : "?",
            (unsigned long long)(memory / (1024ULL * 1024ULL * 1024ULL)));
    fprintf(file, "MetalSharp %s, MetalSharp Wine %s\n", MS_BACKEND_VERSION, ms_wine_release_version());
    {
        static const char* const renderers[] = {"mullion",      "dxmt",        "dxvk",
                                                "vkd3d-proton", "winemetalgl", "wine-opengl"};
        for (size_t i = 0; i < sizeof(renderers) / sizeof(renderers[0]); i++) {
            char version_path[PATH_MAX], version[512];
            snprintf(version_path, sizeof(version_path), "%s/runtime/wine/lib/%s/VERSION", home, renderers[i]);
            first_line(version_path, version, sizeof(version));
            if (version[0])
                fprintf(file, "  %s: %s\n", renderers[i], version);
        }
    }
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    ms_launch_log_prefix_settings(prefix, windows, sizeof(windows), dpi, sizeof(dpi));
    fprintf(file, "Wine settings: Windows %s, DPI %s (sync, DLL overrides and the rest in the summary's env lines)\n",
            windows[0] ? windows : "not set", dpi[0] ? dpi : "96 (100 %, Wine's default)");
    /* the game's last launch, when it was on another route: how that one ended */
    last_launch_path(home, appid, last_path, sizeof(last_path));
    {
        FILE* record = fopen(last_path, "r");
        char last_route[128] = "", line[1024];
        if (record) {
            while (fgets(line, sizeof(line), record)) {
                if (!strncmp(line, "route=", 6)) {
                    snprintf(last_route, sizeof(last_route), "%s", line + 6);
                    last_route[strcspn(last_route, "\n")] = '\0';
                } else if (!strncmp(line, "ended=", 6)) {
                    snprintf(last, sizeof(last), "%s", line + 6);
                    last[strcspn(last, "\n")] = '\0';
                }
            }
            fclose(record);
        }
        if (last_route[0] && route && strcmp(last_route, route) && last[0]) {
            char today[PATH_MAX], day_stamp[64];
            snprintf(text, sizeof(text), "Route changed from %s to %s. The last launch, on %s: %s\n", last_route, route,
                     last_route, last);
            fputs(text, file);
            if (ms_log_today_path(home, today, sizeof(today)) && ms_log_stamp(day_stamp, sizeof(day_stamp))) {
                char line_text[sizeof(text) + 160];
                snprintf(line_text, sizeof(line_text), "[%s] appid=%u %s", day_stamp, appid, text);
                append_text(today, line_text);
            }
        }
    }
    fputs("\n", file);
    fclose(file);
    return true;
}

/* Wine's thread id at the start of its lines ("0124:err:..."), which differs between repeats of a line. */
static const char* without_thread(const char* line) {
    size_t i = 0;
    while (isxdigit((unsigned char)line[i]) && i < 8)
        i++;
    return i >= 4 && line[i] == ':' ? line + i + 1 : line;
}

typedef struct {
    const char* text; /* without its thread id */
    size_t first;     /* the line it first appears at */
    unsigned long count;
} distinct_line;

static uint64_t text_hash(const char* text) {
    uint64_t hash = 1469598103934665603ULL;
    for (; *text; text++)
        hash = (hash ^ (unsigned char)*text) * 1099511628211ULL;
    return hash;
}

static bool read_whole(const char* path, char** data, size_t* length, size_t* skipped) {
    FILE* file = fopen(path, "rb");
    long size;
    *data = NULL;
    *length = *skipped = 0;
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) {
        fclose(file);
        return false;
    }
    if ((size_t)size > GROUP_MOST_BYTES) {
        *skipped = (size_t)size - GROUP_MOST_BYTES;
        size = GROUP_MOST_BYTES;
    }
    if (fseek(file, (long)*skipped, SEEK_SET) != 0 || !(*data = malloc((size_t)size + 1))) {
        fclose(file);
        return false;
    }
    *length = fread(*data, 1, (size_t)size, file);
    (*data)[*length] = '\0';
    fclose(file);
    return true;
}

bool ms_launch_log_group_repeats(const char* path) {
    char *data, *at, temp[PATH_MAX];
    size_t length, skipped, lines = 0, capacity = 1024, distinct_count = 0;
    char** line_list;
    distinct_line* distinct;
    size_t* owner;
    unsigned* slots;
    FILE* out;
    if (!path || !read_whole(path, &data, &length, &skipped))
        return false;
    line_list = malloc(capacity * sizeof(*line_list));
    distinct = calloc(GROUP_MOST_DISTINCT, sizeof(*distinct));
    slots = calloc(GROUP_SLOTS, sizeof(*slots)); /* a distinct line's index + 1; 0 for an empty slot */
    if (!line_list || !distinct || !slots) {
        free(line_list);
        free(distinct);
        free(slots);
        free(data);
        return false;
    }
    for (at = data; *at;) {
        char* end = strchr(at, '\n');
        if (lines == capacity) {
            char** grown = realloc(line_list, (capacity *= 2) * sizeof(*line_list));
            if (!grown)
                break;
            line_list = grown;
        }
        line_list[lines++] = at;
        if (!end)
            break;
        *end = '\0';
        at = end + 1;
    }
    owner = malloc((lines ? lines : 1) * sizeof(*owner));
    if (!owner) {
        free(line_list);
        free(distinct);
        free(slots);
        free(data);
        return false;
    }
    /* owner[i]: the distinct line line i repeats, or SIZE_MAX for a line kept as it is */
    for (size_t i = 0; i < lines; i++) {
        const char* text = without_thread(line_list[i]);
        size_t found = SIZE_MAX;
        owner[i] = SIZE_MAX;
        size_t slot;
        if (!*text)
            continue;
        for (slot = (size_t)(text_hash(text) & (GROUP_SLOTS - 1)); slots[slot]; slot = (slot + 1) & (GROUP_SLOTS - 1))
            if (!strcmp(distinct[slots[slot] - 1].text, text)) {
                found = slots[slot] - 1;
                break;
            }
        if (found == SIZE_MAX && distinct_count < GROUP_MOST_DISTINCT) {
            distinct[distinct_count] = (distinct_line){text, i, 0};
            found = distinct_count++;
            slots[slot] = (unsigned)found + 1;
        }
        if (found != SIZE_MAX) {
            distinct[found].count++;
            owner[i] = found;
        }
    }
    snprintf(temp, sizeof(temp), "%s.grouping", path);
    out = fopen(temp, "w");
    if (out) {
        if (skipped)
            fprintf(out, "[the first %zu bytes are left out]\n", skipped);
        for (size_t i = 0; i < lines; i++) {
            if (owner[i] == SIZE_MAX) {
                fprintf(out, "%s\n", line_list[i]);
                continue;
            }
            if (distinct[owner[i]].first != i)
                continue;
            if (distinct[owner[i]].count > 1)
                fprintf(out, "%s  [repeated %lu times]\n", line_list[i], distinct[owner[i]].count);
            else
                fprintf(out, "%s\n", line_list[i]);
        }
    }
    free(owner);
    free(line_list);
    free(distinct);
    free(slots);
    free(data);
    if (!out || fclose(out) != 0 || rename(temp, path) != 0) {
        (void)unlink(temp);
        return false;
    }
    return true;
}

/* Whether the log shows Wine reporting a crash: an unhandled exception, or its crash dialog's text. */
static bool log_shows_crash(const char* path) {
    FILE* file = fopen(path, "r");
    char line[4096];
    bool crashed = false;
    if (!file)
        return false;
    while (!crashed && fgets(line, sizeof(line), file))
        crashed = strstr(line, "Unhandled exception") || strstr(line, "unhandled exception") ||
                  strstr(line, "Unhandled page fault") || strstr(line, "wine: Assertion failed");
    fclose(file);
    return crashed;
}

typedef struct {
    char home[PATH_MAX];
    char path[PATH_MAX];
    char route[128];
    unsigned appid;
    bool exited;
    int status;
    bool stopped;
    unsigned long long ran_ms;
} launch_end;

static void* launch_end_worker(void* raw) {
    launch_end* end = raw;
    char ending[512], ran[64], text[1024], today[PATH_MAX], stamp[64], last_path[PATH_MAX];
    unsigned long long seconds = end->ran_ms / 1000ULL;
    bool crashed = false;
    if (seconds >= 3600)
        snprintf(ran, sizeof(ran), "%lluh %llum", seconds / 3600, seconds % 3600 / 60);
    else if (seconds >= 60)
        snprintf(ran, sizeof(ran), "%llum %llus", seconds / 60, seconds % 60);
    else
        snprintf(ran, sizeof(ran), "%llus", seconds);
    if (end->stopped)
        snprintf(ending, sizeof(ending), "stopped from MetalSharp after %s", ran);
    else if (end->exited && WIFSIGNALED(end->status)) {
        int signal_number = WTERMSIG(end->status);
        crashed =
            signal_number != SIGTERM && signal_number != SIGKILL && signal_number != SIGINT && signal_number != SIGHUP;
        snprintf(ending, sizeof(ending), "killed by signal %d (%s) after %s", signal_number, strsignal(signal_number),
                 ran);
    } else if (end->exited && WIFEXITED(end->status)) {
        crashed = WEXITSTATUS(end->status) != 0;
        snprintf(ending, sizeof(ending), "exit code %d after %s", WEXITSTATUS(end->status), ran);
    } else
        snprintf(ending, sizeof(ending), "ended after about %s (its exit code isn't known)", ran);
    if (!end->stopped && end->path[0] && log_shows_crash(end->path))
        crashed = true;
    if (crashed)
        strncat(ending, "; it looks like a crash", sizeof(ending) - strlen(ending) - 1);
    if (end->path[0]) {
        (void)ms_launch_log_group_repeats(end->path);
        snprintf(text, sizeof(text), "\nLaunch ended: %s\n", ending);
        append_text(end->path, text);
    }
    if (ms_log_today_path(end->home, today, sizeof(today)) && ms_log_stamp(stamp, sizeof(stamp))) {
        snprintf(text, sizeof(text), "[%s] Launch ended: appid=%u route=%s, %s%s%s\n", stamp, end->appid, end->route,
                 ending, end->path[0] ? ". Log: " : "", end->path[0] ? end->path : "");
        append_text(today, text);
    }
    last_launch_path(end->home, end->appid, last_path, sizeof(last_path));
    {
        char directory[PATH_MAX];
        snprintf(directory, sizeof(directory), "%s/logs/launches/last", end->home);
        if (make_directory(directory)) {
            FILE* record = fopen(last_path, "w");
            if (record) {
                fprintf(record, "route=%s\nended=%s\n", end->route, ending);
                fclose(record);
            }
        }
    }
    free(end);
    return NULL;
}

void ms_launch_log_end(const char* home, const char* path, unsigned appid, const char* route, bool exited, int status,
                       bool stopped, unsigned long long ran_ms) {
    launch_end* end;
    pthread_t thread;
    pthread_attr_t attributes;
    if (!home || !appid || !(end = calloc(1, sizeof(*end))))
        return;
    snprintf(end->home, sizeof(end->home), "%s", home);
    snprintf(end->path, sizeof(end->path), "%s", path ? path : "");
    snprintf(end->route, sizeof(end->route), "%s", route ? route : "?");
    end->appid = appid;
    end->exited = exited;
    end->status = status;
    end->stopped = stopped;
    end->ran_ms = ran_ms;
    if (pthread_attr_init(&attributes) != 0) {
        launch_end_worker(end);
        return;
    }
    (void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attributes, launch_end_worker, end) != 0)
        launch_end_worker(end);
    pthread_attr_destroy(&attributes);
}
