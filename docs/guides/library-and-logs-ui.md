# Library and Logs UI

**Updated:** 2026-07-22

## Library

Steam and backend status remain in the title row as the window narrows. Launch, refresh, search, and filter controls reflow below the title without moving those status badges into the action row.

Installed Steam game cards show the **Steam Emu** toggle followed immediately by
the opt-in **EAC** toggle. EAC is disabled by default for every app. The card
enables it only when the packaged MetalSharp substrate and Linux symbol image
are available on macOS; enabling it persists per-app state under
`~/.metalsharp/sharp-library/eac/` and applies the substrate environment only
to the next MetalSharp Wine launch. It never starts a game automatically. An
opted-in Steam or GPTK selection is routed to the already-installed M12
MetalSharp Wine 11.5 lane so the substrate is not sent through GPTK, another
Wine build, or macOS Steam. Per-app substrate logs and module dumps are kept
under `~/.metalsharp/logs/eac/<appid>/`.

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
