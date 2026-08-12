/// @file test_ws2_32_events.cpp
/// @brief Regression tests for #427: WSAWaitForMultipleEvents must wait on the
///        actual WSA event objects (and their sockets), not poll stdin and
///        fabricate WAIT_OBJECT_0.
///
/// Before the fix, ws2_32_WSAWaitForMultipleEvents ignored lphEvents entirely,
/// poll()ed fd 0 (stdin), and always returned WAIT_OBJECT_0 — so games waiting
/// on socket events woke spuriously (or never) and always saw success. The
/// event-object API (WSACreateEvent/WSASetEvent/WSAResetEvent) did not exist
/// as real state, and WSAEnumNetworkEvents fabricated FD_READ|FD_WRITE|
/// FD_ACCEPT|FD_CONNECT unconditionally.
///
/// The tests drive the shim through createWs2_32Shim() exactly as the PE
/// loader resolves ws2_32.dll imports, over real loopback sockets.

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <metalsharp/ExtraShims.h>
#include <metalsharp/NetworkContext.h>
#include <metalsharp/Win32Types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

using namespace metalsharp::win32;
using metalsharp::ShimLibrary;

#ifndef FALSE
#define FALSE 0
#define TRUE  1
#endif

// --- Function-pointer types matching the shim's MSABI exports ---------------

typedef void*(MSABI* CreateEventFn)();
typedef int(MSABI* CloseEventFn)(void*);
typedef int(MSABI* SetEventFn)(void*);
typedef int(MSABI* ResetEventFn)(void*);
typedef int(MSABI* WaitFn)(DWORD cEvents, void** lphEvents, BOOL fWaitAll, DWORD dwTimeout, BOOL fAlertable);
typedef int(MSABI* EventSelectFn)(void* s, void* hEvent, uint32_t lNetworkEvents);
typedef int(MSABI* EnumNetworkEventsFn)(void* s, void* hEvent, void* lpNetworkEvents);
typedef void*(MSABI* SocketFn)(int af, int type, int proto, void*, DWORD, DWORD);
typedef int(MSABI* CloseSocketFn)(void* s);
typedef int(MSABI* BindFn)(void* s, const void* addr, int len);
typedef int(MSABI* ListenFn)(void* s, int backlog);
typedef int(MSABI* ConnectFn)(void* s, const void* addr, int len);
typedef int(MSABI* GetSockNameFn)(void* s, void* addr, int* len);
typedef void*(MSABI* AcceptFn)(void* s, void* addr, int* len);
typedef int(MSABI* SendFn)(void* s, const char* buf, int len, int flags);
typedef int(MSABI* RecvFn)(void* s, char* buf, int len, int flags);
typedef int(MSABI* ShutdownFn)(void* s, int how);
typedef int(MSABI* GetLastErrorFn)();

// WSANETWORKEVENTS layout (winsock2.h): LONG lNetworkEvents + iErrorCode[FD_MAX_EVENTS].
struct WsaNetworkEvents {
    int32_t lNetworkEvents;
    int32_t iErrorCode[10];
};

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static void* makeLoopbackSockaddr(sockaddr_in* out, uint16_t port) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    out->sin_port = htons(port);
    return out;
}

int main() {
    ShimLibrary shim = createWs2_32Shim();
    auto getFn = [&](const char* name) -> void* { return shim.functions.at(name)(); };

    CreateEventFn createEvent = reinterpret_cast<CreateEventFn>(getFn("WSACreateEvent"));
    CloseEventFn closeEvent = reinterpret_cast<CloseEventFn>(getFn("WSACloseEvent"));
    SetEventFn setEvent = reinterpret_cast<SetEventFn>(getFn("WSASetEvent"));
    ResetEventFn resetEvent = reinterpret_cast<ResetEventFn>(getFn("WSAResetEvent"));
    WaitFn wait = reinterpret_cast<WaitFn>(getFn("WSAWaitForMultipleEvents"));
    EventSelectFn eventSelect = reinterpret_cast<EventSelectFn>(getFn("WSAEventSelect"));
    EnumNetworkEventsFn enumNetworkEvents = reinterpret_cast<EnumNetworkEventsFn>(getFn("WSAEnumNetworkEvents"));
    SocketFn socket = reinterpret_cast<SocketFn>(getFn("WSASocketA"));
    CloseSocketFn closeSocket = reinterpret_cast<CloseSocketFn>(getFn("closesocket"));
    BindFn bind = reinterpret_cast<BindFn>(getFn("bind"));
    ListenFn listen = reinterpret_cast<ListenFn>(getFn("listen"));
    ConnectFn connect = reinterpret_cast<ConnectFn>(getFn("connect"));
    GetSockNameFn getSockName = reinterpret_cast<GetSockNameFn>(getFn("getsockname"));
    AcceptFn accept = reinterpret_cast<AcceptFn>(getFn("accept"));
    SendFn send = reinterpret_cast<SendFn>(getFn("send"));
    RecvFn recv = reinterpret_cast<RecvFn>(getFn("recv"));
    ShutdownFn shutdown = reinterpret_cast<ShutdownFn>(getFn("shutdown"));
    GetLastErrorFn getLastError = reinterpret_cast<GetLastErrorFn>(getFn("WSAGetLastError"));

    // --- 1. Event-object basics ---------------------------------------------
    void* ev = createEvent();
    CHECK(ev != nullptr);

    // A fresh event is nonsignaled: a zero-timeout wait must time out, not
    // fabricate WAIT_OBJECT_0 (the pre-fix behavior).
    CHECK(wait(1, &ev, FALSE, 0, FALSE) == (int)WSA_WAIT_TIMEOUT);

    CHECK(setEvent(ev) == 1);
    CHECK(wait(1, &ev, FALSE, 0, FALSE) == (int)WSA_WAIT_OBJECT_0);
    CHECK(resetEvent(ev) == 1);
    CHECK(wait(1, &ev, FALSE, 0, FALSE) == (int)WSA_WAIT_TIMEOUT);

    // --- 2. Multi-event indexing and fWaitAll --------------------------------
    void* evs[2] = {createEvent(), createEvent()};
    CHECK(evs[0] != nullptr && evs[1] != nullptr);

    CHECK(setEvent(evs[1]) == 1);
    CHECK(wait(2, evs, FALSE, 0, FALSE) == (int)(WSA_WAIT_OBJECT_0 + 1));
    CHECK(setEvent(evs[0]) == 1);
    // Lowest signaled index wins when several are signaled.
    CHECK(wait(2, evs, FALSE, 0, FALSE) == (int)WSA_WAIT_OBJECT_0);
    CHECK(resetEvent(evs[0]) == 1);
    CHECK(resetEvent(evs[1]) == 1);

    // fWaitAll: one signaled event is not enough...
    CHECK(setEvent(evs[0]) == 1);
    CHECK(wait(2, evs, TRUE, 50, FALSE) == (int)WSA_WAIT_TIMEOUT);
    // ...but both are.
    CHECK(setEvent(evs[1]) == 1);
    CHECK(wait(2, evs, TRUE, 0, FALSE) == (int)WSA_WAIT_OBJECT_0);
    CHECK(resetEvent(evs[0]) == 1);
    CHECK(resetEvent(evs[1]) == 1);

    // --- 3. Invalid handles fail the wait -------------------------------------
    CHECK(closeEvent(evs[1]) == 1);
    CHECK(wait(2, evs, FALSE, 0, FALSE) == (int)WSA_WAIT_FAILED);
    CHECK(getLastError() == (int)WSA_INVALID_HANDLE);
    CHECK(closeEvent(evs[0]) == 1);

    CHECK(wait(0, nullptr, FALSE, 0, FALSE) == (int)WSA_WAIT_FAILED);
    CHECK(getLastError() == (int)WSA_INVALID_PARAMETER);

    // --- 4. A cross-thread WSASetEvent wakes a blocked infinite wait ----------
    void* evX = createEvent();
    CHECK(evX != nullptr);
    std::thread signaller([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        setEvent(evX);
    });
    CHECK(wait(1, &evX, FALSE, WSA_INFINITE, FALSE) == (int)WSA_WAIT_OBJECT_0);
    signaller.join();
    CHECK(closeEvent(evX) == 1);

    // --- 5. Real socket activity drives the wait (the core regression) --------
    // Server side: loopback listener on an ephemeral port.
    void* srv = socket(AF_INET, SOCK_STREAM, 0, nullptr, 0, 0);
    CHECK(srv != reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    sockaddr_in sa;
    CHECK(bind(srv, makeLoopbackSockaddr(&sa, 0), sizeof(sa)) == 0);
    sockaddr_in bound;
    socklen_t boundLen = sizeof(bound);
    CHECK(getSockName(srv, &bound, reinterpret_cast<int*>(&boundLen)) == 0);
    CHECK(listen(srv, 2) == 0);

    // Client side: connect (blocking loopback connect completes immediately).
    void* cli = socket(AF_INET, SOCK_STREAM, 0, nullptr, 0, 0);
    CHECK(cli != reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    sockaddr_in csa;
    CHECK(connect(cli, makeLoopbackSockaddr(&csa, ntohs(bound.sin_port)), sizeof(csa)) == 0);
    void* acc = accept(srv, nullptr, nullptr);
    CHECK(acc != reinterpret_cast<void*>(static_cast<intptr_t>(-1)));

    // Watch the client socket for readable data only (FD_WRITE/FD_CONNECT are
    // excluded so the wait cannot be satisfied by writability alone).
    void* evR = createEvent();
    CHECK(evR != nullptr);
    CHECK(eventSelect(cli, evR, FD_READ | FD_CLOSE) == 0);

    // No data yet: a bounded wait must time out (the old shim returned 0).
    CHECK(wait(1, &evR, FALSE, 200, FALSE) == (int)WSA_WAIT_TIMEOUT);

    // Deliver data: the wait must now be satisfied by the socket activity.
    CHECK(send(acc, "hi", 2, 0) == 2);
    CHECK(wait(1, &evR, FALSE, 2000, FALSE) == (int)WSA_WAIT_OBJECT_0);

    // WSAEnumNetworkEvents reports the real, mask-nominated events only.
    WsaNetworkEvents ne;
    memset(&ne, 0, sizeof(ne));
    CHECK(enumNetworkEvents(cli, evR, &ne) == 0);
    CHECK((ne.lNetworkEvents & FD_READ) != 0);
    CHECK((ne.lNetworkEvents & FD_WRITE) == 0);
    CHECK((ne.lNetworkEvents & FD_ACCEPT) == 0);
    CHECK((ne.lNetworkEvents & FD_CONNECT) == 0);
    CHECK((ne.lNetworkEvents & FD_CLOSE) == 0);

    // Enum resets the event; after draining the data the wait times out again.
    char buf[16];
    CHECK(recv(cli, buf, sizeof(buf), 0) == 2);
    CHECK(wait(1, &evR, FALSE, 0, FALSE) == (int)WSA_WAIT_TIMEOUT);

    // Peer close surfaces as FD_CLOSE (edge-triggered: reported once).
    CHECK(shutdown(acc, 1 /*SD_SEND*/) == 0);
    CHECK(wait(1, &evR, FALSE, 2000, FALSE) == (int)WSA_WAIT_OBJECT_0);
    memset(&ne, 0, sizeof(ne));
    CHECK(enumNetworkEvents(cli, evR, &ne) == 0);
    CHECK((ne.lNetworkEvents & FD_CLOSE) != 0);
    CHECK(wait(1, &evR, FALSE, 0, FALSE) == (int)WSA_WAIT_TIMEOUT);

    // --- 6. FD_ACCEPT on a listening socket -----------------------------------
    void* evL = createEvent();
    CHECK(evL != nullptr);
    int selectListenerResult = eventSelect(srv, evL, FD_ACCEPT);
    CHECK(selectListenerResult == 0);

    void* cli2 = socket(AF_INET, SOCK_STREAM, 0, nullptr, 0, 0);
    CHECK(cli2 != reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    sockaddr_in csa2;
    CHECK(connect(cli2, makeLoopbackSockaddr(&csa2, ntohs(bound.sin_port)), sizeof(csa2)) == 0);

    CHECK(wait(1, &evL, FALSE, 2000, FALSE) == (int)WSA_WAIT_OBJECT_0);
    memset(&ne, 0, sizeof(ne));
    CHECK(enumNetworkEvents(srv, evL, &ne) == 0);
    CHECK((ne.lNetworkEvents & FD_ACCEPT) != 0);
    void* acc2 = accept(srv, nullptr, nullptr);
    CHECK(acc2 != reinterpret_cast<void*>(static_cast<intptr_t>(-1)));

    // --- Cleanup --------------------------------------------------------------
    closeSocket(acc2);
    closeSocket(cli2);
    closeSocket(acc);
    closeSocket(cli);
    closeSocket(srv);
    closeEvent(evR);
    closeEvent(evL);
    closeEvent(ev);

    if (g_failures == 0) {
        printf("PASS: ws2_32 event-object shim (WSAWaitForMultipleEvents, WSAEnumNetworkEvents)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
