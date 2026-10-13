#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "metalsharp_backend/http_server.h"
#include "metalsharp_backend/local_socket.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* Each server under test gets its own short-lived home so the socket path
 * stays inside sockaddr_un.sun_path. */
static void new_socket_path(char* path, size_t size) {
    char home_template[] = "/tmp/ms-http-test.XXXXXX";
    const char* home = mkdtemp(home_template);
    assert(home != NULL);
    assert(ms_local_socket_prepare_dir(home));
    assert(ms_local_socket_path(home, MS_LOCAL_SOCKET_BACKEND, path, size));
}

static void remove_socket_home(const char* path) {
    char run_dir[128], home[128];
    char* slash;
    snprintf(run_dir, sizeof(run_dir), "%s", path);
    slash = strrchr(run_dir, '/');
    assert(slash != NULL);
    *slash = '\0';
    snprintf(home, sizeof(home), "%s", run_dir);
    slash = strrchr(home, '/');
    assert(slash != NULL);
    *slash = '\0';
    (void)unlink(path);
    assert(rmdir(run_dir) == 0);
    assert(rmdir(home) == 0);
}

static int wait_for_server(const char* path) {
    int fd = -1;
    for (int attempt = 0; attempt < 200 && fd < 0; ++attempt) {
        const struct timespec delay = {0, 10000000L};
        fd = ms_local_socket_connect(path);
        if (fd < 0)
            (void)nanosleep(&delay, NULL);
    }
    assert(fd >= 0);
    return fd;
}

static void exchange(const char* path, const char* request, int expected_status, bool legacy_cors) {
    int fd = ms_local_socket_connect(path);
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
    char path[128];
    new_socket_path(path, sizeof(path));
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
        _exit(ms_http_serve_unix(path, NULL, authenticated_handler, NULL, captured) == 0 ? 0 : 1);
    close(wait_for_server(path));
    exchange(path, "GET /forbidden HTTP/1.1\r\nHost: localhost\r\n\r\n", 401, false);
    // CORS-simple writes and even incomplete bodies are rejected before dispatch/read.
    exchange(path,
             "POST /forbidden HTTP/1.1\r\nHost: localhost\r\nOrigin: https://store.steampowered.com\r\nContent-Type: "
             "text/plain\r\nContent-Length: 64000000\r\n\r\n",
             401, false);
    exchange(path,
             "GET /forbidden HTTP/1.1\r\nHost: localhost\r\nX-MetalSharp-Client-Token: "
             "0000000000000000000000000000000000000000000000000000000000000000\r\n\r\n",
             401, false);
    snprintf(request, sizeof(request),
             "GET /status HTTP/1.1\r\nHost: localhost\r\nx-MeTaLsHaRp-ClIeNt-ToKeN: %s\r\n\r\n", token);
    exchange(path, request, 200, false);
    snprintf(request, sizeof(request),
             "POST /status HTTP/1.1\r\nHost: localhost\r\nX-MetalSharp-Client-Token: %s\r\nContent-Type: "
             "application/json\r\nContent-Length: 2\r\n\r\n{}",
             token);
    exchange(path, request, 200, false);
    snprintf(request, sizeof(request),
             "GET /forbidden HTTP/1.1\r\nHost: localhost\r\nX-MetalSharp-Client-Token: "
             "%s\r\nx-metalsharp-client-token: %s\r\n\r\n",
             token, token);
    exchange(path, request, 400, false);
    int status;
    assert(kill(child, SIGTERM) == 0);
    assert(waitpid(child, &status, 0) == child);
    remove_socket_home(path);
}

static void socket_lifecycle_test(void) {
    static const char status_request[] = "GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char path[128], response[512], run_dir[128];
    struct stat info;
    int fd, status, decoy;
    ssize_t received;
    new_socket_path(path, sizeof(path));
    snprintf(run_dir, sizeof(run_dir), "%s", path);
    *strrchr(run_dir, '/') = '\0';
    assert(stat(run_dir, &info) == 0 && (info.st_mode & 0777) == 0700);
    {
        char long_home[200];
        memset(long_home, 'a', sizeof(long_home) - 1);
        long_home[0] = '/';
        long_home[sizeof(long_home) - 1] = '\0';
        assert(!ms_local_socket_path(long_home, MS_LOCAL_SOCKET_BACKEND, response, sizeof(response)));
    }
    /* A regular file squatting on the path is never replaced. */
    decoy = open(path, O_CREAT | O_WRONLY, 0600);
    assert(decoy >= 0);
    close(decoy);
    assert(ms_local_socket_listen(path, 1) < 0);
    assert(unlink(path) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
        _exit(ms_http_serve_unix(path, NULL, handler, NULL, NULL) == 0 ? 0 : 1);
    fd = wait_for_server(path);
    assert(stat(path, &info) == 0 && S_ISSOCK(info.st_mode) && (info.st_mode & 0777) == 0600);
    assert(ms_local_socket_peer_is_owner(fd));
    assert(send(fd, status_request, sizeof(status_request) - 1, 0) == (ssize_t)(sizeof(status_request) - 1));
    received = recv(fd, response, sizeof(response) - 1, 0);
    assert(received > 0);
    response[received] = '\0';
    assert(strstr(response, "HTTP/1.1 200 OK") != NULL);
    close(fd);
    /* A live listener is never displaced by a second instance. */
    assert(ms_local_socket_listen(path, 1) < 0);
    assert(kill(child, SIGTERM) == 0);
    assert(waitpid(child, &status, 0) == child);
    /* SIGTERM kills the forked child without cleanup; the stale socket left
     * behind must be reclaimable by the next instance. */
    fd = ms_local_socket_listen(path, 1);
    assert(fd >= 0);
    ms_local_socket_close(path, fd);
    assert(lstat(path, &info) != 0);
    remove_socket_home(path);
}

int main(void) {
    static const char slow_request[] = "GET /slow HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    static const char status_request[] = "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    char path[128];
    pid_t child;
    int fd = -1;
    int status = 0;
    char response[512];
    ssize_t received;

    new_socket_path(path, sizeof(path));
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        _exit(ms_http_serve_unix(path, NULL, handler, NULL, NULL) == 0 ? 0 : 1);
    }

    fd = wait_for_server(path);
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

    fd = ms_local_socket_connect(path);
    assert(fd >= 0);
    assert(send(fd, status_request, sizeof(status_request) - 1, 0) == (ssize_t)(sizeof(status_request) - 1));
    received = recv(fd, response, sizeof(response) - 1, 0);
    assert(received > 0);
    response[received] = '\0';
    assert(strstr(response, "HTTP/1.1 200 OK") != NULL);
    close(fd);

    exchange(path, status_request, 200, false);
    /* Browser-shaped requests are rejected: an Origin header, a non-loopback
     * Host, or a non-JSON body. */
    exchange(path, "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://example.com\r\n\r\n", 401, false);
    exchange(path, "GET /status HTTP/1.1\r\nHost: example.com\r\n\r\n", 401, false);
    exchange(path,
             "POST /status HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}", 401,
             false);
    exchange(
        path,
        "POST /status HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
        200, false);
    assert(kill(child, SIGTERM) == 0);
    assert(waitpid(child, &status, 0) == child);
    remove_socket_home(path);
    session_authentication_test();
    socket_lifecycle_test();
    puts("http server disconnect, session authentication and socket lifecycle tests passed");
    return 0;
}
