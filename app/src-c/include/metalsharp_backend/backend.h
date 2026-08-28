#ifndef METALSHARP_BACKEND_BACKEND_H
#define METALSHARP_BACKEND_BACKEND_H

#include "metalsharp_backend/http_server.h"

#include <stdbool.h>
#include <stddef.h>

#define MS_BACKEND_DEFAULT_VERSION "0.61.0"
#ifndef MS_BACKEND_VERSION
#define MS_BACKEND_VERSION MS_BACKEND_DEFAULT_VERSION
#endif

typedef struct {
    const char* version;
    const char* metalsharp_home;
    unsigned short steam_bridge_port;
    bool dev_mode;
} ms_backend_context;

void ms_backend_context_init(ms_backend_context* context, const char* version);
bool ms_backend_handle(const ms_http_request* request, ms_http_response* response, void* context);

#endif
