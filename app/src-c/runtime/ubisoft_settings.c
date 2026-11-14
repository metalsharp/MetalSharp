#define _DARWIN_C_SOURCE
#include "metalsharp_backend/ubisoft.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Ubisoft Connect's settings.yaml keeps where new games go under misc: game_installation_path (the folder its
 * install dialog offers) and installer_cache_path (where it downloads them first). MetalSharp maps Ubisoft's games
 * drive as Y: (the external drive the games folder is on, or that folder on this Mac), so those point at the games
 * folder there. A folder through Z: (the whole Mac) looks like the Mac's internal disk to Connect, which measures
 * free space at the drive's root: a big game on an external drive is refused for lack of space. */
static const char* const UBISOFT_PATH_KEYS[] = {"game_installation_path", "installer_cache_path"};
#define UBISOFT_PATH_KEY_COUNT (sizeof(UBISOFT_PATH_KEYS) / sizeof(UBISOFT_PATH_KEYS[0]))

typedef struct {
    char* data;
    size_t length, capacity;
} text_buffer;

static bool append(text_buffer* out, const char* text, size_t length) {
    if (out->length + length + 1 > out->capacity) {
        size_t next = out->capacity ? out->capacity * 2 : 4096;
        char* grown;
        while (next < out->length + length + 1)
            next *= 2;
        if (!(grown = realloc(out->data, next)))
            return false;
        out->data = grown;
        out->capacity = next;
    }
    memcpy(out->data + out->length, text, length);
    out->length += length;
    out->data[out->length] = '\0';
    return true;
}

static bool append_str(text_buffer* out, const char* text) {
    return append(out, text, strlen(text));
}

static bool append_key(text_buffer* out, const char* key, const char* dir) {
    return append_str(out, "  ") && append_str(out, key) && append_str(out, ": ") && append_str(out, dir) &&
           append_str(out, "\n");
}

/* A misc key's line ("  <key>: <value>"), and that key's index in UBISOFT_PATH_KEYS, or -1. */
static int path_key_of(const char* line, size_t length, const char** value, size_t* value_length) {
    if (length < 2 || line[0] != ' ' || line[1] != ' ' || line[2] == ' ')
        return -1;
    for (size_t k = 0; k < UBISOFT_PATH_KEY_COUNT; k++) {
        size_t n = strlen(UBISOFT_PATH_KEYS[k]);
        if (length >= 2 + n + 1 && !strncmp(line + 2, UBISOFT_PATH_KEYS[k], n) && line[2 + n] == ':') {
            const char* v = line + 2 + n + 1;
            const char* end = line + length;
            while (v < end && (*v == ' ' || *v == '\t'))
                v++;
            while (end > v && isspace((unsigned char)end[-1]))
                end--;
            *value = v;
            *value_length = (size_t)(end - v);
            return (int)k;
        }
    }
    return -1;
}

/* Whether value is already on dir's drive: a folder the user chose there is kept. */
static bool on_drive(const char* value, size_t length, const char* dir) {
    if (length >= 2 && (value[0] == '"' || value[0] == '\'')) {
        value++;
        length -= 2;
    }
    return length >= 2 && toupper((unsigned char)value[0]) == toupper((unsigned char)dir[0]) && value[1] == ':';
}

char* ms_ubisoft_settings_install_to(const char* text, const char* dir, bool* changed) {
    text_buffer out = {0};
    const char* line = text ? text : "";
    bool in_misc = false, seen_misc = false, have[UBISOFT_PATH_KEY_COUNT] = {false};
    *changed = false;
    if (!dir || !dir[0] || dir[1] != ':')
        return NULL;
    while (*line) {
        const char* end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        size_t full = end ? length + 1 : length;
        bool top_level = length > 0 && line[0] != ' ' && line[0] != '\t' && line[0] != '#';
        const char* value;
        size_t value_length;
        int key;
        if (in_misc && top_level) {
            /* the end of misc: the keys it lacked go at its end */
            for (size_t k = 0; k < UBISOFT_PATH_KEY_COUNT; k++)
                if (!have[k]) {
                    if (!append_key(&out, UBISOFT_PATH_KEYS[k], dir))
                        goto fail;
                    have[k] = *changed = true;
                }
            in_misc = false;
        }
        if (top_level && length >= 5 && !strncmp(line, "misc:", 5)) {
            in_misc = seen_misc = true;
        } else if (in_misc && (key = path_key_of(line, length, &value, &value_length)) >= 0) {
            have[key] = true;
            if (!on_drive(value, value_length, dir)) {
                if (!append_key(&out, UBISOFT_PATH_KEYS[key], dir))
                    goto fail;
                *changed = true;
                line += full;
                continue;
            }
        }
        if (!append(&out, line, full))
            goto fail;
        if (!end && length > 0 && !append_str(&out, "\n"))
            goto fail;
        line += full;
    }
    if (in_misc || !seen_misc) {
        if (!seen_misc && !append_str(&out, "misc:\n"))
            goto fail;
        for (size_t k = 0; k < UBISOFT_PATH_KEY_COUNT; k++)
            if (!have[k]) {
                if (!append_key(&out, UBISOFT_PATH_KEYS[k], dir))
                    goto fail;
                *changed = true;
            }
    }
    if (!out.data && !append_str(&out, ""))
        goto fail;
    return out.data;
fail:
    free(out.data);
    *changed = false;
    return NULL;
}

static char* read_whole(const char* path) {
    FILE* file = fopen(path, "rb");
    char* data = NULL;
    long size;
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 && size < (1L << 22) &&
        fseek(file, 0, SEEK_SET) == 0 && (data = malloc((size_t)size + 1))) {
        if (fread(data, 1, (size_t)size, file) == (size_t)size)
            data[size] = '\0';
        else {
            free(data);
            data = NULL;
        }
    }
    fclose(file);
    return data;
}

static bool write_atomically(const char* path, const char* text) {
    char tmp[4096];
    FILE* file;
    size_t length = strlen(text);
    bool ok;
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.metalsharp-tmp", path) >= sizeof(tmp) || !(file = fopen(tmp, "wb")))
        return false;
    ok = fwrite(text, 1, length, file) == length;
    ok = fclose(file) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

int ms_ubisoft_settings_point_installs_at(const char* prefix, const char* dir) {
    char users[4096];
    DIR* listing;
    struct dirent* entry;
    int updated = 0;
    if (!prefix || (size_t)snprintf(users, sizeof(users), "%s/drive_c/users", prefix) >= sizeof(users) ||
        !(listing = opendir(users)))
        return 0;
    while ((entry = readdir(listing)) != NULL) {
        char path[4096];
        char *text, *next;
        bool changed = false;
        if (entry->d_name[0] == '.' || !strcasecmp(entry->d_name, "Public"))
            continue;
        if ((size_t)snprintf(path, sizeof(path), "%s/%s/AppData/Local/Ubisoft Game Launcher/settings.yaml", users,
                             entry->d_name) >= sizeof(path) ||
            !(text = read_whole(path)))
            continue; /* Connect has not run as this user yet: its first run writes its own defaults */
        next = ms_ubisoft_settings_install_to(text, dir, &changed);
        if (next && changed && write_atomically(path, next))
            updated++;
        free(next);
        free(text);
    }
    closedir(listing);
    return updated;
}
