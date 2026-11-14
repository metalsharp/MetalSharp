#ifndef METALSHARP_BACKEND_NGX_CORE_H
#define METALSHARP_BACKEND_NGX_CORE_H

#include <stdbool.h>

/* Where NVIDIA's NGX (DLSS) looks for its core in a prefix, per graphics lane (ngx_core.c). */

#define MS_NGX_CORE_MARKER ".metalsharp-ngx-core" /* in the prefix: MetalSharp set the core's place for D3DMetal */
#define MS_NGX_CORE_DIR    "C:\\windows\\system32"

typedef enum {
    MS_NGX_CORE_KEEP = 0, /* nothing to change */
    MS_NGX_CORE_SET,      /* a D3DMetal launch: point NGX at Apple's core */
    MS_NGX_CORE_CLEAR,    /* another lane after D3DMetal: take MetalSharp's values back out */
} ms_ngx_core_action;

/* What a launch on a lane (d3dmetal or not) needs in prefix. */
ms_ngx_core_action ms_ngx_core_action_for(const char* prefix, bool d3dmetal);

/* Before a launch on canonical_pipeline, in the prefix $WINEPREFIX names (else home's prefix-steam): sets or clears
 * NGXCore's FullPath and NGXPath with the runtime's Wine (home/runtime/wine/bin/wine). Returns whether the prefix
 * is now as the lane needs. */
bool ms_ngx_core_route(const char* home, const char* canonical_pipeline);

#endif
