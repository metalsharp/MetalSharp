# WineMetalGL on MetalSharp Wine 11.17
**Updated:** 2026-09-22

WineMetalGL is a global upgrade to the Wine 11.17 runtime's OpenGL support. It is part of the Wine runtime; game routing is unchanged.

## Port Boundary

WineMetalGL v0.1.0 is the source and provenance baseline:

- Release asset: `WineMetalGL-0.1.0.tar.zst`
- Release SHA-256: `1796266dc1fe43bb15050851d2a3f43d53c1ae961c7596b193cc9b47a465a553`
- Source: <https://github.com/metalsharp/WineMetalGL/releases/tag/v0.1.0>

The release artifacts are ARM64-host binaries; MetalSharp's Wine 11.17 has an x86_64 host, so the native sidecar is built as x86_64 and the Wine integration changes are ported onto the 11.17 tree:

- the generated OpenGL/WoW64 thunk generator uses safe guest pointer conversion;
- the x86_64 and i386 guest `opengl32.dll` surfaces are rebuilt from the 11.17 tree;
- `winemac.so` loads the x86_64 `metalsharp-opengl.dylib` sidecar;
- Wine's WGL/window/context implementation stays authoritative;
- OpenGL compatibility mode is the default, with the experimental GLSL/SPIR-V/Metal path enabled by `VKMT_OPENGL_METAL_EXPERIMENTAL=1`.

The host sidecar, Unix `opengl32.so`, and `winemac.so` all report x86_64; guest DLLs ship for x86_64 and i386/WoW64.

The runtime carries one canonical `ntdll.so` whose macOS x86_64 GSBASE behavior is gated per process: DXMT enables the Wine TEB/macOS TSD swap; D3DMetal and bare Wine keep the legacy behavior unless `WINE_MACOS_GSBASE_SWAP=on` is set. The launcher validates the canonical file.

## Acceptance Probes

Validated without launching games:

- x86_64 `opengl_runtime.exe`: WGL context, OpenGL 2.1 compatibility path, FBO, clear/readback;
- i386/WoW64 `opengl_runtime.exe`: the same bounded surface;
- x86_64 and i386 `opengl_runtime_probe.exe`: GLSL 1.20 compile/link/draw/readback;
- optional GLSL 3.30/4.50 probes under `VKMT_OPENGL_METAL_EXPERIMENTAL=1`.

Probe sources and expected success markers come from the WineMetalGL release. Game launch validation is a separate gate.

## Build

`tools/bundles/build-winemetalgl-x86.sh OUTPUT_DIR` downloads and verifies the pinned release source, patches its host-architecture selector to x86_64, and builds the sidecar. The ported Wine 11.17 tree then rebuilds `dlls/opengl32` and `dlls/winemac.drv` before staging the five runtime artifacts recorded in `tools/bundles/wine-runtime-hashes.tsv`.
