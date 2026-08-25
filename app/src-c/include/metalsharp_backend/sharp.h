#ifndef METALSHARP_BACKEND_SHARP_BASIC_H
#define METALSHARP_BACKEND_SHARP_BASIC_H
#include <stddef.h>
char* ms_sharp_library_json(const char*);
char* ms_sharp_action_json(const char*, const unsigned char*, size_t, const char*);
char* ms_sharp_launcher_install_json(const char*, const unsigned char*, size_t);
char* ms_sharp_launcher_status_json(const char*);
char* ms_sharp_launcher_launch_json(const char*, const unsigned char*, size_t);
char* ms_sharp_cover_path(const char*, const char*);
#endif
