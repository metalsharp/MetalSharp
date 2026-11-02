#include "metalsharp_backend/metalfx.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/wine_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    char path[PATH_MAX];
    return ms_wine_graphics_conf_path(home, path, sizeof(path)) ? strdup(path) : NULL;
}

static metalfx_state read_state(const char* home) {
    /* Off until it is turned on. */
    metalfx_state state = {false, 2.0, 0};
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
    if (fabs(state.factor - 1.5) > 0.01 && fabs(state.factor - 1.75) > 0.01 && fabs(state.factor - 2.0) > 0.01)
        state.factor = 2.0;
    return state;
}

void ms_metalfx_state(const char* metalsharp_home, bool* enabled, double* factor) {
    metalfx_state state = read_state(metalsharp_home);
    if (enabled)
        *enabled = state.enabled;
    if (factor)
        *factor = state.factor;
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

static bool line_is_key(const char* line, const char* key) {
    size_t length = strlen(key);
    while (*line != '\0' && isspace((unsigned char)*line))
        line++;
    return strncmp(line, key, length) == 0 && (line[length] == ' ' || line[length] == '\t' || line[length] == '=');
}

static bool line_is_factor(const char* line) {
    return line_is_key(line, "d3d11.metalSpatialUpscaleFactor");
}

/* Mullion's switch: games read mullion.conf when they start, so the toggle needs no environment
 * (one a game started from the Steam client would only get from the client's own start). */
static bool line_is_switch(const char* line) {
    return line_is_key(line, "d3d11.metalSpatialUpscale");
}

/* The factor in a conf file; *found tells whether the file has one. */
static double read_conf_file_factor(char* path, bool* found) {
    char* text;
    char* line;
    double result = 2.0;
    *found = false;
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
        *found = true;
        equals = strchr(line, '=');
        if (equals == NULL)
            equals = line + strlen("d3d11.metalSpatialUpscaleFactor");
        errno = 0;
        result = strtod(equals + (equals[0] == '=' ? 1 : 0), &end);
        if (errno != 0 || end == equals || !isfinite(result))
            result = 2.0;
        break;
    }
    free(text);
    /* 1.00: MetalFX off. */
    if (fabs(result - 1.0) > 0.01 && fabs(result - 1.5) > 0.01 && fabs(result - 1.75) > 0.01 &&
        fabs(result - 2.0) > 0.01)
        result = 2.0;
    return result;
}

/* The factor in dxmt.conf (mullion.conf on the fex build); *found tells whether the file has one. */
static double read_conf_factor_found(const char* home, bool* found) {
    return read_conf_file_factor(conf_path(home), found);
}

static double read_conf_factor(const char* home) {
    bool found;
    return read_conf_factor_found(home, &found);
}

static bool write_conf_file(char* path, double factor, bool enabled) {
    char* parent;
    char* text;
    char line[128], switch_line[64];
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
    /* The switch line is Mullion's (the fex build); DXMT turns MetalFX on from the environment. */
    if (ms_wine_is_fex())
        (void)snprintf(switch_line, sizeof(switch_line), "d3d11.metalSpatialUpscale = %s", enabled ? "True" : "False");
    else
        switch_line[0] = '\0';
    if (text != NULL) {
        char* save = NULL;
        char* part = strtok_r(text, "\n", &save);
        while (part != NULL) {
            /* The factor line carries the switch line after it; a separate switch line is dropped. */
            const char* to_write = part;
            size_t part_length;
            if (ms_wine_is_fex() && line_is_switch(part)) {
                part = strtok_r(NULL, "\n", &save);
                continue;
            }
            if (line_is_factor(part)) {
                replaced = true;
                to_write = line;
            }
            part_length = strlen(to_write);
            if (length + part_length + strlen(switch_line) + 3 > capacity) {
                while (length + part_length + strlen(switch_line) + 3 > capacity)
                    capacity *= 2;
                output = (char*)realloc(output, capacity);
                if (output == NULL) {
                    free(text);
                    free(path);
                    return false;
                }
            }
            if (to_write == line && switch_line[0]) {
                length += (size_t)snprintf(output + length, capacity - length, "%s\n%s\n", line, switch_line);
                part = strtok_r(NULL, "\n", &save);
                continue;
            }
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
        /* At the top: after a [program.exe] line, a line applies to that program only. */
        size_t line_length = strlen(line) + strlen(switch_line) + 2;
        if (length + line_length + 1 > capacity) {
            capacity = length + line_length + 1;
            output = (char*)realloc(output, capacity);
            if (output == NULL) {
                free(text);
                free(path);
                return false;
            }
        }
        memmove(output + line_length - (switch_line[0] ? 0 : 1), output, length + 1);
        if (switch_line[0]) {
            memcpy(output, line, strlen(line));
            output[strlen(line)] = '\n';
            memcpy(output + strlen(line) + 1, switch_line, strlen(switch_line));
            output[line_length - 1] = '\n';
            length += line_length;
        } else {
            memcpy(output, line, strlen(line));
            output[strlen(line)] = '\n';
            length += strlen(line) + 1;
        }
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

/* The factor dxmt.conf should hold. WineMetalGL upscales OpenGL windows by
 * the file's factor unless DXMT_METALFX_SPATIAL_SWAPCHAIN=0, and games
 * started from the Steam client keep the client's environment, so off has to
 * be in the file: 1.00. The chosen factor stays in metalfx.overlay.json. */
static double conf_factor_for(metalfx_state state) {
    return state.enabled ? state.factor : 1.0;
}

/* Whether the conf's switch line says `enabled`; false when it has none. */
static bool conf_file_switch_is(char* path, bool enabled) {
    char* text = path ? read_file(path, NULL) : NULL;
    bool matches = false;
    for (char *save = NULL, *line = text ? strtok_r(text, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        if (line_is_switch(line)) {
            const char* value = strchr(line, '=');
            value = value ? value + 1 : "";
            while (isspace((unsigned char)*value))
                value++;
            matches = enabled == (!strncasecmp(value, "true", 4) || !strncmp(value, "1", 1));
            break;
        }
    }
    free(text);
    free(path);
    return matches;
}

/* The conf files MetalSharp keeps the MetalFX lines in: dxmt.conf (mullion.conf on the fex build), and on the fex
 * build VKD3D-Proton's lane's vkd3d.conf and dxvk.conf, with the same lines. Returns how many it put in paths. */
static int conf_paths(const char* home, char* paths[3]) {
    char path[PATH_MAX];
    int count = 0;
    if ((paths[count] = conf_path(home)) != NULL)
        count++;
    for (int i = 0; i < 2; i++)
        if (ms_wine_vkd3d_lane_conf_path(home, i, path, sizeof(path)) && (paths[count] = strdup(path)) != NULL)
            count++;
    return count;
}

static bool write_conf_factor(const char* home, double factor, bool enabled) {
    char* paths[3];
    int count = conf_paths(home, paths);
    bool ok = count > 0;
    for (int i = 0; i < count; i++)
        ok = write_conf_file(paths[i], factor, enabled) && ok;
    return ok;
}

/* Whether a conf file holds the factor and, on the fex build, the switch the state asks for. */
static bool conf_file_matches(const char* path, metalfx_state state) {
    bool found;
    double factor = read_conf_file_factor(strdup(path), &found);
    return found && fabs(factor - conf_factor_for(state)) <= 0.001 &&
           (!ms_wine_is_fex() || conf_file_switch_is(strdup(path), state.enabled));
}

bool ms_metalfx_sync_conf(const char* metalsharp_home) {
    metalfx_state state;
    char* paths[3];
    int count;
    bool matches = true;
    if (metalsharp_home == NULL)
        return false;
    state = read_state(metalsharp_home);
    count = conf_paths(metalsharp_home, paths);
    for (int i = 0; i < count; i++) {
        matches = matches && conf_file_matches(paths[i], state);
        free(paths[i]);
    }
    if (count > 0 && matches)
        return true;
    return write_conf_factor(metalsharp_home, conf_factor_for(state), state.enabled);
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
                                   "change), for OpenGL games on relaunch; factor applies on relaunch");
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
        if (ms_json_as_bool(ms_json_object_get(request, "enabled"), &enabled)) {
            state.enabled = enabled;
        }
        if (state.enabled && ms_json_as_number(ms_json_object_get(request, "factor"), &factor) && isfinite(factor) &&
            (fabs(factor - 1.5) <= 0.01 || fabs(factor - 1.75) <= 0.01 || fabs(factor - 2.0) <= 0.01))
            state.factor = factor;
    }
    ms_json_free(request);
    state.timestamp = now_seconds();
    ok = write_state(metalsharp_home, state) &&
         write_conf_factor(metalsharp_home, conf_factor_for(state), state.enabled);
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
