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

static char* display_preference(const ms_json* config, const char* key, const char* const* allowed, size_t count) {
    char* value = NULL;
    if (ms_json_as_string(ms_json_object_get(config, key), &value)) {
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(value, allowed[i]))
                return value;
        free(value);
    }
    return strdup("default");
}

static char* window_mode(const ms_json* config) {
    static const char* const allowed[] = {"default", "windowed", "fullscreen"};
    return display_preference(config, "windowMode", allowed, sizeof(allowed) / sizeof(allowed[0]));
}

static char* game_resolution(const ms_json* config) {
    static const char* const allowed[] = {"default", "1280x720", "1920x1080", "2560x1440", "3840x2160"};
    return display_preference(config, "gameResolution", allowed, sizeof(allowed) / sizeof(allowed[0]));
}

static bool config_bool(const ms_json* config, const char* key, bool fallback) {
    bool value;
    return json_boolish(ms_json_object_get(config, key), &value) ? value : fallback;
}

bool ms_config_msync_enabled(const char* metalsharp_home) {
    char* path = config_path(metalsharp_home);
    ms_json* config = path == NULL ? NULL : read_json_file(path);
    bool enabled = config_bool(config, "msync", true);
    free(path);
    ms_json_free(config);
    return enabled;
}

bool ms_config_retina_enabled(const char* metalsharp_home) {
    char* path = config_path(metalsharp_home);
    ms_json* config = path == NULL ? NULL : read_json_file(path);
    bool enabled = config_bool(config, "retinaMode", false);
    free(path);
    ms_json_free(config);
    return enabled;
}

bool ms_config_graphics_runtime_logs_enabled(const char* metalsharp_home) {
    const char* env_logs = getenv("METALSHARP_GRAPHICS_RUNTIME_LOGS");
    char* path;
    ms_json* config;
    bool enabled;
    if (env_logs != NULL)
        return truthy(env_logs);
    path = config_path(metalsharp_home);
    config = path == NULL ? NULL : read_json_file(path);
    enabled = config_bool(config, "graphicsRuntimeLogs", config_bool(config, "graphics_runtime_logs", false));
    free(path);
    ms_json_free(config);
    return enabled;
}

bool ms_config_exclude_native_mac_steam_games(const char* metalsharp_home) {
    char* path = config_path(metalsharp_home);
    ms_json* config = path == NULL ? NULL : read_json_file(path);
    bool enabled = config_bool(config, "excludeNativeMacSteamGames", false);
    free(path);
    ms_json_free(config);
    return enabled;
}

char* ms_config_get_json(const char* metalsharp_home) {
    char* path = config_path(metalsharp_home);
    ms_json* config = path == NULL ? NULL : read_json_file(path);
    const char* env_logs = getenv("METALSHARP_GRAPHICS_RUNTIME_LOGS");
    bool logs = env_logs != NULL
                    ? truthy(env_logs)
                    : config_bool(config, "graphicsRuntimeLogs", config_bool(config, "graphics_runtime_logs", false));
    bool msync = config_bool(config, "msync", true);
    bool retina = config_bool(config, "retinaMode", false);
    bool exclude_native_mac_steam_games = config_bool(config, "excludeNativeMacSteamGames", false);
    char* controller = controller_input(config);
    char* display = window_mode(config);
    char* resolution = game_resolution(config);
    ms_json_writer writer;
    char* result;
    if (controller == NULL || display == NULL || resolution == NULL) {
        free(path);
        ms_json_free(config);
        free(controller);
        free(display);
        free(resolution);
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
    ms_json_writer_key(&writer, "windowMode");
    ms_json_writer_string(&writer, display);
    ms_json_writer_key(&writer, "gameResolution");
    ms_json_writer_string(&writer, resolution);
    ms_json_writer_key(&writer, "msync");
    ms_json_writer_bool(&writer, msync);
    ms_json_writer_key(&writer, "retinaMode");
    ms_json_writer_bool(&writer, retina);
    ms_json_writer_key(&writer, "excludeNativeMacSteamGames");
    ms_json_writer_bool(&writer, exclude_native_mac_steam_games);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(controller);
    free(display);
    free(resolution);
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

static bool valid_display_preference(const ms_json* value, const char* const* allowed, size_t count,
                                     char** normalized) {
    char* string = NULL;
    if (!ms_json_as_string(value, &string))
        return false;
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(string, allowed[i])) {
            *normalized = string;
            return true;
        }
    }
    free(string);
    return false;
}

static void write_member(ms_json_writer* writer, const char* key, const ms_json* value) {
    char* serialized = ms_json_stringify(value);
    ms_json_writer_key(writer, key);
    ms_json_writer_raw(writer, serialized == NULL ? "null" : serialized);
    free(serialized);
}

static bool write_config(const char* path, const ms_json* existing, bool set_logs, bool logs, bool set_controller,
                         const char* controller, bool set_msync, bool msync, bool set_retina, bool retina,
                         bool set_exclude_native_mac_steam_games, bool exclude_native_mac_steam_games,
                         bool set_window_mode, const char* window_mode_value, bool set_game_resolution,
                         const char* game_resolution_value) {
    char* parent;
    char* slash;
    ms_json_writer writer;
    size_t i;
    bool emitted_logs_camel = false;
    bool emitted_logs_snake = false;
    bool emitted_controller = false;
    bool emitted_msync = false;
    bool emitted_retina = false;
    bool emitted_exclude_native_mac_steam_games = false;
    bool emitted_window_mode = false;
    bool emitted_game_resolution = false;
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
        } else if (set_retina && strcmp(key, "retinaMode") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_bool(&writer, retina);
            emitted_retina = true;
        } else if (set_exclude_native_mac_steam_games && strcmp(key, "excludeNativeMacSteamGames") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_bool(&writer, exclude_native_mac_steam_games);
            emitted_exclude_native_mac_steam_games = true;
        } else if (set_window_mode && strcmp(key, "windowMode") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_string(&writer, window_mode_value);
            emitted_window_mode = true;
        } else if (set_game_resolution && strcmp(key, "gameResolution") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_string(&writer, game_resolution_value);
            emitted_game_resolution = true;
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
    if (set_retina && !emitted_retina) {
        ms_json_writer_key(&writer, "retinaMode");
        ms_json_writer_bool(&writer, retina);
    }
    if (set_exclude_native_mac_steam_games && !emitted_exclude_native_mac_steam_games) {
        ms_json_writer_key(&writer, "excludeNativeMacSteamGames");
        ms_json_writer_bool(&writer, exclude_native_mac_steam_games);
    }
    if (set_window_mode && !emitted_window_mode) {
        ms_json_writer_key(&writer, "windowMode");
        ms_json_writer_string(&writer, window_mode_value);
    }
    if (set_game_resolution && !emitted_game_resolution) {
        ms_json_writer_key(&writer, "gameResolution");
        ms_json_writer_string(&writer, game_resolution_value);
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
    bool set_logs = false, logs = false, set_msync = false, msync = false, set_retina = false, retina = false,
         set_exclude_native_mac_steam_games = false, exclude_native_mac_steam_games = false, set_controller = false;
    bool set_window_mode = false, set_game_resolution = false;
    static const char* const window_modes[] = {"default", "windowed", "fullscreen"};
    static const char* const resolutions[] = {"default", "1280x720", "1920x1080", "2560x1440", "3840x2160"};
    char *controller = NULL, *window_mode_value = NULL, *game_resolution_value = NULL;
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
        set_window_mode = valid_display_preference(ms_json_object_get(request, "windowMode"), window_modes,
                                                   sizeof(window_modes) / sizeof(window_modes[0]), &window_mode_value);
        set_game_resolution =
            valid_display_preference(ms_json_object_get(request, "gameResolution"), resolutions,
                                     sizeof(resolutions) / sizeof(resolutions[0]), &game_resolution_value);
        value = ms_json_object_get(request, "msync");
        set_msync = value != NULL && ms_json_as_bool(value, &msync);
        value = ms_json_object_get(request, "retinaMode");
        set_retina = value != NULL && ms_json_as_bool(value, &retina);
        value = ms_json_object_get(request, "excludeNativeMacSteamGames");
        set_exclude_native_mac_steam_games = value != NULL && ms_json_as_bool(value, &exclude_native_mac_steam_games);
    }
    if (!write_config(path, existing, set_logs, logs, set_controller, controller, set_msync, msync, set_retina, retina,
                      set_exclude_native_mac_steam_games, exclude_native_mac_steam_games, set_window_mode,
                      window_mode_value, set_game_resolution, game_resolution_value))
        goto fail;
    result = ms_config_get_json(metalsharp_home);
    if (status != NULL)
        *status = result == NULL ? 500 : 200;
    free(controller);
    free(window_mode_value);
    free(game_resolution_value);
    ms_json_free(existing);
    ms_json_free(request);
    free(path);
    return result;
fail:
    free(controller);
    free(window_mode_value);
    free(game_resolution_value);
    ms_json_free(existing);
    ms_json_free(request);
    free(path);
    if (status != NULL)
        *status = 500;
    return strdup("{\"ok\":false,\"error\":\"failed to write configuration\"}");
}
