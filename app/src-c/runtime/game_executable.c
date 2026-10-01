#define _DARWIN_C_SOURCE
#include "metalsharp_backend/game_executable.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define OVERRIDE_PATH_MAX 4096

static bool valid_component(const char* value) {
    size_t length = value ? strlen(value) : 0;
    if (length == 0 || length > 128)
        return false;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)value[i]) && value[i] != '_' && value[i] != '-' && value[i] != '.')
            return false;
    return true;
}

static bool path_within(const char* path, const char* root) {
    size_t length = root ? strlen(root) : 0;
    return length > 0 && !strncmp(path, root, length) && path[length] == '/';
}

static bool valid_executable_path(const char* install_dir, const char* executable, char** real_root_out,
                                  char** real_executable_out) {
    struct stat metadata;
    char* real_root = install_dir ? realpath(install_dir, NULL) : NULL;
    char* real_executable = executable ? realpath(executable, NULL) : NULL;
    const char* extension;
    bool valid = false;
    if (!real_root || !real_executable || stat(real_root, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        stat(real_executable, &metadata) != 0 || !S_ISREG(metadata.st_mode) || !path_within(real_executable, real_root))
        goto done;
    extension = strrchr(real_executable, '.');
    if (!extension || strcasecmp(extension, ".exe"))
        goto done;
    valid = true;
done:
    if (valid) {
        if (real_root_out)
            *real_root_out = real_root;
        else
            free(real_root);
        if (real_executable_out)
            *real_executable_out = real_executable;
        else
            free(real_executable);
    } else {
        free(real_root);
        free(real_executable);
    }
    return valid;
}

static char* override_file_path(const char* home, const char* provider, const char* game_id) {
    char path[OVERRIDE_PATH_MAX];
    int length;
    if (!home || !valid_component(provider) || !valid_component(game_id))
        return NULL;
    length = snprintf(path, sizeof(path), "%s/config/game-executables/%s/%s.txt", home, provider, game_id);
    return length > 0 && (size_t)length < sizeof(path) ? strdup(path) : NULL;
}

static char* read_override(const char* path) {
    char buffer[OVERRIDE_PATH_MAX];
    size_t length;
    FILE* file = path ? fopen(path, "rb") : NULL;
    if (!file)
        return NULL;
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    if (ferror(file) || !feof(file)) {
        fclose(file);
        return NULL;
    }
    fclose(file);
    while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
        length--;
    if (!length)
        return NULL;
    buffer[length] = '\0';
    return strdup(buffer);
}

int ms_game_executable_override_load(const char* home, const char* provider, const char* game_id,
                                     const char* install_dir, char** executable_out) {
    char *path = override_file_path(home, provider, game_id), *relative = NULL, *candidate = NULL;
    char *real_root = NULL, *real_executable = NULL;
    int result = 0;
    if (executable_out)
        *executable_out = NULL;
    if (!path)
        return -1;
    relative = read_override(path);
    if (!relative) {
        result = access(path, F_OK) == 0 ? -1 : 0;
        free(path);
        return result;
    }
    free(path);
    if (relative[0] == '/' || strstr(relative, "../") || !strcmp(relative, "..")) {
        result = -1;
        goto done;
    }
    if (install_dir)
        candidate = malloc(strlen(install_dir) + strlen(relative) + 2);
    if (!candidate) {
        result = -1;
        goto done;
    }
    sprintf(candidate, "%s/%s", install_dir, relative);
    if (!valid_executable_path(install_dir, candidate, &real_root, &real_executable)) {
        result = -1;
        goto done;
    }
    if (executable_out) {
        *executable_out = real_executable;
        real_executable = NULL;
    }
    result = 1;
done:
    free(relative);
    free(candidate);
    free(real_root);
    free(real_executable);
    return result;
}

static bool ensure_directory(const char* path) {
    struct stat metadata;
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return stat(path, &metadata) == 0 && S_ISDIR(metadata.st_mode);
    return false;
}

static bool write_relative_override(const char* path, const char* relative) {
    char* temporary;
    size_t length = strlen(path) + sizeof(".tmp.XXXXXX");
    int fd;
    bool ok = false;
    temporary = malloc(length);
    if (!temporary)
        return false;
    snprintf(temporary, length, "%s.tmp.XXXXXX", path);
    fd = mkstemp(temporary);
    if (fd < 0)
        goto done;
    if (fchmod(fd, 0600) == 0 && write(fd, relative, strlen(relative)) == (ssize_t)strlen(relative) &&
        write(fd, "\n", 1) == 1 && fsync(fd) == 0 && close(fd) == 0) {
        fd = -1;
        if (rename(temporary, path) == 0)
            ok = true;
    }
    if (fd >= 0)
        close(fd);
    if (!ok)
        unlink(temporary);
done:
    free(temporary);
    return ok;
}

bool ms_game_executable_override_save(const char* home, const char* provider, const char* game_id,
                                      const char* install_dir, const char* executable, char* error, size_t error_size) {
    char *directory = NULL, *path = NULL, *real_root = NULL, *real_executable = NULL;
    char config_directory[OVERRIDE_PATH_MAX];
    bool ok = false;
    if (error && error_size)
        error[0] = '\0';
    if (!home || !valid_component(provider) || !valid_component(game_id) || !install_dir) {
        if (error && error_size)
            snprintf(error, error_size, "Invalid game executable request");
        return false;
    }
    path = override_file_path(home, provider, game_id);
    if (!path) {
        if (error && error_size)
            snprintf(error, error_size, "Could not resolve executable settings path");
        return false;
    }
    if (!executable || !executable[0]) {
        ok = unlink(path) == 0 || errno == ENOENT;
        if (!ok && error && error_size)
            snprintf(error, error_size, "Could not clear the saved executable");
        goto done;
    }
    if (!valid_executable_path(install_dir, executable, &real_root, &real_executable)) {
        if (error && error_size)
            snprintf(error, error_size, "Choose a regular .exe inside this game's install folder");
        goto done;
    }
    directory = strdup(path);
    if (!directory)
        goto done;
    char* slash = strrchr(directory, '/');
    if (!slash)
        goto done;
    *slash = '\0';
    slash = strrchr(directory, '/');
    if (!slash)
        goto done;
    *slash = '\0';
    int config_length = snprintf(config_directory, sizeof(config_directory), "%s/config", home);
    if (config_length <= 0 || (size_t)config_length >= sizeof(config_directory) ||
        !ensure_directory(config_directory) || !ensure_directory(directory))
        goto done;
    char provider_dir[OVERRIDE_PATH_MAX];
    int directory_length = snprintf(provider_dir, sizeof(provider_dir), "%s/%s", directory, provider);
    if (directory_length <= 0 || (size_t)directory_length >= sizeof(provider_dir) || !ensure_directory(provider_dir))
        goto done;
    const char* relative = real_executable + strlen(real_root) + 1;
    if (!*relative || !write_relative_override(path, relative))
        goto done;
    ok = true;
done:
    if (!ok && error && error_size && !error[0])
        snprintf(error, error_size, "Could not save the selected executable");
    free(directory);
    free(path);
    free(real_root);
    free(real_executable);
    return ok;
}
