#ifndef METALSHARP_BACKEND_ROSETTA_H
#define METALSHARP_BACKEND_ROSETTA_H

#include <stdbool.h>

/* Upgrading to macOS 27 can leave Rosetta 2 uninstalled, and MetalSharp's Wine is x86_64, so
 * nothing launches until it is installed again. When the macOS major version is 27 or later and
 * differs from the one recorded at the last check (or none is recorded), reinstall Rosetta, even
 * if it looks installed. Returns true when it ran the installer. */
bool ms_rosetta_after_macos_upgrade(const char* metalsharp_home);
/* The same check on a detached thread, so a slow download never holds up startup. */
void ms_rosetta_after_macos_upgrade_async(const char* metalsharp_home);

#endif
