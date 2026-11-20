#define _DARWIN_C_SOURCE
#include "metalsharp_backend/game_tuning.h"
#include "metalsharp_backend/config.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The launch helpers game_tuning.c calls into, which start Wine or watch processes. */
static int g_watch_calls;
static char g_watched[256];
void ms_steam_apply_launch_preferences(const char* home) {
    (void)home;
}
char* ms_steam_cpu_topology_shim(const char* home) {
    (void)home;
    return NULL;
}
void ms_process_watch_game_priority(const char* home, const char* executable) {
    (void)home;
    g_watch_calls++;
    snprintf(g_watched, sizeof(g_watched), "%s", executable);
}
void ms_log_event(const char* home, const char* message) {
    (void)home;
    (void)message;
}

static char* slurp(const char* path) {
    FILE* file = fopen(path, "rb");
    char* text;
    long length;
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    text = calloc(1, (size_t)length + 1);
    assert(text && fread(text, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    return text;
}

static void spit(const char* path, const char* text) {
    FILE* file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void test_env_parsing(void) {
    char** entries = NULL;
    size_t count = 0;
    char error[256];
    ms_game_tuning tuning;
    char* text;

    assert(ms_game_tuning_parse_env("DXVK_HUD=1 GAME_PATH=\"C:\\\\Program Files\\\\Game\" \n -MTL_HUD_ENABLED EMPTY=",
                                    &entries, &count, error, sizeof(error)));
    assert(count == 4);
    assert(!strcmp(entries[0], "DXVK_HUD=1"));
    assert(!strcmp(entries[1], "GAME_PATH=C:\\Program Files\\Game"));
    assert(!strcmp(entries[2], "-MTL_HUD_ENABLED"));
    assert(!strcmp(entries[3], "EMPTY="));
    ms_game_tuning_init(&tuning);
    tuning.env = entries;
    tuning.env_count = count;
    text = ms_game_tuning_env_text(&tuning);
    assert(!strcmp(text, "DXVK_HUD=1 GAME_PATH=\"C:\\\\Program Files\\\\Game\" -MTL_HUD_ENABLED EMPTY="));
    /* The display form parses back to the same entries. */
    assert(ms_game_tuning_parse_env(text, &entries, &count, error, sizeof(error)) && count == 4);
    for (size_t i = 0; i < count; i++)
        assert(!strcmp(entries[i], tuning.env[i]));
    for (size_t i = 0; i < count; i++)
        free(entries[i]);
    free(entries);
    free(text);
    ms_game_tuning_free(&tuning);

    /* A later entry replaces an earlier one; single quotes keep spaces too. */
    assert(ms_game_tuning_parse_env("A=1 B='x y' A=2", &entries, &count, error, sizeof(error)) && count == 2);
    assert(!strcmp(entries[0], "B=x y") && !strcmp(entries[1], "A=2"));
    for (size_t i = 0; i < count; i++)
        free(entries[i]);
    free(entries);

    assert(ms_game_tuning_parse_env("", &entries, &count, error, sizeof(error)) && count == 0 && !entries);
    assert(!ms_game_tuning_parse_env("WINEPREFIX=/tmp/x", &entries, &count, error, sizeof(error)));
    assert(strstr(error, "WINEPREFIX"));
    assert(!ms_game_tuning_parse_env("DYLD_INSERT_LIBRARIES=/x.dylib", &entries, &count, error, sizeof(error)));
    assert(!ms_game_tuning_parse_env("WINEDLLOVERRIDES=d3d9=n", &entries, &count, error, sizeof(error)));
    assert(!ms_game_tuning_parse_env("-WINEDLLPATH", &entries, &count, error, sizeof(error)));
    assert(!ms_game_tuning_parse_env("1ABC=2", &entries, &count, error, sizeof(error)));
    assert(!ms_game_tuning_parse_env("BAD-NAME=2", &entries, &count, error, sizeof(error)));
    assert(!ms_game_tuning_parse_env("NOVALUE", &entries, &count, error, sizeof(error)));
    assert(!ms_game_tuning_parse_env("A=\"open", &entries, &count, error, sizeof(error)));
    assert(!ms_game_tuning_parse_env("A=\"line\nbreak\"", &entries, &count, error, sizeof(error)));
}

static void test_dll_overrides(void) {
    char* canonical = NULL;
    char error[256];
    assert(ms_game_tuning_parse_dll_overrides("d3dx9_43,D3DX9_42.dll=n,b; xinput1_3=builtin ;dinput8= ;d3d9=b,n",
                                              &canonical, error, sizeof(error)));
    assert(!strcmp(canonical, "d3dx9_43=n,b;d3dx9_42=n,b;xinput1_3=b;dinput8=d;d3d9=b,n"));
    free(canonical);
    /* The canonical form parses to itself, and the last word for a name wins. */
    assert(ms_game_tuning_parse_dll_overrides("a=n;b=d;a=b", &canonical, error, sizeof(error)));
    assert(!strcmp(canonical, "a=b;b=d"));
    free(canonical);
    assert(ms_game_tuning_parse_dll_overrides("  ", &canonical, error, sizeof(error)) && !canonical);
    assert(!ms_game_tuning_parse_dll_overrides("d3d9", &canonical, error, sizeof(error)));
    assert(!ms_game_tuning_parse_dll_overrides("d3d9=q", &canonical, error, sizeof(error)));
    assert(!ms_game_tuning_parse_dll_overrides("d3d9=d,n", &canonical, error, sizeof(error)));
    assert(!ms_game_tuning_parse_dll_overrides("d3d9=n,n", &canonical, error, sizeof(error)));
    assert(!ms_game_tuning_parse_dll_overrides("..\\evil=n", &canonical, error, sizeof(error)));
    assert(!ms_game_tuning_parse_dll_overrides("\"x\"=n", &canonical, error, sizeof(error)));

    assert(ms_game_tuning_valid_windows_version("") && ms_game_tuning_valid_windows_version("default"));
    assert(ms_game_tuning_valid_windows_version("winxp") && ms_game_tuning_valid_windows_version("win11"));
    assert(!ms_game_tuning_valid_windows_version("win95") && !ms_game_tuning_valid_windows_version("Win10"));
}

static void test_registry_text(void) {
    char* text =
        ms_game_tuning_registry_text("Game.exe", "d3dx9_43=n,b;dinput8=d", "win7", "old_dll=n;d3dx9_43=b", false);
    assert(text);
    assert(!strcmp(text, "Windows Registry Editor Version 5.00\r\n\r\n"
                         "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\Game.exe\\DllOverrides]\r\n"
                         "\"old_dll\"=-\r\n"
                         "\"d3dx9_43\"=\"native,builtin\"\r\n"
                         "\"dinput8\"=\"\"\r\n"
                         "\r\n"
                         "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\Game.exe]\r\n"
                         "\"Version\"=\"win7\"\r\n"
                         "\r\n"));
    free(text);
    /* Cleared: the previous values are deleted, nothing is added. */
    text = ms_game_tuning_registry_text("Game.exe", NULL, "", "old_dll=n", true);
    assert(!strcmp(text, "Windows Registry Editor Version 5.00\r\n\r\n"
                         "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\Game.exe\\DllOverrides]\r\n"
                         "\"old_dll\"=-\r\n"
                         "\r\n"
                         "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\Game.exe]\r\n"
                         "\"Version\"=-\r\n"
                         "\r\n"));
    free(text);
    assert(!ms_game_tuning_registry_text("bad]name.exe", "a=n", "", NULL, false));
    assert(!ms_game_tuning_registry_text("sub\\Game.exe", "a=n", "", NULL, false));
}

static void test_store(const char* home) {
    ms_game_tuning tuning, loaded;
    char error[256], path[PATH_MAX];
    ms_game_tuning_init(&tuning);
    assert(!ms_game_tuning_valid_key("steam", "../400") && !ms_game_tuning_valid_key("", "400"));
    assert(!ms_game_tuning_valid_key("steam", ".applied") && ms_game_tuning_valid_key("epic", "Fortnite_1.2-x"));
    assert(ms_game_tuning_load(home, "steam", "400", &loaded) && ms_game_tuning_is_default(&loaded));
    assert(ms_game_tuning_parse_env("A=1 B=\"two words\"", &tuning.env, &tuning.env_count, error, sizeof(error)));
    assert(ms_game_tuning_parse_dll_overrides("d3d9=n", &tuning.dll_overrides, error, sizeof(error)));
    snprintf(tuning.windows_version, sizeof(tuning.windows_version), "winxp");
    tuning.cpu_cores = 2;
    tuning.low_priority = true;
    assert(ms_game_tuning_save(home, "steam", "400", &tuning, error, sizeof(error)));
    assert(ms_game_tuning_load(home, "steam", "400", &loaded));
    assert(loaded.env_count == 2 && !strcmp(loaded.env[1], "B=two words"));
    assert(!strcmp(loaded.dll_overrides, "d3d9=n") && !strcmp(loaded.windows_version, "winxp"));
    assert(loaded.cpu_cores == 2 && loaded.low_priority);
    ms_game_tuning_free(&loaded);
    /* Hand-edited entries the launch owns are dropped on load. */
    snprintf(path, sizeof(path), "%s/config/game-tuning/steam/401.json", home);
    spit(path, "{\"env\":[\"WINEPREFIX=/x\",\"OK=1\",\"bad name=1\"],\"cpuCores\":99,\"windowsVersion\":\"win95\"}");
    assert(ms_game_tuning_load(home, "steam", "401", &loaded));
    assert(loaded.env_count == 1 && !strcmp(loaded.env[0], "OK=1") && !loaded.cpu_cores && !loaded.windows_version[0]);
    ms_game_tuning_free(&loaded);
    /* Defaults remove the file. */
    ms_game_tuning_free(&tuning);
    assert(ms_game_tuning_save(home, "steam", "400", &tuning, error, sizeof(error)));
    snprintf(path, sizeof(path), "%s/config/game-tuning/steam/400.json", home);
    assert(access(path, F_OK) != 0);
}

static void test_local_env(const char* home) {
    char dir[PATH_MAX], path[PATH_MAX], error[256];
    ms_game_tuning tuning;
    char* text;
    snprintf(dir, sizeof(dir), "%s/LocalEnvGame", home);
    assert(mkdir(dir, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.metalsharp", dir);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.metalsharp/launch.local.env", dir);
    spit(path, "# mine\nUSER_VAR=1\n");
    ms_game_tuning_init(&tuning);
    assert(ms_game_tuning_parse_env("A=1 B=\"x y\"", &tuning.env, &tuning.env_count, error, sizeof(error)));
    assert(ms_game_tuning_write_local_env(dir, &tuning));
    text = slurp(path);
    assert(!strcmp(text, "# >>> MetalSharp game settings (Game Settings > Advanced; edit them there)\n"
                         "A=1\nB=x y\n"
                         "# <<< MetalSharp game settings\n"
                         "# mine\nUSER_VAR=1\n"));
    free(text);
    /* Rewritten in place, the user's lines kept. */
    ms_game_tuning_free(&tuning);
    assert(ms_game_tuning_parse_env("-C", &tuning.env, &tuning.env_count, error, sizeof(error)));
    assert(ms_game_tuning_write_local_env(dir, &tuning));
    text = slurp(path);
    assert(!strcmp(text, "# >>> MetalSharp game settings (Game Settings > Advanced; edit them there)\n"
                         "-C\n"
                         "# <<< MetalSharp game settings\n"
                         "# mine\nUSER_VAR=1\n"));
    free(text);
    assert(ms_game_tuning_write_local_env(dir, NULL));
    text = slurp(path);
    assert(!strcmp(text, "# mine\nUSER_VAR=1\n"));
    free(text);
    /* Only MetalSharp's block: the file goes when it is cleared. */
    spit(path, "");
    assert(unlink(path) == 0);
    assert(ms_game_tuning_write_local_env(dir, &tuning) && access(path, F_OK) == 0);
    assert(ms_game_tuning_write_local_env(dir, NULL) && access(path, F_OK) != 0);
    assert(!ms_game_tuning_write_local_env("/nonexistent/metalsharp-folder", &tuning));
    ms_game_tuning_free(&tuning);
}

static void test_routes_and_launch(const char* home) {
    char game[PATH_MAX], bin[PATH_MAX], exe[PATH_MAX], prefix[PATH_MAX], path[PATH_MAX];
    int status = 0;
    char* out;
    const char* body;

    body = "{\"provider\":\"steam\",\"id\":620,\"env\":\"DXVK_HUD=fps\",\"dllOverrides\":\"d3dx9_43=n,b\","
           "\"windowsVersion\":\"win7\",\"cpuCores\":4,\"lowPriority\":true}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 200 && strstr(out, "\"cpuCores\":4") && strstr(out, "\"windowsVersion\":\"win7\""));
    free(out);
    /* Fields left out keep their values; an env array takes values with spaces as they are. */
    body = "{\"provider\":\"steam\",\"id\":\"620\",\"env\":[\"GAME_ARGS=-windowed -dx9\",\"-FOO\"]}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 200 && strstr(out, "\"env\":\"GAME_ARGS=\\\"-windowed -dx9\\\" -FOO\""));
    assert(strstr(out, "\"dllOverrides\":\"d3dx9_43=n,b\"") && strstr(out, "\"lowPriority\":true"));
    free(out);
    out = ms_game_tuning_get_json(home, "provider=steam&id=620", &status);
    assert(status == 200 && strstr(out, "\"envList\":[\"GAME_ARGS=-windowed -dx9\",\"-FOO\"]"));
    free(out);
    body = "{\"provider\":\"steam\",\"id\":620,\"env\":\"WINEPREFIX=/tmp\"}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 400 && strstr(out, "WINEPREFIX"));
    free(out);
    body = "{\"provider\":\"steam\",\"id\":620,\"cpuCores\":65}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 400);
    free(out);
    body = "{\"provider\":\"steam\",\"id\":620,\"windowsVersion\":\"win95\"}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 400);
    free(out);
    body = "{\"provider\":\"../x\",\"id\":620}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 400);
    free(out);
    out = ms_game_tuning_get_json(home, "provider=steam", &status);
    assert(status == 400);
    free(out);
    out = ms_game_tuning_get_json(home, "provider=epic&id=Sugar%2DApp", &status);
    assert(status == 200 && strstr(out, "\"id\":\"Sugar-App\"") && strstr(out, "\"cpuCores\":0"));
    free(out);

    /* A launch: launch.local.env in the game folder and the executable's, the priority watch. The prefix
     * has no user.reg here, so no registry import is attempted (nor recorded). */
    snprintf(game, sizeof(game), "%s/Steam Game", home);
    snprintf(bin, sizeof(bin), "%s/bin", game);
    snprintf(exe, sizeof(exe), "%s/Game.exe", bin);
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    assert(mkdir(game, 0700) == 0 && mkdir(bin, 0700) == 0 && mkdir(prefix, 0700) == 0);
    spit(exe, "MZ");
    ms_game_tuning_prepare_launch(home, "steam", "620", prefix, game, exe);
    assert(g_watch_calls == 1 && !strcmp(g_watched, "Game.exe"));
    snprintf(path, sizeof(path), "%s/.metalsharp/launch.local.env", game);
    {
        char* text = slurp(path);
        assert(text && strstr(text, "\nGAME_ARGS=-windowed -dx9\n-FOO\n"));
        free(text);
    }
    snprintf(path, sizeof(path), "%s/.metalsharp/launch.local.env", bin);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/config/game-tuning/steam/620.applied", home);
    {
        char* text = slurp(path);
        assert(text && strstr(text, "\"dirs\":[") && strstr(text, "/bin\"") && !strstr(text, "Game.exe"));
        free(text);
    }

    /* The child side: variables, priority. */
    {
        pid_t child = fork();
        int child_status = 0;
        if (child == 0) {
            setenv("FOO", "set", 1);
            ms_game_tuning_apply_child(home, "steam", "620");
            _exit(!strcmp(getenv("GAME_ARGS") ? getenv("GAME_ARGS") : "", "-windowed -dx9") && !getenv("FOO") &&
                          getpriority(PRIO_PROCESS, 0) >= 10
                      ? 0
                      : 1);
        }
        assert(child > 0 && waitpid(child, &child_status, 0) == child);
        assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    }

    /* Saving clears the variables from the folders it last launched from at once. */
    body = "{\"provider\":\"steam\",\"id\":620,\"env\":\"\"}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 200);
    free(out);
    snprintf(path, sizeof(path), "%s/.metalsharp/launch.local.env", game);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof(path), "%s/.metalsharp/launch.local.env", bin);
    assert(access(path, F_OK) != 0);
}

/* The renderer switches (per game) and the Metal HUD with its metrics (shared): set in the child that starts the game. */
static void test_renderers_and_hud(const char* home) {
    char path[PATH_MAX];
    int status = 0;
    char* out;
    const char* body = "{\"provider\":\"gog\",\"id\":\"1207658924\",\"openglRenderer\":\"zink\","
                       "\"vulkanRenderer\":\"kosmickrisp\"}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 200 && strstr(out, "\"openglRenderer\":\"zink\"") &&
           strstr(out, "\"vulkanRenderer\":\"kosmickrisp\""));
    free(out);
    body = "{\"provider\":\"gog\",\"id\":\"1207658924\",\"vulkanRenderer\":\"dxvk\"}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 400 && strstr(out, "vulkanRenderer"));
    free(out);

    /* HUD metrics: unknown names are refused, known ones are kept once each, in the HUD's order. */
    body = "{\"metalHud\":true,\"metalHudElements\":\"fps,bogus\"}";
    out = ms_config_set_json(home, (const unsigned char*)body, strlen(body), &status);
    assert(status == 400);
    free(out);
    body = "{\"metalHud\":true,\"metalHudElements\":\"gputime,fps,fps\"}";
    out = ms_config_set_json(home, (const unsigned char*)body, strlen(body), &status);
    assert(status == 200 && strstr(out, "\"metalHudElements\":\"fps,gputime\""));
    free(out);

    {
        pid_t child = fork();
        int child_status = 0;
        if (child == 0) {
            unsetenv("SharpGL_Switch");
            unsetenv("SharpVK_Switch");
            ms_game_tuning_apply_child(home, "gog", "1207658924");
            const char *gl = getenv("SharpGL_Switch"), *vk = getenv("SharpVK_Switch"),
                       *hud = getenv("MTL_HUD_ENABLED"), *elements = getenv("MTL_HUD_ELEMENTS");
            _exit(gl && !strcmp(gl, "1") && vk && !strcmp(vk, "1") && hud && !strcmp(hud, "1") && elements &&
                          !strcmp(elements, "fps,gputime")
                      ? 0
                      : 1);
        }
        assert(child > 0 && waitpid(child, &child_status, 0) == child);
        assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    }

    /* Back to the defaults: no switches, and the HUD off with no metric list. */
    body = "{\"provider\":\"gog\",\"id\":\"1207658924\",\"openglRenderer\":\"winemetalgl\","
           "\"vulkanRenderer\":\"moltenvk\"}";
    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 200);
    free(out);
    snprintf(path, sizeof(path), "%s/config/game-tuning/gog/1207658924.json", home);
    assert(access(path, F_OK) != 0);
    body = "{\"metalHud\":false}";
    out = ms_config_set_json(home, (const unsigned char*)body, strlen(body), &status);
    assert(status == 200);
    free(out);
    {
        pid_t child = fork();
        int child_status = 0;
        if (child == 0) {
            unsetenv("SharpGL_Switch");
            ms_game_tuning_apply_child(home, "gog", "1207658924");
            const char* hud = getenv("MTL_HUD_ENABLED");
            _exit(!getenv("SharpGL_Switch") && hud && !strcmp(hud, "0") && !getenv("MTL_HUD_ELEMENTS") ? 0 : 1);
        }
        assert(child > 0 && waitpid(child, &child_status, 0) == child);
        assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    }
}

/* A game's own Retina reaches the prefix it starts in, Steam's included, where Windows programs run; a game that keeps
 * the default sets Settings > Graphics' back (issue 811). Imported only when user.reg does not hold it already. */
static void test_prefix_retina(const char* home) {
    char prefix[PATH_MAX], path[PATH_MAX], log[PATH_MAX], game[PATH_MAX], exe[PATH_MAX];
    int status = 0;
    char *out, *text;
    const char* body = "{\"provider\":\"windows\",\"id\":\"retina-app\",\"retina\":\"on\"}";
    snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home);
    snprintf(path, sizeof(path), "%s/drive_c", prefix);
    (void)mkdir(prefix, 0700);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/runtime", home);
    (void)mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/runtime/wine", home);
    (void)mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/runtime/wine/bin", home);
    (void)mkdir(path, 0700);
    /* Wine's `reg import`: the imported file goes into a log */
    snprintf(log, sizeof(log), "%s/imported.reg", home);
    snprintf(path, sizeof(path), "%s/runtime/wine/bin/wine", home);
    {
        char script[PATH_MAX * 2];
        snprintf(script, sizeof(script),
                 "#!/bin/sh\ncat \"$WINEPREFIX/drive_c/metalsharp-game-settings.reg\" >> '%s'\n", log);
        spit(path, script);
        assert(chmod(path, 0700) == 0);
    }
    snprintf(path, sizeof(path), "%s/user.reg", prefix);
    spit(path, "WINE REGISTRY Version 2\n\n[Software\\\\Wine\\\\Mac Driver] 1\n\"RetinaMode\"=\"N\"\n\n"
               "[Control Panel\\\\Desktop] 1\n\"LogPixels\"=dword:00000060\n");
    snprintf(game, sizeof(game), "%s/Retina Program", home);
    snprintf(exe, sizeof(exe), "%s/Program.exe", game);
    assert(mkdir(game, 0700) == 0);
    spit(exe, "MZ");

    out = ms_game_tuning_save_json(home, body, strlen(body), &status);
    assert(status == 200 && strstr(out, "\"retina\":\"on\""));
    free(out);
    ms_game_tuning_prepare_launch(home, "windows", "retina-app", prefix, NULL, exe);
    text = slurp(log);
    assert(text && strstr(text, "\"RetinaMode\"=\"Y\"") && strstr(text, "\"LogPixels\"=dword:000000c0"));
    free(text);
    (void)unlink(log);

    /* The prefix holds it now (as Wine's import would leave it): nothing is imported again. */
    snprintf(path, sizeof(path), "%s/user.reg", prefix);
    spit(path, "WINE REGISTRY Version 2\n\n[Software\\\\Wine\\\\Mac Driver] 1\n\"RetinaMode\"=\"Y\"\n\n"
               "[Control Panel\\\\Desktop] 1\n\"LogPixels\"=dword:000000c0\n");
    ms_game_tuning_prepare_launch(home, "windows", "retina-app", prefix, NULL, exe);
    assert(access(log, F_OK) != 0);

    /* A Steam game with no Retina of its own after it: Settings > Graphics' (off here) goes back. */
    ms_game_tuning_prepare_launch(home, "steam", "4000", prefix, game, exe);
    text = slurp(log);
    assert(text && strstr(text, "\"RetinaMode\"=\"N\"") && strstr(text, "\"LogPixels\"=dword:00000060"));
    free(text);
    (void)unlink(log);
}

int main(int argc, char** argv) {
    char temporary[] = "/tmp/metalsharp-game-tuning-test-XXXXXX";
    const char* home = argc > 1 ? argv[1] : mkdtemp(temporary);
    assert(home);
    test_env_parsing();
    test_dll_overrides();
    test_registry_text();
    test_store(home);
    test_local_env(home);
    test_routes_and_launch(home);
    test_renderers_and_hud(home);
    test_prefix_retina(home);
    printf("game tuning tests passed\n");
    return 0;
}
