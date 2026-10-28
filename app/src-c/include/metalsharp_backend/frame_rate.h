#ifndef METALSHARP_BACKEND_FRAME_RATE_H
#define METALSHARP_BACKEND_FRAME_RATE_H

#include <stddef.h>

/* The hero card's frame rate limit, per graphics lane: 30, 60, or 0 for none. Mullion ("dxmt") keeps it in
 * mullion.conf (d3d11/d3d12.preferredMaxFrameRate), read when each game starts; VKD3D-Proton ("vkd3d") takes it
 * from VKD3D_FRAME_RATE in the game's environment. */
char* ms_frame_rate_get_json(const char* metalsharp_home);
char* ms_frame_rate_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status);
unsigned ms_frame_rate_limit(const char* metalsharp_home, const char* pipeline);

#endif
