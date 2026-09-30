#ifndef METALSHARP_BACKEND_STEAM_ACTIONS_H
#define METALSHARP_BACKEND_STEAM_ACTIONS_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
bool ms_steam_process_running(const char*);
pid_t ms_steam_odyssey_activity_pid(const char*);
bool ms_steam_stop_odyssey_processes(const char*);
pid_t ms_steam_eve_process_pid(const char*);
bool ms_steam_stop_eve_processes(const char*);
pid_t ms_steam_marvel_rivals_process_pid(const char*);
bool ms_steam_stop_marvel_rivals_processes(const char*);
pid_t ms_steam_baldurs_gate_3_process_pid(const char*);
bool ms_steam_stop_baldurs_gate_3_processes(const char*);
bool ms_steam_migrate_baldurs_gate_3_route_default(const char*);
void ms_steam_cancel_background_tasks(void);
/* Apply registered graphics and global launch preferences to a child process. */
void ms_steam_apply_graphics_route(const char* home, const char* pipeline);
void ms_steam_apply_launch_preferences(const char* home);
/* Apply configured controller shims without overwriting unowned game DLLs. */
void ms_steam_deploy_controller_input_shims(const char* home, const char* game_dir);
/* Return the app-managed Wine shim that opens requested Wine virtual desktops. */
char* ms_steam_wine_launch_wrapper_path(const char* home);
/* Resolve the actual Witcher 3 game executable, not a storefront launcher. */
char* ms_witcher3_game_executable(const char* game_dir);
char* ms_steam_launch_json(const char*, int*);
char* ms_steam_stop_json(const char*, int*);
/* Migration-time guarantee: verify/restore steamwebhelper wrapper, bridge shim and Goldberg payloads. */
bool ms_steam_wrappers_ensure(const char* home);
char* ms_steam_ensure_launch_ready_json(const char*, int*);
char* ms_steam_mac_launch_json(const char*, int*);
char* ms_steam_mac_install_json(int*);
char* ms_steam_mac_stop_json(int*);
char* ms_steam_install_json(const char*, int*);
char* ms_steam_install_game_json(const char*, const char*, size_t, int*);
char* ms_steam_uninstall_game_json(const char*, const char*, size_t, int*);
char* ms_steam_launch_game_json(const char*, const char*, size_t, int*);
char* ms_steam_launch_auto_json(const char*, const char*, size_t, int*);
char* ms_steam_launch_external_json(const char*, const char*, size_t, int*);
char* ms_steam_launch_d3dmetal_json(const char*, unsigned, const char*, const char*, int*);
/* App-aware executable selection for D3DMetal bottles (for example Unreal
 * launchers versus their actual Win64 shipping executable). */
char* ms_steam_d3dmetal_game_executable(const char*, unsigned);
char* ms_steam_d3dmetal_game_local_executable(const char*, unsigned);
/* Remove only byte-matched MetalSharp route DLLs before staging a new route. */
void ms_steam_cleanup_route_dlls(const char*, const char*, const char*, const char*);
/* Stage the selected route beside an arbitrary installed Windows game's executable. */
bool ms_steam_stage_route_for_executable(const char*, const char*, const char*, const char*);
char* ms_steam_prepare_bottle_route_json(const char*, const char*);
/* Create the default manifest for a Steam app when a route is saved before launch. */
bool ms_steam_ensure_bottle_manifest(const char*, unsigned, const char*);
/* Canonical MTSP recipe/prepare/doctor inspection.  mode: 0 prepare, 1 recipe, 2 doctor. */
char* ms_steam_mtsp_inspect_json(const char*, const unsigned char*, size_t, int*, int);
char* ms_steam_launch_offline_json(const char*, const char*, size_t, int*);
char* ms_steam_mac_launch_game_json(const char*, const char*, size_t, int*);
char* ms_steam_view_game_json(const char*, const char*, size_t, int*);
char* ms_steam_stop_targets_json(const char*, int*);
char* ms_steam_misc_json(const char*, const unsigned char*, size_t, int*);
#endif
