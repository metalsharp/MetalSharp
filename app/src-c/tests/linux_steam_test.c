/* Linux Steam (the fex build's experimental Steam client) without a virtual machine: a fake MetalSharp home, and a
 * script standing in for MetalSharp Wine's metalsharp-steam (METALSHARP_LINUX_STEAM_COMMAND) that records how it is
 * run and answers status --json from a file. The Steam client setting, the lane each Play names to the bridge daemon
 * (the launch intent), Linux Steam's library as a Steam library, and the commands Start Steam, Stop and Play run. */
#include "metalsharp_backend/config.h"
#include "metalsharp_backend/linux_steam.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/wine_runtime.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char base[PATH_MAX];
static char home[PATH_MAX];

static char* at(const char* relative) {
    static char paths[32][PATH_MAX];
    static unsigned next;
    char* out = paths[next++ % 32];
    snprintf(out, PATH_MAX, "%s/%s", base, relative);
    return out;
}

static void make_dirs(const char* absolute) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", absolute);
    for (char* p = path + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            assert(mkdir(path, 0755) == 0 || errno == EEXIST);
            *p = '/';
        }
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
}

static void write_at(const char* absolute, const char* text) {
    char parent[PATH_MAX];
    FILE* file;
    snprintf(parent, sizeof(parent), "%s", absolute);
    *strrchr(parent, '/') = '\0';
    make_dirs(parent);
    file = fopen(absolute, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static char* read_at(const char* absolute) {
    static char text[16384];
    FILE* file = fopen(absolute, "rb");
    size_t length;
    if (!file)
        return NULL;
    length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = '\0';
    return text;
}

static void sleep_ms(long ms) {
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

/* The stand-in command: one line per run ("<args> | <environment>"), status --json from state.json. */
static const char stub[] = "#!/bin/bash\n"
                           "log=\"$STUB_DIR/calls.log\"\n"
                           "tty=notty; [ -t 0 ] && tty=tty\n"
                           "printf '%s | vm=%s run=%s steam_prefix=%s game_prefix=%s wine=%s %s\\n' \"$*\" "
                           "\"$METALSHARP_VM_DIR\" \"$METALSHARP_VM_RUN\" \"$METALSHARP_STEAM_PREFIX\" "
                           "\"$METALSHARP_GAME_PREFIX\" \"$METALSHARP_WINE\" \"$tty\" >> \"$log\"\n"
                           "case \"$1\" in\n"
                           "  status) cat \"$STUB_DIR/state.json\";;\n"
                           "  install) echo '==> Downloading Debian'; echo '==> Installing Steam';\n"
                           "           printf '{\"installed\":true,\"vm_running\":false,\"steam_running\":false,"
                           "\"installing\":false}' > \"$STUB_DIR/state.json\";;\n"
                           "  start) printf '{\"installed\":true,\"vm_running\":true,\"steam_running\":true,"
                           "\"installing\":false}' > \"$STUB_DIR/state.json\";;\n"
                           "  stop) printf '{\"installed\":true,\"vm_running\":false,\"steam_running\":false,"
                           "\"installing\":false}' > \"$STUB_DIR/state.json\";;\n"
                           "  url) [ -f \"$STUB_DIR/url-fails\" ] && { echo 'metalsharp-steam: Steam is not "
                           "running' >&2; exit 1; }\n"
                           "       cp \"$METALSHARP_VM_RUN/launch-intent/$STUB_APPID\" \"$STUB_DIR/intent-seen\" "
                           "2>/dev/null;;\n"
                           "esac\n"
                           "exit 0\n";

static void set_state(bool installed, bool vm, bool steam) {
    char text[160];
    snprintf(text, sizeof(text), "{\"installed\":%s,\"vm_running\":%s,\"steam_running\":%s,\"installing\":false}",
             installed ? "true" : "false", vm ? "true" : "false", steam ? "true" : "false");
    write_at(at("stub/state.json"), text);
}

static void reset_calls(void) {
    unlink(at("stub/calls.log"));
}

static const char* calls(void) {
    const char* text = read_at(at("stub/calls.log"));
    return text ? text : "";
}

static void test_config(void) {
    char* json;
    int status = 0;
    assert(!strcmp(ms_config_steam_client(home), "windows"));
    json = ms_config_get_json(home);
    assert(json && strstr(json, "\"steamClient\":\"windows\""));
    free(json);
    json = ms_config_set_json(home, (const unsigned char*)"{\"steamClient\":\"linux\"}", 23, &status);
    assert(status == 200 && json && strstr(json, "\"steamClient\":\"linux\""));
    free(json);
    assert(!strcmp(ms_config_steam_client(home), "linux"));
    /* anything else leaves it as it is */
    json = ms_config_set_json(home, (const unsigned char*)"{\"steamClient\":\"mac\"}", 21, &status);
    assert(status == 200 && json && strstr(json, "\"steamClient\":\"linux\""));
    free(json);
    /* other settings keep it */
    json = ms_config_set_json(home, (const unsigned char*)"{\"metalHud\":true}", 17, &status);
    assert(status == 200 && json && strstr(json, "\"steamClient\":\"linux\"") && strstr(json, "\"metalHud\":true"));
    free(json);
    json = ms_config_set_json(home, (const unsigned char*)"{\"steamClient\":\"windows\"}", 25, &status);
    assert(status == 200 && json && strstr(json, "\"steamClient\":\"windows\""));
    free(json);
    assert(!strcmp(ms_config_steam_client(home), "windows"));
}

static void test_lanes(void) {
    assert(!strcmp(ms_linux_steam_intent_backend("dxmt"), "mullion"));
    assert(!strcmp(ms_linux_steam_intent_backend("dxmt_32"), "mullion"));
    assert(!strcmp(ms_linux_steam_intent_backend("vkd3d"), "vkd3d"));
    assert(!strcmp(ms_linux_steam_intent_backend("dxvk"), "dxvk"));
    assert(!strcmp(ms_linux_steam_intent_backend("wine_bare"), "wine"));
    assert(ms_linux_steam_intent_backend("d3dmetal") == NULL);
    assert(ms_linux_steam_intent_backend("steam") == NULL);
    assert(ms_linux_steam_intent_backend(NULL) == NULL);
}

/* The virtual machine's programs run from MetalSharp-Wine.app, but are not Wine: never killed with it. */
static void test_vm_programs(void) {
    const char* app = "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents";
    const char* vm[] = {"MacOS/krunkit", "bin/krunkit", "bin/gvproxy", "bin/metalsharp-vmd",
                        "share/steamvm/LinuxSteam.app/Contents/MacOS/LinuxSteam"};
    const char* wine[] = {"MacOS/wine", "MacOS/wineserver", "lib/wine/aarch64-unix/wine-preloader"};
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(vm) / sizeof(vm[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", app, vm[i]);
        assert(ms_linux_steam_vm_executable(path));
    }
    for (size_t i = 0; i < sizeof(wine) / sizeof(wine[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", app, wine[i]);
        assert(!ms_linux_steam_vm_executable(path));
    }
}

static void test_intent(void) {
    char path[PATH_MAX];
    struct stat info;
    assert(ms_linux_steam_write_intent(home, 4242, "vkd3d"));
    snprintf(path, sizeof(path), "%s/run/linux-steam/launch-intent", home);
    assert(stat(path, &info) == 0 && S_ISDIR(info.st_mode) && (info.st_mode & 0777) == 0700);
    snprintf(path, sizeof(path), "%s/run/linux-steam/launch-intent/4242", home);
    assert(lstat(path, &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 0777) == 0600);
    assert(info.st_uid == getuid() && time(NULL) - info.st_mtime < 120);
    assert(!strcmp(read_at(path), "vkd3d\n"));
    /* a later Play replaces it */
    assert(ms_linux_steam_write_intent(home, 4242, "dxmt_32"));
    assert(!strcmp(read_at(path), "mullion\n"));
    ms_linux_steam_clear_intent(home, 4242);
    assert(access(path, F_OK) != 0);
    /* a lane the daemon has no backend for writes none */
    assert(!ms_linux_steam_write_intent(home, 4242, "d3dmetal"));
    assert(access(path, F_OK) != 0);
}

/* Linux Steam's default library, drive_c/SteamLibrary of the Steam prefix, is a Steam library in Windows mode too. */
static void test_library(void) {
    char dir[PATH_MAX], expected[PATH_MAX];
    char* found;
    write_at(at("home/prefix-steam/drive_c/SteamLibrary/steamapps/appmanifest_4242.acf"),
             "\"AppState\"\n{\n\t\"appid\"\t\t\"4242\"\n\t\"name\"\t\t\"Linux Test Game\"\n\t\"StateFlags\"\t\t\"4\"\n"
             "\t\"installdir\"\t\t\"LinuxTestGame\"\n}\n");
    snprintf(dir, sizeof(dir), "%s/prefix-steam/drive_c/SteamLibrary/steamapps/common/LinuxTestGame", home);
    write_at(at("home/prefix-steam/drive_c/SteamLibrary/steamapps/common/LinuxTestGame/LinuxTestGame.exe"), "MZ");
    found = ms_steam_game_dir(home, 4242);
    assert(found);
    assert(realpath(dir, expected));
    {
        char resolved[PATH_MAX];
        assert(realpath(found, resolved) && !strcmp(resolved, expected));
    }
    free(found);
    found = ms_steam_library_json(home);
    assert(found && strstr(found, "\"appid\":4242") && strstr(found, "Linux Test Game"));
    free(found);
}

static void test_commands(void) {
    char expected[PATH_MAX * 2];
    char* error;
    pid_t pid = 0;
    /* Windows mode: the stand-in is never run */
    reset_calls();
    set_state(true, true, true);
    assert(!ms_linux_steam_mode(home));
    (void)ms_steam_process_running(home);
    assert(!strcmp(calls(), ""));
    assert(ms_linux_steam_available(home));

    free(ms_config_set_json(home, (const unsigned char*)"{\"steamClient\":\"linux\"}", 23, NULL));
    assert(ms_linux_steam_mode(home));

    /* the environment every run gets, with no terminal */
    reset_calls();
    set_state(true, false, false);
    assert(!ms_steam_process_running(home));
    snprintf(expected, sizeof(expected),
             "status --json | vm=%s/linux-steam run=%s/run/linux-steam steam_prefix=%s/prefix-steam "
             "game_prefix=%s/prefix-steam wine=%s/runtime/MetalSharp-Wine.app/Contents/MacOS/wine notty\n",
             home, home, home, home, home);
    assert(!strcmp(calls(), expected));

    /* the state is cached for a moment */
    set_state(true, true, true);
    assert(!ms_steam_process_running(home));
    sleep_ms(2100);
    assert(ms_steam_process_running(home));

    /* Play: Steam runs, so no start; the intent is written before the link is sent */
    reset_calls();
    setenv("STUB_APPID", "4242", 1);
    unlink(at("stub/intent-seen"));
    error = ms_linux_steam_ensure_running(home, 5);
    assert(error == NULL);
    error = ms_linux_steam_open_url(home, 4242, "dxvk", "steam://run/4242", &pid);
    assert(error == NULL && pid > 0);
    assert(strstr(calls(), "url steam://run/4242 |"));
    assert(!strstr(calls(), "start |"));
    assert(read_at(at("stub/intent-seen")) && !strcmp(read_at(at("stub/intent-seen")), "dxvk\n"));

    /* a failed link takes its intent back, and says why */
    write_at(at("stub/url-fails"), "1");
    error = ms_linux_steam_open_url(home, 4242, "vkd3d", "steam://run/4242", &pid);
    assert(error && strstr(error, "Steam is not running"));
    free(error);
    snprintf(expected, sizeof(expected), "%s/run/linux-steam/launch-intent/4242", home);
    assert(access(expected, F_OK) != 0);
    unlink(at("stub/url-fails"));
    /* only steam:// links */
    error = ms_linux_steam_open_url(home, 4242, "dxvk", "file:///etc/passwd", &pid);
    assert(error);
    free(error);

    /* Play with Steam stopped: start, then wait for it */
    reset_calls();
    set_state(true, false, false);
    error = ms_linux_steam_ensure_running(home, 10);
    assert(error == NULL);
    assert(strstr(calls(), "start |"));

    /* not set up: Play says so and starts nothing */
    reset_calls();
    set_state(false, false, false);
    error = ms_linux_steam_ensure_running(home, 5);
    assert(error && strstr(error, "not set up"));
    free(error);
    assert(!strstr(calls(), "start |"));

    /* Stop */
    reset_calls();
    set_state(true, true, true);
    assert(ms_linux_steam_stop(home, 10));
    assert(strstr(calls(), "stop |"));

    /* stop only what runs: nothing to stop costs one status */
    reset_calls();
    set_state(true, false, false);
    assert(ms_linux_steam_stop_if_running(home, 10));
    assert(!strstr(calls(), "stop |"));
}

/* Start Steam when Linux Steam is not set up: the setup as a background job, its stage, then start and show. */
static void test_start_steam(void) {
    int status = 0;
    char* json;
    char stage[160] = "";
    reset_calls();
    set_state(false, false, false);
    json = ms_steam_launch_json(home, &status);
    assert(status == 200 && json && strstr(json, "\"ok\":true") && strstr(json, "\"installing\":true"));
    free(json);
    for (int i = 0; i < 100 && !strstr(calls(), "show |"); i++)
        sleep_ms(100);
    assert(strstr(calls(), "install |") && strstr(calls(), "start |") && strstr(calls(), "show |"));
    assert(strstr(calls(), "install |") < strstr(calls(), "start |"));
    {
        char log[PATH_MAX];
        snprintf(log, sizeof(log), "%s/logs/linux-steam-setup.log", home);
        assert(read_at(log) && strstr(read_at(log), "==> Installing Steam"));
    }
    sleep_ms(200);
    assert(!ms_linux_steam_installing(home, stage, sizeof(stage)));
    json = ms_steam_status_json(home);
    assert(json && strstr(json, "\"client\":\"linux\"") && strstr(json, "\"installed\":true"));
    free(json);

    /* set up and running: Start Steam shows the window */
    reset_calls();
    json = ms_steam_launch_json(home, &status);
    assert(status == 200 && json && strstr(json, "\"starting\":true"));
    free(json);
    for (int i = 0; i < 50 && !strstr(calls(), "show |"); i++)
        sleep_ms(100);
    assert(strstr(calls(), "show |") && !strstr(calls(), "install |"));

    /* the Windows Steam installer is refused: it can replace the prefix Linux Steam's games share */
    json = ms_steam_install_json(home, &status);
    assert(status == 409 && json && strstr(json, "Linux Steam"));
    free(json);
}

/* Play from MetalSharp: launch.env written as for Windows Steam, then the link through Linux Steam with the lane. */
static void test_play(void) {
    int status = 0;
    char* json;
    char env[PATH_MAX];
    const char* body;
    reset_calls();
    set_state(true, true, true);
    unlink(at("stub/intent-seen"));
    sleep_ms(2100);
    body = "{\"appid\":4242,\"launchMethod\":\"dxvk\"}";
    json = ms_steam_launch_game_json(home, body, strlen(body), &status);
    assert(json);
    if (status != 200)
        fprintf(stderr, "play: %d %s\n", status, json);
    assert(status == 200 && strstr(json, "steam_handoff"));
    free(json);
    assert(strstr(calls(), "url steam://run/4242 |"));
    assert(read_at(at("stub/intent-seen")) && !strcmp(read_at(at("stub/intent-seen")), "dxvk\n"));
    snprintf(env, sizeof(env),
             "%s/prefix-steam/drive_c/SteamLibrary/steamapps/common/LinuxTestGame/.metalsharp/"
             "launch.env",
             home);
    assert(read_at(env) && strstr(read_at(env), "GRAPHICS_BACKEND=dxvk"));
}

static void test_versions(void) {
    assert(ms_wine_compare_versions("7.0", "6.0") == 1);
    assert(ms_wine_compare_versions("6.0", "7.0-pre1") == -1);
    assert(ms_wine_compare_versions("7.0-pre1", "7.0") == -1);
    assert(ms_wine_compare_versions("7.0", "7.0-pre1") == 1);
    assert(ms_wine_compare_versions("7.0-pre2", "7.0-pre1") == 1);
    assert(ms_wine_compare_versions("7.0-rc1", "7.0-pre3") == 1);
    assert(ms_wine_compare_versions("7.0", "7.0") == 0);
    assert(ms_wine_compare_versions("7.0", "7") == 0);
    assert(ms_wine_compare_versions("7.0.1", "7.0") == 1);
    assert(ms_wine_compare_versions("7.0", "dev") == 2);
    assert(ms_wine_compare_versions(NULL, "7.0") == 2);
}

int main(int argc, char** argv) {
    char command[PATH_MAX];
    FILE* file;
    if (argc != 2 || !realpath(argv[1], base)) {
        fprintf(stderr, "usage: %s <empty folder>\n", argv[0]);
        return 2;
    }
    snprintf(home, sizeof(home), "%s/home", base);
    make_dirs(home);
    make_dirs(at("user"));
    make_dirs(at("stub"));
    /* nothing of the user's: their home and data folder are the test's */
    setenv("HOME", at("user"), 1);
    setenv("METALSHARP_HOME", home, 1);
    setenv("METALSHARP_ACTIVE_RUNTIME", "fex", 1);
    setenv("STUB_DIR", at("stub"), 1);
    snprintf(command, sizeof(command), "%s", at("stub/metalsharp-steam"));
    file = fopen(command, "wb");
    assert(file && fputs(stub, file) >= 0 && fclose(file) == 0);
    assert(chmod(command, 0755) == 0);
    setenv("METALSHARP_LINUX_STEAM_COMMAND", command, 1);

    test_versions();
    test_config();
    test_lanes();
    test_vm_programs();
    test_intent();
    test_library();
    test_commands();
    test_start_steam();
    test_play();
    puts("Linux Steam client setting, launch intent, library, and command tests passed");
    return 0;
}
