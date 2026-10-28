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
/* This build's game rule set in a configs folder: mtsp-rules.toml (regular) or mtsp-rules-fex.toml. */
const char* ms_wine_rules_file(void);

/* <home>/runtime/MetalSharp-Wine.app, and its Contents directory. */
bool ms_wine_app_path(const char* home, char* out, size_t out_size);
bool ms_wine_app_contents(const char* home, char* out, size_t out_size);

/* On the fex build, ~/.metalsharp/runtime/wine is a small facade over the app: bin/ has
 * metalsharp-wine, wine, wine64, wineserver and the Wine programs as scripts that clear the
 * x86_64 runtime's library and DLL paths and run the app's wine; lib and share link to the app's;
 * etc/ is MetalSharp's own (dxmt.conf). Code that runs runtime/wine/bin/metalsharp-wine therefore
 * runs MetalSharp Wine on both builds. */
bool ms_wine_fex_write_facade(const char* home, char* error, size_t error_size);
bool ms_wine_fex_facade_ready(const char* home);

/* The graphics pipeline a route runs on MetalSharp Wine. Its backends are Mullion (dxmt), DXVK and
 * VKD3D-Proton for x86_64 programs, and wined3d, so the D3DMetal and GPTK lanes run on VKD3D-Proton
 * and the D3D9 route on DXVK. Returns the pipeline itself on the regular build, and for the pipelines
 * both builds have. */
const char* ms_wine_route_pipeline(const char* pipeline);
/* GRAPHICS_BACKEND for a canonical pipeline on MetalSharp Wine: dxmt, dxvk, vkd3d or wine. */
const char* ms_wine_fex_graphics_backend(const char* pipeline);

/* Whether a process's executable is the Wine MetalSharp runs: the x86_64 runtime's
 * (runtime/wine/bin/wine, lib/wine/x86_64-unix/wine) or MetalSharp-Wine.app's
 * (Contents/MacOS/wine, Contents/lib/wine/aarch64-unix/wine). */
bool ms_wine_is_runtime_wine_path(const char* home, const char* path);
/* The directory every Wine process's executable is under, for telling MetalSharp's Wine processes
 * from others: runtime/wine, or runtime/MetalSharp-Wine.app on fex. The _dup form is malloc'd. */
bool ms_wine_process_root(const char* home, char* out, size_t out_size);
char* ms_wine_process_root_dup(const char* home);
/* The wineserver to stop a prefix with: the app's on fex, runtime/wine/bin/wineserver otherwise. */
bool ms_wine_server_path(const char* home, char* out, size_t out_size);

/* The fex build's MetalSharp Wine install: copies MetalSharp-Wine.app from this MetalSharp's
 * Resources (METALSHARP_WINE_APP_SOURCE overrides it) to <home>/runtime with ditto, when it is
 * missing or a different version; checks its signature (codesign --verify --deep --strict); makes
 * it read-only, so nothing can break its seal; writes the runtime/wine facade. */
bool ms_wine_fex_install_app(const char* home, char* error, size_t error_size);
/* MetalSharp Wine's install lock (<home>/runtime/.metalsharp-wine.lock), which the install takes
 * and the startup refresh only tries for: one process at a time replaces the app. Returns the
 * lock's descriptor, or -1 (busy without `wait`, or an error). */
int ms_wine_fex_lock(const char* home, bool wait);
void ms_wine_fex_unlock(int fd);
/* A migration is pending (the updater's marker) or running (migration.lock): it reinstalls MetalSharp
 * Wine itself, so the startup refresh leaves it alone. */
bool ms_wine_fex_refresh_blocked(const char* home);
/* At backend start, on a detached thread: once setup has installed MetalSharp Wine, reinstall it
 * when this MetalSharp carries a different build, and rewrite the facade's wrappers. */
void ms_wine_fex_refresh_async(const char* home);
/* The MetalSharp-Wine.app this MetalSharp carries, or NULL. Malloc'd. */
char* ms_wine_fex_app_source(void);

/* Prepares a prefix for MetalSharp Wine before anything runs in it: wineboot -u (ARM64X
 * builtins and FEX's WoW64 keys), the bundled VC++ 2022 x64 and x86 runtimes, and the bundled
 * Microsoft core fonts, unpacked with cabextract since their installers fail under Wine. Records
 * it in <prefix>/.metalsharp-wine-arch ("arm64") and .metalsharp-prefix-ready. Done once per
 * prefix; `force` runs it again. */
bool ms_wine_fex_prepare_prefix(const char* home, const char* prefix, bool force, char* error, size_t error_size);
/* Called as each part of preparing or converting a prefix starts, with a short name for the part
 * and a sentence (setup shows them); NULL stops it. */
void ms_wine_set_prefix_progress(void (*progress)(void* context, const char* part, const char* message),
                                 void* context);
bool ms_wine_fex_prefix_ready(const char* prefix);

/* Prefixes move between the builds one way, with a backup:
 * - ms_wine_fex_convert_prefixes (fex): every MetalSharp prefix (prefix-steam, prefix-ubisoft,
 *   sharp-prefix, bottles/<id>/prefix) that the x86_64 Wine built gets its system.reg, user.reg,
 *   userdef.reg and drive_c/windows backed up to <prefix>/.metalsharp-x86_64-backup and is then
 *   prepared for MetalSharp Wine (installed programs and games stay where they are). Returns how
 *   many could not be converted.
 * - ms_wine_restore_x86_64_prefixes (regular): a prefix marked arm64 gets that backup back; one
 *   with no backup is left alone and counted, since the x86_64 Wine must not run it. */
/* The graphics config MetalSharp edits: Mullion's runtime/wine/etc/mullion.conf, named by
 * MULLION_CONFIG_FILE, on the fex build (a dxmt.conf from before is renamed to it once, keeping its
 * settings); DXMT's dxmt.conf and DXMT_CONFIG_FILE on the regular build. */
bool ms_wine_graphics_conf_path(const char* home, char* out, size_t out_size);
const char* ms_wine_graphics_conf_env(void);

/* The caches FEX builds before 1.0.0 let VKD3D-Proton and DXVK write next to a game, before they went
 * in the lanes' folders: exactly vkd3d-proton.cache (with VKD3D-Proton's vkd3d-proton.cache.write)
 * and <executable name>.dxvk-cache, regular files in `dir` only. Once per folder
 * (.metalsharp/old-caches-cleared); returns how many it removed. */
unsigned ms_wine_fex_remove_old_game_caches(const char* dir, const char* executable);

bool ms_wine_prefix_is_arm64(const char* prefix);
/* The build that set up a home: setup.json's `runtime_variant` ("fex" or "regular"). Without one (0.80.x
 * and earlier, or a setup that didn't record it), "fex" when the Steam prefix is MetalSharp Wine's, else
 * "regular". */
const char* ms_wine_recorded_variant(const char* home, const char* recorded);
/* The build a finished setup records in setup.json, or NULL to leave it to migration: the running build,
 * unless the home's Steam prefix is the other build's (an install that still has to move). */
const char* ms_wine_setup_variant_to_record(const char* home);
/* One prefix: converted as above when the x86_64 Wine built it, else prepared (setup's Steam
 * prefix, which may still be the 0.80.0 one). */
bool ms_wine_fex_convert_prefix(const char* home, const char* prefix, char* error, size_t error_size);
unsigned ms_wine_fex_convert_prefixes(const char* home, char* error, size_t error_size);
unsigned ms_wine_restore_x86_64_prefixes(const char* home);

#endif
