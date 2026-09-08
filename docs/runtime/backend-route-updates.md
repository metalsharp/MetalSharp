# Backend route updates

D3DMetal uses the bundled GPTK 4 payload and the Steam-prefix direct launcher,
retaining Steam application identifiers. The framework environment variable names
the framework executable, and the runtime root is supplied separately. Saving a
bottle before library discovery is allowed; staging resolves the game later.
Saved executable selection is refreshed when loading persisted Steam state.
Subnautica 2 targets its Shipping executable; GTA V Enhanced targets its game
binary rather than the BattlEye service.

Route transitions compare DLL bytes with known runtime artifacts before cleanup,
including D3DMetal's `nvngx-on-metalfx.dll`. Modified or game-provided graphics
DLLs are not removed by cleanup. D3DMetal play also invokes the existing controller
shim reconciliation. The controller helper's existing ownership behavior is
unchanged by this PR.

VKD3D selects the isolated MoltenVK lane and enables private API and retained
command buffers. M12 remains DXMT; no contributor hashes are adopted.
MSYNC is read from configuration for launches and diagnostics. Wine caches server
synchronization settings, so a running shared prefix does not hot-switch MSYNC.

The setup UI waits for Steam's x64 installation markers and does not stop Steam
when finishing the wizard. D3DMetal displays a single readiness indicator rather
than obsolete GPTK 3 repair controls.

## Validation and deployment

Run `make -C app/src-c test` and `cd app && npm run build`. The routing regression
harness uses a temporary fixture home and never launches Wine. It checks MSYNC,
D3DMetal environment paths, VKD3D/M12 separation, and byte-matched cleanup.

Manual checks on an external SSD included Nine Sols and Elden Ring through the
Steam-prefix D3DMetal direct route, and Control route staging/cleanup. Executable
selection was checked for GTA V Enhanced and Subnautica 2.

This source-only PR does not include runtime archives, Wine binaries, local prefix
state, or version bumps. DXMT manifest v2 identifies the v0.80 baseline: deployment
must pair setup with that graphics payload, not relabel an older archive. Native
DXMT bridges are ad-hoc signed and verified after extraction.

Rollback: revert this PR and deploy the matching previous graphics payload and
manifest metadata. Re-prepare affected bottles for the selected route; do not
wipe the shared Steam prefix or user game data.
