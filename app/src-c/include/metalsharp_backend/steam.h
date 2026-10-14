#ifndef METALSHARP_BACKEND_STEAM_BASIC_H
#define METALSHARP_BACKEND_STEAM_BASIC_H

#include <stdbool.h>
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
char* ms_steam_native_app_path(const char* metalsharp_home, unsigned appid);
/* Extract a Windows executable's embedded icon into MetalSharp's local artwork cache. */
char* ms_steam_extract_executable_icon(const char* metalsharp_home, const char* executable, unsigned cache_key);
/* Infer a graphics pipeline from the Direct3D DLLs present in a game install. */
const char* ms_steam_detect_graphics_pipeline(const char* game_dir);
/* Resolve a raw libraryfolders.vdf "path" value to a host path. Wine drive paths such as
   Z:\\Volumes\\SSD\\SteamLibrary resolve through the dosdevices of the prefix owning steamapps. */
char* ms_steam_library_host_path(const char* steamapps, const char* value, size_t length);

/* A non-Steam game the user added to Wine Steam (userdata/<id>/config/shortcuts.vdf).
 * Steam gives shortcuts appids with the high bit set, so they never collide with
 * store appids; Steam launches them as steam://rungameid/<appid << 32 | 0x02000000>. */
typedef struct {
    unsigned appid;
    char* name;
    char* executable; /* host path */
    char* start_dir;  /* host path */
    char* launch_options;
} ms_steam_shortcut;

static inline bool ms_steam_appid_is_shortcut(unsigned appid) {
    return (appid & 0x80000000u) != 0;
}
/* Parse a binary shortcuts.vdf; visible entries are appended to `*shortcuts`. */
size_t ms_steam_shortcuts_parse(const unsigned char* data, size_t length, const char* steamapps,
                                ms_steam_shortcut** shortcuts, size_t* count);
/* Every visible shortcut of every Wine Steam account. */
size_t ms_steam_shortcuts_load(const char* metalsharp_home, ms_steam_shortcut** shortcuts);
void ms_steam_shortcuts_free(ms_steam_shortcut* shortcuts, size_t count);
/* The shortcut's executable (host path), or NULL when `appid` is not a shortcut. */
char* ms_steam_shortcut_executable(const char* metalsharp_home, unsigned appid);

#endif
