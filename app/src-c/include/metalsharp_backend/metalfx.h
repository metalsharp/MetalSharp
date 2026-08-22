#ifndef METALSHARP_BACKEND_METALFX_H
#define METALSHARP_BACKEND_METALFX_H

#include <stddef.h>

char* ms_metalfx_get_json(const char* metalsharp_home);
char* ms_metalfx_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length);

#endif
