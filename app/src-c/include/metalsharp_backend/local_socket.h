#ifndef METALSHARP_BACKEND_LOCAL_SOCKET_H
#define METALSHARP_BACKEND_LOCAL_SOCKET_H

#include <stdbool.h>
#include <stddef.h>

/* Owner-only Unix-domain sockets under <METALSHARP_HOME>/run.
 *
 * MetalSharp's local IPC endpoints (backend API, Steam bridge) live in one
 * 0700 directory owned by the current user instead of on fixed TCP ports,
 * which can collide with other software. */

#define MS_LOCAL_SOCKET_DIR          "run"
#define MS_LOCAL_SOCKET_BACKEND      "backend.sock"
#define MS_LOCAL_SOCKET_STEAM_BRIDGE "steam-bridge.sock"

/* Create (or tighten) <home>/run as a 0700 directory owned by the caller.
 * Fails when the path is a symlink, not a directory, or owned by someone else. */
bool ms_local_socket_prepare_dir(const char* home);

/* Write <home>/run/<name> into out. Fails when the result would not fit in
 * sockaddr_un.sun_path. Does not touch the filesystem. */
bool ms_local_socket_path(const char* home, const char* name, char* out, size_t out_size);

/* Bind and listen on path (close-on-exec, mode 0600). A stale socket file
 * left by a crashed process is replaced; a live listener or a non-socket at
 * path fails with EADDRINUSE / EEXIST. Returns the listening fd or -1. */
int ms_local_socket_listen(const char* path, int backlog);

/* Close listen_fd, then remove path unless another live listener owns it. */
void ms_local_socket_close(const char* path, int listen_fd);

/* Connect to path. Returns the fd or -1. */
int ms_local_socket_connect(const char* path);

/* True when the peer on a connected Unix socket runs as the same user. */
bool ms_local_socket_peer_is_owner(int fd);

#endif
