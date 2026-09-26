# Launcher Runtime
**Updated:** 2026-09-22

MetalSharp treats launcher installers as bottle-managed Windows programs: launchers keep their login/session state, child games install into the same bottle, and logs explain why a launcher or child game failed.

## Known Launcher Recipes

The installer classifier recognizes these launcher families before generic .NET, WebView, MSI, or PE import heuristics:

| Launcher | Profile |
|---|---|
| Minecraft Launcher | Java Launcher |
| EA App / Origin | WebView |
| Ubisoft Connect / Uplay | WebView |
| Epic Games Launcher | WebView |
| Rockstar Games Launcher / Social Club | WebView |
| GOG Galaxy | Launcher |

Known launchers are recorded as `known_launcher:<id>` and `launcher_name:<display name>` hints, and install through the bare Wine pipeline so store launchers never inherit game graphics routes. Once a launcher installs a game, the child executable gets its own bottle and route.

The WebView profile provisions Gecko, WebView2, .NET 4.8, VC runtime, and core fonts, because EA-style WiX/MSI launchers can run .NET custom actions after the visible install bar completes.

## Installer Bottles

`Install Windows Program` routes launcher-like EXEs and MSI packages into installer bottles that record: source installer path, installer kind, runtime profile, prefix path, launch log, launch pid/status, and detected installed app candidates. WebView2/Edge helper executables are runtime components and stay out of app detection.

## Native Epic Library Path

The Sharp Library **Epic** tab downloads Epic games through upstream [Legendary](https://github.com/legendary-gl/legendary) 0.21.0 running out of process.

- MetalSharp downloads the official native arm64 release after the user selects **Install Epic Support**; the release URL, version, 64 MiB size ceiling, arm64 Mach-O identity, and SHA-256 are backend-owned and verified before atomic activation at `~/.metalsharp/tools/legendary/legendary-0.21.0`.
- Epic authentication runs in a sandboxed Electron window restricted to Epic, Legendary, and identity-provider HTTPS hosts; the backend receives only the authorization code. Legendary state lives under `~/.metalsharp/epic/legendary/`.
- Library sync requests Windows-installable account entries as JSON on login and manual **Sync**; successful catalogs cache atomically at `~/.metalsharp/epic/library.json`.
- Downloads use Legendary's manifest/CDN pipeline. Each install prompts for a writable location under the user's home or `/Volumes`; `~/.metalsharp/launcher-games/epic/location.txt` is the fallback root. Per-title progress and logs live under `~/.metalsharp/epic/processes/`.
- Installed games require **Initialize Bottle** before first launch; each title owns `~/.metalsharp/bottles/epic_<appName>/prefix` plus a manifest with its selected pipeline and mouse mode (**No Recenter** default; **Mouse Auto** restores cursor warping). Available pipelines include D3DMetal, VKD3D, D3D9, DXMT, DXMT (32-bit), and Mono/FNA; launch applies each graphics route's DLL search paths and environment to that game's isolated Wine process.
- Stop, closing the card, and **Cmd+Opt+Q** terminate the isolated Epic Wineserver. Epic running state refreshes through a lightweight PID-only endpoint while the Epic tab is active. Uninstall removes Legendary's registered game files and the title's bottle. Runtime migration preserves Epic account data, the cached catalog, game location, bottle manifests, and registry/user settings.

Backend routes: `GET /sharp-library/epic/status`, `GET /sharp-library/epic/games`, `GET /sharp-library/epic/running`, and POST actions for `install-tool`, `auth`, `logout`, `sync`, `install`, `progress`, `cancel`, `initialize`, `play`, `stop`, `stop-all`, `uninstall`.

## CEF Compatibility

Steam already uses a wrapped `steamwebhelper.exe` to force CEF onto a Wine-safe software GPU path. Sharp Library bottles generalize that behavior for launcher apps carrying CEF or Chromium payloads (`libcef.dll`, `chrome_*.pak`, `vk_swiftshader.dll`, `app.asar`): the original executable is preserved as `<name>_real.exe`, `<name>.exe` becomes an architecture-matched wrapper relaunching with `--in-process-gpu --disable-gpu`, and a sibling `metalsharp-cefchildhook.dll` handles launchers that spawn renderer, utility, or GPU children.

Proof status:

- **Minecraft Launcher:** installs cleanly as a `java_launcher` bottle from `MinecraftInstaller.msi`; the CEF wrapper deploys, but the embedded browser currently renders a blank surface — child process creation has yet to pass through the hooked paths.
- **EA App:** the installer reaches the MSI apply step, then fails with `0x80070643` / `INST-14-1603`; the WebView profile now provisions `dotnet48` and fresh proof relaunches stay in the selected bottle with `corefonts`, `dotnet48`, `gecko`, `vcrun2019`, and `webview2` installed. The next pass needs deeper Wine MSI/service/elevation inspection around per-machine package cache writes.
- **Ubisoft Connect:** auto-starts Ubisoft Game Loader, then enters the crash-reporter path; a clean direct launch still needs capture.

Launcher evidence reports:

```http
POST /launcher/evidence {"family":"ea"}
POST /launcher/evidence {"family":"ubisoft"}
```

## Remaining Work

- Finish the Minecraft CEF child-process path.
- Persist child game processes as bottle app records.
- Track launcher-owned game install folders separately from the launcher EXE.
- Add launcher-specific repair controls for WebView, Gecko, VC runtime, and session data.
- Add end-to-end smoke cases for at least three launchers.
