# Redistributable Runtime
**Updated:** 2026-09-22

Redistributables are bottle components. MetalSharp finds local assets in Steam Common Redistributables, Sharp Library installer bottles, and `~/.metalsharp/runtime/redist/`, then records installed, missing, or repair-needed state in the bottle.

## Component IDs

`vcrun2019`, `directx_jun2010`, `dotnet48`, `webview2`, `openal`, `xna`, `physx`, `wine-mono`, `gecko`, `corefonts`

Graphics route components (`d3d9`, `d3d10`, `d3d11`, `d3d12`, `dxgi`) verify from the MetalSharp runtime.

## Asset Discovery

Steam game bottles scan installs for `_CommonRedist`, `CommonRedist`, and `installscript.vdf` assets and infer required components:

| Detected payload | Component |
|---|---|
| VC redist | `vcrun2019` |
| DirectX payload or install script | `directx_jun2010` |
| .NET payload | `dotnet48` |
| WebView payload | `webview2` |
| OpenAL payload | `openal` |
| XNA payload or install script | `xna` |
| PhysX payload or install script | `physx` |

The install script parser infers only known redistributable families from obvious names such as `DXSETUP.exe`, `xnafx40_redist.msi`, `oalinst.exe`, or PhysX installers.

## Repair Sources

```text
~/.metalsharp/prefix-steam/drive_c/Program Files (x86)/Steam/steamapps/common/Steamworks Shared/_CommonRedist/
~/.metalsharp/bottles/*/installers/
~/.metalsharp/runtime/redist/
```

MSI redistributables launch through `msiexec /i`; EXE redistributables launch directly with silent arguments where known. XNA Framework 4.0 repairs also reuse a matching Sharp Library installer-bottle payload when `xnafx40_redist.msi` was already staged there. Every repair writes a per-bottle component log.

## Remaining Work

- Persist redist install receipts separately from heuristic file checks.
- Parse more Steam install script actions and conditions.
- Add end-to-end verification against Steamworks Common Redistributables on a real Wine Steam install.
