# Elden Ring VKD3D validation

Validated on Apple M4 Pro with MetalSharp 0.61.0 and Wine 11.5 x86_64 through Rosetta. FEX is not used.

| Component | Source | Artifact SHA-256 |
|---|---|---|
| MoltenVK 1.4.2 | `TimDarcet/MoltenVK`, tag `metalsharp-elden-ring-20260903` | `7f64cf9270f104ac38d440efd197ce033be3aad7e1d42d552b03a4193b034534` |
| SPIRV-Cross | `TimDarcet/SPIRV-Cross`, tag `metalsharp-elden-ring-20260903` | commit `78944d885ba1478dc2773c61b3ce887044af606e` |
| vkd3d-proton | `TimDarcet/vkd3d-proton`, tag `metalsharp-elden-ring-20260903` | `d3d12.dll` `1d7bbf9e4362cc892897665745fb9c28f71bf42b02a1e4293b1f3d02455f8dea`; `d3d12core.dll` `d907d630aa55ecab6cf8cd08f7d472538fd6a3787db2e77145950d1c4ad971e0` |
| DXVK 3.0.2 | `TimDarcet/dxvk`, tag `metalsharp-elden-ring-20260903` | `dxgi.dll` `0a1117b5077247d153f19d96cbfd270923087ac3251d2133132795c0df95c230`; `d3d11.dll` `c21d28a3f061402bb30a6fbc51a7488e2b78013aa51e49e1c70dceacd66aced8` |
| MetalSharp backend | `TimDarcet/MetalSharp`, commit `6b26e052853deb4dfff6d32ed0d08ca40d02d2c5` | Rust implementation used for validation |

These hashes record the original validation stack; they are not the MetalSharp bundle manifest. MetalSharp retains the currently published VKD3D, DXVK, and MoltenVK artifacts and routes this configuration through `vkd3d`. M12 remains the isolated DXMT D3D12 route.

No shader replacement, shader skip, or draw drop was enabled. Validation covered cold and warm vkd3d caches, 80 captured tessellation shaders, world rendering, movement, and attacks. The logs contained no Metal shader compilation failure, primitive-restart warning, device loss, invalid resource, or page fault.

For Elden Ring on VKD3D, a narrowly scoped shim reports the performance-core logical CPU count to Wine. Before a direct launch, MetalSharp resets an orphaned Wine server only when no Wine client is running.
