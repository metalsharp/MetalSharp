#ifndef METALSHARP_BACKEND_KERNEL_DRIVER_H
#define METALSHARP_BACKEND_KERNEL_DRIVER_H
#include <stddef.h>
char* ms_driver_load(const char*, size_t);
char* ms_driver_unload(const char*, size_t);
char* ms_driver_list(const char*, size_t);
char* ms_driver_create_device(const char*, size_t);
char* ms_driver_list_devices(const char*, size_t);
char* ms_driver_dispatch_irp(const char*, size_t);
char* ms_driver_list_irps(const char*, size_t);
char* ms_driver_register_ioctl(const char*, size_t);
char* ms_driver_decode_ioctl(const char*, size_t);
char* ms_driver_list_ioctls(const char*, size_t);
char* ms_driver_type_survey(const char*, size_t);
char* ms_driver_extension_template(const char*, size_t);
char* ms_driver_seed_demo(const char*, size_t);
#endif
