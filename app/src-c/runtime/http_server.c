#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "metalsharp_backend/http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static char* copy_range(const char* start, size_t length) {
    char* copy = (char*)malloc(length + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

void ms_http_request_free(ms_http_request* request) {
    if (request == NULL)
        return;
    free(request->method);
    free(request->target);
    free(request->path);
    free(request->query);
    free(request->body);
    memset(request, 0, sizeof(*request));
}

static ssize_t find_header_end(const unsigned char* buffer, size_t length) {
    size_t i;
    if (length < 4)
        return -1;
    for (i = 3; i < length; ++i) {
        if (buffer[i - 3] == '\r' && buffer[i - 2] == '\n' && buffer[i - 1] == '\r' && buffer[i] == '\n') {
            return (ssize_t)(i + 1);
        }
    }
    return -1;
}

static bool send_all(int fd, const void* data, size_t length) {
    const unsigned char* cursor = (const unsigned char*)data;
    while (length > 0) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t sent = send(fd, cursor, length, flags);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (sent == 0)
            return false;
        cursor += (size_t)sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool receive_request(int fd, ms_http_request* request) {
    unsigned char* buffer = NULL;
    size_t length = 0;
    size_t capacity = 8192;
    ssize_t header_end;
    char* header_copy = NULL;
    char* line;
    char* save = NULL;
    size_t content_length = 0;
    bool content_length_seen = false;

    memset(request, 0, sizeof(*request));
    buffer = (unsigned char*)malloc(capacity);
    if (buffer == NULL)
        return false;
    for (;;) {
        header_end = find_header_end(buffer, length);
        if (header_end >= 0)
            break;
        if (length == MS_HTTP_MAX_HEADER_BYTES) {
            free(buffer);
            return false;
        }
        if (length == capacity) {
            size_t next = capacity * 2;
            if (next > MS_HTTP_MAX_HEADER_BYTES)
                next = MS_HTTP_MAX_HEADER_BYTES;
            buffer = (unsigned char*)realloc(buffer, next);
            if (buffer == NULL)
                return false;
            capacity = next;
        }
        {
            ssize_t received = recv(fd, buffer + length, capacity - length, 0);
            if (received < 0) {
                if (errno == EINTR)
                    continue;
                free(buffer);
                return false;
            }
            if (received == 0) {
                free(buffer);
                return false;
            }
            length += (size_t)received;
        }
    }

    header_copy = copy_range((const char*)buffer, (size_t)header_end);
    if (header_copy == NULL)
        goto fail;
    line = strtok_r(header_copy, "\r\n", &save);
    if (line == NULL)
        goto fail;
    {
        char* first_space = strchr(line, ' ');
        char* second_space;
        if (first_space == NULL)
            goto fail;
        second_space = strchr(first_space + 1, ' ');
        if (second_space == NULL)
            goto fail;
        request->method = copy_range(line, (size_t)(first_space - line));
        request->target = copy_range(first_space + 1, (size_t)(second_space - first_space - 1));
        if (request->method == NULL || request->target == NULL)
            goto fail;
        if (strcmp(second_space + 1, "HTTP/1.1") != 0 && strcmp(second_space + 1, "HTTP/1.0") != 0)
            goto fail;
    }
    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        char* colon = strchr(line, ':');
        char* name;
        char* value;
        if (colon == NULL)
            continue;
        name = copy_range(line, (size_t)(colon - line));
        value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        if (name == NULL)
            goto fail;
        for (char* p = name; *p != '\0'; ++p) {
            if (*p >= 'A' && *p <= 'Z')
                *p = (char)(*p - 'A' + 'a');
        }
        if (strcmp(name, "content-length") == 0) {
            char* end = NULL;
            unsigned long long parsed;
            errno = 0;
            parsed = strtoull(value, &end, 10);
            while (end != NULL && (*end == ' ' || *end == '\t'))
                end++;
            if (errno != 0 || end == value || (end != NULL && *end != '\0') || parsed > MS_HTTP_MAX_BODY_BYTES) {
                free(name);
                goto fail;
            }
            if (content_length_seen) {
                free(name);
                goto fail;
            }
            content_length = (size_t)parsed;
            content_length_seen = true;
        }
        free(name);
    }
    free(header_copy);
    header_copy = NULL;

    {
        const char* query = strchr(request->target, '?');
        request->path =
            copy_range(request->target, query == NULL ? strlen(request->target) : (size_t)(query - request->target));
        request->query = query == NULL ? copy_range("", 0) : strdup(query + 1);
        if (request->path == NULL || request->query == NULL || request->path[0] != '/')
            goto fail;
    }
    if (content_length_seen && content_length > 0) {
        size_t have = length - (size_t)header_end;
        request->body = (unsigned char*)malloc(content_length + 1);
        if (request->body == NULL)
            goto fail;
        if (have > content_length)
            have = content_length;
        memcpy(request->body, buffer + header_end, have);
        while (have < content_length) {
            ssize_t received = recv(fd, request->body + have, content_length - have, 0);
            if (received < 0) {
                if (errno == EINTR)
                    continue;
                goto fail;
            }
            if (received == 0)
                goto fail;
            have += (size_t)received;
        }
        request->body[content_length] = '\0';
        request->body_length = content_length;
    }
    free(buffer);
    return true;
fail:
    free(header_copy);
    free(buffer);
    ms_http_request_free(request);
    return false;
}

static const char* reason_phrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Response";
    }
}

static void send_response(int fd, const ms_http_response* response) {
    char header[512];
    const char* content_type = response->content_type == NULL ? "application/json" : response->content_type;
    int written = snprintf(header, sizeof(header),
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                           response->status, reason_phrase(response->status), content_type, response->body_length);
    if (written > 0 && (size_t)written < sizeof(header)) {
        (void)send_all(fd, header, (size_t)written);
        if (response->body_length > 0 && response->body != NULL)
            (void)send_all(fd, response->body, response->body_length);
    }
}

int ms_http_serve(unsigned short port, volatile sig_atomic_t* stop_flag, ms_http_handler handler, void* context) {
    int server_fd;
    int reuse = 1;
    struct sockaddr_in address;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        return -1;
    /* Backend children (Wine, curl, installers) are forked from request
     * handlers. Never let them inherit the listener, otherwise the port can
     * remain occupied after the backend exits and a later app launch cannot
     * restart the backend. */
    if (fcntl(server_fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(server_fd);
        return -1;
    }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        close(server_fd);
        return -1;
    }
    address.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0 || listen(server_fd, 16) < 0) {
        close(server_fd);
        return -1;
    }
    /* A timeout-free blocking accept is not reliably interrupted by signal()
     * on every supported macOS libc configuration. Polling the listening fd
     * keeps SIGTERM deterministic without a signal-handler socket write. */
    if (fcntl(server_fd, F_SETFL, O_NONBLOCK) < 0) {
        close(server_fd);
        return -1;
    }
    while (stop_flag == NULL || !*stop_flag) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec delay = {0, 10000000L};
                (void)nanosleep(&delay, NULL);
                continue;
            }
            close(server_fd);
            return -1;
        }
        if (fcntl(client_fd, F_SETFD, FD_CLOEXEC) < 0) {
            close(client_fd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        /* Renderer requests can time out while a long-running backend action
         * is still completing. A later response to that closed socket must
         * return EPIPE, not terminate the entire backend with SIGPIPE. */
        {
            int no_sigpipe = 1;
            (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
        }
#endif
        (void)fcntl(client_fd, F_SETFL, 0);
        {
            ms_http_request request;
            ms_http_response response = {500, "application/json", NULL, 0, false};
            if (receive_request(client_fd, &request)) {
                if (!handler(&request, &response, context)) {
                    response.status = 500;
                    response.content_type = "application/json";
                }
                ms_http_request_free(&request);
            } else {
                static const unsigned char bad_request[] = "{\"ok\":false,\"error\":\"bad request\"}";
                response.status = 400;
                response.body = bad_request;
                response.body_length = sizeof(bad_request) - 1;
            }
            send_response(client_fd, &response);
            if (response.owns_body)
                free((void*)response.body);
        }
        close(client_fd);
    }
    close(server_fd);
    return 0;
}
