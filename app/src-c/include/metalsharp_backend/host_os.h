#ifndef METALSHARP_BACKEND_HOST_OS_H
#define METALSHARP_BACKEND_HOST_OS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* The first macOS the newest graphics are for: GPTK 4.0 beta 3's D3DMetal (the x86_64 runtime), Mullion's Metal 4
 * shaders. Below it both builds run Mullion at Metal 3.2 and the VKD3D lane at shader model 3, and the x86_64 runtime
 * GPTK 3.0's D3DMetal (ms_host_before_macos27). */
#define MS_HOST_MACOS_GPTK4 27

/* The host's macOS major version, or METALSHARP_HOST_MACOS's (tests); 0 if unknown. sw_vers rather than a sysctl,
 * since older-SDK processes can be shown a compatibility version (10.16, 16 for 26). Read once per process. */
static inline int ms_host_macos_major(void) {
    static int cached = -1;
    const char* override = getenv("METALSHARP_HOST_MACOS");
    char version[32] = "";
    FILE* pipe;
    if (override && override[0])
        return atoi(override);
    if (cached >= 0)
        return cached;
    cached = 0;
    pipe = popen("/usr/bin/sw_vers -productVersion", "r");
    if (!pipe)
        return cached;
    if (!fgets(version, sizeof(version), pipe))
        version[0] = '\0';
    if (pclose(pipe) == 0)
        cached = atoi(version);
    return cached;
}

/* Whether the host is a known macOS below 27 (an unknown version keeps the newest graphics). */
static inline bool ms_host_before_macos27(void) {
    int major = ms_host_macos_major();
    return major > 0 && major < MS_HOST_MACOS_GPTK4;
}

#endif
