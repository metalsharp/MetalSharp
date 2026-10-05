#ifndef METALSHARP_BACKEND_STEAMCMD_H
#define METALSHARP_BACKEND_STEAMCMD_H
#include <stddef.h>

/* Owned-but-uninstalled Steam games are installed with Valve's steamcmd
 * (Windows depots) straight into one of the Wine Steam client's libraries. */
char* ms_steamcmd_status_json(const char* home);
char* ms_steamcmd_libraries_json(const char* home);
char* ms_steamcmd_login_json(const char* home, const unsigned char* body, size_t length);
char* ms_steamcmd_login_code_json(const unsigned char* body, size_t length);
char* ms_steamcmd_login_cancel_json(void);
char* ms_steamcmd_install_json(const char* home, const unsigned char* body, size_t length);
char* ms_steamcmd_installs_json(void);
char* ms_steamcmd_cancel_json(const unsigned char* body, size_t length);
char* ms_steamcmd_stop_all_json(void);

#endif
