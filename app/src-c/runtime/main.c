#include "metalsharp_backend/backend.h"
#include "metalsharp_backend/epic.h"
#include "metalsharp_backend/logs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static volatile sig_atomic_t stop_requested = 0;

static bool inherited_runtime_variable(const char* name, size_t name_length) {
    static const char* const prefixes[] = {"WINE", "PROTON_", "STEAM_COMPAT_", "DXVK_", "VKD3D_", "DXMT_",
                                           "DYLD_", "VK_"};
    static const char* const exact_names[] = {"STEAM_RUNTIME",   "SteamAppId", "SteamGameId",
                                              "SteamOverlayGameId", "SteamPath",  "GRAPHICS_BACKEND",
                                              "MS_GRAPHICS_BACKEND", "LD_LIBRARY_PATH", "LD_PRELOAD"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t prefix_length = strlen(prefixes[i]);
        if (name_length >= prefix_length && strncasecmp(name, prefixes[i], prefix_length) == 0)
            return true;
    }
    for (size_t i = 0; i < sizeof(exact_names) / sizeof(exact_names[0]); i++)
        if (strlen(exact_names[i]) == name_length && strncasecmp(name, exact_names[i], name_length) == 0)
            return true;
    return false;
}

static void sanitize_inherited_runtime_environment(void) {
    /* MetalSharp always selects its own Wine, graphics runtime, and prefix.
     * Another Wine/Proton app may leave these variables in launchd or in the
     * environment inherited by Electron; prevent those settings from
     * changing Steam setup and game launches. */
    for (;;) {
        bool removed = false;
        for (char** entry = environ; entry && *entry; entry++) {
            char* separator = strchr(*entry, '=');
            size_t name_length;
            char name[256];
            if (!separator)
                continue;
            name_length = (size_t)(separator - *entry);
            if (name_length >= sizeof(name) || !inherited_runtime_variable(*entry, name_length))
                continue;
            memcpy(name, *entry, name_length);
            name[name_length] = '\0';
            (void)unsetenv(name);
            removed = true;
            break; /* unsetenv may move the environ array */
        }
        if (!removed)
            break;
    }
}

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static unsigned short configured_port(void) {
    const char* value = getenv("METALSHARP_PORT");
    char* end = NULL;
    unsigned long parsed;
    if (value == NULL || value[0] == '\0')
        return 9274;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535) {
        fprintf(stderr, "invalid METALSHARP_PORT=%s\n", value);
        exit(EXIT_FAILURE);
    }
    return (unsigned short)parsed;
}

static void sleep_half_second(void) {
    struct timespec delay = {0, 500000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

int main(void) {
    unsigned short port;
    ms_backend_context context;
    unsigned attempt;

    sanitize_inherited_runtime_environment();
    port = configured_port();
    (void)signal(SIGINT, request_stop);
    (void)signal(SIGTERM, request_stop);
    (void)signal(SIGPIPE, SIG_IGN);
    ms_backend_context_init(&context, MS_BACKEND_VERSION);
    if (context.metalsharp_home == NULL) {
        fprintf(stderr, "failed to resolve MetalSharp home\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "metalsharp-backend listening on 127.0.0.1:%u\n", (unsigned)port);
    fprintf(stderr, "MetalSharp v%s backend started on 127.0.0.1:%u\n", context.version, (unsigned)port);
    {
        char message[256];
        snprintf(message, sizeof(message), "MetalSharp v%s backend started on 127.0.0.1:%u", context.version,
                 (unsigned)port);
        ms_log_event(context.metalsharp_home, message);
    }
    ms_epic_sync_on_startup(context.metalsharp_home);

    for (attempt = 1; attempt <= 30 && !stop_requested; ++attempt) {
        if (ms_http_serve(port, &stop_requested, ms_backend_handle, &context) == 0) {
            break;
        }
        if (attempt == 30) {
            fprintf(stderr, "failed to bind 127.0.0.1:%u after 30 attempts: %s\n", (unsigned)port, strerror(errno));
            free((void*)context.metalsharp_home);
            return EXIT_FAILURE;
        }
        fprintf(stderr, "bind 127.0.0.1:%u attempt %u/30 failed: %s — retrying in 500ms\n", (unsigned)port, attempt,
                strerror(errno));
        sleep_half_second();
    }
    free((void*)context.metalsharp_home);
    return EXIT_SUCCESS;
}
