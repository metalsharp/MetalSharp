#ifndef METALSHARP_BACKEND_GOG_BASIC_H
#define METALSHARP_BACKEND_GOG_BASIC_H
#include <stddef.h>
char* ms_gog_status_json(const char*);
char* ms_gog_games_json(const char*);
char* ms_gog_action_json(const char*, const char*, const unsigned char*, size_t);
#endif
