#include "metalsharp_backend/config.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char* join_path(const char* left, const char* right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool slash = left_length > 0 && left[left_length - 1] != '/';
    char* path = (char*)malloc(left_length + right_length + (slash ? 2 : 1));
    if (path == NULL)
        return NULL;
    (void)snprintf(path, left_length + right_length + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return path;
}

static char* config_path(const char* home) {
    char* configs = join_path(home, "configs");
    char* path;
    if (configs == NULL)
        return NULL;
    path = join_path(configs, "config.json");
    free(configs);
    return path;
}

static bool mkdir_p(const char* path) {
    char* copy;
    size_t i;
    struct stat st;
    if (path == NULL || path[0] == '\0')
        return false;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    copy = strdup(path);
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

static ms_json* read_json_file(const char* path) {
    FILE* file;
    long size;
    char* contents;
    size_t read_length;
    char error[128];
    ms_json* value;
    file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || (unsigned long long)size > 4ULL * 1024ULL * 1024ULL || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    contents = (char*)malloc((size_t)size + 1);
    if (contents == NULL) {
        fclose(file);
        return NULL;
    }
    read_length = fread(contents, 1, (size_t)size, file);
    fclose(file);
    contents[read_length] = '\0';
    value = ms_json_parse(contents, read_length, error, sizeof(error));
    free(contents);
    return value;
}

static bool truthy(const char* value) {
    char lower[16];
    size_t i;
    if (value == NULL)
        return false;
    while (*value != '\0' && isspace((unsigned char)*value))
        value++;
    for (i = 0; i + 1 < sizeof(lower) && value[i] != '\0' && !isspace((unsigned char)value[i]); ++i) {
        lower[i] = (char)tolower((unsigned char)value[i]);
    }
    lower[i] = '\0';
    return strcmp(lower, "1") == 0 || strcmp(lower, "true") == 0 || strcmp(lower, "yes") == 0 ||
           strcmp(lower, "on") == 0;
}

static bool json_boolish(const ms_json* value, bool* out) {
    bool boolean;
    double number;
    char* string = NULL;
    if (ms_json_as_bool(value, &boolean)) {
        *out = boolean;
        return true;
    }
    if (ms_json_as_number(value, &number)) {
        *out = number != 0.0;
        return true;
    }
    if (ms_json_as_string(value, &string)) {
        *out = truthy(string);
        free(string);
        return true;
    }
    return false;
}

static bool file_exists(const char* path) {
    return path != NULL && access(path, F_OK) == 0;
}

static bool native_available(const char* metalsharp_home) {
    const char* home = getenv("HOME");
    char* candidate;
    const char* fixed[] = {
        "/Applications/MetalSharp.app/Contents/Resources/metalsharp",
        "/usr/local/bin/metalsharp",
        "/opt/homebrew/bin/metalsharp",
    };
    size_t i;
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (file_exists(fixed[i]))
            return true;
    }
    candidate = join_path(metalsharp_home, "metalsharp");
    if (candidate != NULL) {
        bool found = file_exists(candidate);
        free(candidate);
        if (found)
            return true;
    }
    if (home != NULL) {
        candidate = join_path(home, "metalsharp/build/metalsharp");
        if (candidate != NULL) {
            bool found = file_exists(candidate);
            free(candidate);
            if (found)
                return true;
        }
    }
    return false;
}

static bool mono_available(void) {
    return file_exists("/opt/homebrew/bin/mono") || file_exists("/usr/local/bin/mono");
}

static char* controller_input(const ms_json* config) {
    const ms_json* value = ms_json_object_get(config, "controllerInput");
    char* string = NULL;
    size_t i;
    if (!ms_json_as_string(value, &string))
        return strdup("off");
    for (i = 0; string[i] != '\0'; ++i)
        string[i] = (char)tolower((unsigned char)string[i]);
    if (strcmp(string, "off") != 0 && strcmp(string, "x") != 0 && strcmp(string, "d") != 0) {
        free(string);
        return strdup("off");
    }
    return string;
}

static bool config_bool(const ms_json* config, const char* key, bool fallback) {
    bool value;
    return json_boolish(ms_json_object_get(config, key), &value) ? value : fallback;
}

char* ms_config_get_json(const char* metalsharp_home) {
    char* path = config_path(metalsharp_home);
    ms_json* config = path == NULL ? NULL : read_json_file(path);
    const char* env_logs = getenv("METALSHARP_GRAPHICS_RUNTIME_LOGS");
    bool logs = env_logs != NULL
                    ? truthy(env_logs)
                    : config_bool(config, "graphicsRuntimeLogs", config_bool(config, "graphics_runtime_logs", false));
    bool msync = config_bool(config, "msync", true);
    char* controller = controller_input(config);
    ms_json_writer writer;
    char* result;
    if (controller == NULL) {
        free(path);
        ms_json_free(config);
        return NULL;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "native_available");
    ms_json_writer_bool(&writer, native_available(metalsharp_home));
    ms_json_writer_key(&writer, "mono_available");
    ms_json_writer_bool(&writer, mono_available());
    ms_json_writer_key(&writer, "graphicsRuntimeLogs");
    ms_json_writer_bool(&writer, logs);
    ms_json_writer_key(&writer, "graphics_runtime_logs");
    ms_json_writer_bool(&writer, logs);
    ms_json_writer_key(&writer, "controllerInput");
    ms_json_writer_string(&writer, controller);
    ms_json_writer_key(&writer, "msync");
    ms_json_writer_bool(&writer, msync);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(controller);
    free(path);
    ms_json_free(config);
    return result;
}

static bool valid_controller(const ms_json* value, char** normalized) {
    char* string = NULL;
    size_t i;
    if (!ms_json_as_string(value, &string))
        return false;
    for (i = 0; string[i] != '\0'; ++i)
        string[i] = (char)tolower((unsigned char)string[i]);
    if (strcmp(string, "off") != 0 && strcmp(string, "x") != 0 && strcmp(string, "d") != 0) {
        free(string);
        return false;
    }
    *normalized = string;
    return true;
}

static void write_member(ms_json_writer* writer, const char* key, const ms_json* value) {
    char* serialized = ms_json_stringify(value);
    ms_json_writer_key(writer, key);
    ms_json_writer_raw(writer, serialized == NULL ? "null" : serialized);
    free(serialized);
}

static bool write_config(const char* path, const ms_json* existing, bool set_logs, bool logs, bool set_controller,
                         const char* controller, bool set_msync, bool msync) {
    char* parent;
    char* slash;
    ms_json_writer writer;
    size_t i;
    bool emitted_logs_camel = false;
    bool emitted_logs_snake = false;
    bool emitted_controller = false;
    bool emitted_msync = false;
    parent = strdup(path);
    if (parent == NULL)
        return false;
    slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
        if (!mkdir_p(parent)) {
            free(parent);
            return false;
        }
    }
    free(parent);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (i = 0; existing != NULL && i < ms_json_object_length(existing); ++i) {
        const char* key = ms_json_object_key_at(existing, i);
        const ms_json* value = ms_json_object_value_at(existing, i);
        if (set_logs && strcmp(key, "graphicsRuntimeLogs") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_bool(&writer, logs);
            emitted_logs_camel = true;
        } else if (set_logs && strcmp(key, "graphics_runtime_logs") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_bool(&writer, logs);
            emitted_logs_snake = true;
        } else if (set_controller && strcmp(key, "controllerInput") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_string(&writer, controller);
            emitted_controller = true;
        } else if (set_msync && strcmp(key, "msync") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_bool(&writer, msync);
            emitted_msync = true;
        } else {
            write_member(&writer, key, value);
        }
    }
    if (set_logs && !emitted_logs_camel) {
        ms_json_writer_key(&writer, "graphicsRuntimeLogs");
        ms_json_writer_bool(&writer, logs);
    }
    if (set_logs && !emitted_logs_snake) {
        ms_json_writer_key(&writer, "graphics_runtime_logs");
        ms_json_writer_bool(&writer, logs);
    }
    if (set_controller && !emitted_controller) {
        ms_json_writer_key(&writer, "controllerInput");
        ms_json_writer_string(&writer, controller);
    }
    if (set_msync && !emitted_msync) {
        ms_json_writer_key(&writer, "msync");
        ms_json_writer_bool(&writer, msync);
    }
    ms_json_writer_object_end(&writer);
    {
        char* serialized = ms_json_writer_take(&writer);
        FILE* file;
        if (serialized == NULL)
            return false;
        file = fopen(path, "wb");
        if (file == NULL) {
            free(serialized);
            return false;
        }
        bool ok = fputs(serialized, file) >= 0 && fclose(file) == 0;
        if (!ok)
            fclose(file);
        free(serialized);
        return ok;
    }
}

char* ms_config_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status) {
    char* path = config_path(metalsharp_home);
    ms_json* existing = path == NULL ? NULL : read_json_file(path);
    ms_json* request = NULL;
    char error[128];
    bool set_logs = false, logs = false, set_msync = false, msync = false, set_controller = false;
    char* controller = NULL;
    char* result;
    if (status != NULL)
        *status = 500;
    if (path == NULL)
        goto fail;
    if (existing == NULL || ms_json_type_of(existing) != MS_JSON_OBJECT) {
        ms_json_free(existing);
        existing = NULL;
    }
    if (body != NULL && body_length > 0) {
        request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
        if (request == NULL || ms_json_type_of(request) != MS_JSON_OBJECT) {
            ms_json_free(request);
            request = NULL;
        }
    }
    if (request != NULL) {
        const ms_json* value = ms_json_object_get(request, "graphicsRuntimeLogs");
        if (value == NULL)
            value = ms_json_object_get(request, "graphics_runtime_logs");
        if (value == NULL)
            value = ms_json_object_get(request, "logs");
        if (value != NULL)
            set_logs = json_boolish(value, &logs);
        set_controller = valid_controller(ms_json_object_get(request, "controllerInput"), &controller);
        value = ms_json_object_get(request, "msync");
        set_msync = value != NULL && ms_json_as_bool(value, &msync);
    }
    if (!write_config(path, existing, set_logs, logs, set_controller, controller, set_msync, msync))
        goto fail;
    result = ms_config_get_json(metalsharp_home);
    if (status != NULL)
        *status = result == NULL ? 500 : 200;
    free(controller);
    ms_json_free(existing);
    ms_json_free(request);
    free(path);
    return result;
fail:
    free(controller);
    ms_json_free(existing);
    ms_json_free(request);
    free(path);
    if (status != NULL)
        *status = 500;
    return strdup("{\"ok\":false,\"error\":\"failed to write configuration\"}");
}
