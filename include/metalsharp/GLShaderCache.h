/// @file GLShaderCache.h
/// @brief Process-wide cache for cross-compiled GLSL → MSL output.
///
/// Phase 2i introduces a thread-safe singleton cache that keys cross-compiled
/// MSL output by `(GLSL source, shader stage)` so that repeated compiles of
/// the same guest shader source — common in games where shaders are
/// re-compiled on resource reload, level transitions, and pipeline state
/// object rebuilds — short-circuit straight to cached MSL instead of paying
/// for the glslang → SPIRV-Cross round trip every time.
///
/// Cache key construction:
///   * Source is hashed with djb2 (xor-shift variant) and stage is folded
///     into the key so the same source compiled for a different stage
///     cannot collide.
///   * The key is rendered as a hex string so it can be used as a map key
///     without embedding binary data in std::string.
///   * Output is deterministic — identical inputs always produce identical
///     keys, regardless of process start time or thread schedule.
///
/// Threading:
///   * The cache is a Meyers singleton; first-touch construction is
///     thread-safe under C++11.
///   * All public methods are guarded by a single std::mutex; lookup and
///     insert are constant-time under the lock.
///
/// Lifetime:
///   * The cache lives for the process lifetime. The host (e.g. a launcher
///     or game process) is expected to call clear() on level transitions if
///     it wants bounded memory. The cache is not LRU; for Phase 2 we expect
///     the working set to be small (single game, handful of shaders).
///
/// This is intentionally separate from the D3D-side metalsharp::ShaderCache
/// (include/metalsharp/ShaderCache.h) — that one caches Metal pipeline
/// state objects keyed by DXBC hash, while this one caches MSL source keyed
/// by GLSL source. Different key spaces, different lifecycle.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace metalsharp {

/// Thread-safe cache mapping `(GLSL source, shader stage)` → translated MSL
/// source. Process-wide singleton accessed via instance().
class GLShaderCache {
  public:
    /// Returns the process-wide cache.
    static GLShaderCache& instance();

    /// Hash GLSL source + compile stage into a deterministic cache key.
    /// The same (source, stage) pair always produces the same key string;
    /// the same source with a different stage produces a different key.
    static std::string makeKey(const std::string& source, uint32_t stage);

    /// Store translated MSL for a pre-computed key. Returns true if the
    /// key was not already present (newly inserted), false if the existing
    /// MSL was overwritten.
    bool storeMSL(const std::string& key, const std::string& msl);

    /// Lookup translated MSL by pre-computed key. Returns nullptr if the
    /// key is not in the cache; the returned pointer is invalidated by any
    /// subsequent storeMSL() / clear() call.
    const std::string* lookupMSL(const std::string& key) const;

    /// Convenience overload: hash (source, stage) into a key, then store
    /// the MSL under that key. Returns true if newly inserted.
    bool storeMSL(const std::string& source, uint32_t stage, const std::string& msl);

    /// Convenience overload: hash (source, stage) into a key, then lookup
    /// the MSL. Returns nullptr if no entry exists for that pair.
    const std::string* lookupMSL(const std::string& source, uint32_t stage) const;

    /// Remove all cached entries. Safe to call from any thread.
    void clear();

    /// Number of cached entries. Safe to call from any thread.
    size_t size() const;

  private:
    GLShaderCache() = default;

    // Disable copy/move — singleton owns its mutex and map.
    GLShaderCache(const GLShaderCache&) = delete;
    GLShaderCache& operator=(const GLShaderCache&) = delete;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::string> m_cache;
};

} // namespace metalsharp
