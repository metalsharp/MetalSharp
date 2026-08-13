# Library and Logs UI

**Updated:** 2026-07-22

## Library

Steam and backend status remain in the title row as the window narrows. Launch, refresh, search, and filter controls reflow below the title without moving those status badges into the action row.

Installed Steam game cards show the **Steam Emu** toggle. The EAC substrate
remains backend-controlled while its compatibility work continues; no EAC
toggle is exposed on game cards for now. Existing per-app EAC state and launch
plumbing remain available for diagnostics and migration, with substrate logs
and module dumps kept under `~/.metalsharp/logs/eac/<appid>/`.

The shipped MTSP rules include protected-launcher metadata (`eac_exe_names`)
and normal executable metadata (`exe_names`) for all requested EAC cards:
Elden Ring, ARMORED CORE VI, Rocket League, The Outlast Trials, Halo MCC,
Sea of Thieves, Pavlov, Rust, 7 Days to Die, Vermintide 2, Watch Dogs 2,
Fall Guys, Friday the 13th, VRChat, Rogue Company, Hunt: Showdown 1896,
Total Lockdown, Lost Ark, Gears 5, Halo Infinite, For Honor, REMATCH, Stay
Out, Back 4 Blood, Apex Legends, Lords of the Fallen, Throne and Liberty,
Star Wars: Squadrons, NBA 2K26, Next Day: Survival, Suicide Squad, SCP:
ReEnter, Killing Floor 3, Battlefield 2042, Squad, ARC Raiders, and
MultiVersus. New defaults use M11; the existing Elden Ring and AC6 VKD3D rules
remain unchanged. When saving either game as a D3DMetal bottle, MetalSharp
preserves `Game/start_protected_game.exe` as
`Game/start_protected_game.old` once, then copies the real game executable to
the original protected-launcher path.

The EAC card control is intentionally hidden for now. The card remains
available for every installed Steam game; rule metadata continues to control
protected and normal executable selection in backend launch paths.

### EAC substrate installation lifecycle

The DMG and split scripts/tools bundle must contain both native boundary
artifacts: `metalsharp_eac_substrate.dylib` (x86_64 Mach-O) and
`metalsharp_eac_libc.so.6` (x86-64 ELF). First-run setup installs the verified
pair into `~/.metalsharp/runtime/eac/` after the scripts/tools bundle. The pair
is staged and committed together, so a failed refresh cannot leave one new
artifact beside one old artifact.

An app update verifies those files before replacing the installed app and then
sets the post-update migration marker. Migration schema 5 treats a missing or
invalid durable pair as runtime repair: it reinstalls the substrate, verifies
both files, and only then marks the migration complete. Per-game EAC state JSON
under `sharp-library` is preserved; the backend state remains opt-in and does
not launch a game during installation, update, or migration. The app-card
toggle is hidden until the substrate is ready.

## Sharp Library

Use the **Library source** menu to switch between installed Windows applications and GOG games. The installer view keeps its primary actions focused on installing and refreshing applications; redistributable source controls are not shown in this header.

Installed application cards show their app type, install state, and size on one line. Their primary row contains **Play**, the bottle route selector, and **Tools**. The Tools panel includes:

- **Set Cover** to choose custom artwork.
- **Add Asset** to choose any file, starting in `~/.metalsharp/runtime`, and copy it to that application's bottle under `drive_c/metalsharp-assets`.
- **Uninstall** to remove the application.

Add Asset is unavailable for applications that are not associated with an app-specific bottle.

## Logs

The Logs view presents **Live log stream**, **Crash reports**, and **Recent log files** as three responsive selector buttons. Selecting one opens its content in a bounded panel below the selector row, so the page header and the other selectors stay in place.

The live stream is limited to 1,000 displayed lines. Each time another 1,000-line threshold is reached, the displayed stream clears and begins the next batch while backend polling continues from the last received line.

## Development preview

Run `npm run preview` from `app/` to build and open the live Library views against the existing `~/.metalsharp` data and the development backend. Preview mode skips the first-launch setup wizard only in an unpackaged development build; packaged app first-run behavior is unchanged.
