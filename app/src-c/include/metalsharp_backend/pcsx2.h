#ifndef METALSHARP_BACKEND_PCSX2_H
#define METALSHARP_BACKEND_PCSX2_H

#include <stddef.h>

char* ms_pcsx2_status_json(const char* home);
char* ms_pcsx2_games_json(const char* home);
char* ms_pcsx2_update_json(const char* home, const char* action);
char* ms_pcsx2_action_json(const char* home, const char* action, const unsigned char* body, size_t length);
char* ms_pcsx2_cover_path(const char* home, const char* id);

#endif
