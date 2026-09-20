# Game Streaming (Sunshine + Moonlight)

Stream your MetalSharp games to a phone or tablet while they run on your Mac.
MetalSharp manages [Sunshine](https://github.com/LizardByte/Sunshine) — the
streaming host — and pairs it with [Moonlight](https://moonlight-stream.org),
the free client for iPhone, iPad, and Android.

## Quick start

1. In MetalSharp, click the **Stream** button in the footer (the TV + Wi-Fi
   icon).
2. Click **Install Sunshine**. MetalSharp downloads the official
   `Sunshine-macOS-arm64.dmg` from LizardByte's releases and installs it into
   `/Applications`.
3. Click **Start Streaming Host**. MetalSharp launches Sunshine and provisions
   its web credentials automatically (stored in
   `~/.metalsharp/streaming-creds.json`).
4. On your phone or tablet, install **Moonlight Game Streaming**:
   - [App Store (iOS)](https://apps.apple.com/app/moonlight-game-streaming/id1000551566)
   - [Google Play (Android)](https://play.google.com/store/apps/details?id=com.limelight)
5. Connect both devices to the same Wi-Fi network.
6. Open Moonlight and tap your Mac. Moonlight shows a **4-digit PIN**.
7. Back in MetalSharp, enter the PIN and click **Pair Device**.
8. Tap your game in Moonlight to start streaming.

## How it works

- Your game runs natively on this Mac through MetalSharp (Wine, DXMT/VKD3D
  routes, etc.).
- Sunshine captures the Mac's screen and audio and streams it to Moonlight at
  up to 4K60 over the local network.
- Moonlight sends touch and gamepad input back to the Mac.

## Things to know

- **Sunshine on macOS is experimental.** Gamepads do not work yet — use
  Moonlight's touch controls.
- The first time you stream, macOS asks for **Screen Recording** permission.
  Approve it once in System Settings → Privacy & Security.
- Both devices must be on the same network. Sunshine serves ports
  **47984–48010** (TCP and UDP); allow them through the macOS firewall if
  prompted.
- The Sunshine web UI lives at <https://localhost:47990>. MetalSharp stores
  the web credentials it generates in `~/.metalsharp/streaming-creds.json`;
  if you change the credentials inside Sunshine's own web UI, delete that
  file and relaunch streaming so MetalSharp can re-provision them.
- **Unpair all devices** (in the streaming panel) removes every paired client
  from Sunshine — useful when selling or losing a device.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Moonlight can't see the Mac | Confirm Sunshine is running (the streaming panel shows *Running*) and both devices are on the same network. |
| PIN rejected | PINs are single-use and expire. Tap the Mac in Moonlight again for a fresh PIN, then pair immediately. |
| Black screen in Moonlight | Approve Screen Recording for Sunshine in System Settings → Privacy & Security, then restart streaming. |
| "Sunshine web credentials changed" | Someone set new credentials inside Sunshine's web UI. Delete `~/.metalsharp/streaming-creds.json` and relaunch streaming to re-provision. |
