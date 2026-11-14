/* Packed game apps (packed_app.h) without /Applications, a real launcher or Wine: a folder stands in
 * for /Applications (METALSHARP_PACKED_APPS_DIR), a script for the Cocoa launcher
 * (METALSHARP_GAME_APP_LAUNCHER) and a fake MetalSharp home for ~/.metalsharp. The bundle's layout
 * and Info.plist, its icon from a PNG (when sips is there), the record, which bundles may be
 * replaced or removed, and what a launch, running check and stop call for each kind of game. */
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/packed_app.h"
#include "metalsharp_backend/rockstar.h"
#include "metalsharp_backend/ubisoft.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BODY(text) text, strlen(text)

static char base[PATH_MAX];
static char home[PATH_MAX];
static char apps[PATH_MAX];

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

static void write_bytes(const char* absolute, const void* bytes, size_t length) {
    char parent[PATH_MAX];
    FILE* file;
    snprintf(parent, sizeof(parent), "%s", absolute);
    *strrchr(parent, '/') = '\0';
    make_dir_at(parent);
    file = fopen(absolute, "wb");
    assert(file && fwrite(bytes, 1, length, file) == length && fclose(file) == 0);
}

static void write_text(const char* absolute, const char* text) {
    write_bytes(absolute, text, strlen(text));
}

static char* read_file(const char* absolute) {
    FILE* file = fopen(absolute, "rb");
    static char buffer[65536];
    size_t length;
    if (!file)
        return NULL;
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    return buffer;
}

static bool exists(const char* absolute) {
    struct stat st;
    return lstat(absolute, &st) == 0;
}

static ms_json* parse(const char* text) {
    ms_json* json = ms_json_parse(text, strlen(text), NULL, 0);
    assert(json);
    return json;
}

static bool json_bool(const ms_json* json, const char* key) {
    bool value = false;
    (void)ms_json_as_bool(ms_json_object_get(json, key), &value);
    return value;
}

static char* json_string(const ms_json* json, const char* key) {
    static char out[PATH_MAX];
    char* value = NULL;
    out[0] = '\0';
    if (ms_json_as_string(ms_json_object_get(json, key), &value))
        snprintf(out, sizeof(out), "%s", value);
    free(value);
    return out;
}

static char* create(const char* body, int* status) {
    return ms_packed_app_create_json(home, body, strlen(body), status);
}

/* A 2x2 opaque PNG. */
static const unsigned char PNG[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49,
                                    0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02,
                                    0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44,
                                    0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x9f, 0x81, 0xe1, 0xff,
                                    0xff, 0xff, 0x0c, 0x0c, 0x0c, 0x00, 0x2b, 0xf2, 0x05, 0xfb, 0x2d, 0x6a, 0x1d,
                                    0x5e, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

static void test_create_status_replace_remove(void) {
    const char* bundle_rel = "Applications/Half-Life 2- Episode-One.app";
    char body[PATH_MAX * 2];
    int status = 0;
    char* out;
    ms_json* json;
    char* plist;
    bool sips = access("/usr/bin/sips", X_OK) == 0;

    write_bytes(at("art/card.png"), PNG, sizeof(PNG));
    snprintf(body, sizeof(body),
             "{\"kind\":\"steam\",\"id\":\"220\",\"name\":\"Half-Life 2: Episode/One\",\"iconPath\":\"%s\"}",
             at("art/card.png"));
    out = create(body, &status);
    assert(status == 200);
    json = parse(out);
    assert(json_bool(json, "ok") && json_bool(json, "packed"));
    assert(!strcmp(json_string(json, "path"), at(bundle_rel)));
    assert(json_bool(json, "icon") == sips);
    ms_json_free(json);
    free(out);

    /* The launcher, named for the game, executable; the plist names the game. */
    {
        char exe[PATH_MAX];
        struct stat st;
        snprintf(exe, sizeof(exe), "%s/Contents/MacOS/Half-Life 2- Episode-One", at(bundle_rel));
        assert(stat(exe, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111));
    }
    plist = read_file(at("Applications/Half-Life 2- Episode-One.app/Contents/Info.plist"));
    assert(plist);
    assert(strstr(plist, "<key>CFBundleExecutable</key><string>Half-Life 2- Episode-One</string>"));
    assert(strstr(plist, "<key>CFBundleIdentifier</key><string>com.metalsharp.game.steam.220</string>"));
    assert(strstr(plist, "<key>CFBundleName</key><string>Half-Life 2: Episode/One</string>"));
    assert(strstr(plist, "<key>CFBundleDisplayName</key><string>Half-Life 2: Episode/One</string>"));
    assert(strstr(plist, "<key>LSMinimumSystemVersion</key><string>13.0</string>"));
    assert(strstr(plist, "<key>MetalSharpGameKind</key><string>steam</string>"));
    assert(strstr(plist, "<key>MetalSharpGameId</key><string>220</string>"));
    assert(!!strstr(plist, "<key>CFBundleIconFile</key><string>AppIcon</string>") == sips);
    assert(exists(at("Applications/Half-Life 2- Episode-One.app/Contents/Resources/AppIcon.icns")) == sips);
    if (sips) {
        char* icns = read_file(at("Applications/Half-Life 2- Episode-One.app/Contents/Resources/AppIcon.icns"));
        assert(icns && !memcmp(icns, "icns", 4));
    }
    /* Nothing was left behind in the folder but the app. */
    {
        DIR* dir = opendir(apps);
        struct dirent* entry;
        assert(dir);
        while ((entry = readdir(dir)) != NULL)
            assert(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || entry->d_name[0] != '.');
        closedir(dir);
    }

    /* The record, and the status from it. */
    assert(strstr(read_file(at("home/config/packed-apps.json")), "\"bundleId\":\"com.metalsharp.game.steam.220\""));
    out = ms_packed_app_status_json(home, "kind=steam&id=220", NULL, 0, &status);
    json = parse(out);
    assert(status == 200 && json_bool(json, "packed") && !strcmp(json_string(json, "path"), at(bundle_rel)));
    ms_json_free(json);
    free(out);
    out = ms_packed_app_status_json(home, NULL, BODY("{\"kind\":\"steam\",\"id\":220}"), &status);
    json = parse(out);
    assert(status == 200 && json_bool(json, "packed"));
    ms_json_free(json);
    free(out);
    out = ms_packed_app_status_json(home, "kind=steam&id=221", NULL, 0, &status);
    json = parse(out);
    assert(status == 200 && !json_bool(json, "packed"));
    ms_json_free(json);
    free(out);

    /* Packing again replaces MetalSharp's own app; a rename moves it. */
    write_text(at("Applications/Half-Life 2- Episode-One.app/Contents/Resources/stale"), "x");
    out = create("{\"kind\":\"steam\",\"id\":\"220\",\"name\":\"Half-Life 2: Episode/One\"}", &status);
    assert(status == 200);
    free(out);
    assert(!exists(at("Applications/Half-Life 2- Episode-One.app/Contents/Resources/stale")));
    assert(!exists(at("Applications/Half-Life 2- Episode-One.app/Contents/Resources/AppIcon.icns")));
    assert(!strstr(read_file(at("Applications/Half-Life 2- Episode-One.app/Contents/Info.plist")), "CFBundleIconFile"));
    out = create("{\"kind\":\"steam\",\"id\":\"220\",\"name\":\"Episode One\"}", &status);
    assert(status == 200);
    free(out);
    assert(!exists(at(bundle_rel)) && exists(at("Applications/Episode One.app/Contents/Info.plist")));

    /* Remove takes the app and the record. */
    out = ms_packed_app_remove_json(home, BODY("{\"kind\":\"steam\",\"id\":\"220\"}"), &status);
    json = parse(out);
    assert(status == 200 && json_bool(json, "ok") && !json_bool(json, "packed"));
    ms_json_free(json);
    free(out);
    assert(!exists(at("Applications/Episode One.app")));
    assert(!strstr(read_file(at("home/config/packed-apps.json")), "steam"));
    out = ms_packed_app_status_json(home, "kind=steam&id=220", NULL, 0, &status);
    json = parse(out);
    assert(!json_bool(json, "packed"));
    ms_json_free(json);
    free(out);
}

static void test_never_touches_other_apps(void) {
    int status = 0;
    char* out;
    const char* foreign_plist =
        "<?xml version=\"1.0\"?><plist><dict><key>CFBundleIdentifier</key><string>com.example.game</string>"
        "</dict></plist>";
    write_text(at("Applications/Foreign Game.app/Contents/Info.plist"), foreign_plist);
    out = create("{\"kind\":\"gog\",\"id\":\"1207658924\",\"name\":\"Foreign Game\"}", &status);
    assert(status == 409 && strstr(out, "was not made by MetalSharp"));
    free(out);
    assert(!strcmp(read_file(at("Applications/Foreign Game.app/Contents/Info.plist")), foreign_plist));
    /* A plain file of that name too. */
    write_text(at("Applications/Plain.app"), "x");
    out = create("{\"kind\":\"epic\",\"id\":\"Fortnite\",\"name\":\"Plain\"}", &status);
    assert(status == 409);
    free(out);
    assert(!strcmp(read_file(at("Applications/Plain.app")), "x"));

    /* Another game's MetalSharp app of the same name stays too. */
    out = create("{\"kind\":\"epic\",\"id\":\"Sugar\",\"name\":\"Twin\"}", &status);
    assert(status == 200);
    free(out);
    out = create("{\"kind\":\"gog\",\"id\":\"42\",\"name\":\"Twin\"}", &status);
    assert(status == 409 && strstr(out, "another game"));
    free(out);
    assert(strstr(read_file(at("Applications/Twin.app/Contents/Info.plist")),
                  "<key>MetalSharpGameId</key><string>Sugar</string>"));

    /* Once something else stands where the app was, remove keeps it and drops the record. */
    out = ms_packed_app_remove_json(home, BODY("{\"kind\":\"epic\",\"id\":\"Sugar\"}"), &status);
    assert(status == 200);
    free(out);
    assert(!exists(at("Applications/Twin.app")));
    out = create("{\"kind\":\"epic\",\"id\":\"Sugar\",\"name\":\"Twin\"}", &status);
    assert(status == 200);
    free(out);
    assert(rename(at("Applications/Twin.app"), at("Twin-moved.app")) == 0);
    write_text(at("Applications/Twin.app/Contents/Info.plist"), foreign_plist);
    out = ms_packed_app_remove_json(home, BODY("{\"kind\":\"epic\",\"id\":\"Sugar\"}"), &status);
    assert(status == 200);
    free(out);
    assert(!strcmp(read_file(at("Applications/Twin.app/Contents/Info.plist")), foreign_plist));
    out = ms_packed_app_status_json(home, "kind=epic&id=Sugar", NULL, 0, &status);
    assert(strstr(out, "\"packed\":false"));
    free(out);
}

static void test_requests_are_checked(void) {
    int status = 0;
    char* out;
    const char* bad[] = {
        "{\"kind\":\"steam\",\"id\":\"abc\",\"name\":\"x\"}", "{\"kind\":\"origin\",\"id\":\"1\",\"name\":\"x\"}",
        "{\"kind\":\"epic\",\"id\":\"../x\",\"name\":\"x\"}", "{\"kind\":\"steam\",\"id\":\"0\",\"name\":\"x\"}",
        "{\"kind\":\"epic\",\"id\":\"Sugar\",\"name\":\"\"}", "not json",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        out = create(bad[i], &status);
        assert(status == 400);
        free(out);
    }
    out = ms_packed_app_status_json(home, "kind=steam", NULL, 0, &status);
    assert(status == 400);
    free(out);
    out = ms_packed_app_launch_json(home, BODY("{\"kind\":\"steam\"}"), &status);
    assert(status == 400);
    free(out);
    out = ms_packed_app_stop_json(home, BODY("{}"), &status);
    assert(status == 400);
    free(out);

    /* No launcher binary: nothing is made. */
    setenv("METALSHARP_GAME_APP_LAUNCHER", at("missing-launcher"), 1);
    out = create("{\"kind\":\"epic\",\"id\":\"Nope\",\"name\":\"Nope\"}", &status);
    assert(status == 500 && strstr(out, "no game app launcher"));
    free(out);
    assert(!exists(at("Applications/Nope.app")));
    setenv("METALSHARP_GAME_APP_LAUNCHER", at("launcher"), 1);
}

static void test_plans(void) {
    ms_packed_app_plan plan;
    char relative[128], expected[256];
    unsigned ubisoft = ms_ubisoft_appid("635");

    assert(ms_packed_app_plan_for(home, "steam", "1245620", &plan));
    assert(plan.start_steam && plan.appid == 1245620);
    assert(!strcmp(plan.launch_route, "/steam/launch-game"));
    assert(!strcmp(plan.launch_body, "{\"appid\":1245620,\"launchMethod\":\"auto\"}"));
    assert(!strcmp(plan.stop_route, "/kill") && !strcmp(plan.stop_body, "{\"appid\":1245620}"));
    assert(!strcmp(plan.launcher_stop_route, "/steam/stop"));
    ms_packed_app_plan_free(&plan);

    /* Ubisoft's launch defaults to its store route: the game's saved one is passed. */
    snprintf(relative, sizeof(relative), "home/bottles/ubisoft_%u/bottle.json", ubisoft);
    write_text(at(relative), "{\"preferred_pipeline\":\"vkd3d\"}");
    assert(ms_packed_app_plan_for(home, "ubisoft", "635", &plan));
    assert(!plan.start_steam && plan.appid == ubisoft);
    assert(!strcmp(plan.launch_route, "/ubisoft/launch-game"));
    snprintf(expected, sizeof(expected), "{\"appid\":%u,\"ubisoft_id\":\"635\",\"pipeline\":\"vkd3d\"}", ubisoft);
    assert(!strcmp(plan.launch_body, expected));
    snprintf(expected, sizeof(expected), "{\"appid\":%u}", ubisoft);
    assert(!strcmp(plan.stop_route, "/kill") && !strcmp(plan.stop_body, expected));
    assert(!strcmp(plan.launcher_stop_route, "/ubisoft/stop"));
    ms_packed_app_plan_free(&plan);

    /* No saved route: null, the store's default. */
    assert(ms_packed_app_plan_for(home, "rockstar", "gta5", &plan));
    assert(plan.appid == ms_rockstar_appid("gta5"));
    snprintf(expected, sizeof(expected), "{\"appid\":%u,\"rockstar_id\":\"gta5\",\"pipeline\":null}", plan.appid);
    assert(!strcmp(plan.launch_body, expected));
    assert(!strcmp(plan.launcher_stop_route, "/rockstar/stop"));
    ms_packed_app_plan_free(&plan);
    assert(ms_packed_app_plan_for(home, "battlenet", "wow", &plan));
    assert(!strcmp(plan.launch_route, "/battlenet/launch-game") &&
           strstr(plan.launch_body, "\"battlenet_id\":\"wow\""));
    assert(!strcmp(plan.launcher_stop_route, "/battlenet/stop"));
    ms_packed_app_plan_free(&plan);

    /* GOG's engine as MetalSharp's app saved it; Epic's play reads its own bottle. */
    write_text(at("home/gpui-gog-engines.json"), "{\"1207658924\":\"dxmt\"}");
    assert(ms_packed_app_plan_for(home, "gog", "1207658924", &plan));
    assert(!plan.start_steam && !plan.launcher_stop_route);
    assert(!strcmp(plan.launch_route, "/sharp-library/gog/play"));
    assert(!strcmp(plan.launch_body, "{\"productId\":\"1207658924\",\"engine\":\"dxmt\"}"));
    assert(!strcmp(plan.stop_route, "/sharp-library/gog/stop"));
    ms_packed_app_plan_free(&plan);
    assert(ms_packed_app_plan_for(home, "gog", "1", &plan));
    assert(!strcmp(plan.launch_body, "{\"productId\":\"1\",\"engine\":\"auto\"}"));
    ms_packed_app_plan_free(&plan);
    assert(ms_packed_app_plan_for(home, "epic", "Sugar", &plan));
    assert(!strcmp(plan.launch_route, "/sharp-library/epic/play") &&
           !strcmp(plan.launch_body, "{\"appName\":\"Sugar\"}"));
    assert(!strcmp(plan.stop_route, "/sharp-library/epic/stop") && !plan.launcher_stop_route);
    ms_packed_app_plan_free(&plan);

    assert(!ms_packed_app_plan_for(home, "steam", "x", &plan));
    assert(!ms_packed_app_plan_for(home, "uplay", "1", &plan));
}

/* Games that are not running, and the routes reached for GOG and Epic (no Wine in this home). */
static void test_running_launch_stop_route(void) {
    int status = 0;
    char* out;
    ms_json* json;
    out = ms_packed_app_running_json(home, "kind=epic&id=Sugar", NULL, 0, &status);
    json = parse(out);
    assert(status == 200 && json_bool(json, "ok") && !json_bool(json, "running"));
    ms_json_free(json);
    free(out);
    out = ms_packed_app_running_json(home, NULL, BODY("{\"kind\":\"gog\",\"id\":\"1207658924\"}"), &status);
    json = parse(out);
    assert(status == 200 && !json_bool(json, "running"));
    ms_json_free(json);
    free(out);
    out = ms_packed_app_running_json(home, "kind=ubisoft&id=635", NULL, 0, &status);
    json = parse(out);
    assert(status == 200 && !json_bool(json, "running"));
    ms_json_free(json);
    free(out);

    /* Epic's play refuses a game with no bottle: the launch reached it and reports its error. */
    out = ms_packed_app_launch_json(home, BODY("{\"kind\":\"epic\",\"id\":\"Sugar\"}"), &status);
    json = parse(out);
    assert(status == 500 && !json_bool(json, "ok") && json_string(json, "error")[0]);
    ms_json_free(json);
    free(out);
    /* GOG with no launcher to stop: the stop succeeds though the game was not running. */
    out = ms_packed_app_stop_json(home, BODY("{\"kind\":\"gog\",\"id\":\"1207658924\"}"), &status);
    json = parse(out);
    assert(status == 200 && json_bool(json, "ok") && !json_bool(json, "launcherStopped"));
    ms_json_free(json);
    free(out);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <empty temp dir>\n", argv[0]);
        return 2;
    }
    assert(realpath(argv[1], base));
    snprintf(home, sizeof(home), "%s/home", base);
    snprintf(apps, sizeof(apps), "%s/Applications", base);
    make_dir_at(home);
    make_dir_at(apps);
    write_text(at("launcher"), "#!/bin/sh\nexit 0\n");
    assert(chmod(at("launcher"), 0755) == 0);
    setenv("METALSHARP_PACKED_APPS_DIR", apps, 1);
    setenv("METALSHARP_GAME_APP_LAUNCHER", at("launcher"), 1);
    setenv("METALSHARP_HOME", home, 1);

    test_create_status_replace_remove();
    test_never_touches_other_apps();
    test_requests_are_checked();
    test_plans();
    test_running_launch_stop_route();
    printf("packed app tests passed\n");
    return 0;
}
