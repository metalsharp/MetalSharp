# WineMetalGL on MetalSharp Wine 11.17

MetalSharp's OpenGL integration is a global upgrade to the existing Wine 11.17 runtime. It is not a new launch pipeline and does not change DXMT, VKD3D, D3DMetal, bare Wine, or FNA routing.

## Port boundary

WineMetalGL v0.1.0 is the source and provenance baseline:

- Release asset: `WineMetalGL-0.1.0.tar.zst`
- Release SHA-256: `1796266dc1fe43bb15050851d2a3f43d53c1ae961c7596b193cc9b47a465a553`
- Source: <https://github.com/metalsharp/WineMetalGL/releases/tag/v0.1.0>

The accepted release artifacts are ARM64-host binaries and cannot be copied into MetalSharp's x86_64-host Wine 11.17 tree. MetalSharp therefore builds the native sidecar as x86_64 and ports the Wine integration changes onto the existing 11.17 source tree:

- the generated OpenGL/WoW64 thunk generator uses safe guest pointer conversion;
- the x86_64 and i386 guest `opengl32.dll` surfaces are rebuilt from the 11.17 tree;
- `winemac.so` loads the x86_64 `metalsharp-opengl.dylib` sidecar;
- the existing Wine WGL/window/context implementation remains authoritative;
- OpenGL compatibility mode is enabled by default, with the experimental GLSL/SPIR-V/Metal path opt-in through `VKMT_OPENGL_METAL_EXPERIMENTAL=1`.

Do not mix the ARM64-host release artifacts with the x86_64-host runtime. The host sidecar, Unix `opengl32.so`, and `winemac.so` must all report x86_64; the guest DLLs must be supplied for both x86_64 and i386/WoW64.

The runtime carries one canonical `ntdll.so`. Its macOS x86_64 GSBASE behavior is gated per process: DXMT enables the Wine TEB/macOS TSD swap, while D3DMetal and bare Wine retain the legacy behavior unless `WINE_MACOS_GSBASE_SWAP=on` is explicitly requested. The launcher validates the canonical file and no longer copies route-specific ntdll variants.

## Bounded acceptance probes

The port is validated without launching games:

- x86_64 `opengl_runtime.exe`: WGL context, OpenGL 2.1 compatibility path, FBO, clear/readback;
- i386/WoW64 `opengl_runtime.exe`: the same bounded surface;
- x86_64 and i386 `opengl_runtime_probe.exe`: GLSL 1.20 compile/link/draw/readback;
- optional GLSL 3.30/4.50 probes under `VKMT_OPENGL_METAL_EXPERIMENTAL=1`.

The probe sources and expected success markers are taken from the WineMetalGL release. Game launch validation is separate from this integration gate.

## Build

`tools/bundles/build-winemetalgl-x86.sh OUTPUT_DIR` downloads and verifies the pinned release source, patches its host-architecture selector to x86_64, and builds the sidecar. The ported Wine 11.17 tree must then rebuild `dlls/opengl32` and `dlls/winemac.drv` before staging the five runtime artifacts recorded in `tools/bundles/wine-runtime-hashes.tsv`.
