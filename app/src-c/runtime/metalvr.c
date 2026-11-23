/*
 * The MetalVR route: Mullion, with MetalVR made ready before the game starts, and the VR server the user starts and
 * stops from the game's hero (GET /metalvr/status, POST /metalvr/start, POST /metalvr/stop).
 *
 * MetalVR lives beside the runtime in ~/.metalsharp/metalvr-runtime (bin/metalvrd, the server; bin/metalvr, its
 * helper; lib/metalvr, the OpenXR runtime and the SteamVR driver). Before a game on this route starts:
 *   - Screen Recording is asked for once, as MetalSharp: the headset shows a starting game's window while it loads,
 *     which needs it;
 *   - the VR server must be running: the user starts and stops it with the game's VR Server button
 *     (POST /metalvr/start and /metalvr/stop), so it never outlives what they asked for;
 *   - the Steam prefix's OpenXR runtime is MetalVR's, and SteamVR loads MetalVR's driver, so OpenXR and SteamVR games
 *     both find the headset.
 * The game then launches as on the Mullion route.
 */
#include "metalsharp_backend/metalvr.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/logs.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define METALVR_PORT 37913

static bool path_join(char* out, size_t size, const char* home, const char* rest) {
    return snprintf(out, size, "%s/%s", home, rest) < (int)size;
}

static bool executable(const char* path) {
    return access(path, X_OK) == 0;
}

/* metalvrd answers on its loopback port */
static bool server_listening(void) {
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    bool ok;
    if (fd < 0)
        return false;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(METALVR_PORT);
    address.sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    ok = connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0;
    close(fd);
    return ok;
}

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/* The headsets MetalVR drives (its docs/headsets): the key metalvrd and the UI know it by, its name, the driver that
 * runs it, and the helper that sets it up (metalvr-rift, run as metalvr-lighthouse or metalvr-psvr2). A streamed
 * headset (ALVR) is named to metalvrd with --headset and connects only once trusted. */
typedef struct {
    const char* key;
    const char* name;
    const char* driver;
    const char* display; /* `metalvr display` group, NULL when macOS shows its display as it is */
    bool streamed;
} headset_info;

static const headset_info headsets[] = {
    {"rift-cv1", "Oculus Rift CV1", "rift", "rift", false},
    {"rift-s", "Oculus Rift S", "rift", "rift", false},
    {"quest-1", "Meta Quest", "alvr", NULL, true},
    {"quest-2", "Meta Quest 2", "alvr", NULL, true},
    {"quest-3", "Meta Quest 3", "alvr", NULL, true},
    {"quest-3s", "Meta Quest 3S", "alvr", NULL, true},
    {"quest-pro", "Meta Quest Pro", "alvr", NULL, true},
    {"steam-frame", "Valve Steam Frame", "alvr", NULL, true},
    {"index", "Valve Index", "lighthouse", "lighthouse", false},
    {"vive", "HTC Vive", "lighthouse", "lighthouse", false},
    {"vive-pro", "HTC Vive Pro", "lighthouse", "lighthouse", false},
    {"vive-pro-2", "HTC Vive Pro 2", "lighthouse", "lighthouse", false},
    {"psvr2", "PlayStation VR2", "psvr2", NULL, false},
};

static const headset_info* find_headset(const char* key) {
    for (size_t i = 0; key && i < sizeof(headsets) / sizeof(headsets[0]); ++i)
        if (!strcmp(headsets[i].key, key))
            return &headsets[i];
    return NULL;
}

/* The chosen headset and, for a streamed one, the ALVR client it trusts: ~/.metalsharp/metalvr/headset.json. A
 * setup from before it (metalvr/driver: rift, alvr or sim) still names the driver. */
typedef struct {
    const headset_info* headset;
    char trust[128];
    char legacy_driver[16];
} headset_choice;

static bool valid_trust(const char* name) {
    size_t length = strlen(name);
    if (length == 0 || length >= 128)
        return false;
    for (const char* c = name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' ||
              *c == '-' || *c == '_'))
            return false;
    return true;
}

static void read_choice(const char* home, headset_choice* choice) {
    char path[1024], error[96], text[4096];
    FILE* f;
    size_t length;
    memset(choice, 0, sizeof(*choice));
    if (path_join(path, sizeof(path), home, "metalvr/headset.json") && (f = fopen(path, "r"))) {
        length = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[length] = '\0';
        ms_json* root = ms_json_parse(text, length, error, sizeof(error));
        char *key = NULL, *trust = NULL;
        if (root && ms_json_as_string(ms_json_object_get(root, "headset"), &key))
            choice->headset = find_headset(key);
        if (root && ms_json_as_string(ms_json_object_get(root, "trust"), &trust) && valid_trust(trust))
            snprintf(choice->trust, sizeof(choice->trust), "%s", trust);
        free(key);
        free(trust);
        ms_json_free(root);
        if (choice->headset)
            return;
    }
    if (path_join(path, sizeof(path), home, "metalvr/driver") && (f = fopen(path, "r"))) {
        if (fgets(choice->legacy_driver, (int)sizeof(choice->legacy_driver), f)) {
            choice->legacy_driver[strcspn(choice->legacy_driver, " \t\r\n")] = '\0';
            if (strcmp(choice->legacy_driver, "rift") && strcmp(choice->legacy_driver, "alvr") &&
                strcmp(choice->legacy_driver, "sim"))
                choice->legacy_driver[0] = '\0';
        }
        fclose(f);
    }
}

/* the driver metalvrd runs: the chosen headset's, a setup from before the choice's, else rift */
static const char* choice_driver(const headset_choice* choice) {
    if (choice->headset)
        return choice->headset->driver;
    return choice->legacy_driver[0] ? choice->legacy_driver : "rift";
}

/* Runs argv to its end. True when it exited 0. */
static bool run_and_wait(char* const argv[]) {
    pid_t pid;
    int status = 0;
    bool ok = false;
    pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid > 0) {
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        ok = waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    return ok;
}

/* metalvrd, detached: in a session of its own, its output in the log, holding none of the backend's descriptors
 * (the backend's socket must not stay bound to it) */
static bool start_server(const char* home, const char* server, const headset_choice* choice) {
    char log_path[1024];
    pid_t pid;
    if (!path_join(log_path, sizeof(log_path), home, "logs/metalvrd.log"))
        return false;
    pid = fork();
    if (pid == 0) {
        /* a second fork leaves metalvrd without the backend as its parent */
        if (fork() != 0)
            _exit(0);
        setsid();
        {
            int null = open("/dev/null", O_RDONLY);
            int log = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (null >= 0)
                dup2(null, STDIN_FILENO);
            if (log >= 0) {
                dup2(log, STDOUT_FILENO);
                dup2(log, STDERR_FILENO);
            }
            for (int fd = 3; fd < 1024; ++fd)
                close(fd);
        }
        {
            char* argv[10];
            int argc = 0;
            argv[argc++] = "metalvrd";
            argv[argc++] = "--driver";
            argv[argc++] = (char*)choice_driver(choice);
            if (choice->headset && choice->headset->streamed) {
                argv[argc++] = "--headset";
                argv[argc++] = (char*)choice->headset->key;
                if (choice->trust[0]) {
                    argv[argc++] = "--trust";
                    argv[argc++] = (char*)choice->trust;
                }
            }
            argv[argc++] = "--verbose";
            argv[argc] = NULL;
            execv(server, argv);
        }
        _exit(127);
    }
    if (pid > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    return pid > 0;
}

/* Screen Recording, asked for once as MetalSharp (whose permission metalvrd, a process it starts, uses). The
 * functions come from CoreGraphics at run time, so the backend links no framework for them. */
static void ask_screen_recording(const char* home) {
    typedef bool (*access_fn)(void);
    static bool asked = false;
    void* cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY | RTLD_LOCAL);
    access_fn preflight = cg ? (access_fn)dlsym(cg, "CGPreflightScreenCaptureAccess") : NULL;
    access_fn request = cg ? (access_fn)dlsym(cg, "CGRequestScreenCaptureAccess") : NULL;
    if (!preflight || preflight())
        return;
    if (!asked && request) {
        asked = true;
        ms_log_event(home, "MetalVR asks for Screen Recording, so the headset can show a game's window while it "
                           "loads. Allow MetalSharp in System Settings > Privacy & Security > Screen & System Audio "
                           "Recording.");
        (void)request();
    }
}

/* the prefix's registry already names MetalVR's OpenXR runtime */
static bool openxr_registered(const char* home) {
    char path[1024], line[2048];
    FILE* f;
    bool found = false;
    if (!path_join(path, sizeof(path), home, "prefix-steam/system.reg") || !(f = fopen(path, "r")))
        return false;
    while (!found && fgets(line, sizeof(line), f))
        found = strstr(line, "\"ActiveRuntime\"") && strstr(line, "metalvr_openxr.json");
    fclose(f);
    return found;
}



static char* reply(bool ok, bool running, const char* error) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, ok);
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, running);
    if (error) {
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, error);
    }
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

static bool installed_paths(const char* home, char* server, size_t server_size, char* helper, size_t helper_size) {
    return home && path_join(server, server_size, home, "metalvr-runtime/bin/metalvrd") &&
           path_join(helper, helper_size, home, "metalvr-runtime/bin/metalvr") && executable(server) &&
           executable(helper);
}

/* the Steam prefix's OpenXR runtime is MetalVR's, and SteamVR loads MetalVR's driver (and has not blocked it) */
static void register_runtimes(const char* home, char* helper) {
    if (!openxr_registered(home)) {
        char* argv[] = {helper, "prefix", NULL};
        if (!run_and_wait(argv))
            ms_log_event(home, "MetalVR: the Steam prefix's OpenXR runtime could not be set to MetalVR");
    }
    /* every time: it also lifts a safe-mode block and keeps SteamVR's dashboard off the Windows desktop, which Wine
     * cannot capture (vrdashboard would restart forever and hold up Steam's interface); quick when nothing changes */
    {
        char* argv[] = {helper, "steamvr", NULL};
        if (!run_and_wait(argv))
            ms_log_event(home, "MetalVR: SteamVR could not be given MetalVR's driver");
    }
}

char* ms_metalvr_status_json(const char* home) {
    char server[1024], helper[1024];
    if (!installed_paths(home, server, sizeof(server), helper, sizeof(helper)))
        return reply(true, false, "MetalVR is not installed");
    return reply(true, server_listening(), NULL);
}

char* ms_metalvr_start_json(const char* home, int* status) {
    char server[1024], helper[1024];
    headset_choice choice;
    if (status)
        *status = 200;
    if (!installed_paths(home, server, sizeof(server), helper, sizeof(helper))) {
        if (status)
            *status = 404;
        return reply(false, false, "MetalVR is not installed: ~/.metalsharp/metalvr-runtime has no metalvrd");
    }
    ask_screen_recording(home);
    if (!server_listening()) {
        read_choice(home, &choice);
        if (!start_server(home, server, &choice)) {
            if (status)
                *status = 500;
            return reply(false, false, "The VR server could not be started");
        }
        /* the headset's display and tracking come up in a few seconds */
        for (int i = 0; i < 100 && !server_listening(); ++i)
            sleep_ms(100);
        if (!server_listening()) {
            if (status)
                *status = 500;
            return reply(false, false, "The VR server did not start (see ~/.metalsharp/logs/metalvrd.log)");
        }
        ms_log_event(home, "MetalVR: the VR server is running");
    }
    register_runtimes(home, helper);
    return reply(true, true, NULL);
}

char* ms_metalvr_stop_json(const char* home, int* status) {
    if (status)
        *status = 200;
    if (server_listening()) {
        /* metalvrd leaves on SIGINT, turning the headset's display off and letting go of it */
        char* interrupt[] = {"/usr/bin/pkill", "-INT", "-x", "metalvrd", NULL};
        (void)run_and_wait(interrupt);
        for (int i = 0; i < 50 && server_listening(); ++i)
            sleep_ms(100);
        if (server_listening()) {
            char* kill[] = {"/usr/bin/pkill", "-KILL", "-x", "metalvrd", NULL};
            (void)run_and_wait(kill);
            for (int i = 0; i < 20 && server_listening(); ++i)
                sleep_ms(100);
        }
        ms_log_event(home, server_listening() ? "MetalVR: the VR server did not stop" : "MetalVR: the VR server stopped");
    }
    return reply(!server_listening(), server_listening(),
                 server_listening() ? "The VR server did not stop" : NULL);
}

char* ms_metalvr_prepare(const char* home) {
    char server[1024], helper[1024];
    if (!installed_paths(home, server, sizeof(server), helper, sizeof(helper)))
        return strdup("MetalVR is not installed: ~/.metalsharp/metalvr-runtime has no metalvrd");
    if (!server_listening())
        return strdup("Start the VR server first: the Start VR Server button beside the game's settings");
    ask_screen_recording(home);
    register_runtimes(home, helper);
    return NULL;
}

/* ---- the headset (the game settings' Headset tab) ---- */

#define METALVR_RELEASE     "0.2.5"
#define METALVR_RELEASE_SHA "0c832c8bd97b2cd4bdd4437d27b88f7b59ca88a2c27e5d53e4bddfff138cc13c"

static char* headset_reply(const char* home, const char* error) {
    char server[1024], helper[1024];
    headset_choice choice;
    ms_json_writer w;
    read_choice(home, &choice);
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, error == NULL);
    if (error) {
        ms_json_writer_key(&w, "error");
        ms_json_writer_string(&w, error);
    }
    ms_json_writer_key(&w, "headset");
    ms_json_writer_string(&w, choice.headset ? choice.headset->key : "");
    ms_json_writer_key(&w, "trust");
    ms_json_writer_string(&w, choice.trust);
    ms_json_writer_key(&w, "installed");
    ms_json_writer_bool(&w, installed_paths(home, server, sizeof(server), helper, sizeof(helper)));
    ms_json_writer_key(&w, "running");
    ms_json_writer_bool(&w, server_listening());
    ms_json_writer_key(&w, "headsets");
    ms_json_writer_array_begin(&w);
    for (size_t i = 0; i < sizeof(headsets) / sizeof(headsets[0]); ++i) {
        ms_json_writer_object_begin(&w);
        ms_json_writer_key(&w, "key");
        ms_json_writer_string(&w, headsets[i].key);
        ms_json_writer_key(&w, "name");
        ms_json_writer_string(&w, headsets[i].name);
        ms_json_writer_key(&w, "driver");
        ms_json_writer_string(&w, headsets[i].driver);
        ms_json_writer_key(&w, "streamed");
        ms_json_writer_bool(&w, headsets[i].streamed);
        ms_json_writer_key(&w, "display");
        ms_json_writer_bool(&w, headsets[i].display != NULL);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_metalvr_headset_json(const char* home) {
    return headset_reply(home, NULL);
}

/* POST /metalvr/headset {"headset": key, "trust"?: ALVR client name}: saved for every later start of the server */
char* ms_metalvr_set_headset_json(const char* home, const char* body, size_t length, int* status) {
    char error[96], dir[1024], path[1024], temp[1100];
    ms_json* request = body ? ms_json_parse(body, length, error, sizeof(error)) : NULL;
    char *key = NULL, *trust = NULL;
    const headset_info* headset;
    headset_choice previous;
    FILE* f;
    ms_json_writer w;
    char* text;
    bool ok;
    *status = 400;
    if (!request || !ms_json_as_string(ms_json_object_get(request, "headset"), &key) || !(headset = find_headset(key))) {
        free(key);
        ms_json_free(request);
        return headset_reply(home, "headset must be one MetalVR supports");
    }
    read_choice(home, &previous);
    if (ms_json_object_get(request, "trust") &&
        (!ms_json_as_string(ms_json_object_get(request, "trust"), &trust) || (trust[0] && !valid_trust(trust)))) {
        free(key);
        free(trust);
        ms_json_free(request);
        return headset_reply(home, "trust must be the name the headset's ALVR app shows, such as 1234.client");
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "headset");
    ms_json_writer_string(&w, headset->key);
    ms_json_writer_key(&w, "trust");
    /* a new headset keeps the trusted client only when it says nothing of it and is streamed too */
    ms_json_writer_string(&w, trust ? trust : headset->streamed ? previous.trust : "");
    ms_json_writer_object_end(&w);
    text = ms_json_writer_take(&w);
    ok = text && path_join(dir, sizeof(dir), home, "metalvr") && (mkdir(dir, 0755) == 0 || errno == EEXIST) &&
         path_join(path, sizeof(path), home, "metalvr/headset.json") &&
         snprintf(temp, sizeof(temp), "%s.tmp", path) < (int)sizeof(temp) && (f = fopen(temp, "w")) &&
         (fputs(text, f) >= 0) + (fclose(f) == 0) == 2 && rename(temp, path) == 0;
    free(text);
    free(key);
    free(trust);
    ms_json_free(request);
    *status = ok ? 200 : 500;
    if (ok)
        ms_log_event(home, "MetalVR: headset saved (it applies when the VR server next starts)");
    return headset_reply(home, ok ? NULL : "the headset could not be saved");
}

/* A setup step that may ask for the user's password runs in Terminal, as a .command file MetalSharp writes */
static bool open_in_terminal(const char* home, const char* step, const char* script) {
    char path[1024];
    FILE* f;
    if (!path_join(path, sizeof(path), home, "metalvr") || (mkdir(path, 0755) != 0 && errno != EEXIST))
        return false;
    if (snprintf(path, sizeof(path), "%s/metalvr/setup-%s.command", home, step) >= (int)sizeof(path) ||
        !(f = fopen(path, "w")))
        return false;
    fprintf(f, "#!/bin/zsh\n# MetalSharp: MetalVR setup, %s\nset -e\n%s\necho\necho 'Done. You can close this window.'\n",
            step, script);
    if (fclose(f) != 0 || chmod(path, 0755) != 0)
        return false;
    char* argv[] = {"/usr/bin/open", "-a", "Terminal", path, NULL};
    return run_and_wait(argv);
}

/* POST /metalvr/setup {"step"}: install (MetalVR's release, checked), display (let macOS show the headset's display),
 * steamvr (MetalVR's driver in the Steam prefix's SteamVR), bluetooth and displays (their System Settings panes). */
char* ms_metalvr_setup_json(const char* home, const char* body, size_t length, int* status) {
    char error[96], script[2048], helper[1024], server[1024];
    ms_json* request = body ? ms_json_parse(body, length, error, sizeof(error)) : NULL;
    char* step = NULL;
    headset_choice choice;
    bool ok = false;
    *status = 400;
    if (!request || !ms_json_as_string(ms_json_object_get(request, "step"), &step)) {
        ms_json_free(request);
        return headset_reply(home, "step is required");
    }
    read_choice(home, &choice);
    bool installed = installed_paths(home, server, sizeof(server), helper, sizeof(helper));
    if (!strcmp(step, "install")) {
        snprintf(script, sizeof(script),
                 "cd \"$(mktemp -d)\"\n"
                 "curl -fL --progress-bar -o metalvr.tar.gz "
                 "https://github.com/metalsharp/MetalVR/releases/download/v" METALVR_RELEASE
                 "/metalvr-" METALVR_RELEASE "-macos-arm64.tar.gz\n"
                 "echo '" METALVR_RELEASE_SHA "  metalvr.tar.gz' | shasum -a 256 -c -\n"
                 "tar -xzf metalvr.tar.gz\n"
                 "metalvr-" METALVR_RELEASE "-macos-arm64/bin/metalvr-rift install");
        ok = open_in_terminal(home, step, script);
    } else if (!strcmp(step, "display") || !strcmp(step, "steamvr")) {
        if (!installed) {
            free(step);
            ms_json_free(request);
            *status = 409;
            return headset_reply(home, "Install MetalVR first");
        }
        if (!strcmp(step, "display")) {
            if (!choice.headset || !choice.headset->display) {
                free(step);
                ms_json_free(request);
                return headset_reply(home, "this headset needs no display step");
            }
            snprintf(script, sizeof(script), "'%s' display %s", helper, choice.headset->display);
        } else {
            snprintf(script, sizeof(script), "'%s' steamvr", helper);
        }
        ok = open_in_terminal(home, step, script);
    } else if (!strcmp(step, "bluetooth") || !strcmp(step, "displays")) {
        char* argv[] = {"/usr/bin/open",
                        !strcmp(step, "bluetooth") ? "x-apple.systempreferences:com.apple.BluetoothSettings"
                                                   : "x-apple.systempreferences:com.apple.Displays-Settings.extension",
                        NULL};
        ok = run_and_wait(argv);
    } else {
        free(step);
        ms_json_free(request);
        return headset_reply(home, "unknown setup step");
    }
    free(step);
    ms_json_free(request);
    *status = ok ? 200 : 500;
    return headset_reply(home, ok ? NULL : "the setup step could not be started");
}

