#ifndef METALSHARP_BACKEND_UBISOFT_H
#define METALSHARP_BACKEND_UBISOFT_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

char* ms_ubisoft_status_json(const char* home);
char* ms_ubisoft_library_json(const char* home);
char* ms_ubisoft_launch_json(const char* home, int* status);
char* ms_ubisoft_stop_json(const char* home, int* status);
char* ms_ubisoft_launch_game_json(const char* home, const char* body, size_t body_length, int* status);
char* ms_ubisoft_save_pipeline_json(const char* home, const char* body, size_t body_length, int* status);
char* ms_ubisoft_save_executable_json(const char* home, const char* body, size_t body_length, int* status);
void ms_ubisoft_register_running_games(const char* home);

/* Ubisoft Connect's settings.yaml with misc.game_installation_path and installer_cache_path at dir (a Windows
 * folder, "Y:/MetalSharp Ubisoft Games/"), unless they are already on dir's drive; every other line is kept.
 * *changed says whether anything moved. NULL when out of memory or dir is not a Windows path. */
char* ms_ubisoft_settings_install_to(const char* text, const char* dir, bool* changed);
/* Points new Ubisoft installs at dir in each user's settings.yaml in prefix (users Connect has run as); returns how
 * many it changed. */
int ms_ubisoft_settings_point_installs_at(const char* prefix, const char* dir);

#endif
