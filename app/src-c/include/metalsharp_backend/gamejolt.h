#ifndef METALSHARP_BACKEND_GAMEJOLT_H
#define METALSHARP_BACKEND_GAMEJOLT_H
#include <stddef.h>

char* ms_gamejolt_json(const char* home);
char* ms_gamejolt_storage_json(const char* home);
char* ms_gamejolt_set_name_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_pid_status_json(const unsigned char* body, size_t length);
char* ms_gamejolt_set_engine_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_uninstall_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_set_storage_json(const char* home, const unsigned char* body, size_t length);
char* ms_gamejolt_cover_path(const char* home, const char* id);
char* ms_gamejolt_launch_json(const char* home, const unsigned char* body, size_t length);

#endif
