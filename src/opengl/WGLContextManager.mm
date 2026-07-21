/// @file WGLContextManager.mm
/// @brief NSOpenGLContext-backed implementation of WGLContextManager.
///
/// Each public method on WGLContextManager is a thin shell around
/// AppKit NSOpenGLContext/NSOpenGLPixelFormat. All state lives in a
/// static Impl_ struct guarded by a std::mutex. NSOpenGLContext objects
/// are retained via __bridge_retained when stored and released via
/// __bridge_transfer when removed, so the dictionary owns them for the
/// lifetime they live in the manager.

#import <AppKit/AppKit.h>
#include <metalsharp/WGLContextManager.h>
#include <mutex>
#include <unordered_map>

namespace metalsharp {

struct WGLContextManager::Impl_ {
    std::unordered_map<void*, WGLContext> contexts;
    std::unordered_map<std::thread::id, void*> currentContexts;
    std::mutex mutex;
};

static WGLContextManager::Impl_& impl() {
    static WGLContextManager::Impl_ s_impl;
    return s_impl;
}

WGLContextManager& WGLContextManager::instance() {
    static WGLContextManager s_instance;
    return s_instance;
}

WGLPixelFormat WGLContextManager::choosePixelFormat(const int* attribs) {
    WGLPixelFormat fmt;
    if (!attribs) {
        return fmt;
    }
    for (int i = 0; attribs[i] != 0; i += 2) {
        switch (attribs[i]) {
        case 0x2010: // WGL_COLOR_BITS_ARB
            fmt.colorBits = static_cast<uint32_t>(attribs[i + 1]);
            break;
        case 0x2022: // WGL_DEPTH_BITS_ARB
            fmt.depthBits = static_cast<uint32_t>(attribs[i + 1]);
            break;
        case 0x2023: // WGL_STENCIL_BITS_ARB
            fmt.stencilBits = static_cast<uint32_t>(attribs[i + 1]);
            break;
        case 0x2041: // WGL_SAMPLES_ARB
            fmt.samples = static_cast<uint32_t>(attribs[i + 1]);
            break;
        case 0x2011: // WGL_DOUBLE_BUFFER_ARB
            fmt.doubleBuffer = attribs[i + 1] != 0;
            break;
        case 0x2001: // WGL_ACCELERATION_ARB; we honour "full acceleration"
            // Values: 0x2027 WGL_FULL_ACCELERATION_ARB
            fmt.accelerated = attribs[i + 1] != 0x2028; // WGL_NO_ACCELERATION_ARB
            break;
        default:
            break;
        }
    }
    return fmt;
}

bool WGLContextManager::describePixelFormat(const WGLPixelFormat& /*fmt*/, uint32_t index, uint32_t* /*values*/) {
    return index == 1;
}

void* WGLContextManager::createContext(void* hdc, void* sharedContext) {
    if (m_inWine) {
        // Wine-launched binaries attach their own WGL; this manager
        // is only consulted when a binary loads opengl32.dll directly.
        return reinterpret_cast<void*>(0x1);
    }
    (void)hdc;

    WGLPixelFormat fmt = choosePixelFormat(nullptr);

    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy,
        NSOpenGLPFAColorSize,     static_cast<NSOpenGLPixelFormatAttribute>(fmt.colorBits),
        NSOpenGLPFADepthSize,     static_cast<NSOpenGLPixelFormatAttribute>(fmt.depthBits),
        NSOpenGLPFAStencilSize,   static_cast<NSOpenGLPixelFormatAttribute>(fmt.stencilBits),
        NSOpenGLPFADoubleBuffer,  static_cast<NSOpenGLPixelFormatAttribute>(fmt.doubleBuffer ? 1 : 0),
        NSOpenGLPFAAccelerated,   0};

    NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!pf) {
        return nullptr;
    }

    NSOpenGLContext* shareCtx = (__bridge NSOpenGLContext*)sharedContext;
    NSOpenGLContext* ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:shareCtx];
    if (!ctx) {
        // Releasing pf needs to balance the alloc; ARC handles it once
        // we drop the local pointer.
        pf = nil;
        return nullptr;
    }

    WGLContext wglCtx;
    wglCtx.nsContext = (__bridge_retained void*)ctx;
    wglCtx.pixelFormat = (__bridge_retained void*)pf;
    wglCtx.sharedContext = sharedContext;
    wglCtx.desc = fmt;

    auto& im = impl();
    std::lock_guard<std::mutex> lock(im.mutex);
    im.contexts[wglCtx.nsContext] = wglCtx;

    return wglCtx.nsContext;
}

void* WGLContextManager::createContextAttribs(void* hdc, void* sharedContext, const int* attribs) {
    if (m_inWine) {
        return reinterpret_cast<void*>(0x1);
    }
    (void)hdc;

    WGLPixelFormat fmt = choosePixelFormat(attribs);

    // WGL_ARB_create_context profile bits: WGL_CONTEXT_CORE_PROFILE_BIT_ARB
    // = 0x00000001. Default to core 3.2 when not specified; spec says
    // requests for any other version on macOS still work because the OS
    // only supports the legacy 2.1 profile outside the 3.2 core path.
    NSOpenGLPixelFormatAttribute profile = NSOpenGLProfileVersion3_2Core;
    if (attribs) {
        for (int i = 0; attribs[i] != 0; i += 2) {
            if (attribs[i] == 0x8001 /* WGL_CONTEXT_PROFILE_MASK_ARB */) {
                // 0x8002 WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
                if (attribs[i + 1] == 0x8002) {
                    profile = NSOpenGLProfileVersionLegacy;
                }
            }
        }
    }

    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, profile,
        NSOpenGLPFAColorSize,     static_cast<NSOpenGLPixelFormatAttribute>(fmt.colorBits),
        NSOpenGLPFADepthSize,     static_cast<NSOpenGLPixelFormatAttribute>(fmt.depthBits),
        NSOpenGLPFAStencilSize,   static_cast<NSOpenGLPixelFormatAttribute>(fmt.stencilBits),
        NSOpenGLPFADoubleBuffer,  static_cast<NSOpenGLPixelFormatAttribute>(fmt.doubleBuffer ? 1 : 0),
        NSOpenGLPFAMultisample,   static_cast<NSOpenGLPixelFormatAttribute>(fmt.samples > 0 ? 1 : 0),
        NSOpenGLPFASamples,       static_cast<NSOpenGLPixelFormatAttribute>(fmt.samples),
        NSOpenGLPFAAccelerated,   0};

    NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!pf) {
        return nullptr;
    }

    NSOpenGLContext* shareCtx = (__bridge NSOpenGLContext*)sharedContext;
    NSOpenGLContext* ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:shareCtx];
    if (!ctx) {
        pf = nil;
        return nullptr;
    }

    WGLContext wglCtx;
    wglCtx.nsContext = (__bridge_retained void*)ctx;
    wglCtx.pixelFormat = (__bridge_retained void*)pf;
    wglCtx.sharedContext = sharedContext;
    wglCtx.desc = fmt;

    auto& im = impl();
    std::lock_guard<std::mutex> lock(im.mutex);
    im.contexts[wglCtx.nsContext] = wglCtx;

    return wglCtx.nsContext;
}

bool WGLContextManager::makeCurrent(void* /*hdc*/, void* context) {
    if (m_inWine) {
        return true;
    }

    auto& im = impl();
    std::lock_guard<std::mutex> lock(im.mutex);

    if (context) {
        NSOpenGLContext* ctx = (__bridge NSOpenGLContext*)context;
        [ctx makeCurrentContext];

        // Demote any previously-current context bound on this thread.
        auto prevIt = im.currentContexts.find(std::this_thread::get_id());
        if (prevIt != im.currentContexts.end() && prevIt->second != context) {
            auto entryIt = im.contexts.find(prevIt->second);
            if (entryIt != im.contexts.end()) {
                entryIt->second.isCurrent = false;
            }
        }

        im.currentContexts[std::this_thread::get_id()] = context;
        auto entryIt = im.contexts.find(context);
        if (entryIt != im.contexts.end()) {
            entryIt->second.isCurrent = true;
        }
    } else {
        [NSOpenGLContext clearCurrentContext];
        auto prevIt = im.currentContexts.find(std::this_thread::get_id());
        if (prevIt != im.currentContexts.end()) {
            auto entryIt = im.contexts.find(prevIt->second);
            if (entryIt != im.contexts.end()) {
                entryIt->second.isCurrent = false;
            }
            im.currentContexts.erase(prevIt);
        }
    }
    return true;
}

bool WGLContextManager::deleteContext(void* context) {
    if (m_inWine) {
        return true;
    }
    if (!context) {
        return false;
    }

    auto& im = impl();
    std::lock_guard<std::mutex> lock(im.mutex);

    auto it = im.contexts.find(context);
    if (it == im.contexts.end()) {
        return false;
    }

    // Transfer ownership back to ARC; setting the local ObjC pointers
    // to nil drops the remaining retain counts the manager held.
    NSOpenGLContext* ctx = (__bridge_transfer NSOpenGLContext*)it->second.nsContext;
    NSOpenGLPixelFormat* pf = (__bridge_transfer NSOpenGLPixelFormat*)it->second.pixelFormat;
    (void)ctx;
    (void)pf;
    ctx = nil;
    pf = nil;

    // Unbind from any thread that still had it current.
    for (auto entry = im.currentContexts.begin(); entry != im.currentContexts.end();) {
        if (entry->second == context) {
            entry = im.currentContexts.erase(entry);
        } else {
            ++entry;
        }
    }

    im.contexts.erase(it);
    return true;
}

void* WGLContextManager::getCurrentContext() {
    auto& im = impl();
    std::lock_guard<std::mutex> lock(im.mutex);
    auto it = im.currentContexts.find(std::this_thread::get_id());
    return it != im.currentContexts.end() ? it->second : nullptr;
}

bool WGLContextManager::setSwapInterval(int interval) {
    if (m_inWine) {
        return true;
    }
    void* context = nullptr;
    {
        auto& im = impl();
        std::lock_guard<std::mutex> lock(im.mutex);
        auto it = im.currentContexts.find(std::this_thread::get_id());
        if (it == im.currentContexts.end()) {
            return false;
        }
        context = it->second;
    }
    if (!context) {
        return false;
    }
    NSOpenGLContext* ctx = (__bridge NSOpenGLContext*)context;
    GLint swapInt = interval;
    [ctx setValues:&swapInt forParameter:NSOpenGLContextParameterSwapInterval];
    return true;
}

} // namespace metalsharp
