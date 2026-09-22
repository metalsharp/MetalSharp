# Compatdata Architecture
**Updated:** 2026-09-22

Compatdata records are the launch-authoritative runtime records for Steam games. Wine Steam stays the account, download, and session provider; compatdata owns the per-game launch contract so DLLs, redists, logs, runtime assets, and the selected route can be repaired and inspected per game.

## Paths

```text
~/.metalsharp/compatdata/<appid>/metalsharp-compatdata.json
~/.metalsharp/compatdata/<appid>/logs/
~/.metalsharp/compatdata/<appid>/assets/
~/.metalsharp/bottles/steam_<appid>/bottle.json
```

The record points at the shared Wine Steam prefix as the active prefix; the record itself is authoritative for launch routing, dependency visibility, runtime assets, and diagnostics. Runtime migrations preserve compatdata metadata alongside bottle settings, game metadata, Steam prefix settings, and Sharp Library records.

## Record Contents

Each record stores:

- Steam appid and display name
- linked bottle id, compatdata path, active prefix path, Wine Steam prefix path, game install path
- runtime profile and launch pipeline
- Steam identity mode
- compatibility tool name and launch command template
- log directory
- detected runtime assets and required runtime components
- last launch log path, pid, status, and finish time when known

## API

```text
POST /steam/compatdata {"appid": 620, "pipeline": "vkd3d"}
```

The endpoint ensures the Steam game bottle exists, refreshes detected assets, writes the record, and returns it. Launch responses attach the current record when available.

Wine-backed launches write process output to:

```text
~/.metalsharp/compatdata/<appid>/logs/launch-<timestamp>.log
```

Bottle diagnostics refresh the record and check that the manifest and log directory exist. The Steam game bottle is the repair surface; the compatdata record is the launch ledger.
