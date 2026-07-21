/// @file WGLContextManager.h
/// @brief WGL (Windows GL) context manager singleton.
///
/// Owns the NSOpenGLContext and NSOpenGLPixelFormat objects behind
/// wglCreateContext / wglCreateContextAttribsARB / wglMakeCurrent /
/// wglDeleteContext. Wine-launched binaries side-load their own WGL
/// implementation, so this manager is normally bypassed and the
/// delegates run; when a binary loads opengl32.dll directly without
/// Wine, these objects are created here.
///
/// The manager also tracks per-thread current context so multiple
/// threads inside a guest process can independently make contexts
/// current, matching the WGL behaviour Windows callers expect.

#pragma once

#include <cstdint>
#include <thread>

#ifdef __OBJC__
@class NSOpenGLContext;
@class NSOpenGLPixelFormat;
#else
typedef void* NSOpenGLContextRef;
typedef void* NSOpenGLPixelFormatRef;
#endif

namespace metalsharp {

/// Decoded subset of WGL pixel-format attribute pairs (WGL_ARB_pixel_format).
/// Stored per context so we can later translate the choices into Metal
/// drawable configurations.
struct WGLPixelFormat {
    uint32_t colorBits = 32;
    uint32_t depthBits = 24;
    uint32_t stencilBits = 8;
    uint32_t samples = 0;
    bool doubleBuffer = true;
    bool accelerated = true;
};

/// Internal storage for a single WGL context. Holds the underlying
/// AppKit objects (retained via __bridge_retained) plus a back-pointer
/// to any shared context, the requested pixel-format attributes, and
/// whether the context is currently bound on the calling thread.
struct WGLContext {
    void* nsContext = nullptr;   // NSOpenGLContext* (retained)
    void* pixelFormat = nullptr; // NSOpenGLPixelFormat* (retained)
    void* sharedContext = nullptr;
    WGLPixelFormat desc;
    bool isCurrent = false;
};

/// Singleton that maps WGL-style context/pixel-format APIs to
/// AppKit NSOpenGLContext. The opengl32 shim delegates here for
/// every wgl* entry point so callers see a real GL context rather
/// than the Phase 4b sentinel stubs.
class WGLContextManager {
  public:
    /// Forward-declared opaque impl type. Defined in WGLContextManager.mm
    /// so the headers don't drag AppKit into every translation unit.
    struct Impl_;

    static WGLContextManager& instance();

    /// Decode a WGL_ARB_pixel_format attribute list (terminated by
    /// `WGL_NONE == 0`). Unknown pairs are silently ignored; defaults
    /// from WGLPixelFormat provide reasonable fallbacks.
    WGLPixelFormat choosePixelFormat(const int* attribs);

    /// Stub describe — MetalSharp exposes exactly one pixel format,
    /// so index must be 1. Per-attribute value lookups can be layered
    /// on top later without changing the public signature.
    bool describePixelFormat(const WGLPixelFormat& fmt, uint32_t index, uint32_t* values);

    /// Create a legacy-profile OpenGL context. Returns a `void*`
    /// handle (the retained NSOpenGLContext) suitable for use as the
    /// WGL HGLRC parameter. nullptr on allocation failure.
    void* createContext(void* hdc, void* sharedContext = nullptr);

    /// Create a 3.2 Core profile context from a WGL_ARB_create_context
    /// attribute list. Profile/version bits select the NSOpenGL profile.
    void* createContextAttribs(void* hdc, void* sharedContext, const int* attribs);

    /// Bind `context` to the calling thread (or clear it when nullptr).
    bool makeCurrent(void* hdc, void* context);

    /// Release the NSOpenGLContext/NSOpenGLPixelFormat pair.
    bool deleteContext(void* context);

    /// Lookup the context currently bound on the calling thread.
    void* getCurrentContext();

    /// Set the buffer-swap interval (vertical-sync) on the current
    /// context. Maps directly to `NSOpenGLContextParameterSwapInterval`
    /// so callers can synchronise wglSwapIntervalEXT with the host.
    bool setSwapInterval(int interval);

    /// True when running under Wine, in which case the manager takes
    /// the sentinel shortcut and lets Wine drive the NSOpenGLContext
    /// itself.
    bool isRunningInWine() const { return m_inWine; }

    /// Set during NativeLauncher init to delegate all context work to
    /// Wine rather than spinning up NSOpenGLContext ourselves.
    void setRunningInWine(bool inWine) { m_inWine = inWine; }

  private:
    bool m_inWine = false;
};

} // namespace metalsharp
