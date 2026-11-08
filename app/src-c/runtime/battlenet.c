/* Battle.net in its own prefix (prefix-battlenet) on MetalSharp Wine, as Ubisoft Connect runs in
 * prefix-ubisoft, and the Blizzard and Activision games it installs. */
#include "metalsharp_backend/battlenet.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/store_launcher.h"
#include "metalsharp_backend/wine_runtime.h"
#include <ctype.h>
#include <dirent.h>
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

#define BATTLENET_INSTALLER_URL "https://us.battle.net/download/getInstaller?os=win&installer=Battle.net-Setup.exe"
#define BATTLENET_CACHE_DIR     "cache/battlenet"
#define BATTLENET_PREFIX        "prefix-battlenet"
#define BATTLENET_DIR           "drive_c/Program Files (x86)/Battle.net"
#define BATTLENET_PIPELINE      "d3dmetal"

/* Battle.net is a Chromium (CEF) app. On the x86_64 runtime it starts with the switches its
 * earlier MetalSharp integration used: no GPU process (--in-process-gpu), Chromium compositing in
 * software, and SwiftShader GL, which kept its login page from rendering as a white D3D11 surface
 * under Wine; Ubisoft Connect runs with its GPU process off for the same reason. (That integration
 * also pinned esync and DLL overrides for its own Wine Staging runtime; MetalSharp Wine's sync
 * follows the session's wineserver, so those are not carried over.) MetalSharp Wine gets no
 * switches, as Ubisoft Connect gets none there. */
static const char* const BATTLENET_CEF_ARGS[] = {"--in-process-gpu", "--use-gl=swiftshader",
                                                  "--disable-gpu-compositing"};

static pthread_mutex_t battlenet_install_lock = PTHREAD_MUTEX_INITIALIZER;
static bool battlenet_install_active;

static unsigned battlenet_appid(const char* id) {
    /* Bit 29 only: apart from Steam's ids, Rockstar's (bit 30) and Ubisoft's (bit 31). */
    return 0x20000000u | (ms_store_fnv1a(id) & 0x1fffffffu);
}

static char* battlenet_prefix(const char* home) {
    return ms_store_path_join(home, BATTLENET_PREFIX);
}

typedef struct {
    const char* prefix;
    char* dir;
} battlenet_install_lookup;

static void battlenet_visit_install(const ms_store_registry_section* section, void* context) {
    battlenet_install_lookup* lookup = context;
    const char* key = strrchr(section->key, '\\');
    if (lookup->dir || !key || strcasecmp(key + 1, "Battle.net") || !strstr(section->key, "\\Uninstall\\") ||
        !section->install_location[0])
        return;
    char* host = ms_store_wine_path_to_host(lookup->prefix, section->install_location);
    struct stat info;
    if (host && stat(host, &info) == 0 && S_ISDIR(info.st_mode))
        lookup->dir = host;
    else
        free(host);
}

/* Where Battle.net installed itself: its Uninstall entry's InstallLocation (the setup lets the user
 * pick any folder), else the default C:\Program Files (x86)\Battle.net.  */
static char* battlenet_install_dir(const char* prefix) {
    char* registry = ms_store_path_join(prefix, "system.reg");
    battlenet_install_lookup lookup = {prefix, NULL};
    if (registry)
        (void)ms_store_registry_sections(registry, battlenet_visit_install, &lookup);
    free(registry);
    if (lookup.dir)
        return lookup.dir;
    return ms_store_path_join(prefix, BATTLENET_DIR);
}

static char* battlenet_file(const char* home, const char* name) {
    char* prefix = battlenet_prefix(home);
    char* dir = prefix ? battlenet_install_dir(prefix) : NULL;
    char* path = dir ? ms_store_path_join(dir, name) : NULL;
    free(prefix);
    free(dir);
    if (path && ms_store_is_file(path))
        return path;
    free(path);
    return NULL;
}

/* What starts the client: Battle.net Launcher.exe, else Battle.net.exe. */
static char* battlenet_launcher(const char* home) {
    char* launcher = battlenet_file(home, "Battle.net Launcher.exe");
    return launcher ? launcher : battlenet_file(home, "Battle.net.exe");
}

static char* battlenet_cache_file(const char* home, const char* name) {
    char* dir = ms_store_path_join(home, BATTLENET_CACHE_DIR);
    char* path = dir ? ms_store_path_join(dir, name) : NULL;
    free(dir);
    return path;
}

static bool battlenet_ignored_executable(const char* name) {
    static const char* const ignored[] = {"launcher", "battle.net", "blizzard", "agent", "browser", "error"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        if (ms_store_contains_ci(name, ignored[i]))
            return true;
    return false;
}

/* The game's largest executable (World of Warcraft's _retail_/Wow.exe), not its launcher stub. */
static char* battlenet_find_executable(const ms_store_game* game) {
    char* found = NULL;
    off_t size = 0;
    ms_store_largest_executable(game->path, 0, 3, battlenet_ignored_executable, &found, &size);
    return found;
}

/* A cached catalog fragment (Battle.net's own JSON, a few hundred KB at most). */
static ms_json* battlenet_read_fragment(const char* path) {
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 2 || info.st_size > 4 * 1024 * 1024)
        return NULL;
    FILE* file = fopen(path, "rb");
    if (!file)
        return NULL;
    char* data = malloc((size_t)info.st_size + 1);
    size_t length = data ? fread(data, 1, (size_t)info.st_size, file) : 0;
    fclose(file);
    ms_json* json = NULL;
    if (data && length > 1 && data[0] == '{') {
        data[length] = '\0';
        json = ms_json_parse(data, length, NULL, 0);
    }
    free(data);
    return json;
}

/* The key art a catalog fragment names for uid, as its cache file under cache_dir (malloc'd). */
static char* battlenet_fragment_key_art(const ms_json* fragment, const char* uid, const char* cache_dir) {
    const ms_json* installs = ms_json_object_get(fragment, "installs");
    const ms_json* products = ms_json_object_get(fragment, "products");
    if (!installs || !ms_json_object_get(installs, uid) || !products)
        return NULL;
    const ms_json* base = ms_json_object_get(ms_json_array_get(products, 0), "base");
    char* key = NULL;
    char* hash = NULL;
    char* path = NULL;
    if (!ms_json_as_string(ms_json_object_get(base, "key_art"), &key))
        return NULL;
    const ms_json* files = ms_json_object_get(ms_json_object_get(fragment, "files"), "default");
    if (ms_json_as_string(ms_json_object_get(ms_json_object_get(files, key), "hash"), &hash) && strlen(hash) == 32) {
        char relative[48];
        snprintf(relative, sizeof(relative), "%.2s/%.2s/%s", hash, hash + 2, hash);
        path = ms_store_path_join(cache_dir, relative);
        if (path && !ms_store_is_file(path)) {
            free(path);
            path = NULL;
        }
    }
    free(key);
    free(hash);
    return path;
}

/* Battle.net caches its catalog and art under AppData\Local\Battle.net\Cache\xx\yy\<md5>: JSON
 * fragments list a product's install uids and name its key art (a 600x800 cover with the logo) by
 * the art file's hash. The newest fragment that has art for the game's uid wins. */
static char* battlenet_card_art(const char* home, const ms_store_game* game) {
    char* prefix = game && game->uid[0] ? battlenet_prefix(home) : NULL;
    char* users = prefix ? ms_store_path_join(prefix, "drive_c/users") : NULL;
    DIR* user_dir = users ? opendir(users) : NULL;
    char* best = NULL;
    time_t best_time = 0;
    struct dirent* user;
    while (user_dir && (user = readdir(user_dir))) {
        if (user->d_name[0] == '.')
            continue;
        char cache[PATH_MAX];
        snprintf(cache, sizeof(cache), "%s/%s/AppData/Local/Battle.net/Cache", users, user->d_name);
        DIR* level1 = opendir(cache);
        struct dirent* a;
        while (level1 && (a = readdir(level1))) {
            if (strlen(a->d_name) != 2)
                continue;
            char dir1[PATH_MAX];
            snprintf(dir1, sizeof(dir1), "%s/%s", cache, a->d_name);
            DIR* level2 = opendir(dir1);
            struct dirent* b;
            while (level2 && (b = readdir(level2))) {
                if (strlen(b->d_name) != 2)
                    continue;
                char dir2[PATH_MAX];
                snprintf(dir2, sizeof(dir2), "%s/%s", dir1, b->d_name);
                DIR* level3 = opendir(dir2);
                struct dirent* c;
                while (level3 && (c = readdir(level3))) {
                    if (strlen(c->d_name) != 32)
                        continue;
                    char file[PATH_MAX];
                    struct stat info;
                    snprintf(file, sizeof(file), "%s/%s", dir2, c->d_name);
                    if (stat(file, &info) != 0 || (best && info.st_mtime <= best_time))
                        continue;
                    ms_json* fragment = battlenet_read_fragment(file);
                    char* art = fragment ? battlenet_fragment_key_art(fragment, game->uid, cache) : NULL;
                    ms_json_free(fragment);
                    if (art) {
                        free(best);
                        best = art;
                        best_time = info.st_mtime;
                    }
                }
                if (level3)
                    closedir(level3);
            }
            if (level2)
                closedir(level2);
        }
        if (level1)
            closedir(level1);
    }
    if (user_dir)
        closedir(user_dir);
    free(users);
    free(prefix);
    return best;
}

static const ms_store_provider BATTLENET_PROVIDER = {"battlenet",
                                                     "battlenet_id",
                                                     "Battle.net",
                                                     BATTLENET_PIPELINE,
                                                     battlenet_appid,
                                                     battlenet_find_executable,
                                                     battlenet_card_art};

/* --uid=<uid> in an UninstallString (Blizzard Uninstaller.exe ... --uid=fenris ...). */
static bool battlenet_uid_from_uninstall(const char* uninstall, char* uid, size_t uid_size) {
    const char* at = NULL;
    size_t n = 0;
    for (const char* p = uninstall; p && *p; p++)
        if (!strncasecmp(p, "--uid=", 6) && (p == uninstall || isspace((unsigned char)p[-1]) || p[-1] == '"')) {
            at = p + 6;
            break;
        }
    if (!at || uid_size < 2)
        return false;
    if (*at == '"')
        at++;
    while (at[n] && !isspace((unsigned char)at[n]) && at[n] != '"' && n + 1 < uid_size) {
        uid[n] = at[n];
        n++;
    }
    uid[n] = '\0';
    return ms_store_valid_id(uid);
}

static bool battlenet_publisher(const char* publisher) {
    return ms_store_contains_ci(publisher, "Blizzard") || ms_store_contains_ci(publisher, "Activision");
}

typedef struct {
    const char* prefix;
    char client_dir[PATH_MAX];
    ms_store_game* games;
    size_t count;
    size_t capacity;
} battlenet_scan;

static void battlenet_visit_section(const ms_store_registry_section* section, void* opaque) {
    battlenet_scan* scan = opaque;
    ms_store_game game;
    char uid[sizeof(game.uid)] = "";
    if (!ms_store_contains_ci(section->key, "\\Uninstall\\") || !battlenet_publisher(section->publisher) ||
        !section->install_location[0])
        return;
    (void)battlenet_uid_from_uninstall(section->uninstall_string, uid, sizeof(uid));
    /* Battle.net registers itself too. */
    if (ms_store_contains_ci(section->display_name, "Battle.net") || !strcasecmp(uid, "battle.net") ||
        !strcasecmp(uid, "bna"))
        return;
    if (!ms_store_game_from_folder(scan->prefix, section->install_location, section->display_name, 3, &game))
        return;
    if (scan->client_dir[0] && ms_resolved_path_within(game.path, scan->client_dir))
        return;
    if (uid[0]) {
        snprintf(game.uid, sizeof(game.uid), "%s", uid);
        snprintf(game.id, sizeof(game.id), "%s", uid);
    }
    (void)ms_store_add_game(&scan->games, &scan->count, &scan->capacity, &game);
}

/* Battle.net's installed games, from its prefix's Uninstall entries (malloc'd). */
static size_t battlenet_scan_games(const char* home, ms_store_game** games) {
    char* prefix = battlenet_prefix(home);
    char* registry = prefix ? ms_store_path_join(prefix, "system.reg") : NULL;
    battlenet_scan scan = {prefix, "", NULL, 0, 0};
    char* client_dir = prefix ? battlenet_install_dir(prefix) : NULL;
    if (client_dir)
        snprintf(scan.client_dir, sizeof(scan.client_dir), "%s", client_dir);
    free(client_dir);
    if (registry)
        (void)ms_store_registry_sections(registry, battlenet_visit_section, &scan);
    free(registry);
    free(prefix);
    *games = scan.games;
    return scan.count;
}

/* --- Processes ------------------------------------------------------------------------------- */

typedef struct {
    const char* prefix;
    pid_t group;
} battlenet_client_match;

static bool battlenet_match_client(pid_t pid, pid_t group, const char* command, void* context) {
    const battlenet_client_match* target = context;
    return (ms_store_command_runs(command, "Battle.net.exe") ||
            ms_store_command_runs(command, "Battle.net Launcher.exe")) &&
           (ms_store_contains_ci(command, target->prefix) || ms_store_process_cwd_within(pid, target->prefix) ||
            (target->group > 1 && group == target->group));
}

static pid_t battlenet_read_pid_file(const char* home) {
    char* path = battlenet_cache_file(home, "client.pid");
    char* text = path ? ms_store_read_text(path, 64) : NULL;
    long value = text ? strtol(text, NULL, 10) : 0;
    free(path);
    free(text);
    return value > 1 && value <= INT_MAX ? (pid_t)value : 0;
}

static pid_t battlenet_client_pid(const char* home) {
    char* prefix = battlenet_prefix(home);
    battlenet_client_match target = {prefix, battlenet_read_pid_file(home)};
    pid_t pid = 0;
    if (prefix && ms_store_is_dir(prefix))
        (void)ms_store_wine_processes(home, battlenet_match_client, &target, 0, &pid, NULL);
    free(prefix);
    return pid;
}

static void battlenet_write_cache(const char* home, const char* name, const char* text) {
    char* dir = ms_store_path_join(home, BATTLENET_CACHE_DIR);
    char* path = battlenet_cache_file(home, name);
    FILE* file = dir && path && ms_store_mkdir_p(dir) ? fopen(path, "wb") : NULL;
    if (file) {
        fprintf(file, "%s\n", text);
        fclose(file);
    }
    free(dir);
    free(path);
}

static char* battlenet_read_lane(const char* home) {
    char* path = battlenet_cache_file(home, "client-lane");
    char* text = path ? ms_store_read_text(path, 256) : NULL;
    if (text)
        text[strcspn(text, "\r\n")] = '\0';
    free(path);
    return text;
}

static void battlenet_clear_cache(const char* home) {
    static const char* const names[] = {"client.pid", "client-lane"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char* path = battlenet_cache_file(home, names[i]);
        if (path)
            (void)unlink(path);
        free(path);
    }
}

static bool battlenet_stop_prefix(const char* home) {
    char* prefix = battlenet_prefix(home);
    bool ok = ms_store_stop_prefix(home, prefix);
    free(prefix);
    if (ok)
        battlenet_clear_cache(home);
    return ok;
}

/* --- Starting Battle.net and games ------------------------------------------------------------ */

/* The lane Battle.net runs on: Ubisoft Connect's, D3DMetal on the x86_64 runtime and Mullion on
 * MetalSharp Wine. */
static const char* battlenet_lane(void) {
    return ms_wine_is_fex() ? "dxmt" : "d3dmetal";
}

static void battlenet_apply_env(const char* home, const void* context) {
    ms_steam_apply_graphics_route(home, (const char*)context);
}

static bool battlenet_initialize_prefix(const char* home, char* error, size_t error_size) {
    char* prefix = battlenet_prefix(home);
    bool ok = prefix && ms_store_initialize_prefix(home, prefix, "battlenet", "Battle.net", error, error_size);
    free(prefix);
    return ok;
}

static bool battlenet_wait_for_client(const char* home, unsigned seconds) {
    for (unsigned i = 0; i < seconds * 2; i++) {
        if (battlenet_client_pid(home) > 0)
            return true;
        usleep(500000);
    }
    return false;
}

/* Starts the client on pipeline (its own lane, or on the x86_64 runtime a game's route, which the
 * games it starts inherit). */
static bool battlenet_start_client(const char* home, const char* pipeline) {
    char* launcher = battlenet_launcher(home);
    char* wine = ms_store_wine(home);
    char* prefix = battlenet_prefix(home);
    char* dir = launcher ? strdup(launcher) : NULL;
    pid_t pid = 0;
    bool ok = launcher && wine && prefix && dir;
    if (ok) {
        char* argv[2 + sizeof(BATTLENET_CEF_ARGS) / sizeof(BATTLENET_CEF_ARGS[0]) + 1];
        size_t argc = 0;
        char text[32];
        argv[argc++] = wine;
        argv[argc++] = launcher;
        for (size_t i = 0; !ms_wine_is_fex() && i < sizeof(BATTLENET_CEF_ARGS) / sizeof(BATTLENET_CEF_ARGS[0]); i++)
            argv[argc++] = (char*)BATTLENET_CEF_ARGS[i];
        argv[argc] = NULL;
        *strrchr(dir, '/') = '\0';
        ok = ms_store_run_wine(home, prefix, dir, argv, battlenet_apply_env, pipeline, false, NULL, &pid);
        if (ok) {
            snprintf(text, sizeof(text), "%ld", (long)pid);
            battlenet_write_cache(home, "client.pid", text);
            battlenet_write_cache(home, "client-lane", pipeline);
        }
    }
    free(launcher);
    free(wine);
    free(prefix);
    free(dir);
    return ok;
}

/* Battle.net is not running when this is called. On MetalSharp Wine a client started into the
 * prefix's earlier session stalled for Ubisoft Connect, so Battle.net starts fresh there too. */
static bool battlenet_launch_client(const char* home, const char* pipeline) {
    if (ms_wine_is_fex())
        (void)battlenet_stop_prefix(home);
    return battlenet_start_client(home, pipeline);
}

static void* battlenet_install_worker(void* opaque) {
    char* home = opaque;
    char error[256] = {0};
    char* dir = ms_store_path_join(home, BATTLENET_CACHE_DIR);
    char* installer = dir ? ms_store_path_join(dir, "Battle.net-Setup.exe") : NULL;
    char* wine = ms_store_wine(home);
    char* prefix = battlenet_prefix(home);
    bool ok = dir && installer && prefix && ms_store_mkdir_p(dir);
    if (ok)
        ok = battlenet_initialize_prefix(home, error, sizeof(error));
    if (ok && !wine) {
        ok = false;
        snprintf(error, sizeof(error), "MetalSharp Wine is not available to install Battle.net");
    }
    if (!ok && !error[0])
        snprintf(error, sizeof(error), "Could not prepare the Battle.net installer");
    if (ok) {
        ms_store_write_state(home, BATTLENET_CACHE_DIR, "downloading", NULL);
        ok = ms_store_download(BATTLENET_INSTALLER_URL, installer);
        if (!ok)
            snprintf(error, sizeof(error), "Could not download the Battle.net installer from Blizzard");
    }
    if (ok) {
        /* Interactive setup. Its exit status is not the result: the installed client is. The app
         * goes to C: by default: run from an external drive its built-in browser crashes and restarts
         * under Wine, while games install to the managed Y: folder from inside Battle.net. */
        char* argv[] = {wine, installer, "--installpath=C:\\Program Files (x86)\\Battle.net", NULL};
        int status = 0;
        ms_store_write_state(home, BATTLENET_CACHE_DIR, "installing", NULL);
        ok = ms_store_run_wine(home, prefix, dir, argv, battlenet_apply_env, battlenet_lane(), true, &status, NULL);
        if (!ok)
            snprintf(error, sizeof(error), "The Battle.net installer could not be started");
    }
    if (ok) {
        char* launcher = battlenet_launcher(home);
        if (!launcher) {
            ok = false;
            snprintf(error, sizeof(error), "Installer finished, but Battle.net was not found in its prefix");
        }
        free(launcher);
    }
    /* Setup may have started the client already. */
    if (ok && battlenet_client_pid(home) <= 0 && !battlenet_launch_client(home, battlenet_lane())) {
        ok = false;
        snprintf(error, sizeof(error), "Battle.net installed, but could not be launched");
    }
    ms_store_write_state(home, BATTLENET_CACHE_DIR, ok ? "ready" : "error", ok ? NULL : error);
    free(dir);
    free(installer);
    free(wine);
    free(prefix);
    pthread_mutex_lock(&battlenet_install_lock);
    battlenet_install_active = false;
    pthread_mutex_unlock(&battlenet_install_lock);
    free(home);
    return NULL;
}

static bool battlenet_start_install(const char* home) {
    pthread_t thread;
    char* copy;
    pthread_mutex_lock(&battlenet_install_lock);
    if (battlenet_install_active) {
        pthread_mutex_unlock(&battlenet_install_lock);
        return true;
    }
    battlenet_install_active = true;
    pthread_mutex_unlock(&battlenet_install_lock);
    copy = strdup(home);
    if (!copy || pthread_create(&thread, NULL, battlenet_install_worker, copy) != 0) {
        free(copy);
        pthread_mutex_lock(&battlenet_install_lock);
        battlenet_install_active = false;
        pthread_mutex_unlock(&battlenet_install_lock);
        return false;
    }
    (void)pthread_detach(thread);
    return true;
}

/* --- Routes ----------------------------------------------------------------------------------- */

char* ms_battlenet_status_json(const char* home) {
    bool active;
    char* launcher = battlenet_launcher(home);
    char* prefix = battlenet_prefix(home);
    char* out;
    pthread_mutex_lock(&battlenet_install_lock);
    active = battlenet_install_active;
    pthread_mutex_unlock(&battlenet_install_lock);
    out = ms_store_status_json(home, BATTLENET_CACHE_DIR, launcher != NULL,
                               launcher && battlenet_client_pid(home) > 0, active, prefix);
    free(launcher);
    free(prefix);
    return out;
}

char* ms_battlenet_launch_json(const char* home, int* status) {
    char* launcher = battlenet_launcher(home);
    char error[256] = {0};
    if (status)
        *status = 500;
    if (!launcher) {
        if (!battlenet_start_install(home))
            return ms_store_error_json("Could not start the Battle.net installation");
        if (status)
            *status = 202;
        return strdup("{\"ok\":true,\"installing\":true}");
    }
    free(launcher);
    if (battlenet_client_pid(home) <= 0) {
        if (!battlenet_initialize_prefix(home, error, sizeof(error)))
            return ms_store_error_json(error);
        if (!battlenet_launch_client(home, battlenet_lane()))
            return ms_store_error_json(ms_wine_is_fex() ? "Could not start Battle.net with Mullion"
                                                        : "Could not start Battle.net with D3DMetal");
    }
    if (status)
        *status = 200;
    return strdup("{\"ok\":true,\"running\":true}");
}

char* ms_battlenet_stop_json(const char* home, int* status) {
    bool ok = battlenet_stop_prefix(home);
    if (status)
        *status = ok ? 200 : 500;
    return ok ? strdup("{\"ok\":true,\"running\":false}") : ms_store_error_json("Could not stop Battle.net processes");
}

char* ms_battlenet_library_json(const char* home) {
    ms_store_game* games = NULL;
    size_t count = battlenet_scan_games(home, &games);
    char* out = ms_store_library_json(home, &BATTLENET_PROVIDER, games, count);
    free(games);
    return out;
}

char* ms_battlenet_save_pipeline_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_store_game* games = NULL;
    size_t count = battlenet_scan_games(home, &games);
    char* out = ms_store_save_pipeline_json(home, &BATTLENET_PROVIDER, games, count, body, body_length, status);
    free(games);
    return out;
}

char* ms_battlenet_save_executable_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_store_game* games = NULL;
    size_t count = battlenet_scan_games(home, &games);
    char* out = ms_store_save_executable_json(home, &BATTLENET_PROVIDER, games, count, body, body_length, status);
    free(games);
    return out;
}

char* ms_battlenet_launch_game_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_store_game* games = NULL;
    size_t count = battlenet_scan_games(home, &games);
    const ms_store_game* game;
    char *requested = NULL, *pipeline = NULL, *launcher = NULL, *executable = NULL, *wine = NULL, *prefix = NULL,
         *cwd = NULL, *running_lane = NULL;
    char failure[512] = "Could not launch the installed Battle.net game";
    int code = 400;
    pid_t pid = 0;
    unsigned appid;
    char* out = NULL;
    game = ms_store_request_game(&BATTLENET_PROVIDER, games, count, body, body_length, &requested);
    if (!game) {
        code = 404;
        snprintf(failure, sizeof(failure), "Installed Battle.net game was not found in the local registry");
        goto done;
    }
    appid = battlenet_appid(game->id);
    pipeline = ms_store_canonical_pipeline(requested) ? strdup(ms_store_canonical_pipeline(requested))
                                                      : ms_store_pipeline_for_game(home, &BATTLENET_PROVIDER, appid);
    code = 500;
    if (!pipeline)
        goto done;
    if (!(launcher = battlenet_launcher(home))) {
        code = 409;
        snprintf(failure, sizeof(failure), "Battle.net is not installed in its MetalSharp prefix");
        goto done;
    }
    {
        char init_error[256] = {0};
        if (!battlenet_initialize_prefix(home, init_error, sizeof(init_error))) {
            snprintf(failure, sizeof(failure), "%s",
                     init_error[0] ? init_error : "Could not initialize the Battle.net prefix");
            goto done;
        }
    }
    if (ms_store_game_executable(home, &BATTLENET_PROVIDER, game, &executable) < 0) {
        code = 409;
        snprintf(failure, sizeof(failure), "The saved game executable is no longer valid; choose it again");
        goto done;
    }
    if (!executable) {
        code = 404;
        snprintf(failure, sizeof(failure), "No launchable Windows executable was found for %s", game->name);
        goto done;
    }
    if (!ms_steam_stage_route_for_executable(home, pipeline, game->path, executable)) {
        snprintf(failure, sizeof(failure), "Could not stage the %s graphics route for %s", pipeline, game->name);
        goto done;
    }
    /* Battle.net starts the game: on the fex build the route rides in launch.env. */
    if (!ms_steam_write_game_launch_env(home, appid, pipeline, game->path, executable)) {
        snprintf(failure, sizeof(failure), "Could not write the %s launch environment for %s", pipeline, game->name);
        goto done;
    }
    ms_steam_deploy_controller_input_shims(home, game->path);
    /* A game Battle.net starts inherits its environment. On the bundles' x86_64 runtime a client running on
     * another route is restarted on the game's (Battle.net's own UI renders with SwiftShader, so the route does not
     * change it); on MetalSharp Wine (fex, and the x86_64 app) the client keeps its lane and games take theirs from
     * launch.env. */
    {
        const char* client_pipeline = ms_wine_metalsharp_launch_model() || !game->uid[0] ? battlenet_lane() : pipeline;
        bool running = battlenet_client_pid(home) > 0;
        running_lane = running ? battlenet_read_lane(home) : NULL;
        if (running && !ms_wine_metalsharp_launch_model() && game->uid[0] &&
            strcmp(running_lane ? running_lane : battlenet_lane(), client_pipeline)) {
            (void)battlenet_stop_prefix(home);
            running = false;
        }
        if (!running) {
            if (!battlenet_launch_client(home, client_pipeline)) {
                snprintf(failure, sizeof(failure), "Could not start Battle.net for %s", game->name);
                goto done;
            }
            (void)battlenet_wait_for_client(home, 15);
        }
    }
    wine = ms_store_wine(home);
    prefix = battlenet_prefix(home);
    if (!wine || !prefix)
        goto done;
    /* Battle.net's Play runs the game's executable with "-uid <uid>" while the client is signed in
     * (the game checks in with it); --exec=launch_uid only selects the game in current clients. */
    cwd = strdup(executable);
    if (!cwd || !strrchr(cwd, '/'))
        goto done;
    *strrchr(cwd, '/') = '\0';
    if (game->uid[0]) {
        char* argv[] = {wine, executable, "-uid", (char*)game->uid, NULL};
        if (!ms_store_run_wine(home, prefix, cwd, argv, battlenet_apply_env, pipeline, false, NULL, &pid)) {
            snprintf(failure, sizeof(failure), "Could not start the MetalSharp Wine process for %s", game->name);
            goto done;
        }
    } else {
        char* argv[] = {wine, executable, NULL};
        if (!ms_store_run_wine(home, prefix, cwd, argv, battlenet_apply_env, pipeline, false, NULL, &pid)) {
            snprintf(failure, sizeof(failure), "Could not start the MetalSharp Wine process for %s", game->name);
            goto done;
        }
    }
    /* The game may restart itself once (updates, anti-cheat): it is its executable while that runs. */
    ms_process_register_game_executable(appid, pid, executable);
    ms_process_register_pending_game(appid, pid, 180);
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
    free(wine);
    free(prefix);
    free(cwd);
    free(running_lane);
    return out;
}

typedef struct {
    const ms_store_game* game;
    const char* executable;
} battlenet_game_process;

static bool battlenet_match_game(pid_t pid, pid_t group, const char* command, void* context) {
    const battlenet_game_process* target = context;
    (void)group;
    return ms_store_command_runs(command, target->executable) &&
           (ms_store_command_in_windows_dir(command, target->game->windows_path) ||
            ms_store_contains_ci(command, target->game->path) || ms_store_process_cwd_within(pid, target->game->path));
}

void ms_battlenet_register_running_games(const char* home) {
    ms_store_game* games = NULL;
    size_t count;
    char* launcher = battlenet_launcher(home);
    if (!launcher)
        return;
    free(launcher);
    count = battlenet_scan_games(home, &games);
    for (size_t i = 0; i < count; i++) {
        char* executable = NULL;
        pid_t pid = 0;
        if (ms_store_game_executable(home, &BATTLENET_PROVIDER, &games[i], &executable) >= 0 && executable) {
            battlenet_game_process target = {&games[i], executable};
            (void)ms_store_wine_processes(home, battlenet_match_game, &target, 0, &pid, NULL);
            if (pid > 1)
                /* With its executable: once the game exits, Battle.net does not stand in for it. */
                ms_process_register_game_executable(battlenet_appid(games[i].id), pid, executable);
        }
        free(executable);
    }
    free(games);
}
