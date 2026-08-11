/// @file mscoree_path.h
/// @brief Bounded path helpers shared by the Wine mscoree shim and its tests.
#ifndef MSCOREE_PATH_H
#define MSCOREE_PATH_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline void mscoree_terminate_path(char* path, size_t path_size) {
    if (path && path_size > 0)
        path[path_size - 1] = '\0';
}

static inline void mscoree_copy_path(char* destination, size_t destination_size, const char* source) {
    if (!destination || destination_size == 0)
        return;

    if (!source)
        source = "";
    size_t copy_length = 0;
    while (copy_length < destination_size - 1 && source[copy_length] != '\0')
        copy_length++;
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

static inline int mscoree_build_config_path(char* config_file, size_t config_file_size, const char* exe_path) {
    if (!config_file || config_file_size == 0 || !exe_path)
        return 0;

    int written = snprintf(config_file, config_file_size, "%s.config", exe_path);
    return written >= 0 && (size_t)written < config_file_size;
}

#endif
