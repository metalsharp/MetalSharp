#ifndef METALSHARP_BACKEND_CACHE_H
#define METALSHARP_BACKEND_CACHE_H

#include <stddef.h>

char* ms_cache_size_json(const char* metalsharp_home);
char* ms_cache_clear_json(const char* metalsharp_home, const unsigned char* body, size_t body_length);

#endif
