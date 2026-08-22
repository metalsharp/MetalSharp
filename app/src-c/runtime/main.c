#include "metalsharp_backend/backend.h"
#include "metalsharp_backend/logs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

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
    const unsigned short port = configured_port();
    ms_backend_context context;
    unsigned attempt;

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
