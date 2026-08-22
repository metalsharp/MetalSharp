#ifndef METALSHARP_BACKEND_KERNEL_HANDLES_H
#define METALSHARP_BACKEND_KERNEL_HANDLES_H

#include <stddef.h>

/* Stateful virtual NT handle-table service used by the kernel-translation
 * routes. Every returned string is owned by the caller. */
char* ms_kernel_handle_create(const char* body, size_t length);
char* ms_kernel_handle_close(const char* body, size_t length);
char* ms_kernel_handle_duplicate(const char* body, size_t length);
char* ms_kernel_handle_query(const char* body, size_t length);
char* ms_kernel_handle_enumerate(const char* body, size_t length);
char* ms_kernel_handle_system_info(const char* body, size_t length);
char* ms_kernel_handle_table_status(const char* body, size_t length);
char* ms_kernel_handle_seed_demo(const char* body, size_t length);

#endif
