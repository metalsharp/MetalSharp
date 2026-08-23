#ifndef METALSHARP_BACKEND_KERNEL_INTEGRITY_H
#define METALSHARP_BACKEND_KERNEL_INTEGRITY_H
#include <stddef.h>
char* ms_integrity_query_signing_level(const char*, size_t);
char* ms_integrity_query_process_signing(const char*, size_t);
char* ms_integrity_register_pe(const char*, size_t);
char* ms_integrity_register_macho(const char*, size_t);
char* ms_integrity_set_cached_level(const char*, size_t);
char* ms_integrity_list_modules(const char*, size_t);
char* ms_integrity_seed_demo(const char*, size_t);
#endif
