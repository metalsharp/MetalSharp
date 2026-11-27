#ifndef METALSHARP_BACKEND_CACHE_H
#define METALSHARP_BACKEND_CACHE_H

#include <stddef.h>

char* ms_cache_size_json(const char* metalsharp_home);
/* The folder of a lane's caches under shader-cache/ and pipeline-cache/ (out, returned). Mullion's lanes are one
 * folder for both runtimes where both run Mullion: its caches depend on the GPU and Mullion's version, not on the CPU
 * the runtime translates (FEX or Rosetta). The other lanes are each runtime's, the FEX runtime's as fex-<lane>. */
const char* ms_cache_lane_folder(const char* lane, char* out, size_t out_size);
/* Before a game's launch on a Mullion lane: where its shared folder has nothing yet, the caches either runtime made
 * for it in its own folder become the shared ones (the larger), and the other runtime's copy goes. */
void ms_cache_adopt_mullion_cache(const char* metalsharp_home, const char* lane, unsigned appid);
char* ms_cache_clear_json(const char* metalsharp_home, const unsigned char* body, size_t body_length);

#endif
