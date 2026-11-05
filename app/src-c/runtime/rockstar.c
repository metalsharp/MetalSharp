/* The Rockstar Games Launcher on its own: installed into the Steam prefix (prefix-steam, where
 * Steam also puts it on a Steam copy's first run), started without Steam, and the games it
 * installs. Steam copies of Rockstar titles keep their own path (steam_actions.c). */
#include "metalsharp_backend/rockstar.h"
#include "metalsharp_backend/library_locations.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/store_launcher.h"
#include "metalsharp_backend/wine_runtime.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ROCKSTAR_INSTALLER_URL "https://gamedownloads.rockstargames.com/public/installer/Rockstar-Games-Launcher.exe"
#define ROCKSTAR_CACHE_DIR     "cache/rockstar-launcher"
#define ROCKSTAR_PREFIX        "prefix-steam"
#define ROCKSTAR_LAUNCHER_DIR  "drive_c/Program Files/Rockstar Games/Launcher"
#define ROCKSTAR_PIPELINE      "d3dmetal"

/* A title's executables: what a launch starts (the launcher hands a Play*.exe off to itself), in
 * preference order, then the game's own client, which takes the route. */
static const char* const ROCKSTAR_LAUNCH_EXES[] = {"PlayGTAV.exe",  "GTA5_Enhanced.exe", "GTA5.exe",
                                                   "PlayRDR2.exe",  "RDR2.exe",          "GTAIV.exe",
                                                   "LANoire.exe",   "MaxPayne3.exe",     "Bully.exe",
                                                   "RDR.exe"};
static const char* const ROCKSTAR_CLIENT_EXES[] = {"GTA5_Enhanced.exe", "GTA5.exe",      "RDR2.exe", "GTAIV.exe",
                                                   "LANoire.exe",       "MaxPayne3.exe", "Bully.exe", "RDR.exe"};
/* The launcher's own processes in the prefix (Stop ends these and nothing else of Steam's). */
static const char* const ROCKSTAR_LAUNCHER_PROCESS_EXES[] = {
    "SocialClubHelper.exe", "RockstarService.exe", "RockstarSteamHelper.exe", "RockstarErrorHandler.exe",
    "LauncherPatcher.exe"};

static pthread_mutex_t rockstar_install_lock = PTHREAD_MUTEX_INITIALIZER;
static bool rockstar_install_active;

static unsigned rockstar_appid(const char* id) {
    /* Bit 30 without bit 31: apart from Steam's ids and Ubisoft's (bit 31) and Battle.net's (bit 29 only). */
    return 0x40000000u | (ms_store_fnv1a(id) & 0x3fffffffu);
}

static char* rockstar_prefix(const char* home) {
    return ms_store_path_join(home, ROCKSTAR_PREFIX);
}

static char* rockstar_launcher_path(const char* home) {
    char* prefix = rockstar_prefix(home);
    char* dir = prefix ? ms_store_path_join(prefix, ROCKSTAR_LAUNCHER_DIR) : NULL;
    char* launcher = dir ? ms_store_path_join(dir, "Launcher.exe") : NULL;
    free(prefix);
    free(dir);
    return launcher;
}

static char* rockstar_launcher(const char* home) {
    char* launcher = rockstar_launcher_path(home);
    if (launcher && ms_store_is_file(launcher))
        return launcher;
    free(launcher);
    return NULL;
}

static bool rockstar_file_in(const char* directory, const char* name, char** out) {
    char* candidate = ms_store_path_join(directory, name);
    if (candidate && ms_store_is_file(candidate)) {
        *out = candidate;
        return true;
    }
    free(candidate);
    return false;
}

static bool rockstar_ignored_executable(const char* name) {
    return ms_store_contains_ci(name, "launcher") || ms_store_contains_ci(name, "socialclub");
}

/* Known names first, else the largest top-level executable that is no installer or launcher. */
static char* rockstar_find_executable(const ms_store_game* game) {
    char* found = NULL;
    off_t size = 0;
    for (size_t i = 0; i < sizeof(ROCKSTAR_LAUNCH_EXES) / sizeof(ROCKSTAR_LAUNCH_EXES[0]); i++)
        if (rockstar_file_in(game->path, ROCKSTAR_LAUNCH_EXES[i], &found))
            return found;
    ms_store_largest_executable(game->path, 0, 0, rockstar_ignored_executable, &found, &size);
    return found;
}

/* The game's client beside the launch executable (GTA5.exe beside PlayGTAV.exe), which gets the route. */
static char* rockstar_client_executable(const ms_store_game* game, const char* launch_executable) {
    char* found = NULL;
    for (size_t i = 0; i < sizeof(ROCKSTAR_CLIENT_EXES) / sizeof(ROCKSTAR_CLIENT_EXES[0]); i++)
        if (rockstar_file_in(game->path, ROCKSTAR_CLIENT_EXES[i], &found))
            return found;
    return launch_executable ? strdup(launch_executable) : NULL;
}

static const ms_store_provider ROCKSTAR_PROVIDER = {
    "rockstar", "rockstar_id", "Rockstar", ROCKSTAR_PIPELINE, rockstar_appid, rockstar_find_executable, NULL};

/* Software\[Wow6432Node\]Rockstar Games\<Title>: the title, or NULL for another key. */
static const char* rockstar_title_key(const char* key) {
    static const char* const roots[] = {"Software\\Wow6432Node\\Rockstar Games\\", "Software\\Rockstar Games\\"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        size_t n = strlen(roots[i]);
        if (!strncasecmp(key, roots[i], n) && key[n] && !strchr(key + n, '\\'))
            return key + n;
    }
    return NULL;
}

/* The launcher and Social Club register themselves like titles. */
static bool rockstar_excluded_name(const char* name) {
    return !name || ms_store_contains_ci(name, "launcher") || ms_store_contains_ci(name, "social club") ||
           ms_store_contains_ci(name, "socialclub") || ms_store_contains_ci(name, "library service");
}

static bool rockstar_excluded_folder(const char* windows_folder) {
    const char* base = windows_folder ? windows_folder + strlen(windows_folder) : NULL;
    while (base && base > windows_folder && (base[-1] == '\\' || base[-1] == '/'))
        base--;
    const char* end = base;
    while (base && base > windows_folder && base[-1] != '\\' && base[-1] != '/')
        base--;
    if (!base)
        return true;
    size_t n = (size_t)(end - base);
    return (n == 8 && !strncasecmp(base, "Launcher", n)) || (n == 11 && !strncasecmp(base, "Social Club", n));
}

typedef struct {
    const char* prefix;
    ms_store_game* games;
    size_t count;
    size_t capacity;
} rockstar_scan;

static void rockstar_visit_section(const ms_store_registry_section* section, void* opaque) {
    rockstar_scan* scan = opaque;
    const char* title = rockstar_title_key(section->key);
    const char* name = NULL;
    const char* folder = NULL;
    ms_store_game game;
    if (title && section->install_folder[0]) {
        name = title;
        folder = section->install_folder;
    } else if (ms_store_contains_ci(section->key, "\\Uninstall\\") &&
               ms_store_contains_ci(section->publisher, "Rockstar Games") && section->install_location[0]) {
        name = section->display_name[0] ? section->display_name : NULL;
        folder = section->install_location;
    }
    if (!folder || (name && rockstar_excluded_name(name)) || rockstar_excluded_folder(folder))
        return;
    if (ms_store_game_from_folder(scan->prefix, folder, name, 2, &game))
        (void)ms_store_add_game(&scan->games, &scan->count, &scan->capacity, &game);
}

/* The games the Rockstar Games Launcher installed in the Steam prefix (malloc'd). */
static size_t rockstar_scan_games(const char* home, ms_store_game** games) {
    char* prefix = rockstar_prefix(home);
    char* registry = prefix ? ms_store_path_join(prefix, "system.reg") : NULL;
    rockstar_scan scan = {prefix, NULL, 0, 0};
    if (registry)
        (void)ms_store_registry_sections(registry, rockstar_visit_section, &scan);
    free(registry);
    free(prefix);
    *games = scan.games;
    return scan.count;
}

/* --- Processes ------------------------------------------------------------------------------- */

static bool rockstar_launcher_command(const char* command) {
    return ms_store_contains_path_ci(command, "Rockstar Games\\Launcher\\Launcher.exe");
}

static bool rockstar_match_launcher(pid_t pid, pid_t group, const char* command, void* context) {
    (void)pid;
    (void)group;
    (void)context;
    return rockstar_launcher_command(command);
}

typedef struct {
    const ms_store_game* games;
    size_t count;
} rockstar_process_set;

/* The launcher, its helpers, and games it installed (by their folder). Steam and its games are
 * never matched: a Steam copy lives in steamapps, and Steam's processes are not Rockstar's. */
static bool rockstar_match_owned(pid_t pid, pid_t group, const char* command, void* context) {
    const rockstar_process_set* set = context;
    (void)group;
    if (ms_store_contains_ci(command, "steam.exe") || ms_store_contains_ci(command, "steamwebhelper"))
        return false;
    if (ms_store_contains_path_ci(command, "Rockstar Games\\Launcher\\") ||
        ms_store_contains_path_ci(command, "Rockstar Games\\Social Club\\"))
        return true;
    for (size_t i = 0; i < sizeof(ROCKSTAR_LAUNCHER_PROCESS_EXES) / sizeof(ROCKSTAR_LAUNCHER_PROCESS_EXES[0]); i++)
        if (ms_store_command_runs(command, ROCKSTAR_LAUNCHER_PROCESS_EXES[i]))
            return true;
    for (size_t i = 0; set && i < set->count; i++)
        if (ms_store_command_in_windows_dir(command, set->games[i].windows_path) ||
            (ms_store_contains_ci(command, set->games[i].path) && ms_store_contains_ci(command, ".exe")) ||
            (ms_store_contains_ci(command, ".exe") && ms_store_process_cwd_within(pid, set->games[i].path)))
            return true;
    return false;
}

static pid_t rockstar_launcher_pid(const char* home) {
    pid_t pid = 0;
    (void)ms_store_wine_processes(home, rockstar_match_launcher, NULL, 0, &pid, NULL);
    return pid;
}

static char* rockstar_cache_file(const char* home, const char* name) {
    char* dir = ms_store_path_join(home, ROCKSTAR_CACHE_DIR);
    char* path = dir ? ms_store_path_join(dir, name) : NULL;
    free(dir);
    return path;
}

/* The route the running launcher (and so the games it starts) was given. */
static void rockstar_write_lane(const char* home, const char* lane) {
    char* dir = ms_store_path_join(home, ROCKSTAR_CACHE_DIR);
    char* path = rockstar_cache_file(home, "launcher-lane");
    FILE* file = dir && path && ms_store_mkdir_p(dir) ? fopen(path, "wb") : NULL;
    if (file) {
        fprintf(file, "%s\n", lane);
        fclose(file);
    }
    free(dir);
    free(path);
}

static char* rockstar_read_lane(const char* home) {
    char* path = rockstar_cache_file(home, "launcher-lane");
    char* text = path ? ms_store_read_text(path, 256) : NULL;
    if (text)
        text[strcspn(text, "\r\n")] = '\0';
    free(path);
    return text;
}

static void rockstar_clear_lane(const char* home) {
    char* path = rockstar_cache_file(home, "launcher-lane");
    if (path)
        (void)unlink(path);
    free(path);
}

/* The launcher's lane for a game on the x86_64 runtime: its route, and GTA V Enhanced's Agility
 * frontend on D3DMetal (the launcher's environment is the game's). MetalSharp Wine's launcher
 * always runs on Mullion, and its games take their route from launch.env. */
static void rockstar_lane_key(const char* pipeline, const char* client, char* out, size_t out_size) {
    const char* base = client ? strrchr(client, '/') : NULL;
    base = base ? base + 1 : client;
    if (ms_wine_is_fex())
        snprintf(out, out_size, "mullion");
    else
        snprintf(out, out_size, "%s%s", pipeline,
                 !strcmp(pipeline, "d3dmetal") && base && !strcasecmp(base, "GTA5_Enhanced.exe") ? "+agility" : "");
}

static bool rockstar_stop_processes(const char* home, const rockstar_process_set* set) {
    for (unsigned attempt = 0; attempt < 20; attempt++) {
        bool failed = false;
        size_t signaled = ms_store_wine_processes(home, rockstar_match_owned, (void*)set, SIGKILL, NULL, &failed);
        if (failed)
            return false;
        if (signaled == 0)
            return true;
        usleep(100000);
    }
    bool failed = false;
    return ms_store_wine_processes(home, rockstar_match_owned, (void*)set, 0, NULL, &failed) == 0 && !failed;
}

/* --- Starting the launcher and games --------------------------------------------------------- */

typedef struct {
    const char* pipeline;
    const char* client;
} rockstar_env;

static void rockstar_apply_env(const char* home, const void* opaque) {
    const rockstar_env* env = opaque;
    ms_steam_apply_rockstar_launcher_env(home, env->pipeline, env->client);
}

/* The lane the launcher runs on by itself: D3DMetal with WFDXCompat on the x86_64 runtime (as the
 * Steam copies start Launcher.exe), Mullion on MetalSharp Wine (its launcher policy adds WFDXCompat). */
static const char* rockstar_launcher_lane(void) {
    return ms_wine_is_fex() ? "dxmt" : "d3dmetal";
}

static bool rockstar_prepare_prefix(const char* home, char* error, size_t error_size) {
    char* prefix = rockstar_prefix(home);
    char* registry = prefix ? ms_store_path_join(prefix, "system.reg") : NULL;
    bool ok = ms_store_is_file(registry);
    if (!ok)
        snprintf(error, error_size,
                 "The Rockstar Games Launcher installs into the Steam prefix: set up Steam in MetalSharp first");
    /* The prefix MetalSharp Wine converted in setup: nothing to do there unless it is still x86_64. */
    else if (ms_wine_is_fex())
        ok = ms_wine_fex_convert_prefix(home, prefix, error, error_size);
    /* The folder chosen for Rockstar games becomes R: in the Steam prefix. */
    if (ok)
        (void)ms_library_location_apply(home, "rockstar");
    free(registry);
    free(prefix);
    return ok;
}

static bool rockstar_wait_for_launcher(const char* home, unsigned seconds) {
    for (unsigned i = 0; i < seconds * 2; i++) {
        if (rockstar_launcher_pid(home) > 0)
            return true;
        usleep(500000);
    }
    return false;
}

static bool rockstar_start_launcher(const char* home, const char* pipeline, const char* client) {
    char* launcher = rockstar_launcher(home);
    char* wine = ms_store_wine(home);
    char* prefix = rockstar_prefix(home);
    char* dir = launcher ? strdup(launcher) : NULL;
    rockstar_env env = {pipeline, client};
    char lane[64];
    pid_t pid = 0;
    bool ok = launcher && wine && prefix && dir;
    if (ok) {
        char* argv[] = {wine, launcher, NULL};
        *strrchr(dir, '/') = '\0';
        rockstar_lane_key(pipeline, client, lane, sizeof(lane));
        ok = ms_store_run_wine(home, prefix, dir, argv, rockstar_apply_env, &env, false, NULL, &pid);
        if (ok)
            rockstar_write_lane(home, lane);
    }
    free(launcher);
    free(wine);
    free(prefix);
    free(dir);
    return ok;
}

static void* rockstar_install_worker(void* opaque) {
    char* home = opaque;
    char error[256] = {0};
    char* dir = ms_store_path_join(home, ROCKSTAR_CACHE_DIR);
    char* installer = dir ? ms_store_path_join(dir, "Rockstar-Games-Launcher.exe") : NULL;
    char* wine = ms_store_wine(home);
    char* prefix = rockstar_prefix(home);
    bool ok = dir && installer && prefix && ms_store_mkdir_p(dir);
    if (ok && !wine) {
        ok = false;
        snprintf(error, sizeof(error), "MetalSharp Wine is not available to install the Rockstar Games Launcher");
    }
    if (ok)
        ok = rockstar_prepare_prefix(home, error, sizeof(error));
    if (!ok && !error[0])
        snprintf(error, sizeof(error), "Could not prepare the Rockstar Games Launcher installer");
    if (ok) {
        ms_store_write_state(home, ROCKSTAR_CACHE_DIR, "downloading", NULL);
        ok = ms_store_download(ROCKSTAR_INSTALLER_URL, installer);
        if (!ok)
            snprintf(error, sizeof(error), "Could not download the Rockstar Games Launcher installer from Rockstar");
    }
    if (ok) {
        /* The installer's own window; it can start the launcher when it finishes, which then keeps this
         * environment, so it gets the launcher's. Its exit status is not the result: the launcher is. */
        char* argv[] = {wine, installer, NULL};
        rockstar_env env = {rockstar_launcher_lane(), NULL};
        int status = 0;
        char lane[64];
        ms_store_write_state(home, ROCKSTAR_CACHE_DIR, "installing", NULL);
        ok = ms_store_run_wine(home, prefix, dir, argv, rockstar_apply_env, &env, true, &status, NULL);
        rockstar_lane_key(rockstar_launcher_lane(), NULL, lane, sizeof(lane));
        rockstar_write_lane(home, lane);
        if (!ok)
            snprintf(error, sizeof(error), "The Rockstar Games Launcher installer could not be started");
    }
    if (ok) {
        char* launcher = rockstar_launcher(home);
        if (!launcher) {
            ok = false;
            snprintf(error, sizeof(error),
                     "Installer finished, but the Rockstar Games Launcher was not found in the Steam prefix");
        }
        free(launcher);
    }
    if (ok && rockstar_launcher_pid(home) <= 0 && !rockstar_start_launcher(home, rockstar_launcher_lane(), NULL)) {
        ok = false;
        snprintf(error, sizeof(error), "The Rockstar Games Launcher installed, but could not be started");
    }
    ms_store_write_state(home, ROCKSTAR_CACHE_DIR, ok ? "ready" : "error", ok ? NULL : error);
    free(dir);
    free(installer);
    free(wine);
    free(prefix);
    pthread_mutex_lock(&rockstar_install_lock);
    rockstar_install_active = false;
    pthread_mutex_unlock(&rockstar_install_lock);
    free(home);
    return NULL;
}

static bool rockstar_start_install(const char* home) {
    pthread_t thread;
    char* copy;
    pthread_mutex_lock(&rockstar_install_lock);
    if (rockstar_install_active) {
        pthread_mutex_unlock(&rockstar_install_lock);
        return true;
    }
    rockstar_install_active = true;
    pthread_mutex_unlock(&rockstar_install_lock);
    copy = strdup(home);
    if (!copy || pthread_create(&thread, NULL, rockstar_install_worker, copy) != 0) {
        free(copy);
        pthread_mutex_lock(&rockstar_install_lock);
        rockstar_install_active = false;
        pthread_mutex_unlock(&rockstar_install_lock);
        return false;
    }
    (void)pthread_detach(thread);
    return true;
}

/* --- Routes ----------------------------------------------------------------------------------- */

char* ms_rockstar_status_json(const char* home) {
    bool active;
    char* launcher = rockstar_launcher(home);
    char* prefix = rockstar_prefix(home);
    char* out;
    pthread_mutex_lock(&rockstar_install_lock);
    active = rockstar_install_active;
    pthread_mutex_unlock(&rockstar_install_lock);
    out = ms_store_status_json(home, ROCKSTAR_CACHE_DIR, launcher != NULL, launcher && rockstar_launcher_pid(home) > 0,
                               active, prefix);
    free(launcher);
    free(prefix);
    return out;
}

char* ms_rockstar_launch_json(const char* home, int* status) {
    char* launcher = rockstar_launcher(home);
    char error[256] = {0};
    if (status)
        *status = 500;
    if (!launcher) {
        if (!rockstar_start_install(home))
            return ms_store_error_json("Could not start the Rockstar Games Launcher installation");
        if (status)
            *status = 202;
        return strdup("{\"ok\":true,\"installing\":true}");
    }
    free(launcher);
    if (rockstar_launcher_pid(home) <= 0) {
        if (!rockstar_prepare_prefix(home, error, sizeof(error)))
            return ms_store_error_json(error);
        if (!rockstar_start_launcher(home, rockstar_launcher_lane(), NULL))
            return ms_store_error_json(ms_wine_is_fex()
                                           ? "Could not start the Rockstar Games Launcher with Mullion"
                                           : "Could not start the Rockstar Games Launcher with D3DMetal");
    }
    if (status)
        *status = 200;
    return strdup("{\"ok\":true,\"running\":true}");
}

char* ms_rockstar_stop_json(const char* home, int* status) {
    ms_store_game* games = NULL;
    size_t count = rockstar_scan_games(home, &games);
    rockstar_process_set set = {games, count};
    bool ok = rockstar_stop_processes(home, &set);
    free(games);
    if (ok)
        rockstar_clear_lane(home);
    if (status)
        *status = ok ? 200 : 500;
    return ok ? strdup("{\"ok\":true,\"running\":false}")
              : ms_store_error_json("Could not stop the Rockstar Games Launcher processes");
}

char* ms_rockstar_library_json(const char* home) {
    ms_store_game* games = NULL;
    size_t count = rockstar_scan_games(home, &games);
    char* out = ms_store_library_json(home, &ROCKSTAR_PROVIDER, games, count);
    free(games);
    return out;
}

char* ms_rockstar_save_pipeline_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_store_game* games = NULL;
    size_t count = rockstar_scan_games(home, &games);
    char* out = ms_store_save_pipeline_json(home, &ROCKSTAR_PROVIDER, games, count, body, body_length, status);
    free(games);
    return out;
}

char* ms_rockstar_save_executable_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_store_game* games = NULL;
    size_t count = rockstar_scan_games(home, &games);
    char* out = ms_store_save_executable_json(home, &ROCKSTAR_PROVIDER, games, count, body, body_length, status);
    free(games);
    return out;
}

char* ms_rockstar_launch_game_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_store_game* games = NULL;
    size_t count = rockstar_scan_games(home, &games);
    const ms_store_game* game;
    char *requested = NULL, *pipeline = NULL, *launcher = NULL, *executable = NULL, *client = NULL, *wine = NULL,
         *prefix = NULL, *cwd = NULL, *running_lane = NULL;
    char failure[512] = "Could not launch the installed Rockstar game";
    int code = 400;
    pid_t pid = 0;
    unsigned appid = 0;
    char* out = NULL;
    game = ms_store_request_game(&ROCKSTAR_PROVIDER, games, count, body, body_length, &requested);
    if (!game) {
        code = 404;
        snprintf(failure, sizeof(failure), "Installed Rockstar game was not found in the local registry");
        goto done;
    }
    appid = rockstar_appid(game->id);
    pipeline = ms_store_canonical_pipeline(requested) ? strdup(ms_store_canonical_pipeline(requested))
                                                      : ms_store_pipeline_for_game(home, &ROCKSTAR_PROVIDER, appid);
    code = 500;
    if (!pipeline)
        goto done;
    if (!(launcher = rockstar_launcher(home))) {
        code = 409;
        snprintf(failure, sizeof(failure), "The Rockstar Games Launcher is not installed in the Steam prefix");
        goto done;
    }
    if (!rockstar_prepare_prefix(home, failure, sizeof(failure)))
        goto done;
    if (ms_store_game_executable(home, &ROCKSTAR_PROVIDER, game, &executable) < 0) {
        code = 409;
        snprintf(failure, sizeof(failure), "The saved game executable is no longer valid; choose it again");
        goto done;
    }
    if (!executable) {
        code = 404;
        snprintf(failure, sizeof(failure), "No launchable Windows executable was found for %s", game->name);
        goto done;
    }
    client = rockstar_client_executable(game, executable);
    if (!client)
        goto done;
    if (!ms_steam_stage_route_for_executable(home, pipeline, game->path, client)) {
        snprintf(failure, sizeof(failure), "Could not stage the %s graphics route for %s", pipeline, game->name);
        goto done;
    }
    /* The launcher starts the client: on the fex build the route rides in launch.env. */
    if (!ms_steam_write_game_launch_env(home, appid, pipeline, game->path, client)) {
        snprintf(failure, sizeof(failure), "Could not write the %s launch environment for %s", pipeline, game->name);
        goto done;
    }
    ms_steam_deploy_controller_input_shims(home, game->path);
    ms_steam_prepare_rockstar_client(home, pipeline, client);
    /* The launcher hands its environment to the games it starts. On the x86_64 runtime a launcher
     * running with another game's route is restarted with this one's; MetalSharp Wine's launcher
     * runs on Mullion for every game. */
    {
        char lane[64];
        bool running = rockstar_launcher_pid(home) > 0;
        rockstar_lane_key(pipeline, client, lane, sizeof(lane));
        running_lane = running ? rockstar_read_lane(home) : NULL;
        if (running && !ms_wine_is_fex() && strcmp(running_lane ? running_lane : "d3dmetal", lane)) {
            rockstar_process_set set = {games, count};
            (void)rockstar_stop_processes(home, &set);
            running = false;
        }
        if (!running) {
            if (!rockstar_start_launcher(home, ms_wine_is_fex() ? rockstar_launcher_lane() : pipeline, client)) {
                snprintf(failure, sizeof(failure), "Could not start the Rockstar Games Launcher for %s", game->name);
                goto done;
            }
            (void)rockstar_wait_for_launcher(home, 10);
        }
    }
    wine = ms_store_wine(home);
    prefix = rockstar_prefix(home);
    cwd = strdup(executable);
    if (!wine || !prefix || !cwd || !strrchr(cwd, '/'))
        goto done;
    *strrchr(cwd, '/') = '\0';
    {
        char* argv[] = {wine, executable, NULL};
        rockstar_env env = {pipeline, client};
        if (!ms_store_run_wine(home, prefix, cwd, argv, rockstar_apply_env, &env, false, NULL, &pid)) {
            snprintf(failure, sizeof(failure), "Could not start the MetalSharp Wine process for %s", game->name);
            goto done;
        }
    }
    /* Play*.exe hands the game to the launcher and exits: the game is its client while that runs. */
    ms_process_register_game_executable(appid, pid, client);
    ms_process_register_pending_game(appid, pid, 120);
    {
        char response[96];
        snprintf(response, sizeof(response), "{\"ok\":true,\"pid\":%ld,\"appid\":%u}", (long)pid, appid);
        out = strdup(response);
        code = 200;
    }
done:
    if (status)
        *status = code;
    if (!out)
        out = ms_store_error_json(failure);
    free(games);
    free(requested);
    free(pipeline);
    free(launcher);
    free(executable);
    free(client);
    free(wine);
    free(prefix);
    free(cwd);
    free(running_lane);
    return out;
}

typedef struct {
    const ms_store_game* game;
    const char* client;
} rockstar_game_process;

static bool rockstar_match_game(pid_t pid, pid_t group, const char* command, void* context) {
    const rockstar_game_process* target = context;
    (void)group;
    return ms_store_command_runs(command, target->client) &&
           (ms_store_command_in_windows_dir(command, target->game->windows_path) ||
            ms_store_contains_ci(command, target->game->path) || ms_store_process_cwd_within(pid, target->game->path));
}

void ms_rockstar_register_running_games(const char* home) {
    ms_store_game* games = NULL;
    size_t count;
    char* launcher = rockstar_launcher(home);
    /* No launcher, no game of its running. */
    if (!launcher)
        return;
    free(launcher);
    count = rockstar_scan_games(home, &games);
    for (size_t i = 0; i < count; i++) {
        char* executable = NULL;
        char* client;
        pid_t pid = 0;
        if (ms_store_game_executable(home, &ROCKSTAR_PROVIDER, &games[i], &executable) < 0 || !executable) {
            free(executable);
            continue;
        }
        client = rockstar_client_executable(&games[i], executable);
        if (client) {
            rockstar_game_process target = {&games[i], client};
            (void)ms_store_wine_processes(home, rockstar_match_game, &target, 0, &pid, NULL);
            if (pid > 1)
                /* With its executable: once the game exits, no other Wine program stands in for it. */
                ms_process_register_game_executable(rockstar_appid(games[i].id), pid, client);
        }
        free(client);
        free(executable);
    }
    free(games);
}
