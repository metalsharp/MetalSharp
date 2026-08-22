#ifndef METALSHARP_BACKEND_KERNEL_APC_H
#define METALSHARP_BACKEND_KERNEL_APC_H
#include <stddef.h>
char* ms_kernel_apc_queue(const char*, size_t);
char* ms_kernel_apc_test_alert(const char*, size_t);
char* ms_kernel_apc_wait_alertable(const char*, size_t);
char* ms_kernel_apc_allocate_trampoline(const char*, size_t);
char* ms_kernel_apc_suspend_thread(const char*, size_t);
char* ms_kernel_apc_get_context(const char*, size_t);
char* ms_kernel_apc_set_context(const char*, size_t);
char* ms_kernel_apc_inject(const char*, size_t);
char* ms_kernel_apc_queue_status(const char*, size_t);
char* ms_kernel_apc_trampoline_status(const char*, size_t);
#endif
