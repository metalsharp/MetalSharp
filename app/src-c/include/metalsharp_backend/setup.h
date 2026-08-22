#ifndef METALSHARP_BACKEND_SETUP_H
#define METALSHARP_BACKEND_SETUP_H

#include <stdbool.h>
#include <stddef.h>

char* ms_setup_state_json(const char* metalsharp_home);
char* ms_setup_save_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status);
char* ms_setup_device_name_json(void);
char* ms_setup_dependencies_json(const char* metalsharp_home);
char* ms_setup_agility_versions_json(void);
char* ms_setup_install_progress_json(const char* metalsharp_home);
char* ms_setup_installing_json(void);
char* ms_setup_install_dependencies_json(const unsigned char*, size_t, int*);
char* ms_setup_install_all_json(const char*, int*);
char* ms_setup_install_vcpp_json(const char*, bool, int*);

#endif
