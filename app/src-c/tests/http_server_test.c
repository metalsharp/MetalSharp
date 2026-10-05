#include "metalsharp_backend/http_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool handler(const ms_http_request* request, ms_http_response* response, void* context) {
    static const unsigned char body[] = "{\"ok\":true}";
    (void)context;
    if (strcmp(request->path, "/slow") == 0) {
        const struct timespec delay = {0, 250000000L};
        (void)nanosleep(&delay, NULL);
    }
    response->status = 200;
    response->content_type = "application/json";
    response->body = body;
    response->body_length = sizeof(body) - 1;
    return true;
}

static int connect_loopback(unsigned short port) {
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static unsigned short reserve_port(void) {
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    address.sin_port = 0;
    assert(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr*)&address, &length) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static void exchange(unsigned short port, const char* request, int expected_status, bool legacy_cors) {
    int fd = connect_loopback(port);
    char response[2048], expected[48];
    size_t used = 0;
    assert(fd >= 0);
    struct timeval timeout = {2, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(send(fd, request, strlen(request), 0) == (ssize_t)strlen(request));
    for (;;) {
        ssize_t count = recv(fd, response + used, sizeof(response) - 1 - used, 0);
        assert(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
        assert(used < sizeof(response) - 1);
    }
    response[used] = '\0';
    snprintf(expected, sizeof(expected), "HTTP/1.1 %d ", expected_status);
    assert(strstr(response, expected) != NULL);
    assert((strstr(response, "Access-Control-Allow-Origin: *") != NULL) == legacy_cors);
    assert(strstr(response, "1111111111111111111111111111111111111111111111111111111111111111") == NULL);
    close(fd);
}

static bool authenticated_handler(const ms_http_request* request, ms_http_response* response, void* context) {
    assert(getenv("METALSHARP_CLIENT_TOKEN") == NULL);
    assert(strcmp(request->path, "/forbidden") != 0);
    return handler(request, response, context);
}

static void session_authentication_test(void) {
    static const char token[] = "1111111111111111111111111111111111111111111111111111111111111111";
    char captured[65], request[512];
    assert(setenv("METALSHARP_CLIENT_TOKEN", "invalid-fixture", 1) == 0);
    assert(ms_http_take_client_token(captured) == -1);
    assert(getenv("METALSHARP_CLIENT_TOKEN") == NULL);
    assert(setenv("METALSHARP_CLIENT_TOKEN", token, 1) == 0);
    assert(ms_http_take_client_token(captured) == 1);
    assert(strcmp(captured, token) == 0);
    assert(getenv("METALSHARP_CLIENT_TOKEN") == NULL);
    unsigned short port = reserve_port();
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
        _exit(ms_http_serve_authenticated(port, NULL, authenticated_handler, NULL, captured) == 0 ? 0 : 1);
    int fd = -1;
    for (int attempt = 0; attempt < 200 && fd < 0; ++attempt) {
        const struct timespec delay = {0, 10000000L};
        fd = connect_loopback(port);
        if (fd < 0)
            (void)nanosleep(&delay, NULL);
    }
    assert(fd >= 0);
    close(fd);
    exchange(port, "GET /forbidden HTTP/1.1\r\nHost: localhost\r\n\r\n", 401, false);
    // CORS-simple writes and even incomplete bodies are rejected before dispatch/read.
    exchange(port,
             "POST /forbidden HTTP/1.1\r\nHost: localhost\r\nOrigin: https://store.steampowered.com\r\nContent-Type: "
             "text/plain\r\nContent-Length: 64000000\r\n\r\n",
             401, false);
    exchange(port,
             "GET /forbidden HTTP/1.1\r\nHost: localhost\r\nX-MetalSharp-Client-Token: "
             "0000000000000000000000000000000000000000000000000000000000000000\r\n\r\n",
             401, false);
    snprintf(request, sizeof(request),
             "GET /status HTTP/1.1\r\nHost: localhost\r\nx-MeTaLsHaRp-ClIeNt-ToKeN: %s\r\n\r\n", token);
    exchange(port, request, 200, false);
    snprintf(request, sizeof(request),
             "POST /status HTTP/1.1\r\nHost: localhost\r\nX-MetalSharp-Client-Token: %s\r\nContent-Length: 2\r\n\r\n{}",
             token);
    exchange(port, request, 200, false);
    snprintf(request, sizeof(request),
             "GET /forbidden HTTP/1.1\r\nHost: localhost\r\nX-MetalSharp-Client-Token: "
             "%s\r\nx-metalsharp-client-token: %s\r\n\r\n",
             token, token);
    exchange(port, request, 400, false);
    int status;
    assert(kill(child, SIGTERM) == 0);
    assert(waitpid(child, &status, 0) == child);
}

int main(void) {
    static const char slow_request[] = "GET /slow HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    static const char status_request[] = "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    unsigned short port = reserve_port();
    pid_t child = fork();
    int fd = -1;
    int status = 0;
    char response[512];
    ssize_t received;

    assert(child >= 0);
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        _exit(ms_http_serve(port, NULL, handler, NULL) == 0 ? 0 : 1);
    }

    for (int attempt = 0; attempt < 200 && fd < 0; attempt++) {
        const struct timespec delay = {0, 10000000L};
        fd = connect_loopback(port);
        if (fd < 0)
            (void)nanosleep(&delay, NULL);
    }
    assert(fd >= 0);
    assert(send(fd, slow_request, sizeof(slow_request) - 1, 0) == (ssize_t)(sizeof(slow_request) - 1));
    {
        struct linger reset = {1, 0};
        assert(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    }
    close(fd);

    {
        const struct timespec delay = {0, 500000000L};
        (void)nanosleep(&delay, NULL);
    }
    {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result != 0)
            fprintf(stderr, "server exited early: result=%ld status=%d signal=%d\n", (long)result, status,
                    WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        assert(result == 0);
    }

    fd = connect_loopback(port);
    assert(fd >= 0);
    assert(send(fd, status_request, sizeof(status_request) - 1, 0) == (ssize_t)(sizeof(status_request) - 1));
    received = recv(fd, response, sizeof(response) - 1, 0);
    assert(received > 0);
    response[received] = '\0';
    assert(strstr(response, "HTTP/1.1 200 OK") != NULL);
    close(fd);

    exchange(port, status_request, 200, true);
    assert(kill(child, SIGTERM) == 0);
    assert(waitpid(child, &status, 0) == child);
    session_authentication_test();
    puts("http server disconnect and session authentication tests passed");
    return 0;
}
