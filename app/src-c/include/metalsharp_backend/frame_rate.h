#ifndef METALSHARP_BACKEND_FRAME_RATE_H
#define METALSHARP_BACKEND_FRAME_RATE_H

#include <stdbool.h>
#include <stddef.h>

/* The hero card's frame rate limit, per graphics lane: 30, 60, or 0 for none. Mullion ("dxmt") keeps it in
 * etc/mullion-frame-rate and writes it to the active runtime's mullion.conf for D3D11 and D3D12 alike
 * (d3d11/d3d12.preferredMaxFrameRate), which games read when they start; VKD3D-Proton ("vkd3d") takes it from
 * VKD3D_FRAME_RATE in the game's environment. */
char* ms_frame_rate_get_json(const char* metalsharp_home);
char* ms_frame_rate_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status);
unsigned ms_frame_rate_limit(const char* metalsharp_home, const char* pipeline);
/* Before a launch: the active runtime's mullion.conf gets Mullion's limit for D3D11 and D3D12, which a runtime
 * switched to, or one that ships its own default, would otherwise not have. Returns whether it has it. */
bool ms_frame_rate_sync_conf(const char* metalsharp_home);

#endif
