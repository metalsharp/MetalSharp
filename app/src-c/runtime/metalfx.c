#include "metalsharp_backend/metalfx.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    bool enabled;
    double factor;
    unsigned long long timestamp;
} metalfx_state;

static char* join_path(const char* left, const char* right) {
    size_t a = strlen(left), b = strlen(right);
    bool slash = a > 0 && left[a - 1] != '/';
    char* out = (char*)malloc(a + b + (slash ? 2 : 1));
    if (out != NULL)
        (void)snprintf(out, a + b + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return out;
}

static bool mkdir_p(const char* path) {
    char* copy = strdup(path);
    size_t i;
    if (copy == NULL)
        return false;
    for (i = 1; copy[i] != '\0'; ++i) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            copy[i] = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static char* read_file(const char* path, size_t* length_out) {
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
    if (size < 0 || (unsigned long long)size > 4ULL * 1024ULL * 1024ULL || fseek(file, 0, SEEK_SET) != 0) {
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

static unsigned long long now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (unsigned long long)ts.tv_sec;
}

static char* state_path(const char* home) {
    char* etc = join_path(home, "etc");
    char* path = etc == NULL ? NULL : join_path(etc, "metalfx.overlay.json");
    free(etc);
    return path;
}

static char* conf_path(const char* home) {
    char* runtime = join_path(home, "runtime/wine");
    char* etc = runtime == NULL ? NULL : join_path(runtime, "etc");
    char* path = etc == NULL ? NULL : join_path(etc, "dxmt.conf");
    free(runtime);
    free(etc);
    return path;
}

static metalfx_state read_state(const char* home) {
    metalfx_state state = {true, 1.5, 0};
    char* path = state_path(home);
    size_t length;
    char* text;
    char error[128];
    ms_json* json;
    bool enabled;
    double factor;
    long long timestamp;
    if (path == NULL)
        return state;
    text = read_file(path, &length);
    free(path);
    if (text == NULL)
        return state;
    json = ms_json_parse(text, length, error, sizeof(error));
    free(text);
    if (json == NULL)
        return state;
    if (ms_json_as_bool(ms_json_object_get(json, "enabled"), &enabled))
        state.enabled = enabled;
    if (ms_json_as_number(ms_json_object_get(json, "factor"), &factor) && isfinite(factor))
        state.factor = factor;
    if (ms_json_as_i64(ms_json_object_get(json, "ts"), &timestamp) && timestamp >= 0)
        state.timestamp = (unsigned long long)timestamp;
    ms_json_free(json);
    return state;
}

static bool write_state(const char* home, metalfx_state state) {
    char* etc = join_path(home, "etc");
    char* path = state_path(home);
    ms_json_writer writer;
    char* json;
    FILE* file;
    bool ok;
    if (etc == NULL || path == NULL || !mkdir_p(etc)) {
        free(etc);
        free(path);
        return false;
    }
    free(etc);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "enabled");
    ms_json_writer_bool(&writer, state.enabled);
    ms_json_writer_key(&writer, "factor");
    ms_json_writer_double(&writer, state.factor);
    ms_json_writer_key(&writer, "ts");
    ms_json_writer_u64(&writer, state.timestamp);
    ms_json_writer_object_end(&writer);
    json = ms_json_writer_take(&writer);
    if (json == NULL) {
        free(path);
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        free(json);
        free(path);
        return false;
    }
    ok = fputs(json, file) >= 0 && fclose(file) == 0;
    if (!ok)
        fclose(file);
    free(json);
    free(path);
    return ok;
}

static bool line_is_factor(const char* line) {
    while (*line != '\0' && isspace((unsigned char)*line))
        line++;
    return strncmp(line, "d3d11.metalSpatialUpscaleFactor", strlen("d3d11.metalSpatialUpscaleFactor")) == 0;
}

static double read_conf_factor(const char* home) {
    char* path = conf_path(home);
    char* text;
    char* line;
    double result = 1.5;
    if (path == NULL)
        return result;
    text = read_file(path, NULL);
    free(path);
    if (text == NULL)
        return result;
    for (line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char* equals;
        char* end;
        if (!line_is_factor(line))
            continue;
        equals = strchr(line, '=');
        if (equals == NULL)
            equals = line + strlen("d3d11.metalSpatialUpscaleFactor");
        errno = 0;
        result = strtod(equals + (equals[0] == '=' ? 1 : 0), &end);
        if (errno != 0 || end == equals || !isfinite(result))
            result = 1.5;
        break;
    }
    free(text);
    return result;
}

static bool write_conf_factor(const char* home, double factor) {
    char* path = conf_path(home);
    char* parent;
    char* text;
    char line[128];
    size_t capacity = 1024, length = 0;
    char* output;
    char* cursor;
    bool replaced = false;
    FILE* file;
    if (path == NULL)
        return false;
    parent = strdup(path);
    if (parent == NULL) {
        free(path);
        return false;
    }
    cursor = strrchr(parent, '/');
    if (cursor != NULL) {
        *cursor = '\0';
        if (!mkdir_p(parent)) {
            free(parent);
            free(path);
            return false;
        }
    }
    free(parent);
    text = read_file(path, NULL);
    output = (char*)malloc(capacity);
    if (output == NULL) {
        free(text);
        free(path);
        return false;
    }
    output[0] = '\0';
    (void)snprintf(line, sizeof(line), "d3d11.metalSpatialUpscaleFactor = %.2f", factor);
    if (text != NULL) {
        char* save = NULL;
        char* part = strtok_r(text, "\n", &save);
        while (part != NULL) {
            const char* to_write = line_is_factor(part) ? line : part;
            size_t part_length = strlen(to_write);
            if (line_is_factor(part))
                replaced = true;
            if (length + part_length + 2 > capacity) {
                while (length + part_length + 2 > capacity)
                    capacity *= 2;
                output = (char*)realloc(output, capacity);
                if (output == NULL) {
                    free(text);
                    free(path);
                    return false;
                }
            }
            memcpy(output + length, to_write, part_length);
            length += part_length;
            output[length++] = '\n';
            output[length] = '\0';
            part = strtok_r(NULL, "\n", &save);
        }
    }
    if (!replaced) {
        size_t line_length = strlen(line);
        if (length + line_length + 2 > capacity) {
            capacity = length + line_length + 2;
            output = (char*)realloc(output, capacity);
            if (output == NULL) {
                free(text);
                free(path);
                return false;
            }
        }
        memcpy(output + length, line, line_length);
        length += line_length;
        output[length++] = '\n';
        output[length] = '\0';
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        free(output);
        free(text);
        free(path);
        return false;
    }
    bool ok = fwrite(output, 1, length, file) == length && fclose(file) == 0;
    if (!ok)
        fclose(file);
    free(output);
    free(text);
    free(path);
    return ok;
}

char* ms_metalfx_get_json(const char* metalsharp_home) {
    metalfx_state state = read_state(metalsharp_home);
    ms_json_writer writer;
    char* result;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "enabled");
    ms_json_writer_bool(&writer, state.enabled);
    ms_json_writer_key(&writer, "factor");
    ms_json_writer_double(&writer, state.factor);
    ms_json_writer_key(&writer, "conf_factor");
    ms_json_writer_double(&writer, read_conf_factor(metalsharp_home));
    ms_json_writer_key(&writer, "source");
    ms_json_writer_string(&writer, "metalfx.overlay.json");
    ms_json_writer_key(&writer, "applies");
    ms_json_writer_string(&writer, "on/off takes effect on the game's next swapchain recreate (alt-enter / resolution "
                                   "change); factor applies on relaunch");
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    return result;
}

char* ms_metalfx_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length) {
    metalfx_state state = read_state(metalsharp_home);
    ms_json* request = NULL;
    char error[128];
    bool enabled;
    double factor;
    bool ok;
    if (body != NULL && body_length > 0)
        request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
    if (request != NULL) {
        if (ms_json_as_bool(ms_json_object_get(request, "enabled"), &enabled))
            state.enabled = enabled;
        if (ms_json_as_number(ms_json_object_get(request, "factor"), &factor) && isfinite(factor) && factor >= 1.0 &&
            factor <= 3.0)
            state.factor = factor;
    }
    ms_json_free(request);
    state.timestamp = now_seconds();
    ok = write_state(metalsharp_home, state) && write_conf_factor(metalsharp_home, state.factor);
    if (ok)
        return ms_metalfx_get_json(metalsharp_home);
    {
        ms_json_writer writer;
        char* result;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, false);
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, "failed to write MetalFX state");
        ms_json_writer_key(&writer, "enabled");
        ms_json_writer_bool(&writer, state.enabled);
        ms_json_writer_key(&writer, "factor");
        ms_json_writer_double(&writer, state.factor);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
        return result;
    }
}
