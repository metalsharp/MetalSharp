#include "metalsharp_backend/frame_rate.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/wine_runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* const conf_keys[] = {"d3d11.preferredMaxFrameRate", "d3d12.preferredMaxFrameRate"};

static char* read_file(const char* path) {
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
    if (size < 0 || size > 4L * 1024L * 1024L || fseek(file, 0, SEEK_SET) != 0) {
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
    return data;
}

static bool write_file(const char* path, const char* text) {
    char tmp[PATH_MAX];
    FILE* file;
    size_t length = strlen(text);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return false;
    file = fopen(tmp, "wb");
    if (file == NULL)
        return false;
    if (fwrite(text, 1, length, file) != length) {
        fclose(file);
        unlink(tmp);
        return false;
    }
    if (fclose(file) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool valid_limit(double value) {
    return value == 0.0 || value == 30.0 || value == 60.0;
}

/* "key = value" lines; the key is followed by a space, tab or '='. */
static const char* conf_value(const char* line, const char* key) {
    size_t length = strlen(key);
    while (*line == ' ' || *line == '\t')
        line++;
    if (strncmp(line, key, length) != 0 || (line[length] != ' ' && line[length] != '\t' && line[length] != '='))
        return NULL;
    line += length;
    while (*line == ' ' || *line == '\t')
        line++;
    return *line == '=' ? line + 1 : NULL;
}

static bool is_conf_key_line(const char* line) {
    for (size_t i = 0; i < sizeof(conf_keys) / sizeof(conf_keys[0]); i++)
        if (conf_value(line, conf_keys[i]))
            return true;
    return false;
}

/* Mullion's limit: d3d11.preferredMaxFrameRate in mullion.conf (Mullion's own default is none). */
static unsigned mullion_limit(const char* home) {
    char path[PATH_MAX];
    char* text;
    unsigned limit = 0;
    if (!ms_wine_graphics_conf_path(home, path, sizeof(path)) || !(text = read_file(path)))
        return 0;
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        const char* value = conf_value(line, conf_keys[0]);
        if (value) {
            double parsed = strtod(value, NULL);
            limit = parsed > 0 && parsed < 1000 ? (unsigned)parsed : 0;
            break;
        }
    }
    free(text);
    return limit;
}

/* Rewrites mullion.conf with both D3D11 and D3D12 limits (0 is none), keeping every other line. */
static bool write_mullion_limit(const char* home, unsigned limit) {
    char path[PATH_MAX];
    char* text;
    char* output;
    size_t capacity, length = 0;
    bool ok;
    if (!ms_wine_graphics_conf_path(home, path, sizeof(path)))
        return false;
    text = read_file(path);
    capacity = (text ? strlen(text) : 0) + 128;
    output = (char*)malloc(capacity);
    if (output == NULL) {
        free(text);
        return false;
    }
    output[0] = '\0';
    if (text) {
        for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (is_conf_key_line(line))
                continue;
            length += (size_t)snprintf(output + length, capacity - length, "%s\n", line);
        }
    }
    for (size_t i = 0; i < sizeof(conf_keys) / sizeof(conf_keys[0]); i++)
        length += (size_t)snprintf(output + length, capacity - length, "%s = %u\n", conf_keys[i], limit);
    ok = write_file(path, output);
    free(output);
    free(text);
    return ok;
}

static char* vkd3d_state_path(const char* home) {
    size_t size = strlen(home) + sizeof("/etc/vkd3d-frame-rate");
    char* path = (char*)malloc(size);
    if (path)
        (void)snprintf(path, size, "%s/etc/vkd3d-frame-rate", home);
    return path;
}

/* VKD3D-Proton's limit: a number in etc/vkd3d-frame-rate; none until one is set. */
static unsigned vkd3d_limit(const char* home) {
    char* path = vkd3d_state_path(home);
    char* text = path ? read_file(path) : NULL;
    double parsed = text ? strtod(text, NULL) : 0;
    free(path);
    free(text);
    return valid_limit(parsed) ? (unsigned)parsed : 0;
}

static bool write_vkd3d_limit(const char* home, unsigned limit) {
    char* path = vkd3d_state_path(home);
    char etc[PATH_MAX], text[16];
    bool ok;
    if (path == NULL)
        return false;
    (void)snprintf(etc, sizeof(etc), "%s/etc", home);
    if (mkdir(etc, 0755) != 0 && errno != EEXIST) {
        free(path);
        return false;
    }
    (void)snprintf(text, sizeof(text), "%u\n", limit);
    ok = write_file(path, text);
    free(path);
    return ok;
}

unsigned ms_frame_rate_limit(const char* metalsharp_home, const char* pipeline) {
    if (metalsharp_home == NULL || pipeline == NULL)
        return 0;
    if (!strcmp(pipeline, "vkd3d"))
        return vkd3d_limit(metalsharp_home);
    if (!strcmp(pipeline, "dxmt"))
        return mullion_limit(metalsharp_home);
    return 0;
}

static char* state_json(const char* home, bool ok) {
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, ok);
    ms_json_writer_key(&writer, "dxmt");
    ms_json_writer_u64(&writer, mullion_limit(home));
    ms_json_writer_key(&writer, "vkd3d");
    ms_json_writer_u64(&writer, vkd3d_limit(home));
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

char* ms_frame_rate_get_json(const char* metalsharp_home) {
    return state_json(metalsharp_home, true);
}

/* {"pipeline": "dxmt" | "vkd3d", "fps": 0 | 30 | 60} */
char* ms_frame_rate_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length,
                             int* status) {
    char error[128];
    ms_json* request = body && body_length ? ms_json_parse((const char*)body, body_length, error, sizeof(error)) : NULL;
    char* pipeline = NULL;
    double fps = -1;
    bool ok = false;
    if (request && ms_json_as_string(ms_json_object_get(request, "pipeline"), &pipeline) &&
        ms_json_as_number(ms_json_object_get(request, "fps"), &fps) && valid_limit(fps)) {
        if (!strcmp(pipeline, "dxmt"))
            ok = write_mullion_limit(metalsharp_home, (unsigned)fps);
        else if (!strcmp(pipeline, "vkd3d"))
            ok = write_vkd3d_limit(metalsharp_home, (unsigned)fps);
    }
    if (status)
        *status = ok ? 200 : 400;
    free(pipeline);
    ms_json_free(request);
    return state_json(metalsharp_home, ok);
}
