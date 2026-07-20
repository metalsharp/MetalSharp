/// @file GLErrorTracker.cpp
/// @brief Implementation of the per-process GL error state tracker.
///
/// Implementation is deliberately minimal: the lock covers only a single
/// 32-bit load or store. We avoid std::atomic here because the spec's
/// "first error wins" rule reads-then-writes a shared uint32_t; a mutex
/// makes that read-modify-write explicit instead of leaving it implicit
/// in a CAS loop. Cost is negligible at the call rate the shim actually
/// produces (an error only happens on bad input, which is rare).

#include <metalsharp/GLErrorTracker.h>

namespace metalsharp {

GLErrorTracker& GLErrorTracker::instance() {
    // Meyers singleton: construction is guaranteed thread-safe by C++11.
    static GLErrorTracker tracker;
    return tracker;
}

void GLErrorTracker::setError(uint32_t error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // "First error wins": if we already have an error pending, ignore the
    // new one. This matches the Khronos GL spec — once an error has been
    // recorded, subsequent errors from the same draw call are lost until
    // the guest drains it via glGetError.
    if (m_error == 0) {
        m_error = error;
    }
}

uint32_t GLErrorTracker::getError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Drain: read and clear atomically. Returning GL_NO_ERROR (0) when no
    // error is pending is the contract documented in the header.
    const uint32_t err = m_error;
    m_error = 0;
    return err;
}

} // namespace metalsharp