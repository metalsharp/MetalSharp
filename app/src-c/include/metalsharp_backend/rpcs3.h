#ifndef METALSHARP_BACKEND_RPCS3_H
#define METALSHARP_BACKEND_RPCS3_H

#include <stddef.h>

char* ms_rpcs3_status_json(const char* home);
char* ms_rpcs3_games_json(const char* home);
char* ms_rpcs3_update_json(const char* home, const char* action);
char* ms_rpcs3_action_json(const char* home, const char* action, const unsigned char* body, size_t length);
char* ms_rpcs3_cover_path(const char* home, const char* id);

#endif
