# User Guide
**Updated:** 2026-10-05

## Install

1. Download the DMG from [Releases](https://github.com/metalsharp/MetalSharp/releases) and drag MetalSharp into `/Applications`, or run `brew install --cask metalsharp/tap/metalsharp`.
2. Open it. If macOS blocks it, go to **System Settings → Privacy & Security → Open Anyway**.
3. Let the setup wizard install the runtime (Wine, graphics libraries, redistributables). Homebrew is not required.
4. Start Wine Steam, sign in, and install a Windows game.

## Steam Games

Click **Play** on the Library page. MetalSharp picks a graphics route automatically; if a game doesn't run, pick another from the launch mode dropdown. The routes are listed in the [README](../README.md#launching-games-and-graphics-routes), and tested picks are in [Supported Games](games-supported.md).

**Steam-Emu (Goldberg)** lets supported games, mostly older ones, run offline without Wine Steam. Turn it on from the game card. MetalSharp backs up the original Steam DLLs as `.orig` and restores them when you turn it off.

**Choose EXE:** For Steam and Ubisoft games, open the game's **Settings** popover in the Library hero. For GOG and Epic games, use the gear on the game card. Pick an `.exe` inside the game's install folder. The choice is saved for that game.

## Other Stores and Programs

The **Sharp Library** holds Windows apps, launchers, installers, and non-Steam games. It has tabs for Epic, GOG, Game Jolt, and [emulators](emulators.md). For Ubisoft Connect, see the [Ubisoft Connect guide](ubisoft-connect.md).

- **Install Windows Program** takes an `.exe` or `.msi`. MetalSharp imports it directly, or runs the installer in its own bottle and picks up the installed app.
- Rename an app with the pencil next to its title, and change its cover with **Change Image** (JPEG, PNG, or WebP).
- **Tools → Open Folder** opens the app's bottle in Finder. Install runtime components through the bottle tools; don't copy them in by hand.

## Controller Input

The **Controller** selector in the sidebar sets which input shims MetalSharp deploys to the game folder and Steam prefix:

- **Off** (default): none.
- **X**: XInput (`xinput1_1`–`xinput1_4.dll`, `xinput9_1_0.dll`).
- **D**: DirectInput (`dinput.dll`, `dinput8.dll`).

Switching modes removes the previous set. If the game shipped its own copy of a shim, MetalSharp backs it up and restores it.

## Game Streaming

MetalSharp can stream games to a phone or tablet using [Sunshine](https://github.com/LizardByte/Sunshine) on the Mac and [Moonlight](https://moonlight-stream.org) on the device.

1. Click **Stream** in the footer, then **Install Sunshine**, then **Start Streaming Host**.
2. Install Moonlight ([iOS](https://apps.apple.com/app/moonlight-game-streaming/id1000551566), [Android](https://play.google.com/store/apps/details?id=com.limelight)) and put the device on the same Wi-Fi network as the Mac.
3. In Moonlight, tap your Mac. Enter the PIN it shows into MetalSharp and click **Pair Device**.

Sunshine on macOS is experimental, and gamepads don't work yet, so use Moonlight's touch controls. Approve **Screen Recording** for Sunshine on first use, or the stream will be black. Sunshine uses ports 47984–48010 (TCP and UDP). If you change the login in Sunshine's web UI (<https://localhost:47990>), delete `~/.metalsharp/streaming-creds.json` and restart streaming. PINs work once; if pairing fails, tap the Mac again in Moonlight to get a new one.

## Logs, Settings, and Uninstall

- **Logs** has live logs, crash reports, and recent log files.
- **Settings** covers Steam API sync, backend restart, cache cleanup, runtime maintenance, and updates. Updates download, verify, then ask before closing MetalSharp and stopping Steam/Wine.
- **Settings → Danger Zone → Uninstall MetalSharp** removes all prefixes, bottles, Steam, the runtime, caches, and settings, then moves the app to the Trash.
