# Shader Presets

Pre-compiled Metal shader caches captured from playtesting. Deployed to user's
local cache on first launch to eliminate shader compilation stutter.

## Directory Layout

```
shader-presets/
├── dxmt-metal/          # DXMT D3D11 (M11 pipeline)
│   ├── 312520.db        # Rain World (26 shaders)
│   └── 3164500.db       # Schedule I (285 shaders)
└── dxmt-metal12/        # DXMT D3D12 (VKD3D pipeline)
    ├── 848450.db        # Subnautica Below Zero (1,166 shaders)
    ├── 1139900.db       # Ghostrunner (640 shaders)
    └── 1030300.db       # Hollow Knight: Silksong (14 shaders)
```

## How It Works

1. On game launch, `deploy_preset_cache()` checks for a matching preset DB
2. If no user cache exists, the preset is copied directly (zero-cost deploy)
3. If a user cache already exists, preset shaders are merged in via
   `INSERT OR IGNORE` (new shaders only, never overwrites)
4. DXMT loads the cache normally via `DXMT_SHADER_CACHE_PATH`
5. Any new shaders encountered during play are added to the user's local cache

## Adding New Presets

1. Play a game through MetalSharp with the target pipeline (M11/VKD3D)
2. Copy the shader cache from `~/.metalsharp/shader-cache/<engine>/<appid>/shaders_320.db`
3. Place it in the matching subdirectory as `<appid>.db`
4. Update this README with the game name and shader count

## Format

Each `.db` file is a SQLite database with a `cache_18` table containing
key/value BLOB pairs. Values are compiled metallib (MTLB) binaries targeting
`air64-apple-macosx15.0.0`, AIR v2.7, Metal Shading Language 3.2.
