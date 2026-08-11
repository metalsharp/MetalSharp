/// @file NetworkContext.h
/// @brief Winsock (ws2_32.dll) shim mapping BSD sockets to Winsock semantics.
///
/// Translates Winsock error codes, socket event masks (FD_READ, FD_WRITE, FD_ACCEPT,
/// FD_CONNECT, FD_CLOSE), and WSAOVERLAPPED/WSABUF structures to native BSD socket
/// operations. Manages a socket handle table, named pipe pairs for inter-process
/// communication, and per-thread WSA error state. The ws2_32 shim delegates all
/// actual I/O to this context.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace metalsharp {
namespace win32 {

constexpr uint32_t WSAEWOULDBLOCK = 10035;
constexpr uint32_t WSAEINPROGRESS = 10036;
constexpr uint32_t WSAEALREADY = 10037;
constexpr uint32_t WSAENOTSOCK = 10038;
constexpr uint32_t WSAEDESTADDRREQ = 10039;
constexpr uint32_t WSAEMSGSIZE = 10040;
constexpr uint32_t WSAEPROTOTYPE = 10041;
constexpr uint32_t WSAENOPROTOOPT = 10042;
constexpr uint32_t WSAEPROTONOSUPPORT = 10043;
constexpr uint32_t WSAESOCKTNOSUPPORT = 10044;
constexpr uint32_t WSAEOPNOTSUPP = 10045;
constexpr uint32_t WSAEPFNOSUPPORT = 10046;
constexpr uint32_t WSAEAFNOSUPPORT = 10047;
constexpr uint32_t WSAEADDRINUSE = 10048;
constexpr uint32_t WSAEADDRNOTAVAIL = 10049;
constexpr uint32_t WSAENETDOWN = 10050;
constexpr uint32_t WSAENETUNREACH = 10051;
constexpr uint32_t WSAENETRESET = 10052;
constexpr uint32_t WSAECONNABORTED = 10053;
constexpr uint32_t WSAECONNRESET = 10054;
constexpr uint32_t WSAENOBUFS = 10055;
constexpr uint32_t WSAEISCONN = 10056;
constexpr uint32_t WSAENOTCONN = 10057;
constexpr uint32_t WSAESHUTDOWN = 10058;
constexpr uint32_t WSAETIMEDOUT = 10060;
constexpr uint32_t WSAECONNREFUSED = 10061;
constexpr uint32_t WSAEHOSTDOWN = 10064;
constexpr uint32_t WSAEHOSTUNREACH = 10065;
constexpr uint32_t WSASYSCALLFAILURE = 10107;

// WSA event-object wait results and limits (winsock2.h).
constexpr uint32_t WSA_WAIT_OBJECT_0 = 0;
constexpr uint32_t WSA_WAIT_IO_COMPLETION = 0x000000C0;
constexpr uint32_t WSA_WAIT_TIMEOUT = 0x00000102;
constexpr uint32_t WSA_WAIT_FAILED = 0xFFFFFFFF;
constexpr uint32_t WSA_INFINITE = 0xFFFFFFFF;
constexpr uint32_t WSA_MAXIMUM_WAIT_EVENTS = 64;

// WSA error codes used by the event-object API (winsock2.h / winerror.h).
constexpr uint32_t WSAEFAULT = 14;
constexpr uint32_t WSA_INVALID_HANDLE = 6;
constexpr uint32_t WSA_INVALID_PARAMETER = 87;

constexpr uint32_t FD_READ_BIT = 0;
constexpr uint32_t FD_WRITE_BIT = 1;
constexpr uint32_t FD_ACCEPT_BIT = 3;
constexpr uint32_t FD_CONNECT_BIT = 4;
constexpr uint32_t FD_CLOSE_BIT = 5;

constexpr uint32_t FD_READ = (1 << FD_READ_BIT);
constexpr uint32_t FD_WRITE = (1 << FD_WRITE_BIT);
constexpr uint32_t FD_ACCEPT = (1 << FD_ACCEPT_BIT);
constexpr uint32_t FD_CONNECT = (1 << FD_CONNECT_BIT);
constexpr uint32_t FD_CLOSE = (1 << FD_CLOSE_BIT);

constexpr uint32_t SIO_GET_EXTENSION_FUNCTION_POINTER = 0xC8000006;

struct WSAOVERLAPPED {
    void* Internal;
    void* InternalHigh;
    union {
        struct {
            void* Offset;
            void* OffsetHigh;
        };
        void* Pointer;
    };
    void* hEvent;
};

struct WSABUF {
    uint32_t len;
    char* buf;
};

struct PipeInstance {
    int fds[2];
    std::string name;
    bool serverSide;
    bool connected;
};

class NetworkContext {
  public:
    static NetworkContext& instance();

    int allocSocket(int fd);
    int releaseSocket(int handle);
    int getFd(int handle) const;

    void setWsaError(uint32_t err);
    uint32_t getWsaError() const;

    uint32_t mapErrnoToWsa(int err) const;

    /// Allocate a named-pipe pair and write the resulting handles into
    /// outHandles[0]/outHandles[1] (per-call storage, safe under
    /// concurrency). For a server pipe, outHandles[0] is the pipe handle
    /// (backed by the listen socket until connectPipe wires the accepted
    /// client connection) and outHandles[1] is -1. For a client pipe, both
    /// entries receive the connected handle. Returns false on failure
    /// without modifying outHandles.
    bool allocPipePair(const std::string& name, bool server, int outHandles[2]);
    /// Wire an accepted client socket into a server pipe handle, replacing
    /// the listen socket. Takes ownership of clientFd: it is closed when the
    /// handle is unknown or the pipe is already connected. Returns true once
    /// the pipe is connected and both its read/write fds reference clientFd.
    bool connectPipe(int handle, int clientFd);
    int getPipeReadFd(int handle) const;
    int getPipeWriteFd(int handle) const;
    void closePipe(int handle);

    void setSocketEventMask(int handle, uint32_t mask, void* eventHandle);
    uint32_t getSocketEventMask(int handle) const;

    // WSA event-object table (WSACreateEvent/WSACloseEvent/WSASetEvent/WSAResetEvent).
    int allocEvent();
    bool isValidEvent(int eventId) const;
    void closeEvent(int eventId);
    void setEventSignaled(int eventId, bool signaled);
    bool isEventSignaled(int eventId) const;
    std::vector<int> socketsForEvent(int eventId) const;
    void associateSocketWithEvent(int socketHandle, int eventId);
    void unassociateSocketFromEvent(int socketHandle);

    // Cross-thread wake pipe: WSASetEvent writes a byte so a blocked
    // WSAWaitForMultipleEvents poll() wakes and re-checks the signaled state.
    int eventWakeReadFd();

    // WSAEventSelect/FD_* edge tracking.
    void markSocketConnecting(int handle, bool connecting);
    // Starts a fresh recording period for the edge-triggered FD_CLOSE event
    // after a new WSAEventSelect registration.
    void resetSocketEventTracking(int handle);
    // True once FD_CLOSE has been reported for the socket; such sockets are
    // excluded from later waits (FD_CLOSE fires once, like Windows).
    bool socketHasReportedClose(int handle) const;
    // Classifies poll(2) results for a socket against its event mask, updates
    // the FD_CONNECT/FD_CLOSE edge-tracking state, and returns the FD_* bits
    // (masked by the socket's registered mask) that fired.
    uint32_t applySocketPollResult(int handle, short revents, bool listening);
    // Preserve edge-triggered notifications until WSAEnumNetworkEvents
    // consumes the event record after WSAWaitForMultipleEvents returns.
    void recordSocketEvents(int handle, uint32_t events);
    uint32_t takeSocketEvents(int handle);

    void initialize();

  private:
    // Must be called with m_mutex held.
    void unassociateSocketFromEventLocked(int socketHandle);
    NetworkContext() = default;

    struct SocketEntry {
        int fd;
        uint32_t eventMask = 0;
        void* eventHandle = nullptr;
        bool connecting = false;
        bool closeReported = false;
        uint32_t pendingEvents = 0;
    };

    struct EventEntry {
        bool signaled = false;
        std::vector<int> sockets;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<int, SocketEntry> m_sockets;
    int m_nextHandle = 100;

    std::unordered_map<int, EventEntry> m_events;
    int m_nextEvent = 9000;

    // Must be called with m_mutex held.
    void ensureEventWakePipeLocked();
    void wakePipeWriteLocked();
    int m_eventWakePipe[2] = {-1, -1};

    std::unordered_map<int, PipeInstance> m_pipes;
    int m_nextPipe = 5000;

    static thread_local uint32_t t_wsaError;
};

} // namespace win32
} // namespace metalsharp
