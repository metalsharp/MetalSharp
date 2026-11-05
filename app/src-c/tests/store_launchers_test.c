/* The Rockstar Games Launcher and Battle.net integrations, without Wine, a download or a real
 * prefix: registry parsing, library rules, appid namespaces, path conversion, process matching,
 * per-game routes and executables, and the routes' answers before anything is installed. */
// clang-format off
#include "../runtime/rockstar.c"
#include "../runtime/battlenet.c"
// clang-format on
#include "metalsharp_backend/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fixture(const char* home, const char* relative, const char* bytes) {
    char* path = ms_store_path_join(home, relative);
    char* parent = path ? strdup(path) : NULL;
    FILE* file;
    assert(path && parent && strrchr(parent, '/'));
    *strrchr(parent, '/') = '\0';
    assert(ms_store_mkdir_p(parent));
    file = fopen(path, "wb");
    assert(file);
    assert(fputs(bytes, file) >= 0);
    assert(fclose(file) == 0);
    free(parent);
    free(path);
}

static void sized_fixture(const char* home, const char* relative, size_t size) {
    char* bytes = malloc(size + 1);
    assert(bytes);
    memset(bytes, 'x', size);
    bytes[size] = '\0';
    fixture(home, relative, bytes);
    free(bytes);
}

static bool exists(const char* home, const char* relative) {
    char* path = ms_store_path_join(home, relative);
    bool found = path && access(path, F_OK) == 0;
    free(path);
    return found;
}

static ms_json* parse(const char* raw) {
    ms_json* json = raw ? ms_json_parse(raw, strlen(raw), NULL, 0) : NULL;
    assert(json);
    return json;
}

static const ms_json* game_with(const ms_json* games, const char* key, const char* value) {
    for (size_t i = 0; i < ms_json_array_length(games); i++) {
        const ms_json* game = ms_json_array_get(games, i);
        char* text = NULL;
        bool match = ms_json_as_string(ms_json_object_get(game, key), &text) && !strcmp(text, value);
        free(text);
        if (match)
            return game;
    }
    return NULL;
}

static bool string_is(const ms_json* object, const char* key, const char* expected) {
    char* text = NULL;
    bool match = ms_json_as_string(ms_json_object_get(object, key), &text) && !strcmp(text, expected);
    free(text);
    return match;
}

static bool string_ends_with(const ms_json* object, const char* key, const char* suffix) {
    char* text = NULL;
    bool match = ms_json_as_string(ms_json_object_get(object, key), &text) && strlen(text) >= strlen(suffix) &&
                 !strcmp(text + strlen(text) - strlen(suffix), suffix);
    free(text);
    return match;
}

static unsigned long long number(const ms_json* object, const char* key) {
    long long value = -1;
    assert(ms_json_as_i64(ms_json_object_get(object, key), &value) && value >= 0);
    return (unsigned long long)value;
}

/* The route DLLs save-pipeline stages on the regular build (the fex build stages none). */
static void route_fixtures(const char* home) {
    static const char* const files[] = {"vkd3d/vkd3d-proton/x86_64-windows/d3d12.dll",
                                        "vkd3d/vkd3d-proton/x86_64-windows/d3d12core.dll",
                                        "vkd3d/vkd3d-proton/x86_64-windows/dxgi.dll",
                                        "vkd3d/dxvk/x86_64-windows/d3d11.dll",
                                        "vkd3d/dxvk/x86_64-windows/d3d10core.dll",
                                        "vkd3d/dxvk/x86_64-windows/d3d9.dll",
                                        "runtime/wine/lib/dxmt/x86_64-windows/d3d11.dll",
                                        "runtime/wine/lib/dxmt/x86_64-windows/d3d10core.dll",
                                        "runtime/wine/lib/dxmt/x86_64-windows/dxgi.dll",
                                        "runtime/wine/lib/dxmt/x86_64-windows/winemetal.dll"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        fixture(home, files[i], "route dll");
}

static void test_appid_namespaces(void) {
    static const char* const ids[] = {"grand-theft-auto-v", "red-dead-redemption-2", "fenris", "wow", "", "x"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        unsigned rockstar = rockstar_appid(ids[i]), battlenet = battlenet_appid(ids[i]);
        /* Rockstar: bit 30 without 31. Battle.net: bit 29 without 30 or 31. Ubisoft, GOG, Epic: bit 31.
         * Steam's app ids stay far below bit 29. */
        assert((rockstar & 0xc0000000u) == 0x40000000u);
        assert((battlenet & 0xe0000000u) == 0x20000000u);
        assert(rockstar != battlenet && rockstar >= 0x40000000u && battlenet >= 0x20000000u);
        assert(rockstar_appid(ids[i]) == rockstar);
    }
    assert(rockstar_appid("grand-theft-auto-v") != rockstar_appid("red-dead-redemption-2"));
}

static void test_helpers(const char* home) {
    char slug[64], uid[64];
    char* prefix = ms_store_path_join(home, "paths-prefix");
    char* volume = ms_store_path_join(home, "paths-volume");
    char* host;
    char resolved[PATH_MAX];
    assert(prefix && volume && ms_store_mkdir_p(volume) && ms_store_mkdir_p(prefix));
    assert(ms_store_slug("Grand Theft Auto V", slug, sizeof(slug)) && !strcmp(slug, "grand-theft-auto-v"));
    assert(ms_store_slug("  L.A. Noire: The Complete Edition ", slug, sizeof(slug)) &&
           !strcmp(slug, "l-a-noire-the-complete-edition"));
    assert(!ms_store_slug("!!", slug, sizeof(slug)));
    assert(ms_store_valid_id("wow_classic") && ms_store_valid_id("fenris") && !ms_store_valid_id("a b") &&
           !ms_store_valid_id("..") && !ms_store_valid_id(""));

    host = ms_store_wine_path_to_host(prefix, "C:\\Program Files\\Rockstar Games\\Grand Theft Auto V\\");
    assert(host && strstr(host, "/paths-prefix/drive_c/Program Files/Rockstar Games/Grand Theft Auto V") &&
           host[strlen(host) - 1] == 'V');
    free(host);
    assert(!ms_store_wine_path_to_host(prefix, "C:\\Games\\..\\..\\etc"));
    assert(!ms_store_wine_path_to_host(prefix, "\\\\server\\share"));
    assert(!ms_store_wine_path_to_host(prefix, "Games\\Title"));
    assert(!ms_store_wine_path_to_host(prefix, "Y:\\Games")); /* no Y: yet */
    assert(ms_store_ensure_drive_mapping(prefix, volume));
    host = ms_store_wine_path_to_host(prefix, "y:/Games/Title");
    assert(host && realpath(volume, resolved) && !strncmp(host, resolved, strlen(resolved)) &&
           !strcmp(host + strlen(resolved), "/Games/Title"));
    free(host);

    assert(ms_store_path_in_steam_library("C:\\Program Files (x86)\\Steam\\steamapps\\common\\GTA V", NULL));
    assert(ms_store_path_in_steam_library("D:\\SteamLibrary\\SteamApps\\common\\GTA V", NULL));
    assert(ms_store_path_in_steam_library(NULL, "/Volumes/Games/SteamLibrary/steamapps/common/GTA V"));
    assert(!ms_store_path_in_steam_library("C:\\Program Files\\Rockstar Games\\Grand Theft Auto V", "/tmp/x"));

    assert(ms_store_command_runs("C:\\Program Files\\Rockstar Games\\Grand Theft Auto V\\GTA5.exe -fullscreen",
                                 "/host/GTA5.exe"));
    assert(ms_store_command_runs("\"C:/Games/Diablo IV/Diablo IV.exe\"", "Diablo IV.exe"));
    assert(!ms_store_command_runs("C:\\Games\\NotGTA5.exe", "GTA5.exe"));
    assert(!ms_store_command_runs("C:\\Games\\GTA5.exe.bak", "GTA5.exe"));
    assert(ms_store_command_in_windows_dir("C:\\Program Files\\Rockstar Games\\Max Payne 3\\MaxPayne3.exe",
                                           "c:\\program files\\rockstar games\\max payne 3\\"));
    assert(ms_store_command_in_windows_dir("C:/Program Files/Rockstar Games/Max Payne 3/MaxPayne3.exe",
                                           "C:\\Program Files\\Rockstar Games\\Max Payne 3"));
    assert(!ms_store_command_in_windows_dir("C:\\Program Files\\Rockstar Games\\Max Payne 30\\x.exe",
                                            "C:\\Program Files\\Rockstar Games\\Max Payne 3"));

    {
        char* value = ms_store_registry_string("\"\\\"C:\\\\ProgramData\\\\Battle.net\\\\Agent\\\\Blizzard "
                                               "Uninstaller.exe\\\" --uid=fenris\"");
        assert(value &&
               !strcmp(value, "\"C:\\ProgramData\\Battle.net\\Agent\\Blizzard Uninstaller.exe\" --uid=fenris"));
        assert(battlenet_uid_from_uninstall(value, uid, sizeof(uid)) && !strcmp(uid, "fenris"));
        free(value);
        assert(!ms_store_registry_string("dword:00000001"));
        assert(ms_store_registry_value_is("\"InstallFolder\"=\"C:\\\\x\"", "InstallFolder"));
        assert(!ms_store_registry_value_is("\"InstallFolderSteam\"=\"C:\\\\x\"", "InstallFolder"));
    }
    assert(battlenet_uid_from_uninstall("Uninstaller.exe --lang=enUS --uid=wow_enus --displayname=\"World\"", uid,
                                        sizeof(uid)) &&
           !strcmp(uid, "wow_enus"));
    assert(battlenet_uid_from_uninstall("Uninstaller.exe --uid=\"prometheus\"", uid, sizeof(uid)) &&
           !strcmp(uid, "prometheus"));
    assert(!battlenet_uid_from_uninstall("Uninstaller.exe --lang=enUS", uid, sizeof(uid)));
    assert(!battlenet_uid_from_uninstall("Uninstaller.exe --notuid=x", uid, sizeof(uid)));
    free(prefix);
    free(volume);
}

static void test_rockstar(const char* home) {
    static const char registry[] =
        "WINE REGISTRY Version 2\n"
        "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Rockstar Games Launcher] 1700000000\n"
        "#time=1d9\n"
        "\"DisplayName\"=\"Rockstar Games Launcher\"\n"
        "\"InstallLocation\"=\"C:\\\\Program Files\\\\Rockstar Games\\\\Launcher\"\n"
        "\"Publisher\"=\"Rockstar Games\"\n"
        "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\"
        "{5EFC6C07-6B87-43FC-9524-F9E967241741}] 1\n"
        "\"DisplayName\"=\"Grand Theft Auto V\"\n"
        "\"InstallLocation\"=\"C:\\\\Program Files\\\\Rockstar Games\\\\Grand Theft Auto V\"\n"
        "\"Publisher\"=\"Rockstar Games\"\n"
        "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\{MP3}] 1\n"
        "\"DisplayName\"=\"Max Payne 3\"\n"
        "\"InstallLocation\"=\"C:\\\\Games\\\\Max Payne 3\\\\\"\n"
        "\"Publisher\"=\"Rockstar Games\"\n"
        "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Other] 1\n"
        "\"DisplayName\"=\"Other Game\"\n"
        "\"InstallLocation\"=\"C:\\\\Games\\\\Other\"\n"
        "\"Publisher\"=\"Someone Else\"\n"
        "[Software\\\\Wow6432Node\\\\Rockstar Games\\\\Grand Theft Auto V] 1\n"
        "\"InstallFolder\"=\"C:\\\\Program Files\\\\Rockstar Games\\\\Grand Theft Auto V\"\n"
        "[Software\\\\Wow6432Node\\\\Rockstar Games\\\\Grand Theft Auto V\\\\Settings] 1\n"
        "\"InstallFolder\"=\"C:\\\\Program Files\\\\Rockstar Games\\\\Grand Theft Auto V\\\\update\"\n"
        "[Software\\\\Wow6432Node\\\\Rockstar Games\\\\Launcher] 1\n"
        "\"InstallFolder\"=\"C:\\\\Program Files\\\\Rockstar Games\\\\Launcher\"\n"
        "[Software\\\\Wow6432Node\\\\Rockstar Games\\\\Rockstar Games Social Club] 1\n"
        "\"InstallFolder\"=\"C:\\\\Program Files\\\\Rockstar Games\\\\Social Club\"\n"
        "[Software\\\\Wow6432Node\\\\Rockstar Games\\\\Red Dead Redemption 2] 1\n"
        "\"InstallFolder\"=\"C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\common\\\\Red Dead Redemption 2\"\n"
        "[Software\\\\Wow6432Node\\\\Rockstar Games\\\\L.A. Noire] 1\n"
        "\"InstallFolder\"=\"C:\\\\Program Files\\\\Rockstar Games\\\\Missing\"\n";
    const char* gta = "prefix-steam/drive_c/Program Files/Rockstar Games/Grand Theft Auto V";
    char path[PATH_MAX];
    int status = 0;
    char* raw;
    ms_json* root;
    const ms_json* games;
    const ms_json* game;
    unsigned long long appid;

    /* No Steam prefix: no games, nothing installed, and a launch installs (not here: the request needs
     * a launcher to be absent and only reports the 202 path through the worker, so check status only). */
    raw = ms_rockstar_library_json(home);
    root = parse(raw);
    assert(ms_json_array_length(ms_json_object_get(root, "games")) == 0);
    ms_json_free(root);
    free(raw);
    raw = ms_rockstar_status_json(home);
    assert(raw && strstr(raw, "\"installed\":false") && strstr(raw, "\"running\":false") &&
           strstr(raw, "\"installing\":false") && strstr(raw, "\"state\":\"not_installed\""));
    free(raw);

    fixture(home, "prefix-steam/system.reg", registry);
    snprintf(path, sizeof(path), "%s/GTA5.exe", gta);
    sized_fixture(home, path, 4096);
    snprintf(path, sizeof(path), "%s/PlayGTAV.exe", gta);
    sized_fixture(home, path, 512);
    snprintf(path, sizeof(path), "%s/Installers/Setup.exe", gta);
    sized_fixture(home, path, 9000);
    sized_fixture(home, "prefix-steam/drive_c/Games/Max Payne 3/MaxPayne3.exe", 2048);
    sized_fixture(home, "prefix-steam/drive_c/Games/Max Payne 3/uninstall.exe", 9000);
    sized_fixture(home, "prefix-steam/drive_c/Games/Other/Other.exe", 100);
    sized_fixture(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe", 100);
    sized_fixture(home, "prefix-steam/drive_c/Program Files/Rockstar Games/Social Club/SocialClubHelper.exe", 100);
    sized_fixture(home,
                  "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/common/Red Dead Redemption 2/"
                  "RDR2.exe",
                  100);

    assert(rockstar_title_key("Software\\Wow6432Node\\Rockstar Games\\Grand Theft Auto V") &&
           !strcmp(rockstar_title_key("Software\\Wow6432Node\\Rockstar Games\\Grand Theft Auto V"),
                   "Grand Theft Auto V"));
    assert(rockstar_title_key("Software\\Rockstar Games\\Bully"));
    assert(!rockstar_title_key("Software\\Wow6432Node\\Rockstar Games\\GTA V\\Settings"));
    assert(!rockstar_title_key("Software\\Wow6432Node\\Rockstar Games\\"));
    assert(!rockstar_title_key("Software\\Wow6432Node\\Ubisoft\\Launcher"));
    assert(rockstar_excluded_folder("C:\\Program Files\\Rockstar Games\\Launcher\\"));
    assert(rockstar_excluded_folder("C:\\Program Files\\Rockstar Games\\Social Club"));
    assert(!rockstar_excluded_folder("C:\\Program Files\\Rockstar Games\\Grand Theft Auto V"));
    assert(rockstar_excluded_name("Rockstar Games Launcher") && rockstar_excluded_name("Rockstar Games Social Club") &&
           !rockstar_excluded_name("Grand Theft Auto V"));

    raw = ms_rockstar_library_json(home);
    root = parse(raw);
    games = ms_json_object_get(root, "games");
    /* GTA V once (its Uninstall entry and title key), Max Payne 3; not the launcher, Social Club, the
     * Steam copy of RDR2, a settings subkey, another publisher or a missing folder. */
    assert(ms_json_array_length(games) == 2);
    game = game_with(games, "rockstar_id", "grand-theft-auto-v");
    assert(game && string_is(game, "name", "Grand Theft Auto V") && string_is(game, "source", "rockstar") &&
           string_is(game, "launch_method", "rockstar") && string_is(game, "preferred_pipeline", "d3dmetal") &&
           string_ends_with(game, "executable_path", "/Grand Theft Auto V/PlayGTAV.exe") &&
           string_ends_with(game, "game_dir", "/drive_c/Program Files/Rockstar Games/Grand Theft Auto V"));
    appid = number(game, "appid");
    assert(appid == rockstar_appid("grand-theft-auto-v"));
    snprintf(path, sizeof(path), "rockstar_%llu", appid);
    assert(string_is(game, "bottle_id", path));
    {
        const ms_json* pipelines = ms_json_object_get(game, "available_pipelines");
        bool recommended = false;
        assert(ms_json_array_length(pipelines) == 5);
        assert(string_is(ms_json_array_get(pipelines, 0), "id", "d3dmetal"));
        assert(ms_json_as_bool(ms_json_object_get(ms_json_array_get(pipelines, 0), "recommended"), &recommended) &&
               recommended);
        assert(ms_json_as_bool(ms_json_object_get(ms_json_array_get(pipelines, 1), "recommended"), &recommended) &&
               !recommended);
    }
    game = game_with(games, "rockstar_id", "max-payne-3");
    assert(game && string_is(game, "name", "Max Payne 3") &&
           string_ends_with(game, "executable_path", "/Max Payne 3/MaxPayne3.exe"));
    ms_json_free(root);
    free(raw);

    /* The route goes to the client beside Play*.exe. */
    {
        ms_store_game* found = NULL;
        size_t count = rockstar_scan_games(home, &found);
        char* client = NULL;
        assert(count == 2);
        for (size_t i = 0; i < count; i++)
            if (!strcmp(found[i].id, "grand-theft-auto-v"))
                client = rockstar_client_executable(&found[i], "/x/PlayGTAV.exe");
        assert(client && strstr(client, "/Grand Theft Auto V/GTA5.exe"));
        free(client);

        /* Stop's process set: the launcher, its helpers and these games, never Steam or a Steam copy. */
        rockstar_process_set set = {found, count};
        assert(rockstar_match_owned(1, 1, "C:\\Program Files\\Rockstar Games\\Launcher\\Launcher.exe", &set));
        assert(rockstar_match_owned(1, 1, "C:\\Program Files\\Rockstar Games\\Social Club\\SocialClubHelper.exe "
                                          "--type=renderer",
                                    &set));
        assert(rockstar_match_owned(1, 1, "C:\\windows\\RockstarService.exe", &set));
        assert(rockstar_match_owned(1, 1, "C:\\Program Files\\Rockstar Games\\Grand Theft Auto V\\GTA5.exe", &set));
        assert(rockstar_match_owned(1, 1, "C:\\Games\\Max Payne 3\\MaxPayne3.exe -windowed", &set));
        assert(!rockstar_match_owned(1, 1, "C:\\Program Files (x86)\\Steam\\steam.exe -silent", &set));
        assert(!rockstar_match_owned(1, 1, "C:\\Program Files (x86)\\Steam\\bin\\cef\\steamwebhelper.exe", &set));
        assert(!rockstar_match_owned(
            1, 1, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Red Dead Redemption 2\\RDR2.exe", &set));
        assert(!rockstar_match_owned(1, 1, "C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe", &set));
        assert(rockstar_launcher_command("C:\\Program Files\\Rockstar Games\\Launcher\\Launcher.exe"));
        assert(rockstar_launcher_command("/h/prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe"));
        assert(!rockstar_launcher_command("C:\\CCP\\EVE\\Launcher\\evelauncher.exe"));
        free(found);
    }
    {
        char lane[64];
        rockstar_lane_key("d3dmetal", "/g/GTA5_Enhanced.exe", lane, sizeof(lane));
        assert(!strcmp(lane, ms_wine_is_fex() ? "mullion" : "d3dmetal+agility"));
        rockstar_lane_key("vkd3d", "/g/GTA5.exe", lane, sizeof(lane));
        assert(!strcmp(lane, ms_wine_is_fex() ? "mullion" : "vkd3d"));
    }

    /* Status: the launcher is there, and nothing of it runs (no Wine in this home). */
    raw = ms_rockstar_status_json(home);
    assert(raw && strstr(raw, "\"installed\":true") && strstr(raw, "\"running\":false") &&
           strstr(raw, "\"state\":\"ready\""));
    free(raw);

    /* Per-game route and executable. */
    raw = ms_rockstar_save_pipeline_json(home, "{\"rockstar_id\":\"max-payne-3\",\"pipeline\":\"fna_arm64\"}",
                                         strlen("{\"rockstar_id\":\"max-payne-3\",\"pipeline\":\"fna_arm64\"}"),
                                         &status);
    assert(raw && status == 400);
    free(raw);
    raw = ms_rockstar_save_pipeline_json(home, "{\"rockstar_id\":\"nope\",\"pipeline\":\"dxmt\"}",
                                         strlen("{\"rockstar_id\":\"nope\",\"pipeline\":\"dxmt\"}"), &status);
    assert(raw && status == 404);
    free(raw);
    raw = ms_rockstar_save_pipeline_json(home, "{\"rockstar_id\":\"max-payne-3\",\"pipeline\":\"VKD3D\"}",
                                         strlen("{\"rockstar_id\":\"max-payne-3\",\"pipeline\":\"VKD3D\"}"), &status);
    assert(raw && status == 200 && strstr(raw, "\"pipeline\":\"vkd3d\""));
    free(raw);
    snprintf(path, sizeof(path), "bottles/rockstar_%u/bottle.json", rockstar_appid("max-payne-3"));
    assert(exists(home, path));
    raw = ms_rockstar_library_json(home);
    root = parse(raw);
    game = game_with(ms_json_object_get(root, "games"), "rockstar_id", "max-payne-3");
    assert(game && string_is(game, "preferred_pipeline", "vkd3d"));
    ms_json_free(root);
    free(raw);
    {
        char body[PATH_MAX * 2];
        snprintf(body, sizeof(body),
                 "{\"rockstar_id\":\"max-payne-3\",\"executablePath\":\"%s/prefix-steam/drive_c/Games/Max Payne "
                 "3/uninstall.exe\"}",
                 home);
        raw = ms_rockstar_save_executable_json(home, body, strlen(body), &status);
        assert(raw && status == 200);
        free(raw);
        snprintf(body, sizeof(body), "{\"rockstar_id\":\"max-payne-3\",\"executablePath\":\"/etc/hosts\"}");
        raw = ms_rockstar_save_executable_json(home, body, strlen(body), &status);
        assert(raw && status == 400);
        free(raw);
    }
    raw = ms_rockstar_library_json(home);
    root = parse(raw);
    game = game_with(ms_json_object_get(root, "games"), "rockstar_id", "max-payne-3");
    assert(game && string_ends_with(game, "executable_path", "/Max Payne 3/uninstall.exe"));
    ms_json_free(root);
    free(raw);

    /* Launches: an unknown game, and a game while no Wine runtime is in this home. */
    raw = ms_rockstar_launch_game_json(home, "{\"rockstar_id\":\"nope\"}", strlen("{\"rockstar_id\":\"nope\"}"),
                                       &status);
    assert(raw && status == 404);
    free(raw);
    {
        char body[64];
        snprintf(body, sizeof(body), "{\"appid\":%llu}", (unsigned long long)rockstar_appid("grand-theft-auto-v"));
        ms_store_game* found = NULL;
        size_t count = rockstar_scan_games(home, &found);
        assert(ms_store_request_game(&ROCKSTAR_PROVIDER, found, count, body, strlen(body), NULL));
        free(found);
    }
    /* Stop touches nothing here and reports stopped. */
    raw = ms_rockstar_stop_json(home, &status);
    assert(raw && status == 200 && strstr(raw, "\"running\":false"));
    free(raw);

    /* Without the launcher, a game launch reports it; its games remain listed. */
    snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/Program Files/Rockstar Games/Launcher/Launcher.exe", home);
    assert(unlink(path) == 0);
    raw = ms_rockstar_launch_game_json(home, "{\"rockstar_id\":\"grand-theft-auto-v\",\"pipeline\":null}",
                                       strlen("{\"rockstar_id\":\"grand-theft-auto-v\",\"pipeline\":null}"), &status);
    assert(raw && status == 409 && strstr(raw, "not installed"));
    free(raw);
}

static void test_battlenet(const char* home) {
    static const char registry[] =
        "WINE REGISTRY Version 2\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Battle.net] 1\n"
        "\"DisplayName\"=\"Battle.net\"\n"
        "\"InstallLocation\"=\"C:\\\\Program Files (x86)\\\\Battle.net\"\n"
        "\"Publisher\"=\"Blizzard Entertainment\"\n"
        "\"UninstallString\"=\"\\\"C:\\\\Program Files (x86)\\\\Battle.net\\\\Battle.net.exe\\\" --uninstall\"\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Call of Duty] 1\n"
        "\"DisplayName\"=\"Call of Duty\"\n"
        "\"InstallLocation\"=\"Y:\\\\Games\\\\Call of Duty\"\n"
        "\"Publisher\"=\"Activision\"\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Diablo IV] 1\n"
        "\"DisplayName\"=\"Diablo IV\"\n"
        "\"InstallLocation\"=\"C:\\\\Program Files (x86)\\\\Diablo IV\"\n"
        "\"Publisher\"=\"Blizzard Entertainment\"\n"
        "\"UninstallString\"=\"\\\"C:\\\\ProgramData\\\\Battle.net\\\\Agent\\\\Blizzard Uninstaller.exe\\\" "
        "--lang=enUS --uid=fenris --displayname=\\\"Diablo IV\\\"\"\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Not Blizzard] 1\n"
        "\"DisplayName\"=\"Not Blizzard\"\n"
        "\"InstallLocation\"=\"C:\\\\Games\\\\Other\"\n"
        "\"Publisher\"=\"Someone Else\"\n";
    char* volume = ms_store_path_join(home, "battlenet-volume");
    char* link = ms_store_path_join(home, "prefix-battlenet/dosdevices/y:");
    char* server;
    char* raw;
    ms_json* root;
    const ms_json* games;
    const ms_json* game;
    int status = 0;

    /* No prefix: nothing installed, nothing listed, and Stop has nothing to stop. */
    raw = ms_battlenet_status_json(home);
    assert(raw && strstr(raw, "\"installed\":false") && strstr(raw, "\"running\":false"));
    free(raw);
    raw = ms_battlenet_stop_json(home, &status);
    assert(raw && status == 200 && strstr(raw, "\"running\":false"));
    free(raw);

    fixture(home, "prefix-battlenet/system.reg", registry);
    sized_fixture(home, "prefix-battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net.exe", 100);
    sized_fixture(home, "prefix-battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe", 100);
    sized_fixture(home, "prefix-battlenet/drive_c/Program Files (x86)/Diablo IV/Diablo IV.exe", 4000);
    sized_fixture(home, "prefix-battlenet/drive_c/Program Files (x86)/Diablo IV/Diablo IV Launcher.exe", 9000);
    sized_fixture(home, "prefix-battlenet/drive_c/Program Files (x86)/Diablo IV/BlizzardError.exe", 9000);
    sized_fixture(home, "prefix-battlenet/drive_c/Games/Other/Other.exe", 100);
    sized_fixture(home, "battlenet-volume/Games/Call of Duty/_retail_/cod.exe", 3000);
    assert(volume && link && ms_store_mkdir_p(volume));
    {
        char* prefix = ms_store_path_join(home, "prefix-battlenet");
        assert(prefix && ms_store_ensure_drive_mapping(prefix, volume));
        free(prefix);
    }

    raw = ms_battlenet_library_json(home);
    root = parse(raw);
    games = ms_json_object_get(root, "games");
    /* Diablo IV (by its uid) and Call of Duty on Y: (by its folder); not Battle.net itself or
     * another publisher's game. */
    assert(ms_json_array_length(games) == 2);
    game = game_with(games, "battlenet_id", "fenris");
    assert(game && string_is(game, "name", "Diablo IV") && string_is(game, "source", "battlenet") &&
           string_is(game, "launch_method", "battlenet") &&
           string_ends_with(game, "executable_path", "/Diablo IV/Diablo IV.exe"));
    assert(number(game, "appid") == battlenet_appid("fenris"));
    {
        char bottle[64];
        snprintf(bottle, sizeof(bottle), "battlenet_%u", battlenet_appid("fenris"));
        assert(string_is(game, "bottle_id", bottle));
    }
    game = game_with(games, "battlenet_id", "call-of-duty");
    assert(game && string_ends_with(game, "executable_path", "/Games/Call of Duty/_retail_/cod.exe"));
    ms_json_free(root);
    free(raw);
    {
        ms_store_game* found = NULL;
        size_t count = battlenet_scan_games(home, &found);
        assert(count == 2);
        for (size_t i = 0; i < count; i++)
            assert(!strcmp(found[i].id, "fenris") ? !strcmp(found[i].uid, "fenris") : !found[i].uid[0]);
        free(found);
    }

    raw = ms_battlenet_status_json(home);
    assert(raw && strstr(raw, "\"installed\":true") && strstr(raw, "\"running\":false"));
    free(raw);
    raw = ms_battlenet_save_pipeline_json(home, "{\"battlenet_id\":\"fenris\",\"pipeline\":\"dxmt\"}",
                                          strlen("{\"battlenet_id\":\"fenris\",\"pipeline\":\"dxmt\"}"), &status);
    assert(raw && status == 200);
    free(raw);
    {
        char path[128];
        snprintf(path, sizeof(path), "bottles/battlenet_%u/bottle.json", battlenet_appid("fenris"));
        assert(exists(home, path));
    }
    raw = ms_battlenet_launch_game_json(home, "{\"battlenet_id\":\"nope\"}", strlen("{\"battlenet_id\":\"nope\"}"),
                                        &status);
    assert(raw && status == 404);
    free(raw);

    /* Stop is wineserver -k on Battle.net's own prefix; with no server running it counts as stopped. */
    fixture(home, "runtime/wine/bin/wineserver",
            "#!/bin/sh\n[ -n \"$WINEPREFIX\" ] || exit 3\ncase \"$WINEPREFIX\" in */prefix-battlenet) ;; *) exit 4 ;; "
            "esac\ncase \"$1\" in\n  -k) exit 1 ;;\n  -w) exit 0 ;;\n  *) exit 2 ;;\nesac\n");
    server = ms_store_path_join(home, "runtime/wine/bin/wineserver");
    assert(server && chmod(server, 0755) == 0);
    raw = ms_battlenet_stop_json(home, &status);
    assert(raw && status == 200 && strstr(raw, "\"running\":false"));
    free(raw);
    assert(unlink(server) == 0);

    /* Without the client, a game launch reports it. */
    {
        char* client =
            ms_store_path_join(home, "prefix-battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net.exe");
        char* launcher =
            ms_store_path_join(home, "prefix-battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe");
        assert(unlink(client) == 0 && unlink(launcher) == 0);
        free(client);
        free(launcher);
    }
    raw = ms_battlenet_launch_game_json(home, "{\"battlenet_id\":\"fenris\"}", strlen("{\"battlenet_id\":\"fenris\"}"),
                                        &status);
    assert(raw && status == 409 && strstr(raw, "not installed"));
    free(raw);
    free(server);
    free(volume);
    free(link);
}

int main(int argc, char** argv) {
    char home[PATH_MAX];
    assert(argc == 2);
    assert(realpath(argv[1], home));
    test_appid_namespaces();
    test_helpers(home);
    route_fixtures(home);
    test_rockstar(home);
    test_battlenet(home);
    printf("store launchers test passed (%s)\n", ms_wine_variant());
    return 0;
}
