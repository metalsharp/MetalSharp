#ifndef METALSHARP_BACKEND_PROCESS_BASIC_H
#define METALSHARP_BACKEND_PROCESS_BASIC_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
char* ms_process_launch_json(const char*, const char*, size_t, int*);
char* ms_process_launch_auto_json(const char*, const char*, size_t, int*);
char* ms_process_running_json(const char*);
char* ms_process_kill_json(const char*, const char*, size_t, int*);
char* ms_process_force_quit_json(int*);
char* ms_process_force_kill_json(const char*, int*);
char* ms_process_prepare_json(const char*, const char*, size_t, int*);
void ms_process_register_game(unsigned, pid_t);
unsigned long ms_process_background_task_generation(void);
bool ms_process_background_task_cancelled(unsigned long);
bool ms_process_background_task_begin(unsigned long);
void ms_process_background_task_end(void);
bool ms_process_background_shutdown_requested(void);
void ms_process_cancel_background_tasks(void);
void ms_process_request_background_shutdown(void);
#endif
