/* POST /launchers/uninstall without Wine, a launcher or a real drive: fake MetalSharp homes with
 * fake prefixes, a script standing in for wineserver, a folder standing in for /Volumes
 * (METALSHARP_VOLUMES_ROOT) and HOME for the user's home. What each launcher's removal takes, what
 * it must leave (Steam's prefix, other launchers, links out of the trees), and when the games folder
 * may be deleted. */
#include "metalsharp_backend/battlenet.h"
#include "metalsharp_backend/epic.h"
#include "metalsharp_backend/gog.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/launcher_uninstall.h"
#include "metalsharp_backend/library_locations.h"
#include "metalsharp_backend/rockstar.h"
#include "metalsharp_backend/ubisoft.h"
#include "metalsharp_backend/wine_runtime.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char base[PATH_MAX];

static char* at(const char* relative) {
    static char paths[32][PATH_MAX];
    static unsigned next;
    char* out = paths[next++ % 32];
    snprintf(out, PATH_MAX, "%s/%s", base, relative);
    return out;
}

static void make_dir_at(const char* absolute) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", absolute);
    for (char* p = path + strlen(base) + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            assert(mkdir(path, 0755) == 0 || errno == EEXIST);
            *p = '/';
        }
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
}

static void make_dir(const char* relative) {
    make_dir_at(at(relative));
}

static void write_at(const char* absolute, const char* text) {
    char parent[PATH_MAX];
    FILE* file;
    snprintf(parent, sizeof(parent), "%s", absolute);
    *strrchr(parent, '/') = '\0';
    make_dir_at(parent);
    file = fopen(absolute, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void make_file(const char* relative) {
    write_at(at(relative), "x");
}

static bool exists(const char* relative) {
    struct stat st;
    return lstat(at(relative), &st) == 0;
}

static void make_link(const char* relative, const char* target) {
    assert(symlink(target, at(relative)) == 0);
}

/* A home with a wineserver that stops (or, with stuck, cannot stop) whatever prefix it is given. */
static char* new_home(const char* name, bool stuck) {
    static char homes[16][PATH_MAX];
    static unsigned next;
    char* home = homes[next++ % 16];
    char server[PATH_MAX];
    snprintf(home, PATH_MAX, "%s/%s", base, name);
    make_dir_at(home);
    snprintf(server, sizeof(server), "%s/runtime/wine/bin/wineserver", home);
    write_at(server, stuck ? "#!/bin/sh\nexit 2\n"
                           : "#!/bin/sh\n[ -n \"$WINEPREFIX\" ] || exit 3\ncase \"$1\" in -k|-w) exit 0 ;; esac\n"
                             "exit 2\n");
    assert(chmod(server, 0755) == 0);
    return home;
}

static char* in_home(const char* home, const char* relative) {
    static char paths[32][PATH_MAX];
    static unsigned next;
    char* out = paths[next++ % 32];
    snprintf(out, PATH_MAX, "%s/%s", home, relative);
    return out;
}

static void home_file(const char* home, const char* relative) {
    write_at(in_home(home, relative), "x");
}

static bool home_has(const char* home, const char* relative) {
    struct stat st;
    return lstat(in_home(home, relative), &st) == 0;
}

/* config/library-locations.json with one launcher's location (drive and drive_target optional). */
static void save_location(const char* home, const char* launcher, const char* volume, const char* path,
                          const char* drive, const char* drive_target) {
    char text[8192];
    if (drive)
        snprintf(text, sizeof(text),
                 "{\"version\":1,\"locations\":{\"%s\":{\"volume\":\"%s\",\"path\":\"%s\",\"drive\":\"%s\","
                 "\"drive_target\":\"%s\"}}}",
                 launcher, volume, path, drive, drive_target);
    else
        snprintf(text, sizeof(text), "{\"version\":1,\"locations\":{\"%s\":{\"volume\":\"%s\",\"path\":\"%s\"}}}",
                 launcher, volume, path);
    write_at(in_home(home, "config/library-locations.json"), text);
}

typedef struct {
    int status;
    ms_json* json;
    char* raw;
} reply;

static reply uninstall(const char* home, const char* body) {
    reply r = {0, NULL, NULL};
    r.raw = ms_launcher_uninstall_json(home, body, body ? strlen(body) : 0, &r.status);
    assert(r.raw);
    r.json = ms_json_parse(r.raw, strlen(r.raw), NULL, 0);
    assert(r.json);
    return r;
}

static void done(reply* r) {
    ms_json_free(r->json);
    free(r->raw);
}

static bool removed_has(const reply* r, const char* path) {
    const ms_json* removed = ms_json_object_get(r->json, "removed");
    for (size_t i = 0; i < ms_json_array_length(removed); i++) {
        char* text = NULL;
        bool match = ms_json_as_string(ms_json_array_get(removed, i), &text) && !strcmp(text, path);
        free(text);
        if (match)
            return true;
    }
    return false;
}

static size_t removed_count(const reply* r) {
    return ms_json_array_length(ms_json_object_get(r->json, "removed"));
}

static bool kept_is(const reply* r, const char* expected) {
    const ms_json* kept = ms_json_object_get(r->json, "kept_games_folder");
    char* text = NULL;
    bool match;
    if (!expected)
        return kept && ms_json_type_of(kept) == MS_JSON_NULL;
    match = ms_json_as_string(kept, &text) && !strcmp(text, expected);
    free(text);
    return match;
}

static bool flag(char* raw, const char* key) {
    ms_json* json = ms_json_parse(raw, strlen(raw), NULL, 0);
    bool value = false;
    assert(json);
    assert(ms_json_as_bool(ms_json_object_get(json, key), &value));
    ms_json_free(json);
    free(raw);
    return value;
}

static void test_bad_requests(void) {
    char* home = new_home("bad", false);
    static const char* const bodies[] = {
        NULL,
        "",
        "[]",
        "{}",
        "{\"launcher\":\"steam\",\"delete_games\":false}",
        "{\"launcher\":7,\"delete_games\":false}",
        "{\"launcher\":\"ubisoft\"}",
        "{\"launcher\":\"ubisoft\",\"delete_games\":\"yes\"}",
        "{\"launcher\":\"ubisoft\",\"delete_games\":1}",
        "{\"launcher\":\"gog\",\"delete_games\":null}",
    };
    home_file(home, "prefix-ubisoft/system.reg");
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        reply r = uninstall(home, bodies[i]);
        assert(r.status == 400 && strstr(r.raw, "\"ok\":false"));
        done(&r);
    }
    assert(home_has(home, "prefix-ubisoft/system.reg"));
}

/* Things no launcher's removal may touch. */
static void shared_sentinels(const char* home) {
    home_file(home, "prefix-steam/system.reg");
    home_file(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steam.exe");
    home_file(home, "bottles/steam_1245620/bottle.json");
    home_file(home, "config/game-executables/steam/1245620.txt");
    home_file(home, "setup.json");
}

static void assert_shared_sentinels(const char* home) {
    assert(home_has(home, "prefix-steam/system.reg"));
    assert(home_has(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steam.exe"));
    assert(home_has(home, "bottles/steam_1245620/bottle.json"));
    assert(home_has(home, "config/game-executables/steam/1245620.txt"));
    assert(home_has(home, "setup.json"));
    assert(home_has(home, "runtime/wine/bin/wineserver"));
}

static void test_ubisoft(void) {
    char* home = new_home("ubisoft", false);
    char games[PATH_MAX];
    reply r;
    shared_sentinels(home);
    home_file(home, "prefix-ubisoft/system.reg");
    home_file(home, "prefix-ubisoft/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/UbisoftConnect.exe");
    make_dir("outside");
    make_file("outside/keep.txt");
    make_dir_at(in_home(home, "prefix-ubisoft/dosdevices"));
    make_link("ubisoft/prefix-ubisoft/dosdevices/z:", at("outside"));
    home_file(home, "cache/ubisoft-connect/state.json");
    home_file(home, "cache/ubisoft-connect/UbisoftConnectInstaller.exe");
    home_file(home, "cache/ubisoft-connect/artwork/1073741825.png"); /* Rockstar's icon, shared */
    home_file(home, "bottles/ubisoft_2147483650/bottle.json");
    home_file(home, "bottles/battlenet_536870913/bottle.json");
    home_file(home, "bottles/rockstar_1073741825/bottle.json");
    home_file(home, "config/game-executables/ubisoft/46.txt");
    home_file(home, "prefix-battlenet/system.reg");
    snprintf(games, sizeof(games), "%s", at("volumes/UbiDrive/MetalSharp Ubisoft Games"));
    make_file("volumes/UbiDrive/MetalSharp Ubisoft Games/Far Cry/game.exe");
    save_location(home, "ubisoft", at("volumes/UbiDrive"), games, "y:", games);
    assert(flag(ms_ubisoft_status_json(home), "installed"));

    r = uninstall(home, "{\"launcher\":\"ubisoft\",\"delete_games\":false}");
    assert(r.status == 200 && strstr(r.raw, "\"ok\":true"));
    assert(removed_has(&r, in_home(home, "prefix-ubisoft")));
    assert(removed_has(&r, in_home(home, "cache/ubisoft-connect/state.json")));
    assert(removed_has(&r, in_home(home, "cache/ubisoft-connect/UbisoftConnectInstaller.exe")));
    assert(removed_has(&r, in_home(home, "bottles/ubisoft_2147483650")));
    assert(removed_has(&r, in_home(home, "config/game-executables/ubisoft")));
    assert(removed_count(&r) == 5);
    assert(kept_is(&r, games));
    done(&r);
    assert(!home_has(home, "prefix-ubisoft"));
    assert(exists("outside/keep.txt")); /* the drive link lost only itself */
    assert(home_has(home, "cache/ubisoft-connect/artwork/1073741825.png"));
    assert(home_has(home, "bottles/battlenet_536870913/bottle.json"));
    assert(home_has(home, "bottles/rockstar_1073741825/bottle.json"));
    assert(home_has(home, "prefix-battlenet/system.reg"));
    assert_shared_sentinels(home);
    assert(exists("volumes/UbiDrive/MetalSharp Ubisoft Games/Far Cry/game.exe"));
    {
        ms_library_location location;
        assert(ms_library_location_get(home, "ubisoft", &location, NULL));
        assert(!strcmp(location.path, games) && !location.drive[0]); /* kept; the mapping went with the prefix */
    }
    assert(!flag(ms_ubisoft_status_json(home), "installed"));

    /* Again: nothing left to remove, and still fine. */
    r = uninstall(home, "{\"launcher\":\"ubisoft\",\"delete_games\":false}");
    assert(r.status == 200 && removed_count(&r) == 0 && kept_is(&r, games));
    done(&r);
}

static void test_battlenet_deletes_games(void) {
    char* home = new_home("battlenet", false);
    char games[PATH_MAX];
    reply r;
    shared_sentinels(home);
    home_file(home, "prefix-battlenet/system.reg");
    home_file(home, "prefix-battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net.exe");
    home_file(home, "cache/battlenet/state.json");
    home_file(home, "cache/battlenet/client.pid");
    home_file(home, "bottles/battlenet_536870913/bottle.json");
    home_file(home, "bottles/ubisoft_2147483650/bottle.json");
    home_file(home, "config/game-executables/battlenet/fenris.txt");
    home_file(home, "prefix-ubisoft/system.reg");
    snprintf(games, sizeof(games), "%s", at("user/Games/MetalSharp Battle.net Games"));
    make_file("user/Games/MetalSharp Battle.net Games/Diablo IV/Diablo IV.exe");
    make_dir("precious");
    make_file("precious/photo.jpg");
    make_link("user/Games/MetalSharp Battle.net Games/link-out", at("precious"));
    make_file("user/Games/MetalSharp GOG Games/keep.exe");
    save_location(home, "battlenet", "internal", games, NULL, NULL);
    assert(flag(ms_battlenet_status_json(home), "installed"));

    r = uninstall(home, "{\"launcher\":\"battlenet\",\"delete_games\":true}");
    assert(r.status == 200);
    assert(removed_has(&r, in_home(home, "prefix-battlenet")));
    assert(removed_has(&r, in_home(home, "cache/battlenet")));
    assert(removed_has(&r, in_home(home, "bottles/battlenet_536870913")));
    assert(removed_has(&r, in_home(home, "config/game-executables/battlenet")));
    assert(removed_has(&r, games));
    assert(removed_count(&r) == 5);
    assert(kept_is(&r, NULL));
    done(&r);
    assert(!exists("user/Games/MetalSharp Battle.net Games"));
    assert(exists("precious/photo.jpg"));
    assert(exists("user/Games/MetalSharp GOG Games/keep.exe"));
    assert(exists("user/Games"));
    assert(home_has(home, "bottles/ubisoft_2147483650/bottle.json"));
    assert(home_has(home, "prefix-ubisoft/system.reg"));
    assert_shared_sentinels(home);
    assert(!ms_library_location_get(home, "battlenet", NULL, NULL));
    assert(!flag(ms_battlenet_status_json(home), "installed"));
}

static void test_rockstar(void) {
    char* home = new_home("rockstar", false);
    char games[PATH_MAX], other_drive[PATH_MAX];
    reply r;
    shared_sentinels(home);
    home_file(home, "prefix-steam/user.reg");
    home_file(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe");
    home_file(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Social Club/socialclub.dll");
    /* A title the launcher installed in the prefix, and a Steam copy of another: both stay. */
    home_file(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Grand Theft Auto V/GTA5.exe");
    home_file(home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/common/Red Dead Redemption 2/RDR2.exe");
    home_file(home, "prefix-steam/drive_c/Program Files (x86)/Rockstar Games/Launcher/x86.dll");
    home_file(home, "prefix-steam/drive_c/ProgramData/Rockstar Games/Launcher/settings.json");
    home_file(home, "prefix-steam/drive_c/users/steamuser/Documents/Rockstar Games/GTA V/settings.xml");
    home_file(home, "cache/rockstar-launcher/state.json");
    home_file(home, "cache/rockstar-launcher/launcher-lane");
    home_file(home, "bottles/rockstar_1073741825/bottle.json");
    home_file(home, "config/game-executables/rockstar/grand-theft-auto-v.txt");
    home_file(home, "cache/ubisoft-connect/artwork/1073741825.png");
    /* The Rockstar games folder on an external drive, mapped as R:; D: is a Steam library. */
    make_dir("volumes/Ext");
    snprintf(games, sizeof(games), "%s", at("volumes/Ext/MetalSharp Rockstar Games"));
    make_file("volumes/Ext/MetalSharp Rockstar Games/Red Dead Redemption/RDR.exe");
    make_file("volumes/Ext/SteamLibrary/steamapps/libraryfolders.vdf");
    snprintf(other_drive, sizeof(other_drive), "%s", at("volumes/Ext/SteamLibrary"));
    make_dir_at(in_home(home, "prefix-steam/dosdevices"));
    make_link("rockstar/prefix-steam/dosdevices/r:", games);
    make_link("rockstar/prefix-steam/dosdevices/d:", other_drive);
    make_link("rockstar/prefix-steam/dosdevices/c:", "../drive_c");
    save_location(home, "rockstar", at("volumes/Ext"), games, "r:", games);
    assert(flag(ms_rockstar_status_json(home), "installed"));

    r = uninstall(home, "{\"launcher\":\"rockstar\",\"delete_games\":true}");
    assert(r.status == 200);
    assert(removed_has(&r, in_home(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher")));
    assert(removed_has(&r, in_home(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Social Club")));
    assert(removed_has(&r, in_home(home, "prefix-steam/drive_c/Program Files (x86)/Rockstar Games/Launcher")));
    assert(removed_has(&r, in_home(home, "prefix-steam/drive_c/Program Files (x86)/Rockstar Games")));
    assert(removed_has(&r, in_home(home, "prefix-steam/drive_c/ProgramData/Rockstar Games/Launcher")));
    assert(removed_has(&r, in_home(home, "prefix-steam/drive_c/ProgramData/Rockstar Games")));
    assert(removed_has(&r, in_home(home, "cache/rockstar-launcher")));
    assert(removed_has(&r, in_home(home, "bottles/rockstar_1073741825")));
    assert(removed_has(&r, in_home(home, "config/game-executables/rockstar")));
    assert(removed_has(&r, in_home(home, "prefix-steam/dosdevices/r:")));
    assert(removed_has(&r, games));
    assert(removed_count(&r) == 11);
    assert(kept_is(&r, NULL));
    done(&r);
    /* Steam's prefix keeps everything else. */
    assert_shared_sentinels(home);
    assert(home_has(home, "prefix-steam/user.reg"));
    assert(home_has(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Grand Theft Auto V/GTA5.exe"));
    assert(home_has(home,
                    "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/common/Red Dead Redemption 2/RDR2.exe"));
    assert(home_has(home, "prefix-steam/drive_c/users/steamuser/Documents/Rockstar Games/GTA V/settings.xml"));
    assert(home_has(home, "prefix-steam/dosdevices/d:"));
    assert(home_has(home, "prefix-steam/dosdevices/c:"));
    assert(home_has(home, "cache/ubisoft-connect/artwork/1073741825.png"));
    assert(exists("volumes/Ext/SteamLibrary/steamapps/libraryfolders.vdf"));
    assert(!exists("volumes/Ext/MetalSharp Rockstar Games"));
    assert(!ms_library_location_get(home, "rockstar", NULL, NULL));
    assert(!flag(ms_rockstar_status_json(home), "installed"));
}

static void test_rockstar_keeps_user_drive(void) {
    char* home = new_home("rockstar-user-drive", false);
    char games[PATH_MAX], elsewhere[PATH_MAX];
    reply r;
    shared_sentinels(home);
    home_file(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe");
    snprintf(games, sizeof(games), "%s", at("user/Games/MetalSharp Rockstar Games"));
    make_dir("user/Games/MetalSharp Rockstar Games");
    make_dir("user/elsewhere");
    snprintf(elsewhere, sizeof(elsewhere), "%s", at("user/elsewhere"));
    make_dir_at(in_home(home, "prefix-steam/dosdevices"));
    /* R: was repointed by the user since MetalSharp mapped it: it stays. */
    make_link("rockstar-user-drive/prefix-steam/dosdevices/r:", elsewhere);
    save_location(home, "rockstar", "internal", games, "r:", games);
    r = uninstall(home, "{\"launcher\":\"rockstar\",\"delete_games\":false}");
    assert(r.status == 200 && kept_is(&r, games));
    assert(!removed_has(&r, in_home(home, "prefix-steam/dosdevices/r:")));
    done(&r);
    assert(home_has(home, "prefix-steam/dosdevices/r:"));
    assert(exists("user/Games/MetalSharp Rockstar Games"));
    assert_shared_sentinels(home);
}

static void test_gog(void) {
    char* home = new_home("gog", false);
    reply r;
    shared_sentinels(home);
    home_file(home, "bottles/gog-prefix/prefix/system.reg");
    home_file(home, "bottles/gog-prefix/prefix/drive_c/windows/win.ini");
    home_file(home, "config/game-executables/gog/1207658924.txt");
    home_file(home, "gog_store/auth.json");
    home_file(home, "gog/library.json");
    r = uninstall(home, "{\"launcher\":\"gog\",\"delete_games\":true}"); /* no folder saved: nothing to delete */
    assert(r.status == 200);
    assert(removed_has(&r, in_home(home, "bottles/gog-prefix")));
    assert(removed_has(&r, in_home(home, "config/game-executables/gog")));
    assert(removed_count(&r) == 2 && kept_is(&r, NULL));
    done(&r);
    assert(!home_has(home, "bottles/gog-prefix"));
    assert(home_has(home, "gog_store/auth.json")); /* signed in, as Remove Prefix leaves it */
    assert(home_has(home, "gog/library.json"));
    assert_shared_sentinels(home);
    {
        char* raw = ms_gog_status_json(home);
        assert(raw && strstr(raw, "\"prefixInitialized\":false"));
        free(raw);
    }
}

static void test_epic(void) {
    char* home = new_home("epic", false);
    reply r;
    char* raw;
    shared_sentinels(home);
    home_file(home, "bottles/epic_Fortnite/prefix/system.reg");
    home_file(home, "bottles/epic_Fortnite/bottle.json");
    home_file(home, "epic/legendary/user.json");
    home_file(home, "epic/legendary/installed.json");
    home_file(home, "epic/legendary/metadata/Fortnite.json");
    home_file(home, "epic/library.json");
    home_file(home, "epic/logs/legendary.log");
    home_file(home, "tools/legendary/legendary-0.21.0");
    home_file(home, "tools/gogdl");
    home_file(home, "config/game-executables/epic/Fortnite.txt");
    r = uninstall(home, "{\"launcher\":\"epic\",\"delete_games\":false}");
    assert(r.status == 200);
    assert(removed_has(&r, in_home(home, "bottles/epic_Fortnite")));
    assert(removed_has(&r, in_home(home, "epic/legendary")));
    assert(removed_has(&r, in_home(home, "epic/library.json")));
    assert(removed_has(&r, in_home(home, "config/game-executables/epic")));
    assert(removed_has(&r, in_home(home, "tools/legendary")));
    assert(removed_count(&r) == 5 && kept_is(&r, NULL));
    done(&r);
    assert(home_has(home, "epic/logs/legendary.log"));
    assert(home_has(home, "tools/gogdl"));
    assert_shared_sentinels(home);
    raw = ms_epic_status_json(home);
    assert(raw && strstr(raw, "\"toolAvailable\":false") && strstr(raw, "\"authenticated\":false"));
    free(raw);
}

/* delete_games with a folder that fails a check: 409 and nothing removed. */
static void refused(const char* name, const char* launcher, const char* volume, const char* path) {
    char* home = new_home(name, false);
    char body[128];
    reply r;
    home_file(home, "prefix-ubisoft/system.reg");
    home_file(home, "prefix-battlenet/system.reg");
    save_location(home, launcher, volume, path, NULL, NULL);
    snprintf(body, sizeof(body), "{\"launcher\":\"%s\",\"delete_games\":true}", launcher);
    r = uninstall(home, body);
    assert(r.status == 409 && strstr(r.raw, "\"ok\":false") && strstr(r.raw, "Nothing was removed"));
    done(&r);
    assert(home_has(home, "prefix-ubisoft/system.reg") && home_has(home, "prefix-battlenet/system.reg"));
    assert(ms_library_location_get(home, launcher, NULL, NULL));
    /* Keeping the folder still uninstalls. */
    snprintf(body, sizeof(body), "{\"launcher\":\"%s\",\"delete_games\":false}", launcher);
    r = uninstall(home, body);
    assert(r.status == 200 && kept_is(&r, path));
    done(&r);
}

static void test_games_folder_checks(void) {
    /* A link where the folder should be (to a folder that must survive). */
    make_file("linked-target/MetalSharp Ubisoft Games/save.dat");
    make_dir("user/Games");
    make_link("user/Games/MetalSharp Ubisoft Games", at("linked-target/MetalSharp Ubisoft Games"));
    refused("refuse-link", "ubisoft", "internal", at("user/Games/MetalSharp Ubisoft Games"));
    assert(exists("linked-target/MetalSharp Ubisoft Games/save.dat"));
    assert(unlink(at("user/Games/MetalSharp Ubisoft Games")) == 0);

    /* Not the launcher's folder name (another launcher's, or any other). */
    make_file("user/Games/MetalSharp Ubisoft Games/keep.exe");
    refused("refuse-other-launcher", "battlenet", "internal", at("user/Games/MetalSharp Ubisoft Games"));
    make_file("user/Games/Ubisoft/keep.exe");
    refused("refuse-name", "ubisoft", "internal", at("user/Games/Ubisoft"));
    assert(exists("user/Games/MetalSharp Ubisoft Games/keep.exe") && exists("user/Games/Ubisoft/keep.exe"));

    /* The right name under another parent. */
    make_file("user/Documents/MetalSharp Ubisoft Games/keep.exe");
    refused("refuse-parent", "ubisoft", "internal", at("user/Documents/MetalSharp Ubisoft Games"));
    make_file("volumes/Ext2/Nested/MetalSharp Ubisoft Games/keep.exe");
    refused("refuse-nested", "ubisoft", at("volumes/Ext2/Nested"), at("volumes/Ext2/Nested/MetalSharp Ubisoft Games"));
    refused("refuse-mismatch", "ubisoft", at("volumes/Ext2"), at("user/Games/MetalSharp Ubisoft Games"));
    assert(exists("user/Documents/MetalSharp Ubisoft Games/keep.exe"));
    assert(exists("volumes/Ext2/Nested/MetalSharp Ubisoft Games/keep.exe"));

    /* A drive that is not mounted, and one that is only a link in the volumes folder. */
    refused("refuse-unmounted", "ubisoft", at("volumes/Gone"), at("volumes/Gone/MetalSharp Ubisoft Games"));
    make_file("alias-target/MetalSharp Ubisoft Games/keep.exe");
    make_link("volumes/Alias", at("alias-target"));
    refused("refuse-alias", "ubisoft", at("volumes/Alias"), at("volumes/Alias/MetalSharp Ubisoft Games"));
    assert(exists("alias-target/MetalSharp Ubisoft Games/keep.exe"));
    assert(unlink(at("volumes/Alias")) == 0);

    /* A file where the folder should be. */
    make_dir("volumes/Ext3");
    make_file("volumes/Ext3/MetalSharp Ubisoft Games");
    refused("refuse-file", "ubisoft", at("volumes/Ext3"), at("volumes/Ext3/MetalSharp Ubisoft Games"));
    assert(exists("volumes/Ext3/MetalSharp Ubisoft Games"));

    /* Saved and valid but already gone: forgotten, nothing deleted. */
    {
        char* home = new_home("absent-folder", false);
        reply r;
        make_dir("volumes/Ext4");
        save_location(home, "epic", at("volumes/Ext4"), at("volumes/Ext4/MetalSharp Epic Games"), NULL, NULL);
        r = uninstall(home, "{\"launcher\":\"epic\",\"delete_games\":true}");
        assert(r.status == 200 && kept_is(&r, NULL) && removed_count(&r) == 0);
        done(&r);
        assert(!ms_library_location_get(home, "epic", NULL, NULL));
        assert(exists("volumes/Ext4"));
    }
}

static void test_still_running(void) {
    char* home = new_home("stuck", true);
    reply r;
    home_file(home, "prefix-ubisoft/system.reg");
    home_file(home, "prefix-battlenet/system.reg");
    home_file(home, "bottles/gog-prefix/prefix/system.reg");
    r = uninstall(home, "{\"launcher\":\"ubisoft\",\"delete_games\":false}");
    assert(r.status == 409 && strstr(r.raw, "still running"));
    done(&r);
    r = uninstall(home, "{\"launcher\":\"battlenet\",\"delete_games\":false}");
    assert(r.status == 409 && strstr(r.raw, "still running"));
    done(&r);
    r = uninstall(home, "{\"launcher\":\"gog\",\"delete_games\":false}");
    assert(r.status == 409 && strstr(r.raw, "still running"));
    done(&r);
    assert(home_has(home, "prefix-ubisoft/system.reg"));
    assert(home_has(home, "prefix-battlenet/system.reg"));
    assert(home_has(home, "bottles/gog-prefix/prefix/system.reg"));
}

static void test_links_inside_home(void) {
    /* bottles is a link out of the home: nothing is removed through it. */
    char* home = new_home("linked-bottles", false);
    reply r;
    make_file("bottles-elsewhere/battlenet_7/bottle.json");
    make_link("linked-bottles/bottles", at("bottles-elsewhere"));
    r = uninstall(home, "{\"launcher\":\"battlenet\",\"delete_games\":false}");
    assert(r.status == 500 && strstr(r.raw, "\"path\""));
    done(&r);
    assert(exists("bottles-elsewhere/battlenet_7/bottle.json"));
}

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(realpath(argv[1], base));
    make_dir("user");
    make_dir("volumes");
    assert(setenv("HOME", at("user"), 1) == 0);
    assert(setenv(MS_LIBRARY_VOLUMES_ROOT_ENV, at("volumes"), 1) == 0);
    assert(unsetenv("METALSHARP_EPIC_LEGENDARY_BIN") == 0);
    assert(unsetenv("METALSHARP_GOGDL_BIN") == 0);
    test_bad_requests();
    test_ubisoft();
    test_battlenet_deletes_games();
    test_rockstar();
    test_rockstar_keeps_user_drive();
    test_gog();
    test_epic();
    test_games_folder_checks();
    test_still_running();
    test_links_inside_home();
    printf("launcher uninstall test passed (%s)\n", ms_wine_variant());
    return 0;
}
