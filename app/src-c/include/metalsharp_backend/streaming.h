#ifndef METALSHARP_BACKEND_STREAMING_H
#define METALSHARP_BACKEND_STREAMING_H

#include <stdbool.h>
#include <stddef.h>

/* Game streaming host integration (Sunshine). MetalSharp manages install,
 * launch, credentials, and Moonlight PIN pairing; the mobile device runs
 * Moonlight (App Store / Play Store) against this Mac as the host. */

char* ms_streaming_status_json(const char* metalsharp_home);
char* ms_streaming_install_json(const char* metalsharp_home, int* status);
char* ms_streaming_launch_json(const char* metalsharp_home, int* status);
char* ms_streaming_stop_json(const char* metalsharp_home, int* status);
char* ms_streaming_pin_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status);
char* ms_streaming_unpair_all_json(const char* metalsharp_home, int* status);

#endif
