#ifndef METALSHARP_BACKEND_STEAM_ACTIONS_H
#define METALSHARP_BACKEND_STEAM_ACTIONS_H
#include <stddef.h>
char* ms_steam_launch_json(const char*, int*);
char* ms_steam_stop_json(const char*, int*);
char* ms_steam_mac_launch_json(const char*, int*);
char* ms_steam_mac_install_json(int*);
char* ms_steam_mac_stop_json(int*);
char* ms_steam_install_json(const char*, int*);
char* ms_steam_install_game_json(const char*, const char*, size_t, int*);
char* ms_steam_uninstall_game_json(const char*, const char*, size_t, int*);
char* ms_steam_launch_game_json(const char*, const char*, size_t, int*);
char* ms_steam_launch_auto_json(const char*, const char*, size_t, int*);
char* ms_steam_launch_d3dmetal_json(const char*, unsigned, const char*, const char*, int*);
char* ms_steam_prepare_bottle_route_json(const char*, const char*);
/* Canonical MTSP recipe/prepare/doctor inspection.  mode: 0 prepare, 1 recipe, 2 doctor. */
char* ms_steam_mtsp_inspect_json(const char*, const unsigned char*, size_t, int*, int);
char* ms_steam_launch_offline_json(const char*, const char*, size_t, int*);
char* ms_steam_mac_launch_game_json(const char*, const char*, size_t, int*);
char* ms_steam_view_game_json(const char*, const char*, size_t, int*);
char* ms_steam_stop_targets_json(const char*, int*);
char* ms_steam_misc_json(const char*, const unsigned char*, size_t, int*);
#endif
