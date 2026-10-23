#ifndef METALSHARP_BACKEND_WINE_RUNTIME_H
#define METALSHARP_BACKEND_WINE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

/* Which Wine this build of MetalSharp runs, fixed at build time (make VARIANT=fex):
 *   regular  the x86_64 Wine 11.17 runtime in ~/.metalsharp/runtime/wine, run through Rosetta
 *   fex      MetalSharp Wine, a native arm64 Wine that runs x86 code with FEX, from its signed
 *            MetalSharp-Wine.app in ~/.metalsharp/runtime (never written to or re-signed)
 * The regular build never takes a fex branch. */
const char* ms_wine_variant(void);
bool ms_wine_is_fex(void);

/* <home>/runtime/MetalSharp-Wine.app, and its Contents directory. */
bool ms_wine_app_path(const char* home, char* out, size_t out_size);
bool ms_wine_app_contents(const char* home, char* out, size_t out_size);

/* On the fex build, ~/.metalsharp/runtime/wine is a small facade over the app: bin/ has
 * metalsharp-wine, wine, wine64, wineserver and the Wine programs as scripts that clear the
 * x86_64 runtime's library and DLL paths and run the app's wine; lib links to the app's lib;
 * etc/ is MetalSharp's own (dxmt.conf). Code that runs runtime/wine/bin/metalsharp-wine therefore
 * runs MetalSharp Wine on both builds. */
bool ms_wine_fex_write_facade(const char* home, char* error, size_t error_size);
bool ms_wine_fex_facade_ready(const char* home);

/* The graphics pipeline a route runs on MetalSharp Wine. Its backends are Mullion (dxmt), DXVK
 * for x86_64 programs, and wined3d, so the D3DMetal, VKD3D and GPTK lanes run on Mullion. Returns
 * the pipeline itself on the regular build, and for the pipelines both builds have. */
const char* ms_wine_route_pipeline(const char* pipeline);
/* GRAPHICS_BACKEND for a canonical pipeline on MetalSharp Wine: dxmt, dxvk or wine. */
const char* ms_wine_fex_graphics_backend(const char* pipeline);

/* Whether a process's executable is the Wine MetalSharp runs: the x86_64 runtime's
 * (runtime/wine/bin/wine, lib/wine/x86_64-unix/wine) or MetalSharp-Wine.app's
 * (Contents/MacOS/wine, Contents/lib/wine/aarch64-unix/wine). */
bool ms_wine_is_runtime_wine_path(const char* home, const char* path);
/* The wineserver to stop a prefix with: the app's on fex, runtime/wine/bin/wineserver otherwise. */
bool ms_wine_server_path(const char* home, char* out, size_t out_size);

#endif
