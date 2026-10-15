#ifndef METALSHARP_BACKEND_METALFX_H
#define METALSHARP_BACKEND_METALFX_H

#include <stdbool.h>
#include <stddef.h>

char* ms_metalfx_get_json(const char* metalsharp_home);
char* ms_metalfx_set_json(const char* metalsharp_home, const unsigned char* body, size_t body_length);
void ms_metalfx_state(const char* metalsharp_home, bool* enabled, double* factor);
/* Writes the factor in effect (1.00 when MetalFX is off) to runtime/wine/etc/dxmt.conf,
 * which DXMT (DXMT_CONFIG_FILE) and WineMetalGL both read when a game starts. */
bool ms_metalfx_sync_conf(const char* metalsharp_home);

#endif
