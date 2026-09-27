#ifndef METALSHARP_BACKEND_SHARP_BASIC_H
#define METALSHARP_BACKEND_SHARP_BASIC_H
#include <stddef.h>
char* ms_sharp_library_json(const char*);
char* ms_sharp_action_json(const char*, const unsigned char*, size_t, const char*);
char* ms_sharp_running_json(void);
char* ms_sharp_track_running_json(const char*, const unsigned char*, size_t);
char* ms_sharp_stop_json(const unsigned char*, size_t, int*);
char* ms_sharp_stop_all_json(int*);
char* ms_sharp_cover_path(const char*, const char*);
#endif
