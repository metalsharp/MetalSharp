#ifndef METALSHARP_BACKEND_ROCKSTAR_H
#define METALSHARP_BACKEND_ROCKSTAR_H

#include <stddef.h>
#include <sys/types.h>

char* ms_rockstar_status_json(const char* home);
char* ms_rockstar_library_json(const char* home);
char* ms_rockstar_launch_json(const char* home, int* status);
char* ms_rockstar_stop_json(const char* home, int* status);
char* ms_rockstar_launch_game_json(const char* home, const char* body, size_t body_length, int* status);
char* ms_rockstar_save_pipeline_json(const char* home, const char* body, size_t body_length, int* status);
char* ms_rockstar_save_executable_json(const char* home, const char* body, size_t body_length, int* status);
void ms_rockstar_register_running_games(const char* home);

#endif
