/// @file test_ws2_select.cpp
/// @brief Regression test for ws2_32 select() Winsock TIMEVAL handling
///        (metalsharp/MetalSharp#423).
///
/// A Winsock TIMEVAL is {LONG tv_sec; LONG tv_usec} — two 32-bit fields.
/// The shim previously read only the first 32-bit word and treated it as a
/// microsecond count, so {0, 500000} (a 500 ms timeout) became a zero
/// timeout that returned immediately and {1, 500000} (1.5 s) became ~1 us.
/// The test drives the real ws2_32 `select` export through the shim table
/// and measures that the requested wait time is actually honored.

#include <chrono>
#include <cstdint>
#include <cstdio>

#include <metalsharp/ExtraShims.h>
#include <metalsharp/Win32Types.h>

using namespace metalsharp::win32;

/// Winsock TIMEVAL layout as seen by the guest: two signed 32-bit fields.
struct WinTimeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

using SelectFn = int(MSABI*)(int, void*, void*, void*, const void*);

static SelectFn selectExport() {
    metalsharp::ShimLibrary lib = createWs2_32Shim();
    auto* raw = lib.functions["select"]();
    return reinterpret_cast<SelectFn>(raw);
}

static double waitMillis(SelectFn select, const WinTimeval* timeout) {
    auto start = std::chrono::steady_clock::now();
    // Empty fd sets: select can only return 0 after the full timeout.
    int ret = select(0, nullptr, nullptr, nullptr, timeout);
    auto end = std::chrono::steady_clock::now();
    if (ret != 0) {
        fprintf(stderr, "ws2_select: select returned %d (expected 0)\n", ret);
        return -1.0;
    }
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static int testsPassed = 0;
static int testsFailed = 0;

static void check(const char* name, bool ok) {
    printf("  TEST: %-55s%s\n", name, ok ? "PASS" : "FAIL");
    if (ok) {
        testsPassed++;
    } else {
        testsFailed++;
    }
}

int main() {
    SelectFn select = selectExport();
    if (!select) {
        fprintf(stderr, "ws2_select: ws2_32 select export not found\n");
        return 1;
    }

    printf("\n--- ws2_32 select TIMEVAL regression ---\n");

    {
        WinTimeval timeout{0, 250000}; // 250 ms
        double elapsed = waitMillis(select, &timeout);
        check("sub-second timeout {0, 250000} waits ~250 ms", elapsed >= 200.0);
        printf("       elapsed=%.1f ms\n", elapsed);
    }

    {
        WinTimeval timeout{1, 100000}; // 1.1 s
        double elapsed = waitMillis(select, &timeout);
        check("second+usec timeout {1, 100000} waits ~1100 ms", elapsed >= 1000.0);
        printf("       elapsed=%.1f ms\n", elapsed);
    }

    {
        WinTimeval timeout{0, 1500000}; // 1.5 s expressed in usec
        double elapsed = waitMillis(select, &timeout);
        check("overflowing usec {0, 1500000} normalizes to ~1500 ms", elapsed >= 1400.0);
        printf("       elapsed=%.1f ms\n", elapsed);
    }

    {
        WinTimeval timeout{0, 0}; // poll, no wait
        double elapsed = waitMillis(select, &timeout);
        check("zero timeout {0, 0} returns immediately", elapsed >= 0.0 && elapsed < 200.0);
        printf("       elapsed=%.1f ms\n", elapsed);
    }

    printf("\nws2_select: %d passed, %d failed\n", testsPassed, testsFailed);
    return testsFailed == 0 ? 0 : 1;
}
