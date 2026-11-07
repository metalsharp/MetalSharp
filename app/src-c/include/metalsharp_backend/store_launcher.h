#ifndef METALSHARP_BACKEND_STORE_LAUNCHER_H
#define METALSHARP_BACKEND_STORE_LAUNCHER_H

/* What the Rockstar Games Launcher (rockstar.c) and Battle.net (battlenet.c) integrations share,
 * modeled on ubisoft.c: an install state file, the installer download, Wine processes started into
 * a prefix on a graphics route, the prefix registry's installed games, and their library JSON,
 * per-game route (bottles/<source>_<appid>/bottle.json) and executable choice. */

#include "metalsharp_backend/json_writer.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    char id[129];           /* the store's id: [A-Za-z0-9_.-] (a game executable override's file name) */
    char name[512];         /* display name */
    char path[PATH_MAX];    /* the install folder on the host */
    char windows_path[PATH_MAX]; /* the install folder as the registry has it (C:\...) */
    char uid[129];          /* Battle.net's product uid (--uid=<uid>), or "" */
} ms_store_game;

typedef struct ms_store_provider {
    const char* source;      /* "rockstar": the library's source, bottle prefix and override provider */
    const char* id_key;      /* "rockstar_id" */
    const char* label;       /* "Rockstar", for messages */
    const char* default_pipeline;
    unsigned (*appid)(const char* id);
    /* The executable the library shows and a launch starts (malloc'd), when no override is saved. */
    char* (*find_executable)(const ms_store_game* game);
    /* The launcher's own card art for an installed game (malloc'd host path), or NULL. Optional. */
    char* (*card_art)(const char* home, const ms_store_game* game);
} ms_store_provider;

/* A Wine process MetalSharp started: its pid, process group and command line (Windows path). */
typedef bool (*ms_store_process_match)(pid_t pid, pid_t group, const char* command, void* context);
/* Sets a child's environment before it runs Wine (after WINEPREFIX and the launch preferences). */
typedef void (*ms_store_apply_env)(const char* home, const void* context);

char* ms_store_path_join(const char* a, const char* b);
bool ms_store_mkdir_p(const char* path);
bool ms_store_is_file(const char* path);
bool ms_store_is_dir(const char* path);
char* ms_store_read_text(const char* path, size_t max_size);
bool ms_store_contains_ci(const char* text, const char* needle);
char* ms_store_error_json(const char* message);

/* <home>/runtime/wine/bin/metalsharp-wine (or wine) and wineserver, when present. */
char* ms_store_wine(const char* home);
char* ms_store_wineserver(const char* home);

/* <home>/<cache_dir>/state.json: {"state","error","updated_at"}. */
void ms_store_write_state(const char* home, const char* cache_dir, const char* state, const char* error);
/* GET /<L>/status. */
char* ms_store_status_json(const char* home, const char* cache_dir, bool installed, bool running, bool worker_active,
                           const char* prefix);
/* Downloads an installer over HTTPS only, keeping it when curl succeeds and it is 1 MB to 512 MB. */
bool ms_store_download(const char* url, const char* destination);

/* Runs Wine in prefix (argv[0] is wine). wait: waits and returns its exit status in *status;
 * otherwise starts it in its own process group and returns its pid in *pid. */
bool ms_store_run_wine(const char* home, const char* prefix, const char* cwd, char* const argv[],
                       ms_store_apply_env apply_env, const void* env_context, bool wait, int* status, pid_t* pid);
/* Prefix init as ubisoft.c's: MetalSharp Wine converts it (fex), wineboot -u builds it once (regular),
 * then launcher's library location (library_locations.h) gets its drive letter, or, with none
 * saved, a writable external volume becomes Y:. */
bool ms_store_initialize_prefix(const char* home, const char* prefix, const char* launcher, const char* label,
                                char* error, size_t error_size);
/* Y: -> volume, keeping a user's existing working Y: mapping. */
bool ms_store_ensure_drive_mapping(const char* prefix, const char* volume);
/* wineserver -k on the prefix; a prefix with no server counts as stopped. */
bool ms_store_stop_prefix(const char* home, const char* prefix);

/* Calls match for each Wine process of this MetalSharp's Wine; with signal_number signals each match.
 * Returns the number of matches and the first in *first. *failed when ps could not run. */
size_t ms_store_wine_processes(const char* home, ms_store_process_match match, void* context, int signal_number,
                               pid_t* first, bool* failed);
bool ms_store_process_cwd_within(pid_t pid, const char* root);
/* Whether the command line runs executable (its file name, after a path separator or at the start). */
bool ms_store_command_runs(const char* command, const char* executable);
/* Whether text holds needle, case and path separators ('/', '\\') aside. */
bool ms_store_contains_path_ci(const char* text, const char* needle);
/* Whether the command line holds the Windows folder (C:\Games\Title), separators and case aside. */
bool ms_store_command_in_windows_dir(const char* command, const char* windows_dir);

/* The registry value text after `"Name"=` (a quoted string), unescaped; NULL for other types. */
char* ms_store_registry_string(const char* value);
/* `"Name"=...` -> whether line's value name is name. */
bool ms_store_registry_value_is(const char* line, const char* name);
/* A drive path (C:\..., Y:\...) in prefix, on the host: drive_c, or the dosdevices link. */
char* ms_store_wine_path_to_host(const char* prefix, const char* value);
/* A folder in a Steam library (a steamapps folder on its path). */
bool ms_store_path_in_steam_library(const char* windows_path, const char* host_path);
/* text as an id: lower case letters and digits, other runs as one '-'. */
bool ms_store_slug(const char* text, char* out, size_t out_size);
bool ms_store_valid_id(const char* id);
unsigned ms_store_fnv1a(const char* text);

/* The largest .exe under path (to max_depth), skipping installers, redistributables, crash
 * reporters and what ignore_name also skips. */
void ms_store_largest_executable(const char* path, unsigned depth, unsigned max_depth,
                                 bool (*ignore_name)(const char* name), char** best, off_t* best_size);
/* The installed-game executable: the saved override (1), or provider->find_executable (0); -1 for
 * an override that is no longer valid. */
int ms_store_game_executable(const char* home, const ms_store_provider* provider, const ms_store_game* game,
                             char** executable);

/* A registry section's values the stores read. key is unescaped (single backslashes). */
typedef struct {
    char key[1024];
    char display_name[512];
    char publisher[256];
    char install_location[PATH_MAX];
    char install_folder[PATH_MAX];
    char uninstall_string[2048];
} ms_store_registry_section;
typedef void (*ms_store_registry_visit)(const ms_store_registry_section* section, void* context);
/* Calls visit for each section of a Wine registry file (system.reg). */
bool ms_store_registry_sections(const char* registry_path, ms_store_registry_visit visit, void* context);
/* A game from an install folder the registry names: a folder in prefix, outside any Steam library,
 * with a Windows executable (to max_depth). Its id is the folder name as a slug. */
bool ms_store_game_from_folder(const char* prefix, const char* windows_folder, const char* name, unsigned max_depth,
                               ms_store_game* game);
/* Appends game (a growing array of at most 1024) unless its folder is listed already. */
bool ms_store_add_game(ms_store_game** games, size_t* count, size_t* capacity, const ms_store_game* game);

const char* ms_store_canonical_pipeline(const char* pipeline);
/* A store game's available_pipelines: the active runtime's lanes, as a Steam game is offered them */
void ms_store_write_pipeline_options(ms_json_writer* writer, const char* default_pipeline);
/* bottles/<source>_<appid>/bottle.json's preferred_pipeline, or the provider's default. */
char* ms_store_pipeline_for_game(const char* home, const ms_store_provider* provider, unsigned appid);
void ms_store_write_game_json(ms_json_writer* writer, const char* home, const ms_store_provider* provider,
                              const ms_store_game* game);
char* ms_store_library_json(const char* home, const ms_store_provider* provider, const ms_store_game* games,
                            size_t count);
/* The request's <id_key> (or its appid) among games. */
const ms_store_game* ms_store_request_game(const ms_store_provider* provider, const ms_store_game* games,
                                           size_t count, const char* body, size_t body_length, char** pipeline);
char* ms_store_save_pipeline_json(const char* home, const ms_store_provider* provider, const ms_store_game* games,
                                  size_t count, const char* body, size_t body_length, int* status);
char* ms_store_save_executable_json(const char* home, const ms_store_provider* provider, const ms_store_game* games,
                                    size_t count, const char* body, size_t body_length, int* status);

#endif
