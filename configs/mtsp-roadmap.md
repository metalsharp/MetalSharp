# MTSP Engine Roadmap

## Overview

MTSP (Metal Translation & Shading Pipeline) replaces hardcoded per-game launch
logic with a unified, data-driven pipeline system. Adding a game means editing
TOML, not Rust code.

## Phase 0 — Core Engine (current)

- [x] PipelineId enum and declarative PipelineNode configs
- [x] PE import parser with D3D API auto-detection
- [x] TOML override rules (~70 appid mappings)
- [x] Resolver: TOML → managed/FNA eligibility → PE analysis → directory heuristics → fallback
- [x] Unified launcher with DLL deployment matrix
- [x] API endpoints: GET /mtsp/pipelines, POST /mtsp/prepare
- [x] Backward-compatible launch delegates
- [x] GPTK dependency removed
- [x] VKD3D complete Vulkan pipeline (D3D9/D3D10/D3D11/D3D12 via DXVK-macOS + vkd3d-proton)
- [x] Clippy clean, fmt pass, CI green

## Phase 1 — Frontend Integration

- [x] Public route selector UI in game library
- [x] Pipeline info panel with description and alternatives
- [x] Per-game bottle route override for public routes
- [ ] Experimental pipeline warnings

## Phase 1.5 — Runtime Bottles

- [x] Steam game bottle records (`steam_<appid>`) for launch preflight and runtime assets
- [x] Installer/Sharp Library bottle records for Windows installers and imported apps
- [x] Runtime Doctor surfaces for Steam cards, Sharp cards, and Runtime Bottles
- [x] Component/source policy reporting for Wine Mono, Gecko, .NET, VC runtime, DirectX, WebView2, and fonts

## Phase 2 — Advanced Features

- [ ] Pipeline benchmark mode (run frame test, compare pipelines)
- [ ] Auto-pipeline tuning based on GPU family (M1 vs M4)
- [ ] Community pipeline recommendations (shared TOML rules)
- [ ] Hot-reload rules TOML without restart

## Public Route Reference

| ID | Name | Backend | Notes |
|---|---|---|---|
| VKD3D | D3D12 → Metal | dxmt | Public D3D12 route; deploys D3D12/DXGI/DXIL/Agility sidecars when selected |
| M11 | D3D11 → Metal | dxmt | Public D3D11 route |
| M10 | D3D10 → Metal | dxmt | Public D3D10 route |
| VKD3D | D3D9/D3D10/D3D11/D3D12 → Metal | vkd3d-proton + DXVK-macOS | Complete Vulkan pipeline via MoltenVK |
| Mono/FNA | Windows XNA/FNA → native Mono | mono | Public FNA/XNA route; launcher picks ARM64 or x86_64 Mono internally |

## Internal Route IDs

| ID | Role |
|---|---|
| DXMT | Auto-router that selects VKD3D/DXMT/DXMT(32) from rules and PE evidence |
| M32 | 32-bit Wine fallback retained for diagnostics/legacy records |
| Steam | Windows Steam client handoff retained for bootstrap/diagnostics |
| MacOS Steam | Native Steam handoff retained for diagnostics/special cases |
| WineBare | Plain Wine fallback retained for installer/custom-app internals |
