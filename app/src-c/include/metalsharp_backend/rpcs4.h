#ifndef METALSHARP_BACKEND_RPCS4_H
#define METALSHARP_BACKEND_RPCS4_H

#include <stddef.h>

char* ms_rpcs4_status_json(const char* home);
char* ms_rpcs4_games_json(const char* home);
char* ms_rpcs4_action_json(const char* home, const char* action, const unsigned char* body, size_t length);

#endif
