#ifndef METALSHARP_BACKEND_PROCESS_BASIC_H
#define METALSHARP_BACKEND_PROCESS_BASIC_H
#include <stddef.h>
char* ms_process_launch_json(const char*, const char*, size_t, int*);
char* ms_process_launch_auto_json(const char*, const char*, size_t, int*);
char* ms_process_running_json(void);
char* ms_process_kill_json(const char*, size_t, int*);
char* ms_process_force_quit_json(int*);
char* ms_process_force_kill_json(const char*, int*);
char* ms_process_prepare_json(const char*, const char*, size_t, int*);
#endif
