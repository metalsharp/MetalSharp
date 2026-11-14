#ifndef METALSHARP_BACKEND_GAME_REGISTRY_H
#define METALSHARP_BACKEND_GAME_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

/* Registry values a game needs in its prefix, whoever starts it: Steam, the game's own launcher, MetalSharp
 * (game_registry.c). */

#define MS_GAME_REGISTRY_MARKER_DIR ".metalsharp-game-registry" /* in the prefix: one file per game, its revision */

typedef struct {
    const char* key;   /* e.g. HKCU\Software\Wine\AppDefaults\acs.exe\DllOverrides */
    const char* name;  /* the value's name */
    const char* value; /* REG_SZ */
} ms_game_registry_value;

/* appid's values (count in *count), or NULL when it needs none. */
const ms_game_registry_value* ms_game_registry_values(unsigned appid, size_t* count);

/* Before a launch of appid: writes its values into prefix with home's runtime Wine (home/runtime/wine/bin/wine), once
 * per prefix and revision of them. Returns whether the prefix has them (true for a game that needs none). */
bool ms_game_registry_apply(const char* home, const char* prefix, unsigned appid);

#endif
