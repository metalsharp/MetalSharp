#ifndef METALSHARP_BACKEND_PACKED_APP_H
#define METALSHARP_BACKEND_PACKED_APP_H

/* A game packed as its own app (/Applications/<Game>.app): a small Cocoa launcher
 * (tools/metalsharp-game-app) that starts MetalSharp's backend if it has to, launches the game
 * through /pack-app/launch and quits with it. The backend makes, finds and removes these bundles,
 * and only ever replaces or removes one whose Info.plist names the game (MetalSharpGameKind and
 * MetalSharpGameId); <home>/config/packed-apps.json records where each one is.
 *
 * Games are named by kind and id: steam (its appid), ubisoft, rockstar, battlenet (the store's
 * id), gog (its productId) and epic (its appName).
 *
 * $METALSHARP_PACKED_APPS_DIR replaces /Applications, and $METALSHARP_GAME_APP_LAUNCHER the
 * launcher binary, for tests. */

#include <stdbool.h>
#include <stddef.h>

/* POST /pack-app/create {kind,id,name,executablePath?,iconPath?}: the app's icon is the game
 * executable's own when one can be extracted, else iconPath's image (the card art), else none. */
char* ms_packed_app_create_json(const char* home, const char* body, size_t length, int* status);
/* GET /pack-app/status?kind=&id= (or POST {kind,id}): {"packed","path"}. */
char* ms_packed_app_status_json(const char* home, const char* query, const char* body, size_t length, int* status);
/* POST /pack-app/remove {kind,id}. */
char* ms_packed_app_remove_json(const char* home, const char* body, size_t length, int* status);
/* POST /pack-app/launch {kind,id}: the game's launcher first (Wine Steam for a Steam game), then
 * the game on its saved graphics route. Blocks as the launch routes do. */
char* ms_packed_app_launch_json(const char* home, const char* body, size_t length, int* status);
/* GET /pack-app/running?kind=&id= (or POST {kind,id}): {"running"}. */
char* ms_packed_app_running_json(const char* home, const char* query, const char* body, size_t length, int* status);
/* POST /pack-app/stop {kind,id}: the game, then its launcher (Wine Steam, Ubisoft Connect, ...). */
char* ms_packed_app_stop_json(const char* home, const char* body, size_t length, int* status);

/* What a launch and a stop of a game call: backend routes and their bodies. */
typedef struct {
    char kind[16];
    char id[129];
    unsigned appid;                  /* the library's appid: GET /game/running, POST /kill (0 for GOG, Epic) */
    bool start_steam;                /* Wine Steam is started first */
    const char* launch_route;        /* "/steam/launch-game", "/ubisoft/launch-game", ... */
    char* launch_body;               /* malloc'd JSON */
    const char* stop_route;          /* the game's stop: "/kill", "/sharp-library/gog/stop", ... */
    char* stop_body;                 /* malloc'd JSON */
    const char* launcher_stop_route; /* "/steam/stop", "/ubisoft/stop", ... or NULL */
} ms_packed_app_plan;

bool ms_packed_app_plan_for(const char* home, const char* kind, const char* id, ms_packed_app_plan* plan);
void ms_packed_app_plan_free(ms_packed_app_plan* plan);

#endif
