/// @file GLShaderCache.cpp
/// @brief Implementation of the GLSL → MSL cache.
///
/// All public methods are thread-safe via a single std::mutex held on the
/// instance. Lookup and insert are O(1) under the lock; the hash computation
/// in makeKey() runs outside the lock so the critical section stays tight.

#include <metalsharp/GLShaderCache.h>

#include <cstdint>
#include <cstdio>

namespace metalsharp {

namespace {

/// Render an 8-digit lowercase hex string of a 32-bit value. Used to render
/// the djb2 hash as a map key without leaking binary data into std::string.
void appendHex(std::string& out, uint32_t value) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", value);
    out.append(buf);
}

/// djb2-style xor-shift hash. Cheap, deterministic, and produces a uniform
/// distribution across the 32-bit space for the kinds of input we expect
/// (GLSL source blobs ranging from a few hundred bytes to a few KiB).
uint32_t djb2(const std::string& input) {
    uint32_t hash = 5381u;
    for (unsigned char c : input) {
        // hash * 33 ^ c, with overflow allowed (unsigned wrap).
        hash = ((hash << 5) + hash) ^ static_cast<uint32_t>(c);
    }
    return hash;
}

} // namespace

GLShaderCache& GLShaderCache::instance() {
    // Meyers singleton: construction is guaranteed thread-safe by C++11.
    static GLShaderCache cache;
    return cache;
}

std::string GLShaderCache::makeKey(const std::string& source, uint32_t stage) {
    // Fold the stage into the hash so the same source compiled for a
    // different stage cannot collide. We mix the stage into both halves of
    // a 64-bit hash so neither half alone can be tricked into producing the
    // same output.
    const uint32_t h1 = djb2(source) ^ stage;
    const uint32_t h2 =
        djb2(std::string(reinterpret_cast<const char*>(&stage), sizeof(stage))) ^ static_cast<uint32_t>(source.size());

    std::string key;
    key.reserve(17);
    appendHex(key, h1);
    key.push_back('-');
    appendHex(key, h2);
    return key;
}

bool GLShaderCache::storeMSL(const std::string& key, const std::string& msl) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto [it, inserted] = m_cache.emplace(key, msl);
    if (!inserted) {
        // Overwrite an existing entry. This matches the expected behavior
        // when a caller invalidates a stale cache slot (e.g. after a
        // translator option change).
        it->second = msl;
        return false;
    }
    return true;
}

const std::string* GLShaderCache::lookupMSL(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(key);
    if (it == m_cache.end()) {
        return nullptr;
    }
    return &it->second;
}

bool GLShaderCache::storeMSL(const std::string& source, uint32_t stage, const std::string& msl) {
    return storeMSL(makeKey(source, stage), msl);
}

const std::string* GLShaderCache::lookupMSL(const std::string& source, uint32_t stage) const {
    return lookupMSL(makeKey(source, stage));
}

void GLShaderCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
}

size_t GLShaderCache::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.size();
}

} // namespace metalsharp
