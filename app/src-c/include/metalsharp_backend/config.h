#ifndef METALSHARP_BACKEND_CONFIG_H
#define METALSHARP_BACKEND_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

char* ms_config_get_json(const char* metalsharp_home);
char* ms_config_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status);

#endif
