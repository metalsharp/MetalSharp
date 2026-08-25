#ifndef METALSHARP_BACKEND_SHARPEMU_H
#define METALSHARP_BACKEND_SHARPEMU_H

#include <stddef.h>

char* ms_sharpemu_status_json(const char* home);
char* ms_sharpemu_games_json(const char* home);
char* ms_sharpemu_sessions_json(const char* home);
char* ms_sharpemu_update_json(const char* home, const char* action);
char* ms_sharpemu_action_json(const char* home, const char* action, const unsigned char* body, size_t length);
char* ms_sharpemu_cover_path(const char* home, const char* id);

#endif
