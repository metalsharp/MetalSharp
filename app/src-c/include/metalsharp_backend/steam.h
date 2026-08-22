#ifndef METALSHARP_BACKEND_STEAM_BASIC_H
#define METALSHARP_BACKEND_STEAM_BASIC_H

#include <stddef.h>

char* ms_steam_api_key_json(const char* metalsharp_home);
char* ms_steam_save_api_key_json(const char* metalsharp_home, const unsigned char* body, size_t body_length,
                                 int* status);
char* ms_steam_is_running_json(const char* metalsharp_home);
char* ms_steam_bridge_status_json(const char* metalsharp_home);
char* ms_steam_watch_json(const char* metalsharp_home);
char* ms_steam_status_json(const char* metalsharp_home);
char* ms_steam_library_json(const char* metalsharp_home);

#endif
