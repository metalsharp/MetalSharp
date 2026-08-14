# MetalSharp Compatdata Architecture
**Updated:** 2026-07-08


Status: Phase 2 foundation

MetalSharp compatdata records are the launch-authoritative runtime records for Steam games. They are inspired by Proton's per-app runtime discipline, but they keep Wine Steam as the account, download, and session provider.

## Paths

Steam game compatdata records live at:

```text
~/.metalsharp/compatdata/<appid>/metalsharp-compatdata.json
~/.metalsharp/compatdata/<appid>/logs/
~/.metalsharp/compatdata/<appid>/assets/
```

Existing Steam bottle manifests still live at:

```text
~/.metalsharp/bottles/steam_<appid>/bottle.json
```

The current implementation records the shared Wine Steam prefix as the active prefix because Wine Steam must remain a stable, long-lived entity. The compatdata record is still authoritative for launch routing, dependency visibility, runtime assets, and diagnostics.

Runtime migrations preserve compatdata metadata alongside bottle settings, game metadata, Steam prefix settings, and Sharp Library records. They do not stage full Wine prefixes, Steam `steamapps/` payloads, or downloaded game installations.

## Record Contents

Each Steam compatdata record stores:

- Steam appid and display name
- linked bottle id
- compatdata path
- active prefix path
- Wine Steam prefix path
- game install path
- runtime profile
- launch pipeline
- Steam identity mode
- compatibility tool name and launch command template
- log directory
- detected runtime assets from the game install
- required runtime components
- last launch log path, pid, status, and finish time when known

## API

`POST /steam/compatdata`

Request:

```json
{
  "appid": 620,
  "pipeline": "vkd3d"
}
```

The endpoint ensures the Steam game bottle exists, refreshes detected assets, writes the compatdata record, and returns the record.

Steam game launches also attach the current compatdata record to launch responses when available.

Wine-backed MTSP Steam launches write process output to:

```text
~/.metalsharp/compatdata/<appid>/logs/launch-<timestamp>.log
```

Bottle diagnostics refresh the Steam compatdata record and check that both the compatdata manifest and log directory exist. That makes the Steam game bottle the repair surface while the compatdata record remains the authoritative launch ledger.

## Why This Matters

Steam should not be torn down or replaced just to pass route-specific game runtime state. Steam remains responsible for login, ownership, downloads, cloud/session behavior, and staying alive while games run. MetalSharp compatdata owns the game launch contract so DLLs, redists, logs, runtime assets, and selected pipeline state can be repaired and inspected per game.

Future work can move individual game runtime state into `compatdata/<appid>/pfx` only where the Steam/game process model allows it cleanly.
