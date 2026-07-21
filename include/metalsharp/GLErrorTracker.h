/// @file GLErrorTracker.h
/// @brief Process-wide GL error state tracker for the opengl32 shim.
///
/// Phase 5a: GL follows a single-error-state-machine model where the most
/// recent error is reported by @ref glGetError and then cleared. The shim
/// needs to surface errors that originate inside the bridge (e.g. invalid
/// enum passed to glBindBuffer, cross-compile failure producing no usable
/// handle) ahead of whatever the native framework might be holding, so we
/// maintain our own shadow error register and check it first in the
/// @c glGetError override in EntryPoint.cpp.
///
/// Thread safety:
///   * This class is a Meyers singleton accessed via @ref instance().
///   * Every public method takes a single std::mutex for read/write; the
///     critical sections are short (single uint32_t load/store) so the
///     lock is uncontended under normal draw-call workloads.
///   * setError() is non-sticky across errors: if an error is already
///     pending, the new error is dropped (matching the Khronos GL spec's
///     "record any error" behaviour, where the first error wins).
///
/// Error semantics:
///   * 0 (GL_NO_ERROR) means no error pending.
///   * Non-zero values are the standard GL error enums (GL_INVALID_ENUM
///     0x0500, GL_INVALID_VALUE 0x0501, GL_INVALID_OPERATION 0x0502,
///     GL_STACK_OVERFLOW 0x0503, GL_STACK_UNDERFLOW 0x0504,
///     GL_OUT_OF_MEMORY 0x0505, GL_INVALID_FRAMEBUFFER_OPERATION
///     0x0506, GL_CONTEXT_LOST 0x0507). The four helper methods
///     (invalidEnum/invalidValue/invalidOperation/outOfMemory) are the
///     ones the shim currently calls; other enums can be set via the raw
///     setError() if a future interception point needs them.

#pragma once

#include <cstdint>
#include <mutex>

namespace metalsharp {

class GLErrorTracker {
  public:
    static GLErrorTracker& instance();

    /// Set the current GL error (only if no error is already pending).
    void setError(uint32_t error);

    /// Get and clear the current GL error. Returns GL_NO_ERROR (0) if none.
    uint32_t getError();

    /// A GL operation was called with an invalid enum value.
    void invalidEnum() { setError(0x0500); } // GL_INVALID_ENUM

    /// A GL operation was called with an invalid value.
    void invalidValue() { setError(0x0501); } // GL_INVALID_VALUE

    /// A GL operation was called while in an invalid state.
    void invalidOperation() { setError(0x0502); } // GL_INVALID_OPERATION

    /// Out of memory.
    void outOfMemory() { setError(0x0505); } // GL_OUT_OF_MEMORY

  private:
    uint32_t m_error = 0; // 0 = GL_NO_ERROR
    std::mutex m_mutex;
};

} // namespace metalsharp