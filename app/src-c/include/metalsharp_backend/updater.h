#ifndef METALSHARP_BACKEND_UPDATER_BASIC_H
#define METALSHARP_BACKEND_UPDATER_BASIC_H

#include <stddef.h>

char* ms_update_check_json(void);
char* ms_update_progress_json(const char* metalsharp_home);
char* ms_update_cleanup_json(const char* metalsharp_home);
char* ms_update_dmg_path_json(const char* metalsharp_home);
char* ms_update_start_json(const char* metalsharp_home);

#endif
