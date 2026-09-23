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
char* ms_steam_library_refresh_json(const char* metalsharp_home);
char* ms_steam_game_dir(const char* metalsharp_home, unsigned appid);
/* Resolve a raw libraryfolders.vdf "path" value to a host path. Wine drive paths such as
   Z:\\Volumes\\SSD\\SteamLibrary resolve through the dosdevices of the prefix owning steamapps. */
char* ms_steam_library_host_path(const char* steamapps, const char* value, size_t length);

#endif
