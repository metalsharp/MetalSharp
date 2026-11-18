#ifndef METALSHARP_BACKEND_GAME_TUNING_H
#define METALSHARP_BACKEND_GAME_TUNING_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A game's own fine-tuning (Game Settings -> Advanced), kept in
 * <home>/config/game-tuning/<provider>/<id>.json and keyed like the executable
 * overrides: steam/<appid>, ubisoft|rockstar|battlenet/<launcher id>,
 * gog/<productId>, epic/<appName>, windows/<Sharp app id>, gamejolt/<id>.
 *
 * - env: the user's variables, written to .metalsharp/launch.local.env in the
 *   game's folder and its executable's (MetalSharp Wine reads it after
 *   launch.env when a program there starts, however it starts), and set in
 *   the environment of a game MetalSharp starts itself.
 * - dll_overrides / windows_version: HKCU\Software\Wine\AppDefaults\<exe> in
 *   the game's prefix (DllOverrides and Version), for that executable only.
 * - cpu_cores: the processor count the game sees (the CPU topology shim);
 *   only where MetalSharp starts the game's process itself.
 * - low_priority: the game runs at a lower scheduling priority (nice 10), set
 *   where MetalSharp starts it and on its process once it is seen running.
 */
#define MS_GAME_TUNING_MAX_ENV  64
#define MS_GAME_TUNING_MAX_DLLS 64

typedef struct {
    char** env; /* NAME=value, or -NAME to remove a variable */
    size_t env_count;
    char* dll_overrides;      /* "name=n,b;name2=d": n native, b builtin, d disabled; NULL for none */
    char windows_version[12]; /* "" keeps the prefix's own */
    unsigned cpu_cores;       /* 0: every core */
    bool low_priority;
    signed char retina; /* -1: the Retina setting (Settings > Graphics); 0 off, 1 on. A game in a bottle of its own only:
                           Retina is the prefix's (RetinaMode and its DPI), so a shared prefix keeps the setting */
    bool zink;          /* OpenGL through Zink (SharpGL_Switch=1) instead of WineMetalGL */
    bool kosmickrisp;   /* Vulkan through KosmicKrisp (SharpVK_Switch=1) instead of MoltenVK; macOS 26 or later */
} ms_game_tuning;

void ms_game_tuning_init(ms_game_tuning* tuning);
void ms_game_tuning_free(ms_game_tuning* tuning);
bool ms_game_tuning_is_default(const ms_game_tuning* tuning);
bool ms_game_tuning_valid_key(const char* provider, const char* id);

/* What the user typed: NAME=value entries separated by spaces or lines ("quotes" keep spaces in a
 * value), -NAME to remove one. Rejects names the launch itself owns (WINEPREFIX, DYLD_*, ...). */
bool ms_game_tuning_parse_env(const char* text, char*** entries, size_t* count, char* error, size_t error_size);
/* WINEDLLOVERRIDES-style "d3dx9_43,d3dx9_42=n,b;xinput1_3=b"; canonical "name=n,b;..." out, NULL when empty. */
bool ms_game_tuning_parse_dll_overrides(const char* text, char** canonical, char* error, size_t error_size);
/* "" (or "default"), winxp, winxp64, vista, win7, win8, win81, win10, win11. */
bool ms_game_tuning_valid_windows_version(const char* version);
/* The env entries as one line, quoting values with spaces (parses back to the same entries). */
char* ms_game_tuning_env_text(const ms_game_tuning* tuning);

/* load: true with defaults when nothing is saved; false for an invalid key or an unreadable file. */
bool ms_game_tuning_load(const char* home, const char* provider, const char* id, ms_game_tuning* tuning);
/* Saves (or, for defaults, removes) the game's tuning. */
bool ms_game_tuning_save(const char* home, const char* provider, const char* id, const ms_game_tuning* tuning,
                         char* error, size_t error_size);

/* Whether the game has settings saved, or had some applied that a launch may need to remove: a cheap
 * check before a caller works out the game's executable just for ms_game_tuning_prepare_launch. */
bool ms_game_tuning_has_state(const char* home, const char* provider, const char* id);

/* Rewrites MetalSharp's block in <dir>/.metalsharp/launch.local.env (the user's own lines there stay);
 * removes the file when nothing is left in it. */
bool ms_game_tuning_write_local_env(const char* dir, const ms_game_tuning* tuning);
/* The .reg text that sets AppDefaults\<executable_name> to dll_overrides and windows_version, deleting
 * the previously applied DLL names no longer listed (and Version when windows_version is ""). */
char* ms_game_tuning_registry_text(const char* executable_name, const char* dll_overrides, const char* windows_version,
                                   const char* previous_dll_overrides, bool previous_version);

/* Before a game starts (any launcher): its launch.local.env in game_dir and the executable's folder,
 * its AppDefaults in prefix (imported only when they changed), the CPU shim built when a core count is
 * set, and the low-priority watch on its executable. */
void ms_game_tuning_prepare_launch(const char* home, const char* provider, const char* id, const char* prefix,
                                   const char* game_dir, const char* executable);
/* In the forked child that execs the game (or the launcher tool that starts it): the variables, the
 * CPU count and the priority. */
void ms_game_tuning_apply_child(const char* home, const char* provider, const char* id);

/* GET /game-tuning?provider=&id= and POST /game-tuning. */
char* ms_game_tuning_get_json(const char* home, const char* query, int* status);
char* ms_game_tuning_save_json(const char* home, const char* body, size_t length, int* status);

#endif
