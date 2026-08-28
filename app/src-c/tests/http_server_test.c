#include "metalsharp_backend/http_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

    assert(kill(child, SIGTERM) == 0);
    assert(waitpid(child, &status, 0) == child);
    puts("http server disconnect test passed");
    return 0;
}
