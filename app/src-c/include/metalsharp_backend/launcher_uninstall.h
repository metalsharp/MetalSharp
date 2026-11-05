#ifndef METALSHARP_BACKEND_LAUNCHER_UNINSTALL_H
#define METALSHARP_BACKEND_LAUNCHER_UNINSTALL_H

/* POST /launchers/uninstall {"launcher":"ubisoft"|"battlenet"|"rockstar"|"gog"|"epic","delete_games":bool}
 * -> 200 {"ok":true,"removed":[paths...],"kept_games_folder":"<path>"|null}
 *    400 a bad body or launcher
 *    409 {"ok":false,"error"} the launcher or one of its games could not be stopped, an install is
 *        running, or delete_games asks for a games folder that may not be deleted (nothing removed)
 *    500 {"ok":false,"error","path","removed"} a removal failed (path names it)
 *
 * Stops the launcher, then removes what MetalSharp keeps for it inside the MetalSharp home: Ubisoft
 * Connect and Battle.net their whole prefix, Rockstar only its own folders in the Steam prefix,
 * GOG its prefix bottle, Epic its game bottles and Legendary's data and tool; each with its cache,
 * per-game bottles (bottles/<launcher>_*) and saved executable choices. With delete_games, the
 * launcher's "MetalSharp <Name> Games" folder (library_locations.h) goes too and its saved location
 * is forgotten; without, the folder and its saved location stay. */

#include <stddef.h>

char* ms_launcher_uninstall_json(const char* home, const char* body, size_t body_length, int* status);

#endif
