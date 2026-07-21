/// @file test_wgl_context.mm
/// @brief Tests for WGLContextManager: pixel format decoding, context
/// creation/make-current/deletion, swap-interval plumbing, and the
/// Wine sentinel shortcut. Real-context tests are skipped on headless
/// CI runners where NSOpenGLContext is unavailable.

#import <AppKit/AppKit.h>
#include <cstdio>
#include <metalsharp/WGLContextManager.h>

static int passed = 0;
static int failed = 0;
static int skipped = 0;

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

#define CHECK_SKIP(msg)                                                                                                \
    do {                                                                                                               \
        printf("  [SKIP] %s\n", msg);                                                                                  \
        skipped++;                                                                                                     \
    } while (0)

int main() {
    printf("=== WGL Context Tests ===\n\n");

    auto& mgr = metalsharp::WGLContextManager::instance();

    // Detect whether a real display is available. On headless CI runners
    // NSOpenGLPixelFormat creation throws an exception.
    bool hasDisplay = false;
    @try {
        NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFAAccelerated, 0};
        NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (pf) {
            hasDisplay = true;
            pf = nil;
        }
    } @catch (NSException* e) {
        hasDisplay = false;
    }
    printf("  [INFO] hasDisplay = %s\n\n", hasDisplay ? "true" : "false");

    // --- Pixel format decoding (no context needed) ---
    {
        metalsharp::WGLPixelFormat fmt = mgr.choosePixelFormat(nullptr);
        CHECK(fmt.colorBits >= 24, "choosePixelFormat colorBits default >= 24");
        CHECK(fmt.depthBits == 24, "choosePixelFormat depthBits default == 24");
        CHECK(fmt.doubleBuffer, "choosePixelFormat doubleBuffer default == true");
    }

    {
        const int attribs[] = {
            0x2010, 16, // WGL_COLOR_BITS_ARB
            0x2022, 16, // WGL_DEPTH_BITS_ARB
            0x2023, 0,  // WGL_STENCIL_BITS_ARB
            0x2011, 0,  // WGL_DOUBLE_BUFFER_ARB
            0,
        };
        metalsharp::WGLPixelFormat fmt = mgr.choosePixelFormat(attribs);
        CHECK((fmt.colorBits == 16u), "choosePixelFormat colorBits override == 16");
        CHECK((fmt.depthBits == 16u), "choosePixelFormat depthBits override == 16");
        CHECK((fmt.stencilBits == 0u), "choosePixelFormat stencilBits override == 0");
        CHECK(!fmt.doubleBuffer, "choosePixelFormat doubleBuffer override == false");
    }

    // --- describePixelFormat stub ---
    {
        uint32_t values = 0;
        bool descOk = mgr.describePixelFormat(metalsharp::WGLPixelFormat{}, 1, &values);
        CHECK(descOk, "describePixelFormat(1) returns true");
        descOk = mgr.describePixelFormat(metalsharp::WGLPixelFormat{}, 2, nullptr);
        CHECK(!descOk, "describePixelFormat(2) returns false");
    }

    // --- Wine sentinel short-circuit (no display needed) ---
    {
        mgr.setRunningInWine(true);
        void* wineCtx = mgr.createContext(nullptr, nullptr);
        CHECK(wineCtx == reinterpret_cast<void*>(0x1), "createContext in Wine returns sentinel 0x1");
        CHECK(mgr.makeCurrent(nullptr, wineCtx), "makeCurrent in Wine succeeds");
        CHECK(mgr.deleteContext(wineCtx), "deleteContext in Wine succeeds");
        CHECK(mgr.setSwapInterval(1), "setSwapInterval in Wine succeeds");
        mgr.setRunningInWine(false);
    }

    // --- Real context tests (skip if headless) ---
    if (hasDisplay) {
        void* ctx = mgr.createContext(nullptr, nullptr);
        CHECK(ctx != nullptr, "createContext returned non-null");
        CHECK((__bridge NSOpenGLContext*)ctx != nil, "createContext produces valid NSOpenGLContext");

        bool ok = mgr.makeCurrent(nullptr, ctx);
        CHECK(ok, "makeCurrent succeeded");
        CHECK(mgr.getCurrentContext() == ctx, "getCurrentContext matches");

        mgr.makeCurrent(nullptr, nullptr);
        CHECK(mgr.getCurrentContext() == nullptr, "makeCurrent(null) clears context");

        void* ctx2 = mgr.createContextAttribs(nullptr, nullptr, nullptr);
        CHECK(ctx2 != nullptr, "createContextAttribs returned non-null");

        bool swapOk = mgr.setSwapInterval(1);
        CHECK(!swapOk, "setSwapInterval fails when no current context");

        mgr.makeCurrent(nullptr, ctx2);
        swapOk = mgr.setSwapInterval(1);
        CHECK(swapOk, "setSwapInterval succeeds with current context");

        mgr.makeCurrent(nullptr, nullptr);
        CHECK(mgr.deleteContext(ctx), "deleteContext succeeded");
        CHECK(mgr.deleteContext(ctx2), "deleteContext(ctx2) succeeded");
        CHECK(!mgr.deleteContext(ctx), "double delete returns false");

        void* ctx3 = mgr.createContext(nullptr, nullptr);
        CHECK(ctx3 != nullptr, "createContext after Wine toggle succeeds");
        mgr.deleteContext(ctx3);
    } else {
        CHECK_SKIP("Real context tests (display required)");
    }

    printf("\n=== Summary: %d passed, %d failed, %d skipped ===\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
