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

/* The driver metalvrd runs: ~/.metalsharp/metalvr/driver holds one word (rift, alvr or sim); rift without it */
static void read_driver(const char* home, char* out, size_t size) {
    char path[1024];
    FILE* f;
    snprintf(out, size, "rift");
    if (!path_join(path, sizeof(path), home, "metalvr/driver") || !(f = fopen(path, "r")))
        return;
    if (fgets(out, (int)size, f)) {
        out[strcspn(out, " \t\r\n")] = '\0';
        if (strcmp(out, "rift") && strcmp(out, "alvr") && strcmp(out, "sim"))
            snprintf(out, size, "rift");
    }
    fclose(f);
}

/* Runs argv to its end, as the backend's other short-lived children: SIGCHLD is held on this thread so the
 * process-wide reaper (bottle_actions.c) cannot take the child. True when it exited 0. */
static bool run_and_wait(char* const argv[]) {
    sigset_t block, previous;
    pid_t pid;
    int status = 0;
    bool ok = false;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block, &previous);
    pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
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
        /* taken by the reaper on another thread: the exit status is unknown, and the command ran */
        ok = waited < 0 ? errno == ECHILD : (WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return ok;
}

/* metalvrd, detached: in a session of its own, its output in the log, holding none of the backend's descriptors
 * (the backend's socket must not stay bound to it) */
static bool start_server(const char* home, const char* server, const char* driver) {
    char log_path[1024];
    sigset_t block, previous;
    pid_t pid;
    if (!path_join(log_path, sizeof(log_path), home, "logs/metalvrd.log"))
        return false;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block, &previous);
    pid = fork();
    if (pid == 0) {
        /* a second fork leaves metalvrd without the backend as its parent */
        if (fork() != 0)
            _exit(0);
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
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
        execl(server, "metalvrd", "--driver", driver, "--verbose", (char*)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
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
    char server[1024], helper[1024], driver[16];
    if (status)
        *status = 200;
    if (!installed_paths(home, server, sizeof(server), helper, sizeof(helper))) {
        if (status)
            *status = 404;
        return reply(false, false, "MetalVR is not installed: ~/.metalsharp/metalvr-runtime has no metalvrd");
    }
    ask_screen_recording(home);
    if (!server_listening()) {
        read_driver(home, driver, sizeof(driver));
        if (!start_server(home, server, driver)) {
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
