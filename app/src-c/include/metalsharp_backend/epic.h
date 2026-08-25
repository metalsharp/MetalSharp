#ifndef METALSHARP_BACKEND_EPIC_H
#define METALSHARP_BACKEND_EPIC_H

#include <stddef.h>

char* ms_epic_status_json(const char* home);
void ms_epic_sync_on_startup(const char* home);
char* ms_epic_install_tool_json(const char* home);
char* ms_epic_auth_json(const char* home, const unsigned char* body, size_t body_length);
char* ms_epic_logout_json(const char* home);
char* ms_epic_games_json(const char* home, int force_refresh);
char* ms_epic_install_json(const char* home, const unsigned char* body, size_t body_length);
char* ms_epic_progress_json(const char* home, const unsigned char* body, size_t body_length);
char* ms_epic_cancel_json(const char* home, const unsigned char* body, size_t body_length);
char* ms_epic_initialize_json(const char* home, const unsigned char* body, size_t body_length);
char* ms_epic_launch_json(const char* home, const unsigned char* body, size_t body_length);
char* ms_epic_stop_json(const char* home, const unsigned char* body, size_t body_length);
char* ms_epic_stop_all_json(const char* home);
char* ms_epic_uninstall_json(const char* home, const unsigned char* body, size_t body_length);

#endif
