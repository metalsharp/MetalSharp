#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "metalsharp_backend/local_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

bool ms_local_socket_prepare_dir(const char* home) {
    char dir[sizeof(((struct sockaddr_un*)0)->sun_path)];
    struct stat info;
    int written;
    if (home == NULL || home[0] == '\0')
        return false;
    written = snprintf(dir, sizeof(dir), "%s/%s", home, MS_LOCAL_SOCKET_DIR);
    if (written < 0 || (size_t)written >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;
    if (lstat(dir, &info) != 0)
        return false;
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid()) {
        errno = EPERM;
        return false;
    }
    if ((info.st_mode & 0777) != 0700 && chmod(dir, 0700) != 0)
        return false;
    return true;
}

bool ms_local_socket_path(const char* home, const char* name, char* out, size_t out_size) {
    int written;
    if (home == NULL || home[0] == '\0' || name == NULL || out == NULL)
        return false;
    written = snprintf(out, out_size, "%s/%s/%s", home, MS_LOCAL_SOCKET_DIR, name);
    if (written < 0 || (size_t)written >= out_size || (size_t)written >= sizeof(((struct sockaddr_un*)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

static bool fill_address(const char* path, struct sockaddr_un* address) {
    size_t length = path == NULL ? 0 : strlen(path);
    if (length == 0 || length >= sizeof(address->sun_path)) {
        errno = ENAMETOOLONG;
        return false;
    }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, length + 1);
    return true;
}

static int new_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    {
        int no_sigpipe = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
    }
#endif
    return fd;
}

int ms_local_socket_connect(const char* path) {
    struct sockaddr_un address;
    int fd;
    if (!fill_address(path, &address))
        return -1;
    fd = new_socket();
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int ms_local_socket_listen(const char* path, int backlog) {
    struct sockaddr_un address;
    struct stat info;
    int fd;
    if (!fill_address(path, &address))
        return -1;
    if (lstat(path, &info) == 0) {
        int probe;
        if (!S_ISSOCK(info.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        probe = ms_local_socket_connect(path);
        if (probe >= 0) {
            close(probe);
            errno = EADDRINUSE;
            return -1;
        }
        if (unlink(path) != 0 && errno != ENOENT)
            return -1;
    }
    fd = new_socket();
    if (fd < 0)
        return -1;
    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0 || chmod(path, 0600) != 0 ||
        listen(fd, backlog) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

void ms_local_socket_close(const char* path, int listen_fd) {
    struct stat info;
    int probe;
    if (listen_fd >= 0)
        close(listen_fd);
    if (path == NULL || lstat(path, &info) != 0 || !S_ISSOCK(info.st_mode))
        return;
    /* A newer instance may already own the path; only remove a dead socket. */
    probe = ms_local_socket_connect(path);
    if (probe >= 0) {
        close(probe);
        return;
    }
    (void)unlink(path);
}

bool ms_local_socket_peer_is_owner(int fd) {
    uid_t uid;
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) != 0)
        return false;
    return uid == geteuid();
}
