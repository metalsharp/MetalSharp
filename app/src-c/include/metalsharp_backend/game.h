#ifndef METALSHARP_BACKEND_GAME_BASIC_H
#define METALSHARP_BACKEND_GAME_BASIC_H
#include <stddef.h>
char* ms_game_resolve_json(const char*, const unsigned char*, size_t, int*);
char* ms_game_dual_json(const char*, const char*, int*);
char* ms_goldberg_json(const char*, const char*, int*);
char* ms_goldberg_toggle_json(const char*, const unsigned char*, size_t, int*);
#endif
