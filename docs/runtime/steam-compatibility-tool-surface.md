# Steam Compatibility Tool Surface
**Updated:** 2026-09-26

Steam game compatdata records a compatibility-tool-like contract while MetalSharp's backend stays the launcher. Steam owns account/session/download state; MetalSharp owns the game process route, bottle, compatdata, logs, and runtime assets.

## Recorded Fields

- `compat_tool_name`
- `launch_command_template`
- `launch_pipeline`
- `steam_identity_mode`
- `bottle_id`
- `prefix_path`
- `steam_prefix_path`
- runtime assets, components, and launch ledger state

The launch command template is backend-shaped:

```text
POST /steam/launch-game {"appid":<appid>,"launchMethod":"<pipeline>"}
```

The supported launch path is MetalSharp starting the game process while Wine Steam stays alive in the background for Steamworks connectivity. macOS Steam does not provide Linux Proton's `compatibilitytools.d` tool surface, and Wine Steam needs to remain a normal Windows Steam client for login, downloads, and sessions.

## Native macOS Steam Games

Installed Steam manifests whose install directory contains a `.app` bundle are identified as native macOS builds. They are labeled **Native macOS**, omit the Wine bottle/graphics-route selector, and launch the discovered app bundle with macOS `open` instead of searching for a Windows executable. Settings can optionally exclude these native titles from the MetalSharp library; changing the setting refreshes the library immediately.

## Remaining Work

- Verify whether any current macOS Steam or Wine Steam path honors compatibility tool metadata.
- Add last-known-good runtime rollback per appid.
- Add a visible per-game route template/debug view showing exactly what MetalSharp will launch.
