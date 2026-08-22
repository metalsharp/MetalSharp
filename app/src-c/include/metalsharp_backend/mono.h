#ifndef METALSHARP_BACKEND_MONO_BASIC_H
#define METALSHARP_BACKEND_MONO_BASIC_H
#include <stddef.h>
char* ms_mono_status_json(const char*, const char*);
char* ms_mono_install_json(const char*, const char*, size_t);
char* ms_mono_reset_json(const char*, const char*, size_t);
#endif
