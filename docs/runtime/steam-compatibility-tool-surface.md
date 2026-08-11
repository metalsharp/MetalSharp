# MetalSharp Steam Compatibility Tool Surface
**Updated:** 2026-08-11


Status: Phase 7 foundation

MetalSharp should behave like a compatibility runtime without pretending macOS Steam exposes Linux Proton's exact `compatibilitytools.d` contract. The current app-owned model stays authoritative: Steam owns account/session/download state, while MetalSharp owns the game process route, bottle, compatdata, logs, and runtime assets.

## Current Surface

Steam game compatdata now records:

- `compat_tool_name`
- `launch_command_template`
- `launch_pipeline`
- `steam_identity_mode`
- `bottle_id`
- `prefix_path`
- `steam_prefix_path`
- runtime assets, components, and launch ledger state

The launch command template is intentionally backend-shaped:

```text
POST /steam/launch-game {"appid":<appid>,"launchMethod":"<pipeline>"}
```

That keeps the contract honest. The current supported path is still MetalSharp launching the game process while Wine Steam remains alive in the background for Steamworks connectivity.

## Steam App ID Input Contract

Backend routes that accept a JSON `appid` use the same boundary validation:

- the value must be a JSON number in the positive `u32` range (`1..=4,294,967,295`);
- missing, string, zero, negative, fractional, or out-of-range values receive a
  `400` response; and
- the value is validated before pipeline resolution, preparation, launch,
  diagnostics, game edits, or uninstall work begins.

This prevents a `u64` request value from silently truncating into another Steam
game's `u32` app ID. The compatibility `/game/prepare` route, canonical
`/mtsp/prepare`, `/mtsp/recipe`, `/mtsp/doctor`, and `/game/launch-auto` routes
share this contract with the other Steam game operations.

## Why Not Fake Proton

Linux Proton is installed as a Steam compatibility tool under `compatibilitytools.d`. macOS Steam does not provide that same documented Proton tool surface. Wine Steam also needs to remain a normal Windows Steam client for login, downloads, and session state. For now, MetalSharp records a compatibility-tool-like contract in compatdata and uses its backend as the launcher.

## Remaining Work

- Verify whether any current macOS Steam or Wine Steam path honors compatibility tool metadata in a useful way.
- Generate optional `compatibilitytool.vdf` scaffolding only for experiments, not as the default app path.
- Add last-known-good runtime rollback per appid.
- Add a visible per-game route template/debug view so users can see exactly what MetalSharp will launch.
