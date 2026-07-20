#pragma once
#include <cstdint>
#include <cstring>

namespace metalsharp {

/// Parsed GLSL version from a #version directive.
struct GLSLVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    bool isES = false;  // #version 300 es
    bool valid = false; // true if #version was found and parsed
};

/// Parse a #version directive from GLSL source code.
/// Returns true if a valid directive was found.
/// Handles: "#version 120", "#version 330 core", "#version 300 es", etc.
/// Skips whitespace, handles comments conservatively.
inline bool parseGLSLVersion(const char* source, GLSLVersion& out) {
    if (!source)
        return false;

    // Skip leading whitespace
    while (*source == ' ' || *source == '\t' || *source == '\n' || *source == '\r') {
        source++;
    }

    // Must start with '#'
    if (*source != '#')
        return false;
    source++;

    // Skip whitespace after '#'
    while (*source == ' ' || *source == '\t')
        source++;

    // Expect "version"
    if (strncmp(source, "version", 7) != 0)
        return false;
    source += 7;

    // Skip whitespace
    while (*source == ' ' || *source == '\t')
        source++;

    // Parse major version number
    uint32_t major = 0;
    while (*source >= '0' && *source <= '9') {
        major = major * 10 + (*source - '0');
        source++;
    }
    if (major == 0 || major > 999)
        return false;

    out.major = major;
    out.minor = 0;

    // Check for ".minor" (e.g., #version 3.30)
    if (*source == '.') {
        source++;
        uint32_t minor = 0;
        while (*source >= '0' && *source <= '9') {
            minor = minor * 10 + (*source - '0');
            source++;
        }
        out.minor = minor;
    }

    // Check for "es" or "core" profile keyword AFTER the version number
    // Skip whitespace before profile keyword
    while (*source == ' ' || *source == '\t')
        source++;

    // Check for "es" (OpenGL ES)
    if (strncmp(source, "es", 2) == 0) {
        out.isES = true;
        source += 2;
    }
    // Check for "core" (OpenGL core profile) - we don't store it, just skip it
    else if (strncmp(source, "core", 4) == 0) {
        source += 4;
    }
    // Check for "compatibility"
    else if (strncmp(source, "compatibility", 13) == 0) {
        source += 13;
    }

    out.valid = true;
    return true;
}

/// Returns true if a GLSL version requires cross-compilation (i.e., is not
/// supported by macOS native OpenGL 2.1 / GLSL 1.20).
/// macOS legacy profile only supports GLSL 1.20 (#version 120).
/// GLSL 1.30+ (#version 130+) requires SPIRV-Cross → MSL.
/// GLSL ES (#version 100 / #version 300 es) also requires cross-compilation.
inline bool needsCrossCompile(const GLSLVersion& ver) {
    if (!ver.valid)
        return false;
    // ES always needs cross-compile — macOS has no native ES support
    if (ver.isES)
        return true;
    // GLSL > 1.20 needs cross-compile
    if (ver.major > 1)
        return true;
    if (ver.major == 1 && ver.minor > 20)
        return true;
    return false;
}

/// Returns the effective GLSL version as a packed integer (major * 100 + minor).
/// 1.20 → 120, 3.30 → 330, 4.60 → 460, etc.
inline uint32_t packedGLSLVersion(const GLSLVersion& ver) {
    return ver.major * 100 + ver.minor;
}

} // namespace metalsharp
