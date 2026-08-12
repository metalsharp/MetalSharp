#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <metalsharp/SyncContext.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using metalsharp::win32::INFINITE;
using metalsharp::win32::SyncContext;
using metalsharp::win32::WAIT_FAILED;
using metalsharp::win32::WAIT_OBJECT_0;

namespace {

int runEventSignalScenario() {
    SyncContext& context = SyncContext::instance();
    void* event = context.createEvent(true, false, "");
    if (!event)
        return 1;

    std::atomic<bool> waiterStarted{false};
    std::atomic<uint32_t> waitResult{WAIT_FAILED};
    std::thread waiter([&]() {
        waiterStarted.store(true, std::memory_order_release);
        waitResult.store(context.waitForSingleObject(event, INFINITE), std::memory_order_release);
    });

    for (int i = 0; i < 1000 && !waiterStarted.load(std::memory_order_acquire); i++)
        usleep(1000);

    // Give the waiter time to enter the blocking path. On the buggy
    // implementation, this call cannot acquire the global handle-table lock.
    usleep(100000);
    bool setResult = context.setEvent(event);
    waiter.join();

    uint32_t result = waitResult.load(std::memory_order_acquire);
    context.destroyHandle(event);
    return setResult && result == WAIT_OBJECT_0 ? 0 : 1;
}

bool test_event_wait_can_be_signaled() {
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0)
        _exit(runEventSignalScenario());

    int waitStatus = 0;
    constexpr uint32_t timeoutMs = 2000;
    for (uint32_t elapsed = 0; elapsed < timeoutMs; elapsed += 5) {
        pid_t result = waitpid(child, &waitStatus, WNOHANG);
        if (result == child)
            return WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0;
        if (result < 0 && errno != EINTR)
            break;
        usleep(5000);
    }

    kill(child, SIGKILL);
    waitpid(child, &waitStatus, 0);
    return false;
}

} // namespace

int main() {
    printf("=== SyncContext Tests ===\n\n");
    bool passed = test_event_wait_can_be_signaled();
    printf("  [%s] WaitForSingleObject event can be released by SetEvent\n", passed ? "OK" : "FAIL");
    return passed ? 0 : 1;
}
