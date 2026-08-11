/// @file NetworkContext.cpp
/// @brief Winsock shim for socket handle management and pipe pairs.
///
/// Wraps POSIX socket operations behind Win32 SOCKET handles and implements Winsock initialization, socket creation,
/// and named pipe pairs. Provides the networking layer that some games need for multiplayer or DRM.
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace metalsharp {
namespace win32 {

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

bool NetworkContext::allocPipePair(const std::string& name, bool server, int outHandles[2]) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int handle = m_nextPipe++;

    const char* home = getenv("HOME");
    std::string baseDir = home ? std::string(home) + "/.metalsharp/prefix/pipe" : "/tmp/metalsharp/pipe";
    mkdir(baseDir.c_str(), 0755);

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
