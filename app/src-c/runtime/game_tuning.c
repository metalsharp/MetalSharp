#define _DARWIN_C_SOURCE
#include "metalsharp_backend/game_tuning.h"
#include "metalsharp_backend/config.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/logs.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/steam_actions.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TUNING_FILE_MAX   65536
#define ENV_NAME_MAX      128
#define ENV_VALUE_MAX     2048
#define DLL_NAME_MAX      64
#define LOW_PRIORITY_NICE 10

static const char LOCAL_ENV_BEGIN[] = "# >>> MetalSharp game settings (Game Settings > Advanced; edit them there)";
static const char LOCAL_ENV_END[] = "# <<< MetalSharp game settings";

static void set_error(char* error, size_t error_size, const char* format, const char* detail) {
    if (error && error_size)
        snprintf(error, error_size, format, detail ? detail : "");
}

static char* join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    char* path = malloc(x + y + 2);
    if (path)
        snprintf(path, x + y + 2, "%s%s%s", a, x && a[x - 1] == '/' ? "" : "/", b);
    return path;
}

void ms_game_tuning_init(ms_game_tuning* tuning) {
    if (tuning) {
        memset(tuning, 0, sizeof(*tuning));
        tuning->retina = -1;
    }
}

static void free_entries(char** entries, size_t count) {
    for (size_t i = 0; entries && i < count; i++)
        free(entries[i]);
    free(entries);
}

void ms_game_tuning_free(ms_game_tuning* tuning) {
    if (!tuning)
        return;
    free_entries(tuning->env, tuning->env_count);
    free(tuning->dll_overrides);
    ms_game_tuning_init(tuning);
}

bool ms_game_tuning_is_default(const ms_game_tuning* tuning) {
    return !tuning || (!tuning->env_count && (!tuning->dll_overrides || !tuning->dll_overrides[0]) &&
                       !tuning->windows_version[0] && !tuning->cpu_cores && !tuning->low_priority &&
                       tuning->retina < 0 && !tuning->zink && !tuning->kosmickrisp);
}

static bool valid_component(const char* value) {
    size_t length = value ? strlen(value) : 0;
    if (length == 0 || length > 128 || value[0] == '.')
        return false;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)value[i]) && value[i] != '_' && value[i] != '-' && value[i] != '.')
            return false;
    return true;
}

bool ms_game_tuning_valid_key(const char* provider, const char* id) {
    return valid_component(provider) && valid_component(id);
}

/* ---- environment variables ---- */

/* Variables the launch itself owns: the prefix and runtime paths, the dynamic loader's (which only
 * work before a process starts), and the DLL overrides the graphics routes set (DLL overrides below
 * set an executable's own). */
static bool env_name_blocked(const char* name) {
    static const char* const blocked[] = {"WINEPREFIX",
                                          "WINEDLLPATH",
                                          "WINELOADER",
                                          "WINESERVER",
                                          "WINEARCH",
                                          "WINEDLLOVERRIDES",
                                          "WINEMSYNC",
                                          "WINEESYNC",
                                          "WINEFSYNC",
                                          "METALSHARP_HOME",
                                          "METALSHARP_PIPELINE",
                                          "GRAPHICS_BACKEND",
                                          "VK_DRIVER_FILES",
                                          "VK_ICD_FILENAMES",
                                          "PATH",
                                          "HOME",
                                          "USER",
                                          "TMPDIR",
                                          "SHELL",
                                          "PWD"};
    if (!strncmp(name, "DYLD_", 5) || !strncmp(name, "LD_", 3) || !strncmp(name, "METALSHARP_DYLD_", 16))
        return true;
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
        if (!strcmp(name, blocked[i]))
            return true;
    return false;
}

static bool valid_env_name(const char* name, size_t length) {
    if (!length || length > ENV_NAME_MAX || isdigit((unsigned char)name[0]))
        return false;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_')
            return false;
    return true;
}

/* One stored entry: NAME=value (any characters but control ones in the value) or -NAME. */
static bool valid_env_entry(const char* entry) {
    const char* name = entry && entry[0] == '-' ? entry + 1 : entry;
    size_t length = name ? (entry[0] == '-' ? strlen(name) : strcspn(name, "=")) : 0;
    char bare[ENV_NAME_MAX + 1];
    if (!name || !valid_env_name(name, length) || (entry[0] != '-' && name[length] != '='))
        return false;
    if (entry[0] != '-') {
        const char* value = name + length + 1;
        if (strlen(value) > ENV_VALUE_MAX)
            return false;
        for (const char* c = value; *c; c++)
            if ((unsigned char)*c < 0x20)
                return false;
    }
    snprintf(bare, sizeof(bare), "%.*s", (int)length, name);
    return !env_name_blocked(bare);
}

/* The next whitespace-separated token; "..." groups (\" and \\ escaped) and '...' groups keep spaces. */
static bool next_token(const char** cursor, char* token, size_t token_size, bool* found, char* error,
                       size_t error_size) {
    const char* p = *cursor;
    size_t used = 0;
    *found = false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (!*p) {
        *cursor = p;
        return true;
    }
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p++;
            while (*p && *p != quote) {
                char c = *p++;
                if (quote == '"' && c == '\\' && (*p == '"' || *p == '\\'))
                    c = *p++;
                if ((unsigned char)c < 0x20) {
                    set_error(error, error_size, "%s", "Environment values cannot contain line breaks");
                    return false;
                }
                if (used + 1 >= token_size) {
                    set_error(error, error_size, "%s", "An environment variable is too long");
                    return false;
                }
                token[used++] = c;
            }
            if (*p != quote) {
                set_error(error, error_size, "%s", "An environment value is missing its closing quote");
                return false;
            }
            p++;
            continue;
        }
        if ((unsigned char)*p < 0x20) {
            set_error(error, error_size, "%s", "Environment variables cannot contain control characters");
            return false;
        }
        if (used + 1 >= token_size) {
            set_error(error, error_size, "%s", "An environment variable is too long");
            return false;
        }
        token[used++] = *p++;
    }
    token[used] = '\0';
    *cursor = p;
    *found = true;
    return true;
}

bool ms_game_tuning_parse_env(const char* text, char*** entries_out, size_t* count_out, char* error,
                              size_t error_size) {
    char token[ENV_NAME_MAX + ENV_VALUE_MAX + 4];
    const char* cursor = text ? text : "";
    char** entries = NULL;
    size_t count = 0;
    *entries_out = NULL;
    *count_out = 0;
    for (;;) {
        bool found;
        bool removal;
        const char* name;
        size_t name_length;
        char* entry;
        if (!next_token(&cursor, token, sizeof(token), &found, error, error_size)) {
            free_entries(entries, count);
            return false;
        }
        if (!found)
            break;
        removal = token[0] == '-';
        name = removal ? token + 1 : token;
        name_length = removal ? strlen(name) : strcspn(name, "=");
        if (!removal && name[name_length] != '=') {
            set_error(error, error_size, "\"%s\" is not NAME=value", token);
            free_entries(entries, count);
            return false;
        }
        if (!valid_env_name(name, name_length)) {
            char shown[ENV_NAME_MAX + 1];
            snprintf(shown, sizeof(shown), "%.*s", (int)(name_length > ENV_NAME_MAX ? ENV_NAME_MAX : name_length),
                     name);
            set_error(error, error_size, "\"%s\" is not a valid variable name", shown);
            free_entries(entries, count);
            return false;
        }
        {
            char bare[ENV_NAME_MAX + 1];
            snprintf(bare, sizeof(bare), "%.*s", (int)name_length, name);
            if (env_name_blocked(bare)) {
                set_error(error, error_size, "%s is set by MetalSharp for every launch and cannot be changed here",
                          bare);
                free_entries(entries, count);
                return false;
            }
        }
        if (!removal && strlen(name + name_length + 1) > ENV_VALUE_MAX) {
            set_error(error, error_size, "%s", "An environment value is too long");
            free_entries(entries, count);
            return false;
        }
        entry = strdup(token);
        if (!entry) {
            free_entries(entries, count);
            return false;
        }
        /* A later entry for the same name replaces the earlier one. */
        for (size_t i = 0; i < count; i++) {
            const char* other = entries[i][0] == '-' ? entries[i] + 1 : entries[i];
            size_t other_length = entries[i][0] == '-' ? strlen(other) : strcspn(other, "=");
            if (other_length == name_length && !strncmp(other, name, name_length)) {
                free(entries[i]);
                memmove(entries + i, entries + i + 1, (count - i - 1) * sizeof(*entries));
                count--;
                break;
            }
        }
        if (count >= MS_GAME_TUNING_MAX_ENV) {
            free(entry);
            set_error(error, error_size, "%s", "Too many environment variables (64 at most)");
            free_entries(entries, count);
            return false;
        }
        {
            char** grown = realloc(entries, (count + 1) * sizeof(*entries));
            if (!grown) {
                free(entry);
                free_entries(entries, count);
                return false;
            }
            entries = grown;
            entries[count++] = entry;
        }
    }
    *entries_out = entries;
    *count_out = count;
    return true;
}

char* ms_game_tuning_env_text(const ms_game_tuning* tuning) {
    size_t capacity = 1, used = 0;
    char* text;
    for (size_t i = 0; tuning && i < tuning->env_count; i++)
        capacity += strlen(tuning->env[i]) * 2 + 4;
    text = malloc(capacity);
    if (!text)
        return NULL;
    text[0] = '\0';
    for (size_t i = 0; tuning && i < tuning->env_count; i++) {
        const char* entry = tuning->env[i];
        const char* equals = entry[0] == '-' ? NULL : strchr(entry, '=');
        bool quote = equals && (strpbrk(equals + 1, " \t\"'\\") != NULL);
        if (used)
            text[used++] = ' ';
        if (!quote) {
            used += (size_t)snprintf(text + used, capacity - used, "%s", entry);
            continue;
        }
        used += (size_t)snprintf(text + used, capacity - used, "%.*s\"", (int)(equals - entry + 1), entry);
        for (const char* c = equals + 1; *c; c++) {
            if (*c == '"' || *c == '\\')
                text[used++] = '\\';
            text[used++] = *c;
        }
        text[used++] = '"';
        text[used] = '\0';
    }
    return text;
}

/* ---- DLL overrides ---- */

static bool parse_dll_mode(const char* text, size_t length, char* mode, size_t mode_size) {
    size_t used = 0;
    bool disabled = false;
    mode[0] = '\0';
    if (!length) {
        snprintf(mode, mode_size, "d");
        return true;
    }
    for (size_t start = 0; start <= length;) {
        size_t end = start;
        char word[16];
        size_t word_length;
        while (end < length && text[end] != ',')
            end++;
        word_length = end - start;
        if (word_length >= sizeof(word))
            return false;
        for (size_t i = 0; i < word_length; i++)
            word[i] = (char)tolower((unsigned char)text[start + i]);
        word[word_length] = '\0';
        if (!strcmp(word, "n") || !strcmp(word, "native")) {
            if (disabled || strchr(mode, 'n') || used + 3 > mode_size)
                return false;
            used += (size_t)snprintf(mode + used, mode_size - used, "%sn", used ? "," : "");
        } else if (!strcmp(word, "b") || !strcmp(word, "builtin")) {
            if (disabled || strchr(mode, 'b') || used + 3 > mode_size)
                return false;
            used += (size_t)snprintf(mode + used, mode_size - used, "%sb", used ? "," : "");
        } else if (!strcmp(word, "d") || !strcmp(word, "disabled") || !word[0]) {
            if (used || disabled || length > word_length)
                return false;
            disabled = true;
            snprintf(mode, mode_size, "d");
        } else
            return false;
        start = end + 1;
    }
    return mode[0] != '\0';
}

typedef struct {
    char name[DLL_NAME_MAX + 1];
    char mode[8];
} dll_override;

static bool valid_dll_name(const char* name) {
    size_t length = strlen(name);
    if (!length || length > DLL_NAME_MAX)
        return false;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-' && name[i] != '.' && name[i] != '+')
            return false;
    return name[0] != '.';
}

static bool parse_dll_list(const char* text, dll_override* list, size_t* count, char* error, size_t error_size) {
    const char* p = text ? text : "";
    *count = 0;
    while (*p) {
        const char *entry_end, *equals;
        char mode[8];
        while (*p == ';' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        entry_end = p + strcspn(p, "; \t\n\r");
        equals = memchr(p, '=', (size_t)(entry_end - p));
        if (!equals) {
            char shown[96];
            snprintf(shown, sizeof(shown), "%.*s", (int)(entry_end - p > 80 ? 80 : entry_end - p), p);
            set_error(error, error_size, "\"%s\" needs =n, =b, =n,b, =b,n or =d", shown);
            return false;
        }
        if (!parse_dll_mode(equals + 1, (size_t)(entry_end - equals - 1), mode, sizeof(mode))) {
            char shown[96];
            snprintf(shown, sizeof(shown), "%.*s", (int)(entry_end - p > 80 ? 80 : entry_end - p), p);
            set_error(error, error_size, "\"%s\": the load order is n, b, n,b, b,n or d", shown);
            return false;
        }
        for (const char* name = p; name < equals;) {
            const char* name_end = name;
            char lowered[DLL_NAME_MAX + 8];
            size_t length;
            while (name_end < equals && *name_end != ',')
                name_end++;
            length = (size_t)(name_end - name);
            if (length >= sizeof(lowered)) {
                set_error(error, error_size, "%s", "A DLL name is too long");
                return false;
            }
            for (size_t i = 0; i < length; i++)
                lowered[i] = (char)tolower((unsigned char)name[i]);
            lowered[length] = '\0';
            /* Wine keys its overrides by module name without .dll. */
            if (length > 4 && !strcmp(lowered + length - 4, ".dll"))
                lowered[length - 4] = '\0';
            if (!valid_dll_name(lowered)) {
                set_error(error, error_size, "\"%s\" is not a valid DLL name", lowered);
                return false;
            }
            {
                size_t index = 0;
                while (index < *count && strcmp(list[index].name, lowered))
                    index++;
                if (index == *count) {
                    if (*count >= MS_GAME_TUNING_MAX_DLLS) {
                        set_error(error, error_size, "%s", "Too many DLL overrides (64 at most)");
                        return false;
                    }
                    snprintf(list[index].name, sizeof(list[index].name), "%s", lowered);
                    (*count)++;
                }
                snprintf(list[index].mode, sizeof(list[index].mode), "%s", mode);
            }
            name = name_end < equals ? name_end + 1 : name_end;
        }
        p = entry_end;
    }
    return true;
}

bool ms_game_tuning_parse_dll_overrides(const char* text, char** canonical, char* error, size_t error_size) {
    dll_override* list = calloc(MS_GAME_TUNING_MAX_DLLS, sizeof(*list));
    size_t count = 0, capacity = 1, used = 0;
    char* out;
    *canonical = NULL;
    if (!list)
        return false;
    if (!parse_dll_list(text, list, &count, error, error_size)) {
        free(list);
        return false;
    }
    if (!count) {
        free(list);
        return true;
    }
    for (size_t i = 0; i < count; i++)
        capacity += strlen(list[i].name) + strlen(list[i].mode) + 2;
    out = malloc(capacity);
    if (out) {
        out[0] = '\0';
        for (size_t i = 0; i < count; i++)
            used += (size_t)snprintf(out + used, capacity - used, "%s%s=%s", i ? ";" : "", list[i].name, list[i].mode);
    }
    free(list);
    *canonical = out;
    return out != NULL;
}

bool ms_game_tuning_valid_windows_version(const char* version) {
    static const char* const versions[] = {"winxp", "winxp64", "vista", "win7", "win8", "win81", "win10", "win11"};
    if (!version || !version[0] || !strcmp(version, "default"))
        return true;
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
        if (!strcmp(version, versions[i]))
            return true;
    return false;
}

/* ---- the store ---- */

static char* tuning_path(const char* home, const char* provider, const char* id, const char* suffix) {
    char path[PATH_MAX];
    int length;
    if (!home || !ms_game_tuning_valid_key(provider, id))
        return NULL;
    length = snprintf(path, sizeof(path), "%s/config/game-tuning/%s/%s%s", home, provider, id, suffix);
    return length > 0 && (size_t)length < sizeof(path) ? strdup(path) : NULL;
}

static char* read_text(const char* path, bool* missing) {
    FILE* file = path ? fopen(path, "rb") : NULL;
    char* text;
    size_t length;
    if (missing)
        *missing = !file && errno == ENOENT;
    if (!file)
        return NULL;
    text = malloc(TUNING_FILE_MAX + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    length = fread(text, 1, TUNING_FILE_MAX, file);
    if (ferror(file) || !feof(file)) {
        fclose(file);
        free(text);
        return NULL;
    }
    fclose(file);
    text[length] = '\0';
    return text;
}

static bool ensure_directory(const char* path) {
    struct stat metadata;
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return stat(path, &metadata) == 0 && S_ISDIR(metadata.st_mode);
    return false;
}

static bool ensure_parent_directories(const char* home, const char* provider) {
    char path[PATH_MAX];
    const char* const levels[] = {"config", "config/game-tuning"};
    for (size_t i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", home, levels[i]);
        if (!ensure_directory(path))
            return false;
    }
    snprintf(path, sizeof(path), "%s/config/game-tuning/%s", home, provider);
    return ensure_directory(path);
}

static bool write_atomic(const char* path, const char* text, mode_t mode) {
    size_t length = strlen(path) + sizeof(".tmp.XXXXXX");
    char* temporary = malloc(length);
    size_t size = strlen(text);
    bool ok = false;
    int fd;
    if (!temporary)
        return false;
    snprintf(temporary, length, "%s.tmp.XXXXXX", path);
    fd = mkstemp(temporary);
    if (fd >= 0) {
        ok = fchmod(fd, mode) == 0 && write(fd, text, size) == (ssize_t)size;
        ok = close(fd) == 0 && ok && rename(temporary, path) == 0;
        if (!ok)
            (void)unlink(temporary);
    }
    free(temporary);
    return ok;
}

bool ms_game_tuning_load(const char* home, const char* provider, const char* id, ms_game_tuning* tuning) {
    char* path = tuning_path(home, provider, id, ".json");
    bool missing = false;
    char* text;
    char error[96];
    ms_json* root;
    const ms_json* value;
    ms_game_tuning_init(tuning);
    if (!path)
        return false;
    text = read_text(path, &missing);
    free(path);
    if (!text)
        return missing;
    root = ms_json_parse(text, strlen(text), error, sizeof(error));
    free(text);
    if (!root || ms_json_type_of(root) != MS_JSON_OBJECT) {
        ms_json_free(root);
        return false;
    }
    /* Entries are checked again: the file may have been edited by hand. */
    value = ms_json_object_get(root, "env");
    for (size_t i = 0; value && i < ms_json_array_length(value) && tuning->env_count < MS_GAME_TUNING_MAX_ENV; i++) {
        char* entry = NULL;
        char** grown;
        if (ms_json_as_string(ms_json_array_get(value, i), &entry) && valid_env_entry(entry) &&
            (grown = realloc(tuning->env, (tuning->env_count + 1) * sizeof(*grown)))) {
            tuning->env = grown;
            tuning->env[tuning->env_count++] = entry;
            entry = NULL;
        }
        free(entry);
    }
    {
        char* dlls = NULL;
        if (ms_json_as_string(ms_json_object_get(root, "dllOverrides"), &dlls) &&
            !ms_game_tuning_parse_dll_overrides(dlls, &tuning->dll_overrides, NULL, 0))
            tuning->dll_overrides = NULL;
        free(dlls);
    }
    {
        char* version = NULL;
        if (ms_json_as_string(ms_json_object_get(root, "windowsVersion"), &version) &&
            ms_game_tuning_valid_windows_version(version) && strcmp(version, "default"))
            snprintf(tuning->windows_version, sizeof(tuning->windows_version), "%s", version);
        free(version);
    }
    {
        long long cores = 0;
        if (ms_json_as_i64(ms_json_object_get(root, "cpuCores"), &cores) && cores > 0 && cores <= 64)
            tuning->cpu_cores = (unsigned)cores;
    }
    (void)ms_json_as_bool(ms_json_object_get(root, "lowPriority"), &tuning->low_priority);
    {
        char* retina = NULL;
        if (ms_json_as_string(ms_json_object_get(root, "retina"), &retina))
            tuning->retina = !strcmp(retina, "on") ? 1 : !strcmp(retina, "off") ? 0 : -1;
        free(retina);
    }
    {
        char* renderer = NULL;
        if (ms_json_as_string(ms_json_object_get(root, "openglRenderer"), &renderer))
            tuning->zink = !strcmp(renderer, "zink");
        free(renderer);
        renderer = NULL;
        if (ms_json_as_string(ms_json_object_get(root, "vulkanRenderer"), &renderer))
            tuning->kosmickrisp = !strcmp(renderer, "kosmickrisp");
        free(renderer);
    }
    ms_json_free(root);
    return true;
}

bool ms_game_tuning_save(const char* home, const char* provider, const char* id, const ms_game_tuning* tuning,
                         char* error, size_t error_size) {
    char* path = tuning_path(home, provider, id, ".json");
    ms_json_writer writer;
    char* text;
    bool ok;
    if (!path) {
        set_error(error, error_size, "%s", "Invalid game settings request");
        return false;
    }
    if (ms_game_tuning_is_default(tuning)) {
        ok = unlink(path) == 0 || errno == ENOENT;
        free(path);
        if (!ok)
            set_error(error, error_size, "%s", "Could not clear the game's settings");
        return ok;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "env");
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < tuning->env_count; i++)
        ms_json_writer_string(&writer, tuning->env[i]);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "dllOverrides");
    ms_json_writer_string(&writer, tuning->dll_overrides ? tuning->dll_overrides : "");
    ms_json_writer_key(&writer, "windowsVersion");
    ms_json_writer_string(&writer, tuning->windows_version[0] ? tuning->windows_version : "default");
    ms_json_writer_key(&writer, "cpuCores");
    ms_json_writer_u64(&writer, tuning->cpu_cores);
    ms_json_writer_key(&writer, "lowPriority");
    ms_json_writer_bool(&writer, tuning->low_priority);
    ms_json_writer_key(&writer, "retina");
    ms_json_writer_string(&writer, tuning->retina > 0 ? "on" : tuning->retina == 0 ? "off" : "default");
    ms_json_writer_key(&writer, "openglRenderer");
    ms_json_writer_string(&writer, tuning->zink ? "zink" : "winemetalgl");
    ms_json_writer_key(&writer, "vulkanRenderer");
    ms_json_writer_string(&writer, tuning->kosmickrisp ? "kosmickrisp" : "moltenvk");
    ms_json_writer_object_end(&writer);
    text = ms_json_writer_take(&writer);
    ok = text && ensure_parent_directories(home, provider) && write_atomic(path, text, 0600);
    if (!ok)
        set_error(error, error_size, "%s", "Could not save the game's settings");
    free(text);
    free(path);
    return ok;
}

bool ms_game_tuning_has_state(const char* home, const char* provider, const char* id) {
    char* settings = tuning_path(home, provider, id, ".json");
    char* applied = tuning_path(home, provider, id, ".applied");
    bool found = (settings && access(settings, F_OK) == 0) || (applied && access(applied, F_OK) == 0);
    free(settings);
    free(applied);
    return found;
}

/* ---- launch.local.env ---- */

bool ms_game_tuning_write_local_env(const char* dir, const ms_game_tuning* tuning) {
    char *meta, *path, *existing, *kept;
    size_t kept_used = 0, capacity;
    bool missing = false, in_block = false, ok;
    struct stat metadata;
    if (!dir || !dir[0] || stat(dir, &metadata) != 0 || !S_ISDIR(metadata.st_mode))
        return false;
    meta = join(dir, ".metalsharp");
    path = meta ? join(meta, "launch.local.env") : NULL;
    if (!path) {
        free(meta);
        return false;
    }
    existing = read_text(path, &missing);
    if (!existing && !missing) {
        free(meta);
        free(path);
        return false;
    }
    capacity = (existing ? strlen(existing) : 0) + 2;
    for (size_t i = 0; tuning && i < tuning->env_count; i++)
        capacity += strlen(tuning->env[i]) + 1;
    capacity += sizeof(LOCAL_ENV_BEGIN) + sizeof(LOCAL_ENV_END) + 4;
    kept = malloc(capacity);
    if (!kept) {
        free(existing);
        free(meta);
        free(path);
        return false;
    }
    kept[0] = '\0';
    /* MetalSharp's block first, then the user's own lines, which MetalSharp Wine applies after it. */
    if (tuning && tuning->env_count) {
        kept_used += (size_t)snprintf(kept + kept_used, capacity - kept_used, "%s\n", LOCAL_ENV_BEGIN);
        for (size_t i = 0; i < tuning->env_count; i++)
            kept_used += (size_t)snprintf(kept + kept_used, capacity - kept_used, "%s\n", tuning->env[i]);
        kept_used += (size_t)snprintf(kept + kept_used, capacity - kept_used, "%s\n", LOCAL_ENV_END);
    }
    for (char* line = existing; line && *line;) {
        char* next = strchr(line, '\n');
        size_t length = next ? (size_t)(next - line) : strlen(line);
        size_t compare = length && line[length - 1] == '\r' ? length - 1 : length;
        if (compare == strlen(LOCAL_ENV_BEGIN) && !strncmp(line, LOCAL_ENV_BEGIN, compare))
            in_block = true;
        else if (in_block && compare == strlen(LOCAL_ENV_END) && !strncmp(line, LOCAL_ENV_END, compare))
            in_block = false;
        else if (!in_block) {
            memcpy(kept + kept_used, line, length);
            kept_used += length;
            kept[kept_used++] = '\n';
            kept[kept_used] = '\0';
        }
        line = next ? next + 1 : NULL;
    }
    {
        bool blank = true;
        for (const char* c = kept; *c && blank; c++)
            blank = isspace((unsigned char)*c) != 0;
        if (existing && !strcmp(existing, kept))
            ok = true;
        else if (blank)
            ok = unlink(path) == 0 || errno == ENOENT;
        else
            ok = ensure_directory(meta) && write_atomic(path, kept, 0644);
    }
    free(kept);
    free(existing);
    free(meta);
    free(path);
    return ok;
}

/* ---- the registry ---- */

static bool valid_executable_name(const char* name) {
    size_t length = name ? strlen(name) : 0;
    if (!length || length > 255)
        return false;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)name[i] < 0x20 || strchr("\\/[]\"", name[i]))
            return false;
    return true;
}

static const char* registry_mode(const char* mode) {
    if (!strcmp(mode, "n"))
        return "native";
    if (!strcmp(mode, "b"))
        return "builtin";
    if (!strcmp(mode, "n,b"))
        return "native,builtin";
    if (!strcmp(mode, "b,n"))
        return "builtin,native";
    return "";
}

char* ms_game_tuning_registry_text(const char* executable_name, const char* dll_overrides, const char* windows_version,
                                   const char* previous_dll_overrides, bool previous_version) {
    dll_override *current = calloc(MS_GAME_TUNING_MAX_DLLS, sizeof(*current)),
                 *previous = calloc(MS_GAME_TUNING_MAX_DLLS, sizeof(*previous));
    size_t current_count = 0, previous_count = 0, capacity, used = 0;
    char* text = NULL;
    if (!current || !previous || !valid_executable_name(executable_name) ||
        !parse_dll_list(dll_overrides, current, &current_count, NULL, 0) ||
        !parse_dll_list(previous_dll_overrides, previous, &previous_count, NULL, 0) ||
        !ms_game_tuning_valid_windows_version(windows_version))
        goto done;
    capacity = 256 + strlen(executable_name) * 2 + (current_count + previous_count) * (DLL_NAME_MAX + 40);
    text = malloc(capacity);
    if (!text)
        goto done;
#define APPEND(...) used += (size_t)snprintf(text + used, capacity - used, __VA_ARGS__)
    APPEND("Windows Registry Editor Version 5.00\r\n\r\n");
    if (current_count || previous_count) {
        APPEND("[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\%s\\DllOverrides]\r\n", executable_name);
        for (size_t i = 0; i < previous_count; i++) {
            bool kept = false;
            for (size_t j = 0; j < current_count && !kept; j++)
                kept = !strcmp(previous[i].name, current[j].name);
            if (!kept)
                APPEND("\"%s\"=-\r\n", previous[i].name);
        }
        for (size_t i = 0; i < current_count; i++)
            APPEND("\"%s\"=\"%s\"\r\n", current[i].name, registry_mode(current[i].mode));
        APPEND("\r\n");
    }
    if ((windows_version && windows_version[0] && strcmp(windows_version, "default")) || previous_version) {
        APPEND("[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\%s]\r\n", executable_name);
        if (windows_version && windows_version[0] && strcmp(windows_version, "default"))
            APPEND("\"Version\"=\"%s\"\r\n", windows_version);
        else
            APPEND("\"Version\"=-\r\n");
        APPEND("\r\n");
    }
#undef APPEND
done:
    free(current);
    free(previous);
    return text;
}

/* What was last written to a prefix for the game (<id>.applied): so a cleared setting is removed from
 * the registry, and an unchanged one is not imported again. */
typedef struct {
    char* prefix;
    char* executable;
    char* dll_overrides;
    char windows_version[12];
    char* dirs[2];
} applied_state;

static void applied_free(applied_state* state) {
    free(state->prefix);
    free(state->executable);
    free(state->dll_overrides);
    free(state->dirs[0]);
    free(state->dirs[1]);
    memset(state, 0, sizeof(*state));
}

static void applied_load(const char* home, const char* provider, const char* id, applied_state* state) {
    char* path = tuning_path(home, provider, id, ".applied");
    char* text = path ? read_text(path, NULL) : NULL;
    char error[96];
    ms_json* root = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
    const ms_json* dirs;
    char* version = NULL;
    memset(state, 0, sizeof(*state));
    if (root && ms_json_type_of(root) == MS_JSON_OBJECT) {
        (void)ms_json_as_string(ms_json_object_get(root, "prefix"), &state->prefix);
        (void)ms_json_as_string(ms_json_object_get(root, "executable"), &state->executable);
        (void)ms_json_as_string(ms_json_object_get(root, "dllOverrides"), &state->dll_overrides);
        if (ms_json_as_string(ms_json_object_get(root, "windowsVersion"), &version) &&
            ms_game_tuning_valid_windows_version(version))
            snprintf(state->windows_version, sizeof(state->windows_version), "%s", version);
        dirs = ms_json_object_get(root, "dirs");
        for (size_t i = 0; dirs && i < ms_json_array_length(dirs) && i < 2; i++)
            (void)ms_json_as_string(ms_json_array_get(dirs, i), &state->dirs[i]);
    }
    free(version);
    ms_json_free(root);
    free(text);
    free(path);
}

static void applied_save(const char* home, const char* provider, const char* id, const applied_state* state) {
    char* path = tuning_path(home, provider, id, ".applied");
    ms_json_writer writer;
    char* text;
    if (!path)
        return;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "prefix");
    ms_json_writer_string(&writer, state->prefix ? state->prefix : "");
    ms_json_writer_key(&writer, "executable");
    ms_json_writer_string(&writer, state->executable ? state->executable : "");
    ms_json_writer_key(&writer, "dllOverrides");
    ms_json_writer_string(&writer, state->dll_overrides ? state->dll_overrides : "");
    ms_json_writer_key(&writer, "windowsVersion");
    ms_json_writer_string(&writer, state->windows_version);
    ms_json_writer_key(&writer, "dirs");
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < 2; i++)
        if (state->dirs[i])
            ms_json_writer_string(&writer, state->dirs[i]);
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    text = ms_json_writer_take(&writer);
    if (text && ensure_parent_directories(home, provider))
        (void)write_atomic(path, text, 0600);
    free(text);
    free(path);
}

static bool same_text(const char* a, const char* b) {
    return !strcmp(a ? a : "", b ? b : "");
}

/* `wine reg import` of the text into prefix (as the other per-executable AppDefaults are written). */
static bool import_registry(const char* home, const char* prefix, const char* text) {
    char *drive_c = join(prefix, "drive_c"), *reg_file = drive_c ? join(drive_c, "metalsharp-game-settings.reg") : NULL;
    char *wine = join(home, "runtime/wine/bin/metalsharp-wine"), *user_reg = join(prefix, "user.reg");
    pid_t child;
    int status = 0;
    bool ok = false;
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        wine = join(home, "runtime/wine/bin/wine");
    }
    /* Only an existing prefix: a launcher creates its own before the game's first start. */
    if (!reg_file || !wine || access(wine, X_OK) != 0 || !user_reg || access(user_reg, F_OK) != 0 ||
        !write_atomic(reg_file, text, 0644))
        goto done;
    child = fork();
    if (child == 0) {
        char library_env[4096];
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        setenv("WINEPREFIX", prefix, 1);
        setenv("METALSHARP_HOME", home, 1);
        ms_steam_apply_launch_preferences(home);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "none", 1);
        setenv("WINEDLLOVERRIDES", "winedbg=d", 1);
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
        execl(wine, wine, "reg", "import", "C:\\metalsharp-game-settings.reg", (char*)NULL);
        _exit(127);
    }
    if (child > 0) {
        /* A minute at most: a cold wineserver start can take a while under FEX. */
        for (int waited = 0; waited < 600; waited++) {
            pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child) {
                ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                break;
            }
            if (done < 0 && errno != EINTR)
                break;
            {
                struct timespec delay = {0, 100000000};
                (void)nanosleep(&delay, NULL);
            }
            if (waited == 599) {
                (void)kill(child, SIGKILL);
                (void)waitpid(child, NULL, 0);
            }
        }
    }
    (void)unlink(reg_file);
done:
    free(drive_c);
    free(reg_file);
    free(wine);
    free(user_reg);
    return ok;
}

/* Whether user.reg still has the executable's AppDefaults (a recreated prefix has none). */
static bool prefix_has_app_defaults(const char* prefix, const char* executable_name) {
    char* path = join(prefix, "user.reg");
    FILE* file = path ? fopen(path, "rb") : NULL;
    char section[600];
    char* line = NULL;
    size_t capacity = 0, length;
    bool found = false;
    length = (size_t)snprintf(section, sizeof(section), "[Software\\\\Wine\\\\AppDefaults\\\\%s", executable_name);
    while (file && !found && getline(&line, &capacity, file) > 0)
        found = !strncasecmp(line, section, length) && (line[length] == ']' || line[length] == '\\');
    if (file)
        fclose(file);
    free(line);
    free(path);
    return found;
}

static const char* base_name(const char* path) {
    const char* base = path ? path : "";
    for (const char* at = base; *at; at++)
        if (*at == '/' || *at == '\\')
            base = at + 1;
    return base;
}

static char* parent_dir(const char* path) {
    char* copy = path ? strdup(path) : NULL;
    char* slash = copy ? strrchr(copy, '/') : NULL;
    if (!slash) {
        free(copy);
        return NULL;
    }
    *slash = '\0';
    return copy;
}

/* user.reg's `section` (the "[...]" line it starts with, without its timestamp) holds the line `value` */
static bool prefix_section_has(const char* prefix, const char* section, const char* value) {
    char* path = join(prefix, "user.reg");
    FILE* file = path ? fopen(path, "rb") : NULL;
    char* line = NULL;
    size_t capacity = 0, section_length = strlen(section), value_length = strlen(value);
    bool inside = false, found = false;
    while (file && !found && getline(&line, &capacity, file) > 0) {
        if (line[0] == '[')
            inside = !strncasecmp(line, section, section_length);
        else if (inside)
            found = !strncmp(line, value, value_length) && (line[value_length] == '\n' || line[value_length] == '\r');
    }
    if (file)
        fclose(file);
    free(line);
    free(path);
    return found;
}

/* The prefix's Retina (Mac Driver RetinaMode and its DPI) for the game about to start: the game's own setting or
 * else Settings > Graphics' (metalsharp/MetalSharp#797). Every prefix, not only a bottle of the game's own: a Windows
 * program runs in Steam's prefix, and its own setting was ignored there; and one program's setting stayed in Steam's
 * prefix for every Steam game after it, until Steam's next start set Settings > Graphics' again (#811). Each launch
 * now sets its own. Imported only when the prefix's user.reg does not already hold it. */
static void apply_prefix_retina(const char* home, const char* prefix, const ms_game_tuning* tuning) {
    bool retina = tuning->retina >= 0 ? tuning->retina > 0 : ms_config_retina_enabled(home);
    const char* retina_line = retina ? "\"RetinaMode\"=\"Y\"" : "\"RetinaMode\"=\"N\"";
    const char* dpi_line = retina ? "\"LogPixels\"=dword:000000c0" : "\"LogPixels\"=dword:00000060";
    bool current;
    char reg[256];
    if (!prefix || !prefix[0])
        return;
    /* A bottle Wine has not made yet starts with Retina off at 96 DPI, so only Retina on needs writing to it. */
    if (!retina) {
        char* user_reg = join(prefix, "user.reg");
        bool made = user_reg && access(user_reg, F_OK) == 0;
        free(user_reg);
        if (!made)
            return;
    }
    current = prefix_section_has(prefix, "[Software\\\\Wine\\\\Mac Driver]", retina_line) &&
              prefix_section_has(prefix, "[Control Panel\\\\Desktop]", dpi_line);
    if (current)
        return;
    snprintf(reg, sizeof(reg),
             "Windows Registry Editor Version 5.00\r\n\r\n"
             "[HKEY_CURRENT_USER\\Software\\Wine\\Mac Driver]\r\n\"RetinaMode\"=\"%c\"\r\n\r\n"
             "[HKEY_CURRENT_USER\\Control Panel\\Desktop]\r\n\"LogPixels\"=dword:%08x\r\n",
             retina ? 'Y' : 'N', retina ? 192 : 96);
    if (!import_registry(home, prefix, reg))
        ms_log_event(home, "Game settings: could not write the prefix's Retina setting");
}

void ms_game_tuning_prepare_launch(const char* home, const char* provider, const char* id, const char* prefix,
                                   const char* game_dir, const char* executable) {
    ms_game_tuning tuning;
    applied_state state;
    char* exe_dir = parent_dir(executable);
    const char* exe_name = base_name(executable);
    const char* dirs[2] = {game_dir && game_dir[0] ? game_dir : NULL, NULL};
    bool changed = false;
    if (!home || !ms_game_tuning_valid_key(provider, id) || !ms_game_tuning_load(home, provider, id, &tuning)) {
        free(exe_dir);
        return;
    }
    apply_prefix_retina(home, prefix, &tuning);
    applied_load(home, provider, id, &state);
    if (ms_game_tuning_is_default(&tuning) && !state.prefix && !state.dirs[0]) {
        /* Never tuned: nothing to write or remove. */
        applied_free(&state);
        free(exe_dir);
        return;
    }
    if (exe_dir && (!dirs[0] || strcmp(exe_dir, dirs[0])))
        dirs[dirs[0] ? 1 : 0] = exe_dir;
    /* Folders it was written to before and no longer launches from lose MetalSharp's block. */
    for (size_t i = 0; i < 2; i++) {
        bool current = state.dirs[i] &&
                       ((dirs[0] && !strcmp(state.dirs[i], dirs[0])) || (dirs[1] && !strcmp(state.dirs[i], dirs[1])));
        if (state.dirs[i] && !current)
            (void)ms_game_tuning_write_local_env(state.dirs[i], NULL);
    }
    for (size_t i = 0; i < 2; i++) {
        if (dirs[i])
            (void)ms_game_tuning_write_local_env(dirs[i], &tuning);
        if (!same_text(state.dirs[i], dirs[i])) {
            free(state.dirs[i]);
            state.dirs[i] = dirs[i] ? strdup(dirs[i]) : NULL;
            changed = true;
        }
    }
    if (prefix && prefix[0] && valid_executable_name(exe_name)) {
        bool same_target =
            same_text(state.prefix, prefix) && state.executable && !strcasecmp(state.executable, exe_name);
        bool had_any = (state.dll_overrides && state.dll_overrides[0]) || state.windows_version[0];
        bool wants_any = (tuning.dll_overrides && tuning.dll_overrides[0]) || tuning.windows_version[0];
        /* Another executable or prefix than last time: the old one's values go. */
        if (!same_target && had_any && state.prefix && state.executable && valid_executable_name(state.executable)) {
            char* text = ms_game_tuning_registry_text(state.executable, NULL, NULL, state.dll_overrides,
                                                      state.windows_version[0] != '\0');
            if (text)
                (void)import_registry(home, state.prefix, text);
            free(text);
        }
        if (!same_target || !same_text(state.dll_overrides, tuning.dll_overrides) ||
            strcmp(state.windows_version, tuning.windows_version) ||
            (wants_any && !prefix_has_app_defaults(prefix, exe_name))) {
            char* text = (wants_any || (same_target && had_any))
                             ? ms_game_tuning_registry_text(exe_name, tuning.dll_overrides, tuning.windows_version,
                                                            same_target ? state.dll_overrides : NULL,
                                                            same_target && state.windows_version[0])
                             : NULL;
            bool imported = !text || import_registry(home, prefix, text);
            free(text);
            if (imported) {
                free(state.prefix);
                free(state.executable);
                free(state.dll_overrides);
                state.prefix = strdup(prefix);
                state.executable = strdup(exe_name);
                state.dll_overrides = tuning.dll_overrides ? strdup(tuning.dll_overrides) : NULL;
                snprintf(state.windows_version, sizeof(state.windows_version), "%s", tuning.windows_version);
                changed = true;
            } else {
                char message[512];
                snprintf(message, sizeof(message),
                         "Game settings: could not write %s's DLL overrides and Windows version", exe_name);
                ms_log_event(home, message);
            }
        }
    }
    if (changed)
        applied_save(home, provider, id, &state);
    /* Built here, before the launch forks: the child only checks it is there. */
    if (tuning.cpu_cores) {
        char* shim = ms_steam_cpu_topology_shim(home);
        if (!shim)
            ms_log_event(home,
                         "Game settings: the CPU core limit needs the CPU topology shim, which could not be built");
        free(shim);
    }
    if (tuning.low_priority && exe_name[0])
        ms_process_watch_game_priority(home, exe_name);
    applied_free(&state);
    ms_game_tuning_free(&tuning);
    free(exe_dir);
}

void ms_game_tuning_apply_child(const char* home, const char* provider, const char* id) {
    ms_game_tuning tuning;
    if (!home)
        return;
    /* The Metal HUD switch and its metrics, for every store's games; the game's own variables below can still
     * change them. */
    ms_config_apply_metal_hud_env(home);
    if (!ms_game_tuning_load(home, provider, id, &tuning))
        return;
    /* MetalSharp Wine 8.0's renderer switches: Zink for OpenGL in WineMetalGL's place, KosmicKrisp for Vulkan in
     * MoltenVK's. Wine reads them when it starts, before launch.env, so they are set here rather than written to
     * the game's launch.local.env. */
    if (tuning.zink)
        setenv("SharpGL_Switch", "1", 1);
    if (tuning.kosmickrisp)
        setenv("SharpVK_Switch", "1", 1);
    for (size_t i = 0; i < tuning.env_count; i++) {
        char* entry = tuning.env[i];
        char* equals = entry[0] == '-' ? NULL : strchr(entry, '=');
        if (entry[0] == '-')
            unsetenv(entry + 1);
        else if (equals) {
            *equals = '\0';
            setenv(entry, equals + 1, 1);
            *equals = '=';
        }
    }
    if (tuning.cpu_cores) {
        char count[8], path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/runtime/shims/libmetalsharp_cpu_topology.dylib", home);
        if (access(path, R_OK) == 0) {
            /* The metalsharp-wine wrapper restores DYLD_INSERT_LIBRARIES from METALSHARP_DYLD_INSERT_LIBRARIES,
             * which macOS leaves alone when it runs a script or a launcher tool. */
            const char* const names[] = {"METALSHARP_DYLD_INSERT_LIBRARIES", "DYLD_INSERT_LIBRARIES"};
            snprintf(count, sizeof(count), "%u", tuning.cpu_cores);
            setenv("METALSHARP_CPU_COUNT", count, 1);
            for (size_t i = 0; i < 2; i++) {
                const char* current = getenv(names[i]);
                if (!current || !current[0])
                    setenv(names[i], path, 1);
                else if (!strstr(current, path)) {
                    char combined[PATH_MAX * 4];
                    snprintf(combined, sizeof(combined), "%s:%s", current, path);
                    setenv(names[i], combined, 1);
                }
            }
        }
    }
    if (tuning.low_priority && getpriority(PRIO_PROCESS, 0) < LOW_PRIORITY_NICE)
        (void)setpriority(PRIO_PROCESS, 0, LOW_PRIORITY_NICE);
    ms_game_tuning_free(&tuning);
}

/* ---- routes ---- */

static char* error_json(const char* message) {
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "error");
    ms_json_writer_string(&writer, message);
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

static char* tuning_json(const char* provider, const char* id, const ms_game_tuning* tuning) {
    ms_json_writer writer;
    char* env_text = ms_game_tuning_env_text(tuning);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "provider");
    ms_json_writer_string(&writer, provider);
    ms_json_writer_key(&writer, "id");
    ms_json_writer_string(&writer, id);
    ms_json_writer_key(&writer, "env");
    ms_json_writer_string(&writer, env_text ? env_text : "");
    ms_json_writer_key(&writer, "envList");
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < tuning->env_count; i++)
        ms_json_writer_string(&writer, tuning->env[i]);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "dllOverrides");
    ms_json_writer_string(&writer, tuning->dll_overrides ? tuning->dll_overrides : "");
    ms_json_writer_key(&writer, "windowsVersion");
    ms_json_writer_string(&writer, tuning->windows_version[0] ? tuning->windows_version : "default");
    ms_json_writer_key(&writer, "cpuCores");
    ms_json_writer_u64(&writer, tuning->cpu_cores);
    ms_json_writer_key(&writer, "lowPriority");
    ms_json_writer_bool(&writer, tuning->low_priority);
    ms_json_writer_key(&writer, "retina");
    ms_json_writer_string(&writer, tuning->retina > 0 ? "on" : tuning->retina == 0 ? "off" : "default");
    ms_json_writer_key(&writer, "openglRenderer");
    ms_json_writer_string(&writer, tuning->zink ? "zink" : "winemetalgl");
    ms_json_writer_key(&writer, "vulkanRenderer");
    ms_json_writer_string(&writer, tuning->kosmickrisp ? "kosmickrisp" : "moltenvk");
    ms_json_writer_object_end(&writer);
    free(env_text);
    return ms_json_writer_take(&writer);
}

static int hex_value(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

static bool query_value(const char* query, const char* name, char* out, size_t out_size) {
    size_t name_length = strlen(name);
    for (const char* p = query; p && *p;) {
        const char* end = p + strcspn(p, "&");
        if ((size_t)(end - p) > name_length && !strncmp(p, name, name_length) && p[name_length] == '=') {
            size_t used = 0;
            for (const char* c = p + name_length + 1; c < end; c++) {
                char decoded = *c;
                if (*c == '+')
                    decoded = ' ';
                else if (*c == '%' && c + 2 < end && hex_value(c[1]) >= 0 && hex_value(c[2]) >= 0) {
                    decoded = (char)(hex_value(c[1]) * 16 + hex_value(c[2]));
                    c += 2;
                }
                if (used + 1 >= out_size)
                    return false;
                out[used++] = decoded;
            }
            out[used] = '\0';
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

char* ms_game_tuning_get_json(const char* home, const char* query, int* status) {
    char provider[160], id[160];
    ms_game_tuning tuning;
    char* out;
    if (status)
        *status = 400;
    if (!query_value(query, "provider", provider, sizeof(provider)) || !query_value(query, "id", id, sizeof(id)) ||
        !ms_game_tuning_valid_key(provider, id))
        return error_json("provider and id required");
    if (!ms_game_tuning_load(home, provider, id, &tuning)) {
        if (status)
            *status = 500;
        return error_json("Could not read the game's settings");
    }
    out = tuning_json(provider, id, &tuning);
    ms_game_tuning_free(&tuning);
    if (status)
        *status = 200;
    return out;
}

static char* request_id(const ms_json* request) {
    const ms_json* value = ms_json_object_get(request, "id");
    char* id = NULL;
    long long number;
    if (ms_json_as_string(value, &id))
        return id;
    if (ms_json_as_i64(value, &number) && number >= 0) {
        char text[32];
        snprintf(text, sizeof(text), "%lld", number);
        return strdup(text);
    }
    return NULL;
}

char* ms_game_tuning_save_json(const char* home, const char* body, size_t length, int* status) {
    char parse_error[96], error[256] = "";
    ms_json* request = ms_json_parse(body ? body : "", body ? length : 0, parse_error, sizeof(parse_error));
    char *provider = NULL, *id = NULL, *out = NULL;
    const ms_json* value;
    ms_game_tuning tuning;
    bool loaded = false;
    if (status)
        *status = 400;
    if (!request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        !ms_json_as_string(ms_json_object_get(request, "provider"), &provider) || !(id = request_id(request)) ||
        !ms_game_tuning_valid_key(provider, id)) {
        out = error_json("provider and id required");
        goto done;
    }
    if (!(loaded = ms_game_tuning_load(home, provider, id, &tuning))) {
        /* An unreadable file is replaced by what is saved now. */
        ms_game_tuning_init(&tuning);
        loaded = true;
    }
    /* Fields left out keep their saved values. */
    if ((value = ms_json_object_get(request, "env"))) {
        char* text = NULL;
        char** entries = NULL;
        size_t count = 0;
        if (ms_json_type_of(value) == MS_JSON_ARRAY) {
            /* One entry per item, NAME=value as is (the value may hold spaces) or -NAME: quoted into the
             * typed form and parsed as one. */
            size_t capacity = 1, used = 0;
            for (size_t i = 0; i < ms_json_array_length(value); i++) {
                char* item = NULL;
                if (ms_json_as_string(ms_json_array_get(value, i), &item))
                    capacity += strlen(item) * 2 + 4;
                free(item);
            }
            text = malloc(capacity);
            for (size_t i = 0; text && i < ms_json_array_length(value); i++) {
                char* item = NULL;
                const char* equals;
                if (!ms_json_as_string(ms_json_array_get(value, i), &item)) {
                    snprintf(error, sizeof(error), "env entries must be strings");
                    break;
                }
                equals = item[0] == '-' ? NULL : strchr(item, '=');
                if (used)
                    text[used++] = ' ';
                if (!equals) {
                    memcpy(text + used, item, strlen(item));
                    used += strlen(item);
                } else {
                    memcpy(text + used, item, (size_t)(equals - item + 1));
                    used += (size_t)(equals - item + 1);
                    text[used++] = '"';
                    for (const char* c = equals + 1; *c; c++) {
                        if (*c == '"' || *c == '\\')
                            text[used++] = '\\';
                        text[used++] = *c;
                    }
                    text[used++] = '"';
                }
                free(item);
            }
            if (text)
                text[used] = '\0';
            if (!error[0] && (!text || !ms_game_tuning_parse_env(text, &entries, &count, error, sizeof(error))) &&
                !error[0])
                snprintf(error, sizeof(error), "env must be NAME=value entries");
        } else if (!ms_json_as_string(value, &text) ||
                   !ms_game_tuning_parse_env(text, &entries, &count, error, sizeof(error))) {
            if (!error[0])
                snprintf(error, sizeof(error), "env must be text");
        }
        free(text);
        if (error[0]) {
            free_entries(entries, count);
            out = error_json(error);
            goto done;
        }
        free_entries(tuning.env, tuning.env_count);
        tuning.env = entries;
        tuning.env_count = count;
    }
    if ((value = ms_json_object_get(request, "dllOverrides"))) {
        char *text = NULL, *canonical = NULL;
        if (ms_json_type_of(value) != MS_JSON_NULL &&
            (!ms_json_as_string(value, &text) ||
             !ms_game_tuning_parse_dll_overrides(text, &canonical, error, sizeof(error)))) {
            free(text);
            out = error_json(error[0] ? error : "dllOverrides must be text");
            goto done;
        }
        free(text);
        free(tuning.dll_overrides);
        tuning.dll_overrides = canonical;
    }
    if ((value = ms_json_object_get(request, "windowsVersion"))) {
        char* version = NULL;
        if (ms_json_type_of(value) != MS_JSON_NULL &&
            (!ms_json_as_string(value, &version) || !ms_game_tuning_valid_windows_version(version))) {
            free(version);
            out =
                error_json("windowsVersion must be default, winxp, winxp64, vista, win7, win8, win81, win10 or win11");
            goto done;
        }
        snprintf(tuning.windows_version, sizeof(tuning.windows_version), "%s",
                 version && strcmp(version, "default") ? version : "");
        free(version);
    }
    if ((value = ms_json_object_get(request, "cpuCores"))) {
        long long cores = 0;
        if (ms_json_type_of(value) != MS_JSON_NULL && (!ms_json_as_i64(value, &cores) || cores < 0 || cores > 64)) {
            out = error_json("cpuCores must be 0 (every core) to 64");
            goto done;
        }
        tuning.cpu_cores = (unsigned)cores;
    }
    if ((value = ms_json_object_get(request, "lowPriority"))) {
        bool low = false;
        if (ms_json_type_of(value) != MS_JSON_NULL && !ms_json_as_bool(value, &low)) {
            out = error_json("lowPriority must be true or false");
            goto done;
        }
        tuning.low_priority = low;
    }
    if ((value = ms_json_object_get(request, "retina"))) {
        char* retina = NULL;
        if (ms_json_type_of(value) != MS_JSON_NULL &&
            (!ms_json_as_string(value, &retina) ||
             (strcmp(retina, "default") && strcmp(retina, "on") && strcmp(retina, "off")))) {
            free(retina);
            out = error_json("retina must be \"default\", \"on\" or \"off\"");
            goto done;
        }
        tuning.retina = retina && !strcmp(retina, "on") ? 1 : retina && !strcmp(retina, "off") ? 0 : -1;
        free(retina);
    }
    if ((value = ms_json_object_get(request, "openglRenderer"))) {
        char* renderer = NULL;
        if (ms_json_type_of(value) != MS_JSON_NULL &&
            (!ms_json_as_string(value, &renderer) || (strcmp(renderer, "winemetalgl") && strcmp(renderer, "zink")))) {
            free(renderer);
            out = error_json("openglRenderer must be \"winemetalgl\" or \"zink\"");
            goto done;
        }
        tuning.zink = renderer && !strcmp(renderer, "zink");
        free(renderer);
    }
    if ((value = ms_json_object_get(request, "vulkanRenderer"))) {
        char* renderer = NULL;
        if (ms_json_type_of(value) != MS_JSON_NULL &&
            (!ms_json_as_string(value, &renderer) ||
             (strcmp(renderer, "moltenvk") && strcmp(renderer, "kosmickrisp")))) {
            free(renderer);
            out = error_json("vulkanRenderer must be \"moltenvk\" or \"kosmickrisp\"");
            goto done;
        }
        tuning.kosmickrisp = renderer && !strcmp(renderer, "kosmickrisp");
        free(renderer);
    }
    if (!ms_game_tuning_save(home, provider, id, &tuning, error, sizeof(error))) {
        if (status)
            *status = 500;
        out = error_json(error);
        goto done;
    }
    /* The folders it last launched from get the new variables now; the registry, CPU and priority
     * settings apply at its next start. */
    {
        applied_state state;
        applied_load(home, provider, id, &state);
        for (size_t i = 0; i < 2; i++)
            if (state.dirs[i])
                (void)ms_game_tuning_write_local_env(state.dirs[i], &tuning);
        applied_free(&state);
    }
    out = tuning_json(provider, id, &tuning);
    if (status)
        *status = 200;
done:
    if (loaded)
        ms_game_tuning_free(&tuning);
    ms_json_free(request);
    free(provider);
    free(id);
    return out;
}
