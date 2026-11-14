# Updates

MetalSharp updates itself with [Sparkle](https://sparkle-project.org) 2.9.6. MetalSharp draws the update in its own sheet; Sparkle downloads, checks, installs and relaunches. A release carries a `.delta` patch from each of its last two Sparkle releases, so an update downloads only what changed.

## How an update runs

1. **Finding it.** The backend's `/update/check` compares the latest GitHub release with this build. That drives the update sheet: the release notes, the choice of builds, switching builds. This part hasn't changed.
2. **Installing a newer version of the running build** (`ui_live.rs` `begin_update_download`, `sparkle.rs`, `app-gpui/sparkle/MSSparkleBridge.m`):
   - Sparkle reads the build's appcast (`SUFeedURL`): `appcast.xml` for x86_64, `appcast-fex.xml` for FEX, both from `releases/latest/download/`.
   - It downloads the `.delta` made from the version that is running. If there isn't one, it downloads the full `.zip`.
   - It checks the download's EdDSA signature (`SUPublicEDKey`, `tools/sparkle/public-key.txt`) before extracting it. It checks the code signature against this app's.
   - When the update is ready, MetalSharp does what `update.sh` did before replacing the app: it stops Steam and Wine (`/steam/stop`), writes `~/.metalsharp/.post-update-migration` for the new version's first launch, and stops the backend.
   - Sparkle then installs. MetalSharp quits, and Sparkle's installer replaces `MetalSharp.app` and relaunches it. The relaunched app runs the post-update migration as before (the runtime refresh, `APP_UPDATE`).
   - `MSSparkleBridge.m` is a Sparkle user driver that never shows Sparkle's windows. `sparkle::state()` is what the sheet draws: phase, version, bytes, extraction, error.
3. **Everything else uses the release's DMG and `update.sh`, as before:**
   - switching between the x86_64 and FEX builds (Sparkle only installs newer versions);
   - a release whose appcast has nothing newer (releases before Sparkle);
   - builds without Sparkle: development builds, or `METALSHARP_SPARKLE=0` when packaging.

Sparkle never checks on its own (`SUEnableAutomaticChecks` is false), and it never sends a system profile. MetalSharp decides when to look.

## Building

- `tools/sparkle/fetch-sparkle.sh` downloads Sparkle 2.9.6 (checksum-pinned) into `build/sparkle/2.9.6`. That gives `Sparkle.framework` plus `BinaryDelta`, `sign_update` and `generate_keys`.
- `app-gpui/package-local-preview.sh` (through `package-app.sh`) does the rest:
  - builds with the `sparkle` cargo feature;
  - copies `Sparkle.framework` into `Contents/Frameworks`;
  - writes the Sparkle keys into `Info.plist`;
  - signs Sparkle inside-out as Sparkle documents: `Installer.xpc`, then `Downloader.xpc` (keeping its own entitlements), `Autoupdate`, `Updater.app`, and the framework. None of them gets MetalSharp's entitlements.
- `METALSHARP_SPARKLE=0` packages without Sparkle.
- `app-gpui/build.rs` compiles `MSSparkleBridge.m` and links the framework, only with the `sparkle` feature. Without the feature (`cargo test`, development builds) nothing is linked, and `sparkle::available()` is false.

## Releasing

Release CI (`.github/workflows/release.yml`), for each build, after the app is packaged and signed and before its DMG is made:

1. **`sparkle_release.py notarize`:** notarizes the app itself and staples its ticket. Sparkle installs the app, not the DMG. The DMG is then made from the stapled app and notarized as before.
2. **`sparkle_release.py publish`:**
   - `MetalSharp-X.Y.Z-arm64[-FEX].zip`: the full update.
   - `MetalSharp-X.Y.Z-arm64[-FEX]-from<old>.delta`: one from each of the last two releases that published this build's `.zip`. Each delta is applied to a copy of that release's app, which must come out byte-identical to the new app and pass `codesign --verify --deep --strict`. If not, the release stops.
   - The build's appcast, with one item: this version, its archive and its deltas, each signed with `SPARKLE_ED_PRIVATE_KEY`.

The release job publishes them next to the DMGs. Since apps read `releases/latest/download/<appcast>`, the newest release's appcast is the feed. A release is in it once it's published and not a prerelease.

The first release with Sparkle has no deltas, because no earlier release has an archive. Users on 1.2.0 and earlier reach it through their own DMG updater. From then on, updates come through Sparkle.

## The signing key

- Updates are signed with an EdDSA key made for MetalSharp. The public half is in `tools/sparkle/public-key.txt`, which becomes `SUPublicEDKey`.
- The private half is in the repository secret `SPARKLE_ED_PRIVATE_KEY`, and in the release maintainer's login keychain under the Sparkle account `metalsharp-releases`. To sign locally: `build/sparkle/2.9.6/bin/sign_update --account metalsharp-releases <file>`.
- If the key is ever replaced, every installed MetalSharp rejects updates signed with the new one. It would have to ship in a release still signed with the old key first.

## Testing

- `python3 -m unittest tools.sparkle.tests.test_sparkle_release`: naming, which releases deltas come from, `sign_update` parsing, the appcast.
- `cargo test` (`sparkle::tests`): the bridge's state, the sheet's progress, the migration marker.
- **`tools/sparkle/e2e/run-e2e.sh <scratch folder>`** runs the whole path on a Mac with a Developer ID identity and the keychain key:
  1. builds a small host app (`host.m`) around `MSSparkleBridge.m` at 1.0.1 and 1.0.2, signed the way MetalSharp signs Sparkle;
  2. makes 1.0.2's archive, delta and appcast with `sparkle_release.py publish`;
  3. serves them locally and lets 1.0.1 update itself.

  It passes only if 1.0.1 relaunches as 1.0.2, the installed app verifies, and the delta (not the archive) was downloaded.
