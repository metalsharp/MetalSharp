#ifndef METALSHARP_BACKEND_SHADPS4_H
#define METALSHARP_BACKEND_SHADPS4_H

#include <stddef.h>

char* ms_shadps4_status_json(const char* home);
char* ms_shadps4_games_json(const char* home);
char* ms_shadps4_update_json(const char* home, const char* action);
char* ms_shadps4_action_json(const char* home, const char* action, const unsigned char* body, size_t length);
char* ms_shadps4_cover_path(const char* home, const char* id);

#endif
