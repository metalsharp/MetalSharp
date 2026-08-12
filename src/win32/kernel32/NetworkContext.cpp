/// @file NetworkContext.cpp
/// @brief Winsock shim for socket handle management and pipe pairs.
///
/// Wraps POSIX socket operations behind Win32 SOCKET handles and implements Winsock initialization, socket creation,
/// and named pipe pairs. Provides the networking layer that some games need for multiplayer or DRM.
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <metalsharp/Logger.h>
#include <metalsharp/NetworkContext.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace metalsharp {
namespace win32 {

namespace {
// Recursively create a directory path (mkdir -p). Mirrors the helper in
// VirtualFileSystem.cpp; needed here because the pipe base directory lives
// under ~/.metalsharp/prefix, which may not exist on a fresh install.
bool mkdirRecursive(const std::string& path) {
    size_t pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0) {
        std::string parent = path.substr(0, pos);
        struct stat st;
        if (stat(parent.c_str(), &st) != 0)
            mkdirRecursive(parent);
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}
} // namespace

thread_local uint32_t NetworkContext::t_wsaError = 0;

NetworkContext& NetworkContext::instance() {
    static NetworkContext ctx;
    return ctx;
}

void NetworkContext::initialize() {
    MS_INFO("NetworkContext: initializing");
}

int NetworkContext::allocSocket(int fd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int handle = m_nextHandle++;
    m_sockets[handle] = {fd, 0, nullptr};
    return handle;
}

int NetworkContext::releaseSocket(int handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    if (it == m_sockets.end())
        return -1;
    unassociateSocketFromEventLocked(handle);
    int fd = it->second.fd;
    m_sockets.erase(it);
    return fd;
}

int NetworkContext::getFd(int handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    return it != m_sockets.end() ? it->second.fd : -1;
}

void NetworkContext::setWsaError(uint32_t err) {
    t_wsaError = err;
}

uint32_t NetworkContext::getWsaError() const {
    return t_wsaError;
}

uint32_t NetworkContext::mapErrnoToWsa(int err) const {
    switch (err) {
    case EWOULDBLOCK:
        return WSAEWOULDBLOCK;
    case EINPROGRESS:
        return WSAEINPROGRESS;
    case EALREADY:
        return WSAEALREADY;
    case ENOTSOCK:
        return WSAENOTSOCK;
    case EDESTADDRREQ:
        return WSAEDESTADDRREQ;
    case EMSGSIZE:
        return WSAEMSGSIZE;
    case EPROTOTYPE:
        return WSAEPROTOTYPE;
    case ENOPROTOOPT:
        return WSAENOPROTOOPT;
    case EPROTONOSUPPORT:
        return WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT:
        return WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP:
        return WSAEOPNOTSUPP;
    case EPFNOSUPPORT:
        return WSAEPFNOSUPPORT;
    case EAFNOSUPPORT:
        return WSAEAFNOSUPPORT;
    case EADDRINUSE:
        return WSAEADDRINUSE;
    case EADDRNOTAVAIL:
        return WSAEADDRNOTAVAIL;
    case ENETDOWN:
        return WSAENETDOWN;
    case ENETUNREACH:
        return WSAENETUNREACH;
    case ENETRESET:
        return WSAENETRESET;
    case ECONNABORTED:
        return WSAECONNABORTED;
    case ECONNRESET:
        return WSAECONNRESET;
    case ENOBUFS:
        return WSAENOBUFS;
    case EISCONN:
        return WSAEISCONN;
    case ENOTCONN:
        return WSAENOTCONN;
    case ESHUTDOWN:
        return WSAESHUTDOWN;
    case ETIMEDOUT:
        return WSAETIMEDOUT;
    case ECONNREFUSED:
        return WSAECONNREFUSED;
    case EHOSTDOWN:
        return WSAEHOSTDOWN;
    case EHOSTUNREACH:
        return WSAEHOSTUNREACH;
    default:
        return WSASYSCALLFAILURE;
    }
}

void NetworkContext::setSocketEventMask(int handle, uint32_t mask, void* eventHandle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    if (it != m_sockets.end()) {
        it->second.eventMask = mask;
        it->second.eventHandle = eventHandle;
    }
}

uint32_t NetworkContext::getSocketEventMask(int handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    return it != m_sockets.end() ? it->second.eventMask : 0;
}

int NetworkContext::allocEvent() {
    std::lock_guard<std::mutex> lock(m_mutex);
    int eventId = m_nextEvent++;
    m_events[eventId] = EventEntry{};
    return eventId;
}

bool NetworkContext::isValidEvent(int eventId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_events.find(eventId) != m_events.end();
}

void NetworkContext::closeEvent(int eventId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.erase(eventId);
}

void NetworkContext::setEventSignaled(int eventId, bool signaled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_events.find(eventId);
    if (it == m_events.end())
        return;
    if (signaled && !it->second.signaled)
        wakePipeWriteLocked(); // cross-thread wake for blocked waits
    it->second.signaled = signaled;
}

bool NetworkContext::isEventSignaled(int eventId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_events.find(eventId);
    return it != m_events.end() && it->second.signaled;
}

std::vector<int> NetworkContext::socketsForEvent(int eventId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_events.find(eventId);
    if (it == m_events.end())
        return {};
    return it->second.sockets;
}

void NetworkContext::associateSocketWithEvent(int socketHandle, int eventId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto ev = m_events.find(eventId);
    if (ev == m_events.end())
        return;
    // A socket belongs to at most one event object at a time.
    unassociateSocketFromEventLocked(socketHandle);
    ev->second.sockets.push_back(socketHandle);
}

void NetworkContext::unassociateSocketFromEventLocked(int socketHandle) {
    for (auto& pair : m_events) {
        auto& sockets = pair.second.sockets;
        sockets.erase(std::remove(sockets.begin(), sockets.end(), socketHandle), sockets.end());
    }
}

void NetworkContext::unassociateSocketFromEvent(int socketHandle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    unassociateSocketFromEventLocked(socketHandle);
}

void NetworkContext::markSocketConnecting(int handle, bool connecting) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    if (it != m_sockets.end())
        it->second.connecting = connecting;
}

void NetworkContext::resetSocketEventTracking(int handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    if (it != m_sockets.end())
        it->second.closeReported = false;
}

bool NetworkContext::socketHasReportedClose(int handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    return it != m_sockets.end() && it->second.closeReported;
}

int NetworkContext::eventWakeReadFd() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ensureEventWakePipeLocked();
    return m_eventWakePipe[0];
}

void NetworkContext::ensureEventWakePipeLocked() {
    if (m_eventWakePipe[0] >= 0)
        return;
    if (::pipe(m_eventWakePipe) != 0) {
        m_eventWakePipe[0] = m_eventWakePipe[1] = -1;
        return;
    }
    for (int end : {m_eventWakePipe[0], m_eventWakePipe[1]}) {
        int flags = fcntl(end, F_GETFL, 0);
        fcntl(end, F_SETFL, flags | O_NONBLOCK);
    }
}

void NetworkContext::wakePipeWriteLocked() {
    ensureEventWakePipeLocked();
    if (m_eventWakePipe[1] < 0)
        return;
    char byte = 1;
    (void)::write(m_eventWakePipe[1], &byte, 1); // EAGAIN when full: pending byte suffices
}

uint32_t NetworkContext::applySocketPollResult(int handle, short revents, bool listening) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    if (it == m_sockets.end())
        return 0;
    SocketEntry& entry = it->second;
    const uint32_t mask = entry.eventMask;
    if (mask == 0)
        return 0;

    uint32_t events = 0;
    const bool sawPollOut = (revents & POLLOUT) != 0;

    // FD_READ / FD_ACCEPT are level-triggered: report them whenever the
    // condition still holds (data pending / connection pending).
    if ((revents & POLLIN) && (mask & (FD_READ | FD_ACCEPT))) {
        if (listening) {
            if (mask & FD_ACCEPT)
                events |= FD_ACCEPT;
        } else if ((mask & FD_ACCEPT) && !(mask & FD_READ)) {
            // macOS does not expose SO_ACCEPTCONN for every socket family;
            // an FD_ACCEPT-only registration is itself an unambiguous listen
            // request, so preserve the accept notification in that case.
            events |= FD_ACCEPT;
        } else {
            int available = 0;
            if (ioctl(entry.fd, FIONREAD, &available) == 0) {
                if (available > 0 && (mask & FD_READ)) {
                    events |= FD_READ;
                } else if ((mask & FD_CLOSE) && !entry.closeReported) {
                    // An orderly peer shutdown is reported by poll(2) as
                    // readable EOF (POLLIN with no bytes), not necessarily
                    // as POLLHUP. Windows surfaces that transition as
                    // FD_CLOSE rather than a spurious FD_READ.
                    events |= FD_CLOSE;
                    entry.closeReported = true;
                }
            }
        }
    }

    // FD_CONNECT fires once when a non-blocking connect completes (with or
    // without error). FD_WRITE is polled level-triggered: reported whenever
    // the socket is writable and FD_WRITE is nominated — a game's send-until-
    // WSAEWOULDBLOCK loop makes progress, and the wait blocks once the send
    // buffer fills (no POLLOUT).
    if ((revents & (POLLOUT | POLLERR | POLLHUP)) && (mask & FD_CONNECT) && entry.connecting) {
        events |= FD_CONNECT;
        entry.connecting = false;
    } else if (sawPollOut && (mask & FD_WRITE)) {
        events |= FD_WRITE;
    }

    // FD_CLOSE fires once per close (edge-triggered, like Windows); the wait
    // loop excludes sockets that already reported it, so a half-closed socket
    // cannot keep a poll() round busy.
    if ((revents & (POLLHUP | POLLERR)) && (mask & FD_CLOSE) && !entry.closeReported) {
        events |= FD_CLOSE;
        entry.closeReported = true;
    }
    return events;
}

void NetworkContext::recordSocketEvents(int handle, uint32_t events) {
    if (events == 0)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    if (it != m_sockets.end())
        it->second.pendingEvents |= events;
}

uint32_t NetworkContext::takeSocketEvents(int handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(handle);
    if (it == m_sockets.end())
        return 0;
    uint32_t events = it->second.pendingEvents;
    it->second.pendingEvents = 0;
    return events;
}

bool NetworkContext::allocPipePair(const std::string& name, bool server, int outHandles[2]) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int handle = m_nextPipe++;

    const char* home = getenv("HOME");
    std::string baseDir = home ? std::string(home) + "/.metalsharp/prefix/pipe" : "/tmp/metalsharp/pipe";
    if (!mkdirRecursive(baseDir))
        return false;

    std::string pipePath = baseDir + "/" + name;
    for (auto& c : pipePath) {
        if (c == '\\')
            c = '_';
    }

    if (server) {
        unlink(pipePath.c_str());
        int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd < 0)
            return false;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, pipePath.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(listenFd);
            return false;
        }
        listen(listenFd, 1);

        PipeInstance pi;
        pi.name = name;
        pi.serverSide = true;
        pi.connected = false;
        pi.fds[0] = listenFd;
        pi.fds[1] = -1;
        m_pipes[handle] = pi;

        outHandles[0] = handle;
        outHandles[1] = -1;
        return true;
    } else {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return false;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, pipePath.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        PipeInstance pi;
        pi.name = name;
        pi.serverSide = false;
        pi.connected = true;
        pi.fds[0] = fd;
        pi.fds[1] = fd;
        m_pipes[handle] = pi;

        outHandles[0] = handle;
        outHandles[1] = handle;
        return true;
    }
}

bool NetworkContext::connectPipe(int handle, int clientFd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pipes.find(handle);
    if (it == m_pipes.end() || it->second.connected) {
        close(clientFd);
        return false;
    }
    if (it->second.fds[0] >= 0)
        close(it->second.fds[0]); // the listen socket has served its purpose
    it->second.fds[0] = clientFd;
    it->second.fds[1] = clientFd;
    it->second.connected = true;
    return true;
}

int NetworkContext::getPipeReadFd(int handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pipes.find(handle);
    if (it == m_pipes.end())
        return -1;
    return it->second.fds[0];
}

int NetworkContext::getPipeWriteFd(int handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pipes.find(handle);
    if (it == m_pipes.end())
        return -1;
    if (it->second.serverSide && !it->second.connected)
        return -1;
    return it->second.fds[1];
}

void NetworkContext::closePipe(int handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pipes.find(handle);
    if (it == m_pipes.end())
        return;
    if (it->second.fds[0] >= 0)
        close(it->second.fds[0]);
    if (it->second.fds[1] >= 0 && it->second.fds[1] != it->second.fds[0])
        close(it->second.fds[1]);
    m_pipes.erase(it);
}

} // namespace win32
} // namespace metalsharp
