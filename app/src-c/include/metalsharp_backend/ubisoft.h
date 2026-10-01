#ifndef METALSHARP_BACKEND_UBISOFT_H
#define METALSHARP_BACKEND_UBISOFT_H

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

#endif
