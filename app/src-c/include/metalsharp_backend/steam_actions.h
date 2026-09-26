#ifndef METALSHARP_BACKEND_STEAM_ACTIONS_H
#define METALSHARP_BACKEND_STEAM_ACTIONS_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
bool ms_steam_process_running(const char*);
pid_t ms_steam_odyssey_activity_pid(const char*);
bool ms_steam_stop_odyssey_processes(const char*);
void ms_steam_cancel_background_tasks(void);
/* Apply a registered graphics route to a child process environment. */
void ms_steam_apply_graphics_route(const char* home, const char* pipeline);
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
/* Remove only byte-matched MetalSharp route DLLs before staging a new route. */
void ms_steam_cleanup_route_dlls(const char*, const char*, const char*, const char*);
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
