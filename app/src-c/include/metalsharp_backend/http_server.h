#ifndef METALSHARP_BACKEND_HTTP_SERVER_H
#define METALSHARP_BACKEND_HTTP_SERVER_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#define MS_HTTP_MAX_HEADER_BYTES (64U * 1024U)
#define MS_HTTP_MAX_BODY_BYTES   (64U * 1024U * 1024U)

typedef struct {
    char* method;
    char* target;
    char* path;
    char* query;
    unsigned char* body;
    size_t body_length;
} ms_http_request;

typedef struct {
    int status;
    const char* content_type;
    const unsigned char* body;
    size_t body_length;
    bool owns_body;
} ms_http_response;

typedef bool (*ms_http_handler)(const ms_http_request* request, ms_http_response* response, void* context);

void ms_http_request_free(ms_http_request* request);

/* Serve loopback HTTP/1.1 requests until stop_flag becomes non-zero. */
int ms_http_serve(unsigned short port, volatile sig_atomic_t* stop_flag, ms_http_handler handler, void* context);

#endif
