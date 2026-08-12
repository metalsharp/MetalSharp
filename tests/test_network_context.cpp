/// @file test_network_context.cpp
/// @brief Regression tests for NetworkContext named-pipe handle management.
///
/// Covers issue #426: allocPipePair returned a pointer to a shared static
/// array, so concurrent (and even sequential) calls clobbered each other's
/// handles, and shim_ConnectNamedPipe discarded the accepted client fd (an
/// fd leak) while plumbing a fresh anonymous pipe() that was never connected
/// to the handle table.
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <metalsharp/NetworkContext.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  [OK] %s\n", msg);                                                                                \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

using metalsharp::win32::NetworkContext;

static bool fdIsOpen(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

int main() {
    printf("=== NetworkContext Named Pipe Tests ===\n\n");

    NetworkContext& ctx = NetworkContext::instance();

    {
        printf("--- allocPipePair uses per-call storage ---\n");
        int a[2];
        int b[2];
        CHECK(ctx.allocPipePair("np_test_sequential_a", true, a), "server pipe A allocated");
        CHECK(ctx.allocPipePair("np_test_sequential_b", true, b), "server pipe B allocated");
        // Regression: the old implementation returned one shared static
        // array, so a[0] and b[0] aliased the same storage and the second
        // call clobbered the first.
        CHECK(a[0] != b[0], "second allocation does not clobber first handle");
        CHECK(a[1] == -1 && b[1] == -1, "unconnected server pipes expose no second handle");
        CHECK(ctx.getPipeReadFd(a[0]) >= 0, "pipe A listen fd registered");
        CHECK(ctx.getPipeReadFd(b[0]) >= 0, "pipe B listen fd registered");
        CHECK(ctx.getPipeReadFd(a[0]) != ctx.getPipeReadFd(b[0]), "pipe A/B listen fds are distinct");
        CHECK(ctx.getPipeWriteFd(a[0]) == -1, "server pipe not writable before connect");
        ctx.closePipe(a[0]);
        ctx.closePipe(b[0]);
    }

    {
        printf("--- connectPipe wires the accepted client fd ---\n");
        int server[2];
        int client[2];
        CHECK(ctx.allocPipePair("np_test_data", true, server), "server pipe allocated");
        CHECK(ctx.allocPipePair("np_test_data", false, client), "client pipe connected");
        CHECK(client[0] == client[1], "client pipe exposes one handle for both ends");

        int listenFd = ctx.getPipeReadFd(server[0]);
        int accepted = accept(listenFd, nullptr, nullptr);
        CHECK(accepted >= 0, "server accepts the client connection");
        CHECK(ctx.connectPipe(server[0], accepted), "accepted client fd wired into pipe table");
        CHECK(ctx.getPipeReadFd(server[0]) == accepted, "server read fd is the accepted socket");
        CHECK(ctx.getPipeWriteFd(server[0]) == accepted, "server write fd is the accepted socket");

        int clientFd = ctx.getPipeWriteFd(client[0]);
        CHECK(fdIsOpen(clientFd), "client fd registered");

        const char ping[] = "ping";
        CHECK(write(clientFd, ping, sizeof(ping) - 1) == (ssize_t)(sizeof(ping) - 1), "client writes through pipe");
        char buf[16] = {0};
        CHECK(read(accepted, buf, sizeof(buf)) == (ssize_t)(sizeof(ping) - 1), "server reads through pipe");
        CHECK(strcmp(buf, ping) == 0, "client->server payload matches");

        const char pong[] = "pong";
        CHECK(write(accepted, pong, sizeof(pong) - 1) == (ssize_t)(sizeof(pong) - 1), "server writes through pipe");
        memset(buf, 0, sizeof(buf));
        CHECK(read(clientFd, buf, sizeof(buf)) == (ssize_t)(sizeof(pong) - 1), "client reads through pipe");
        CHECK(strcmp(buf, pong) == 0, "server->client payload matches");

        ctx.closePipe(server[0]);
        ctx.closePipe(client[0]);
        CHECK(!fdIsOpen(accepted), "closePipe closes the wired client fd");
        CHECK(!fdIsOpen(clientFd), "closePipe closes the client fd");
    }

    {
        printf("--- concurrent allocPipePair calls stay isolated ---\n");
        constexpr int kThreads = 8;
        std::vector<int> handles(kThreads, -1);
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; i++) {
            threads.emplace_back([&ctx, &handles, i]() {
                int out[2];
                if (ctx.allocPipePair("np_test_concurrent_" + std::to_string(i), true, out))
                    handles[i] = out[0];
            });
        }
        for (auto& t : threads)
            t.join();

        for (int i = 0; i < kThreads; i++) {
            CHECK(handles[i] >= 0, "thread pipe allocated");
            CHECK(ctx.getPipeReadFd(handles[i]) >= 0, "thread pipe listen fd registered");
        }

        bool handlesDistinct = true;
        for (int i = 0; i < kThreads && handlesDistinct; i++)
            for (int j = i + 1; j < kThreads; j++)
                if (handles[i] == handles[j])
                    handlesDistinct = false;
        CHECK(handlesDistinct, "concurrent allocations yield distinct handles");

        bool fdsDistinct = true;
        for (int i = 0; i < kThreads && fdsDistinct; i++)
            for (int j = i + 1; j < kThreads; j++)
                if (ctx.getPipeReadFd(handles[i]) == ctx.getPipeReadFd(handles[j]))
                    fdsDistinct = false;
        CHECK(fdsDistinct, "concurrent allocations yield distinct listen fds");

        for (int i = 0; i < kThreads; i++)
            ctx.closePipe(handles[i]);
    }

    {
        printf("--- connectPipe failure paths close the fd (no leak) ---\n");
        int scratch[2];
        CHECK(pipe(scratch) == 0, "scratch pipe created");

        CHECK(!ctx.connectPipe(999999, scratch[0]), "connectPipe rejects an unknown handle");
        CHECK(!fdIsOpen(scratch[0]), "fd closed on unknown-handle failure");

        int server[2];
        int client[2];
        int extra[2] = {-1, -1};
        CHECK(ctx.allocPipePair("np_test_double", true, server), "server pipe allocated");
        CHECK(ctx.allocPipePair("np_test_double", false, client), "first client connected");

        int listenFd = ctx.getPipeReadFd(server[0]);
        // Accept the first client before connecting the second: the listen
        // backlog is 1, so a second connect() would block while the first
        // connection is still queued. The second client must also connect
        // while the listen socket is still alive, because connectPipe closes
        // it once the first client is wired.
        int first = accept(listenFd, nullptr, nullptr);
        CHECK(first >= 0, "first client accepted");
        CHECK(ctx.allocPipePair("np_test_double", false, extra), "second client connected");

        CHECK(ctx.connectPipe(server[0], first), "first connect succeeds");

        CHECK(accept(listenFd, nullptr, nullptr) < 0, "listen socket closed after connect");
        CHECK(!ctx.connectPipe(server[0], scratch[1]), "second connect rejected (already connected)");
        CHECK(!fdIsOpen(scratch[1]), "rejected client fd closed, no leak");

        ctx.closePipe(server[0]);
        ctx.closePipe(client[0]);
        ctx.closePipe(extra[0]);
    }

    printf("\n=== %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
