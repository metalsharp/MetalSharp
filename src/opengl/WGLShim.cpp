/// @file WGLShim.cpp
/// @brief WGL (Windows GL) entry points for opengl32 shim.
///
/// Each function here delegates to WGLContextManager, which owns the
/// AppKit NSOpenGLContext objects that back the Windows-side HGLRC
/// handles. The shim is what direct opengl32.dll callers (i.e. binaries
/// running outside Wine) see; under Wine, Wine's own WGL implementation
/// owns the contexts and these stubs short-circuit.

#include <cstdint>
#include <metalsharp/OpenGLBridge.h>
#include <metalsharp/WGLContextManager.h>

extern "C" {

void* wglCreateContext(void* hdc) {
    return metalsharp::WGLContextManager::instance().createContext(hdc, nullptr);
}

int32_t wglMakeCurrent(void* hdc, void* hglrc) {
    return metalsharp::WGLContextManager::instance().makeCurrent(hdc, hglrc) ? 1 : 0;
}

int32_t wglDeleteContext(void* hglrc) {
    return metalsharp::WGLContextManager::instance().deleteContext(hglrc) ? 1 : 0;
}

void* wglGetProcAddress(const char* name) {
    // Lightweight path: don't cache a bridge instance per call; the bridge
    // init is idempotent and guarded by std::once_flag internally for the
    // shim EntryPoint. Here we just construct a temporary bridge and let
    // the framework dlopen happen if it hasn't already. The cost is paid
    // once per process because dlsym(RTLD_NOW) on an already-loaded
    // framework is cheap.
    if (!name) {
        return nullptr;
    }
    metalsharp::OpenGLBridge bridge;
    bridge.init();
    return bridge.getGLProcAddress(name);
}

// wglShareLists is implemented at context creation time on macOS —
// callers wanting sharing must pass the share context into
// wglCreateContext / wglCreateContextAttribsARB. Return FALSE so a
// Windows caller that actively requests post-creation sharing learns
// the operation was rejected rather than silently dropped.
int32_t wglShareLists(void* /*hglrc1*/, void* /*hglrc2*/) {
    return 0; // FALSE — share lists not supported post-creation
}

// WGL_ARB_create_context entry point. Maps to NSOpenGL's 3.2 Core
// profile factory in the manager.
void* wglCreateContextAttribsARB(void* hdc, void* shareContext, const int* attribs) {
    return metalsharp::WGLContextManager::instance().createContextAttribs(hdc, shareContext, attribs);
}

int32_t wglSwapIntervalEXT(int32_t interval) {
    return metalsharp::WGLContextManager::instance().setSwapInterval(static_cast<int>(interval)) ? 1 : 0;
}

// WGL_ARB_pixel_format chose path. MetalSharp currently exposes a
// single pixel format; WGL's expected behaviour is to return a list of
// matches. We just return the single index, mirroring the index used by
// describePixelFormat below.
int32_t wglChoosePixelFormat(void* /*hdc*/, const int* /*attribs*/) {
    return 1;
}

int32_t wglDescribePixelFormat(void* /*hdc*/, int32_t format, uint32_t /*size*/, uint32_t* values) {
    auto fmt = metalsharp::WGLContextManager::instance().choosePixelFormat(nullptr);
    return metalsharp::WGLContextManager::instance().describePixelFormat(fmt, static_cast<uint32_t>(format), values)
               ? 1
               : 0;
}

} // extern "C"
