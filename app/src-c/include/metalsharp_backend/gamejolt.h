#ifndef METALSHARP_BACKEND_GAMEJOLT_H
#define METALSHARP_BACKEND_GAMEJOLT_H
#include <stddef.h>
#include <sys/types.h>

char* ms_gamejolt_json(const char* home);
char* ms_gamejolt_storage_json(const char* home);
char* ms_gamejolt_set_name_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_pid_status_json(const unsigned char* body, size_t length);
char* ms_gamejolt_set_engine_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_uninstall_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_set_storage_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_cover_path(const char* home, const char* id);
char* ms_gamejolt_launch_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_running_json(void);
char* ms_gamejolt_stop_json(const unsigned char* body, size_t length, int* status);
char* ms_gamejolt_stop_all_json(int* status);
void ms_gamejolt_register_game_process(const char* home, const char* id, pid_t pid, const char* executable);

#endif
