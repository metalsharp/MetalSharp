#ifndef METALSHARP_BACKEND_BOTTLE_ACTIONS_H
#define METALSHARP_BACKEND_BOTTLE_ACTIONS_H
#include <stddef.h>
void ms_bottle_poll(void);
char* ms_bottle_relaunch_installer_json(const char*, const unsigned char*, size_t, int*);
char* ms_bottle_action_json(const char*, const char*, const unsigned char*, size_t, int*);
#endif
