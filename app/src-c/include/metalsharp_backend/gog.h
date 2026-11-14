#ifndef METALSHARP_BACKEND_GOG_BASIC_H
#define METALSHARP_BACKEND_GOG_BASIC_H
#include <stdbool.h>
#include <stddef.h>
char* ms_gog_status_json(const char*);
char* ms_gog_games_json(const char*);
char* ms_gog_action_json(const char*, const char*, const unsigned char*, size_t);
/* Whether the GOG game's last launch (its gogdl process, or group on the fex build) is still running. */
bool ms_gog_game_running(const char* home, const char* product_id);
#endif
