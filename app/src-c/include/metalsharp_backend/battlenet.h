#ifndef METALSHARP_BACKEND_BATTLENET_H
#define METALSHARP_BACKEND_BATTLENET_H

#include <stddef.h>
#include <sys/types.h>

char* ms_battlenet_status_json(const char* home);
char* ms_battlenet_library_json(const char* home);
char* ms_battlenet_launch_json(const char* home, int* status);
char* ms_battlenet_stop_json(const char* home, int* status);
char* ms_battlenet_launch_game_json(const char* home, const char* body, size_t body_length, int* status);
char* ms_battlenet_save_pipeline_json(const char* home, const char* body, size_t body_length, int* status);
char* ms_battlenet_save_executable_json(const char* home, const char* body, size_t body_length, int* status);
void ms_battlenet_register_running_games(const char* home);

#endif
