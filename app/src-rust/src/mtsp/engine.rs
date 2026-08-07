use serde::Serialize;
use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PipelineId {
    Dxmt,
    M9,
    M10,
    M10_32,
    M11,
    M11_32,
    M12,
    M13,
    D3DMetal,
    M32,
    FnaArm64,
    Steam,
    MacSteam,
    WineBare,
}

#[derive(Debug, Clone, Serialize)]
pub struct DllDeploy {
    pub source_subpath: &'static str,
    pub filename: &'static str,
    pub dest_filename: Option<&'static str>,
}

#[derive(Debug, Clone, Serialize)]
pub struct EnvVar {
    pub key: &'static str,
    pub value: &'static str,
}

#[derive(Debug, Clone, Serialize)]
pub struct PipelineNode {
    pub id: PipelineId,
    pub name: &'static str,
    pub description: &'static str,
    pub backend: &'static str,
    pub graphics_backend: &'static str,
    pub experimental: bool,
    pub requires_wine: bool,
    pub wine_overrides: Option<&'static str>,
    pub dyld_paths: Vec<&'static str>,
    pub winedllpath_dirs: Vec<&'static str>,
    pub deploy_dlls: Vec<DllDeploy>,
    pub env_vars: Vec<EnvVar>,
    pub launch_args: Vec<&'static str>,
    pub alternatives: Vec<PipelineId>,
    pub shader_cache_subdir: Option<&'static str>,
}

impl PipelineNode {
    pub fn uses_winedllpath_routing(&self) -> bool {
        !self.winedllpath_dirs.is_empty()
    }
}

static PIPELINES: OnceLock<Vec<PipelineNode>> = OnceLock::new();
const DXMT_70_PERCENT_UPSCALE_CONFIG: &str = "d3d11.metalSpatialUpscaleFactor=1.43;d3d11.preferredMaxFrameRate=60";

/// Cache of the effective M12 node (vkd3d-proton by default, DXMT when
/// `m12Backend=dxmt`). Built once at first access from the config so the
/// backend is stable for the process lifetime.
static M12_EFFECTIVE_NODE: OnceLock<PipelineNode> = OnceLock::new();

/// The M12 pipeline node for the vkd3d-proton graphics stack:
/// D3D12 -> vkd3d-proton -> Vulkan -> MoltenVK -> Metal.
///
/// Deploys vkd3d-proton's `d3d12.dll` + `d3d12core.dll` plus DXVK's
/// `dxgi.dll` (vkd3d-proton ships no dxgi of its own) into the game dir,
/// routes D3D12/DXGI through the vkd3d-proton lane, and points the Vulkan
/// loader at the VKMT MoltenVK ICD.
fn m12_vkd3d_proton_node() -> PipelineNode {
    PipelineNode {
        id: PipelineId::M12,
        name: "M12",
        description: "D3D12 -> Metal via vkd3d-proton (Vulkan) + MoltenVK",
        backend: "vkd3d-proton",
        graphics_backend: "vkd3d-proton",
        experimental: false,
        requires_wine: true,
        wine_overrides: Some(
            "d3d12,d3d12core,dxgi=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
        ),
        dyld_paths: vec!["lib/moltenvk-vkmt", "lib/wine/x86_64-unix"],
        winedllpath_dirs: vec![],
        deploy_dlls: vec![
            DllDeploy {
                source_subpath: "lib/vkd3d-proton/x86_64-windows",
                filename: "d3d12.dll",
                dest_filename: None,
            },
            DllDeploy {
                source_subpath: "lib/vkd3d-proton/x86_64-windows",
                filename: "d3d12core.dll",
                dest_filename: None,
            },
            DllDeploy {
                source_subpath: "lib/dxvk/x86_64-windows",
                filename: "dxgi.dll",
                dest_filename: None,
            },
        ],
        env_vars: vec![
            EnvVar { key: "MVK_PRESENT_MODE", value: "1" },
            EnvVar { key: "VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT", value: "1" },
        ],
        launch_args: vec!["-windowed", "-ResX=1280", "-ResY=720", "-ForceRes"],
        alternatives: vec![PipelineId::M11, PipelineId::M10, PipelineId::M9, PipelineId::Steam, PipelineId::MacSteam],
        shader_cache_subdir: Some("m12"),
    }
}

pub fn pipelines() -> &'static Vec<PipelineNode> {
    PIPELINES.get_or_init(|| {
        vec![
            PipelineNode {
                id: PipelineId::Dxmt,
                name: "DXMT",
                description: "Auto-selected D3D9/D3D10/D3D11/D3D12 -> Metal via unified DXMT runtime",
                backend: "dxmt",
                graphics_backend: "dxmt",
                experimental: false,
                requires_wine: true,
                wine_overrides: None,
                dyld_paths: vec![],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![],
                launch_args: vec![],
                alternatives: vec![PipelineId::M12, PipelineId::M11, PipelineId::M10, PipelineId::M9],
                shader_cache_subdir: None,
            },
            PipelineNode {
                id: PipelineId::M12,
                name: "M12",
                description: "D3D12 -> Metal via vkd3d-proton (MoltenVK)",
                backend: "vkd3d-proton",
                graphics_backend: "vkd3d-proton",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some(
                    "d3d12,d3d12core,dxgi=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
                ),
                dyld_paths: vec!["lib/moltenvk-vkmt", "lib/wine/x86_64-unix"],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![
                    DllDeploy {
                        source_subpath: "lib/vkd3d-proton/x86_64-windows",
                        filename: "d3d12.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/vkd3d-proton/x86_64-windows",
                        filename: "d3d12core.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxvk/x86_64-windows",
                        filename: "dxgi.dll",
                        dest_filename: None,
                    },
                ],
                env_vars: vec![
                    EnvVar { key: "MVK_PRESENT_MODE", value: "1" },
                    EnvVar { key: "VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT", value: "1" },
                ],
                launch_args: vec!["-windowed", "-ResX=1280", "-ResY=720", "-ForceRes"],
                alternatives: vec![
                    PipelineId::M11,
                    PipelineId::M10,
                    PipelineId::M9,
                    PipelineId::Steam,
                    PipelineId::MacSteam,
                ],
                shader_cache_subdir: Some("m12"),
            },
            PipelineNode {
                id: PipelineId::M11,
                name: "M11",
                description: "D3D11 -> Metal via DXMT",
                backend: "dxmt",
                graphics_backend: "dxmt",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some("winemetal,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"),
                dyld_paths: vec!["lib/wine/x86_64-unix", "lib/dxmt/x86_64-unix"],
                winedllpath_dirs: vec!["lib/dxmt/x86_64-windows", "lib/metalsharp/x86_64-windows"],
                deploy_dlls: vec![
                    DllDeploy { source_subpath: "lib/dxmt/x86_64-windows", filename: "d3d11.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/dxmt/x86_64-windows", filename: "dxgi.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "dxgi_dxmt.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "d3d10core.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "winemetal.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "nvapi64.dll",
                        dest_filename: None,
                    },
                    DllDeploy { source_subpath: "lib/dxmt/x86_64-windows", filename: "nvngx.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/metalsharp/x86_64-windows",
                        filename: "metalsharp_ntdll_hook.dll",
                        dest_filename: None,
                    },
                ],
                env_vars: vec![
                    EnvVar { key: "DXMT_METALFX_SPATIAL_SWAPCHAIN", value: "1" },
                    EnvVar { key: "DXMT_ASYNC_PIPELINE_COMPILE", value: "1" },
                    EnvVar { key: "DXMT_CONFIG", value: DXMT_70_PERCENT_UPSCALE_CONFIG },
                ],
                launch_args: vec![],
                alternatives: vec![
                    PipelineId::M12,
                    PipelineId::M10,
                    PipelineId::M9,
                    PipelineId::Steam,
                    PipelineId::MacSteam,
                    PipelineId::WineBare,
                ],
                shader_cache_subdir: Some("m11"),
            },
            PipelineNode {
                id: PipelineId::M11_32,
                name: "M11(32)",
                description: "D3D11 -> Metal via DXMT (32-bit / i386)",
                backend: "dxmt",
                graphics_backend: "dxmt",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some("d3d11,dxgi,winemetal=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"),
                dyld_paths: vec!["lib/wine/x86_64-unix", "lib/dxmt/i386-unix", "lib/wine"],
                winedllpath_dirs: vec!["lib/dxmt/i386-windows", "lib/wine/i386-windows", "lib/wine/x86_64-windows"],
                deploy_dlls: vec![
                    DllDeploy { source_subpath: "lib/dxmt/i386-windows", filename: "d3d11.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/dxmt/i386-windows", filename: "dxgi.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/dxmt/i386-windows",
                        filename: "dxgi_dxmt.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/i386-windows",
                        filename: "d3d10core.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/i386-windows",
                        filename: "winemetal.dll",
                        dest_filename: None,
                    },
                ],
                env_vars: vec![
                    EnvVar { key: "DXMT_WINEMETAL_UNIXLIB", value: "winemetal.so" },
                    EnvVar { key: "DXMT_METALFX_SPATIAL_SWAPCHAIN", value: "1" },
                    EnvVar { key: "DXMT_ASYNC_PIPELINE_COMPILE", value: "1" },
                    EnvVar { key: "DXMT_CONFIG", value: DXMT_70_PERCENT_UPSCALE_CONFIG },
                ],
                launch_args: vec![],
                alternatives: vec![
                    PipelineId::M11,
                    PipelineId::M12,
                    PipelineId::M10,
                    PipelineId::M9,
                    PipelineId::Steam,
                    PipelineId::MacSteam,
                    PipelineId::WineBare,
                ],
                shader_cache_subdir: Some("m11_32"),
            },
            PipelineNode {
                id: PipelineId::M10,
                name: "M10",
                description: "D3D10 -> Metal via DXMT",
                backend: "dxmt",
                graphics_backend: "dxmt",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some(
                    "winemetal,d3d10,d3d10_1,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
                ),
                dyld_paths: vec!["lib/wine/x86_64-unix", "lib/dxmt/x86_64-unix"],
                winedllpath_dirs: vec![
                    "lib/wine/x86_64-windows",
                    "lib/dxmt/x86_64-windows",
                    "lib/metalsharp/x86_64-windows",
                ],
                deploy_dlls: vec![
                    DllDeploy { source_subpath: "lib/wine/x86_64-windows", filename: "d3d10.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/wine/x86_64-windows",
                        filename: "d3d10_1.dll",
                        dest_filename: None,
                    },
                    DllDeploy { source_subpath: "lib/dxmt/x86_64-windows", filename: "d3d11.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/dxmt/x86_64-windows", filename: "dxgi.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "dxgi_dxmt.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "d3d10core.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "winemetal.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "nvapi64.dll",
                        dest_filename: None,
                    },
                    DllDeploy { source_subpath: "lib/dxmt/x86_64-windows", filename: "nvngx.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/metalsharp/x86_64-windows",
                        filename: "metalsharp_ntdll_hook.dll",
                        dest_filename: None,
                    },
                ],
                env_vars: vec![
                    EnvVar { key: "DXMT_METALFX_SPATIAL_SWAPCHAIN", value: "1" },
                    EnvVar { key: "DXMT_ASYNC_PIPELINE_COMPILE", value: "1" },
                    EnvVar { key: "DXMT_CONFIG", value: DXMT_70_PERCENT_UPSCALE_CONFIG },
                ],
                launch_args: vec![],
                alternatives: vec![
                    PipelineId::M11,
                    PipelineId::M9,
                    PipelineId::Steam,
                    PipelineId::MacSteam,
                    PipelineId::WineBare,
                ],
                shader_cache_subdir: Some("m10"),
            },
            PipelineNode {
                id: PipelineId::M10_32,
                name: "M10(32)",
                description: "D3D10 -> Metal via DXMT (32-bit / i386)",
                backend: "dxmt",
                graphics_backend: "dxmt",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some(
                    "d3d10,d3d10_1,d3d10core,d3d11,dxgi,winemetal=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
                ),
                dyld_paths: vec!["lib/wine/x86_64-unix", "lib/dxmt/i386-unix", "lib/wine"],
                winedllpath_dirs: vec!["lib/wine/i386-windows", "lib/dxmt/i386-windows", "lib/wine/x86_64-windows"],
                deploy_dlls: vec![
                    DllDeploy { source_subpath: "lib/wine/i386-windows", filename: "d3d10.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/wine/i386-windows", filename: "d3d10_1.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/dxmt/i386-windows", filename: "d3d11.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/dxmt/i386-windows", filename: "dxgi.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/dxmt/i386-windows",
                        filename: "dxgi_dxmt.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/i386-windows",
                        filename: "d3d10core.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/i386-windows",
                        filename: "winemetal.dll",
                        dest_filename: None,
                    },
                ],
                env_vars: vec![
                    EnvVar { key: "DXMT_WINEMETAL_UNIXLIB", value: "winemetal.so" },
                    EnvVar { key: "DXMT_METALFX_SPATIAL_SWAPCHAIN", value: "1" },
                    EnvVar { key: "DXMT_ASYNC_PIPELINE_COMPILE", value: "1" },
                    EnvVar { key: "DXMT_CONFIG", value: DXMT_70_PERCENT_UPSCALE_CONFIG },
                ],
                launch_args: vec![],
                alternatives: vec![
                    PipelineId::M10,
                    PipelineId::M11_32,
                    PipelineId::M11,
                    PipelineId::M9,
                    PipelineId::Steam,
                    PipelineId::MacSteam,
                    PipelineId::WineBare,
                ],
                shader_cache_subdir: Some("m10_32"),
            },
            PipelineNode {
                id: PipelineId::M9,
                name: "M9",
                description: "D3D9 -> Metal via DXMT launch family",
                backend: "dxmt",
                graphics_backend: "dxmt",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some("d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"),
                dyld_paths: vec!["lib/wine/x86_64-unix", "lib/dxmt/x86_64-unix"],
                winedllpath_dirs: vec![
                    "lib/wine/x86_64-windows",
                    "lib/wine/i386-windows",
                    "lib/dxmt/x86_64-windows",
                    "lib/metalsharp/x86_64-windows",
                ],
                deploy_dlls: vec![
                    DllDeploy { source_subpath: "lib/wine/x86_64-windows", filename: "d3d9.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/wine/i386-windows", filename: "d3d9.dll", dest_filename: None },
                    DllDeploy { source_subpath: "lib/wine/i386-windows", filename: "dxgi.dll", dest_filename: None },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "nvapi64.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/metalsharp/x86_64-windows",
                        filename: "metalsharp_ntdll_hook.dll",
                        dest_filename: None,
                    },
                ],
                env_vars: vec![
                    EnvVar { key: "DXMT_METALFX_SPATIAL_SWAPCHAIN", value: "1" },
                    EnvVar { key: "DXMT_ASYNC_PIPELINE_COMPILE", value: "1" },
                    EnvVar { key: "DXMT_CONFIG", value: DXMT_70_PERCENT_UPSCALE_CONFIG },
                ],
                launch_args: vec![],
                alternatives: vec![PipelineId::M11, PipelineId::M10, PipelineId::Steam, PipelineId::MacSteam],
                shader_cache_subdir: Some("m9"),
            },
            PipelineNode {
                id: PipelineId::M13,
                name: "M13",
                description: "D3D11/D3D12 via Apple Game Porting Toolkit (D3DMetal)",
                backend: "gptk",
                graphics_backend: "gptk",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some(
                    "d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
                ),
                dyld_paths: vec!["lib/wine/x86_64-unix"],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![],
                launch_args: vec![],
                alternatives: vec![PipelineId::M12, PipelineId::M11, PipelineId::Steam],
                shader_cache_subdir: Some("m13"),
            },
            PipelineNode {
                id: PipelineId::D3DMetal,
                name: "D3DMetal",
                description: "D3D11/D3D12 via Apple D3DMetal 4.0 (GPTK Wine)",
                backend: "d3dmetal",
                graphics_backend: "d3dmetal",
                experimental: true,
                requires_wine: false,
                wine_overrides: Some(
                    "d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
                ),
                dyld_paths: vec![],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![],
                launch_args: vec![],
                alternatives: vec![PipelineId::M12, PipelineId::M11, PipelineId::M13],
                shader_cache_subdir: Some("d3dmetal"),
            },
            PipelineNode {
                id: PipelineId::M32,
                name: "M32",
                description: "32-bit Wine fallback",
                backend: "wine32",
                graphics_backend: "wine",
                experimental: false,
                requires_wine: true,
                wine_overrides: None,
                dyld_paths: vec!["lib/wine/x86_64-unix"],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![],
                launch_args: vec![],
                alternatives: vec![PipelineId::M9, PipelineId::M11, PipelineId::Steam, PipelineId::MacSteam],
                shader_cache_subdir: Some("m32"),
            },
            PipelineNode {
                id: PipelineId::FnaArm64,
                name: "Mono/FNA",
                description: "Windows XNA/FNA via MetalSharp Mono runtime",
                backend: "mono",
                graphics_backend: "native",
                experimental: false,
                requires_wine: false,
                wine_overrides: None,
                dyld_paths: vec![],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![EnvVar { key: "METAL_DEVICE_WRAPPER_TYPE", value: "0" }],
                launch_args: vec![],
                alternatives: vec![PipelineId::MacSteam, PipelineId::Steam],
                shader_cache_subdir: Some("fna-arm64"),
            },
            PipelineNode {
                id: PipelineId::Steam,
                name: "Steam",
                description: "Wine Steam",
                backend: "wine-steam",
                graphics_backend: "wine",
                experimental: false,
                requires_wine: false,
                wine_overrides: None,
                dyld_paths: vec![],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![],
                launch_args: vec![],
                alternatives: vec![PipelineId::MacSteam, PipelineId::M11, PipelineId::WineBare],
                shader_cache_subdir: Some("steam-wine"),
            },
            PipelineNode {
                id: PipelineId::MacSteam,
                name: "MacOS Steam",
                description: "Native macOS Steam",
                backend: "macos-steam",
                graphics_backend: "native",
                experimental: false,
                requires_wine: false,
                wine_overrides: None,
                dyld_paths: vec![],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![],
                launch_args: vec![],
                alternatives: vec![PipelineId::Steam, PipelineId::FnaArm64, PipelineId::M11],
                shader_cache_subdir: Some("steam-native"),
            },
            PipelineNode {
                id: PipelineId::WineBare,
                name: "Wine",
                description: "Plain Wine (Custom Library)",
                backend: "wine",
                graphics_backend: "wine",
                experimental: false,
                requires_wine: true,
                wine_overrides: None,
                dyld_paths: vec!["lib/wine/x86_64-unix"],
                winedllpath_dirs: vec![],
                deploy_dlls: vec![],
                env_vars: vec![],
                launch_args: vec![],
                alternatives: vec![],
                shader_cache_subdir: Some("wine-bare"),
            },
        ]
    })
}

pub fn get_pipeline(id: PipelineId) -> &'static PipelineNode {
    if id == PipelineId::M12 {
        return m12_effective_node();
    }
    pipelines().iter().find(|p| p.id == id).expect("pipeline not found")
}

/// The M12 node selected by the `m12Backend` config: vkd3d-proton by
/// default, legacy DXMT when the user opted back. Cached once per process.
pub fn m12_effective_node() -> &'static PipelineNode {
    M12_EFFECTIVE_NODE.get_or_init(|| {
        if crate::launch::m12_backend_mode() == "dxmt" {
            pipelines().iter().find(|p| p.id == PipelineId::M12).expect("M12 pipeline not found").clone()
        } else {
            m12_vkd3d_proton_node()
        }
    })
}

impl PipelineId {
    pub fn is_dxmt_family(self) -> bool {
        matches!(
            self,
            PipelineId::Dxmt
                | PipelineId::M9
                | PipelineId::M10
                | PipelineId::M10_32
                | PipelineId::M11
                | PipelineId::M11_32
                | PipelineId::M12
        )
    }

    pub fn is_user_selectable(self) -> bool {
        matches!(
            self,
            PipelineId::M12
                | PipelineId::D3DMetal
                | PipelineId::M11
                | PipelineId::M11_32
                | PipelineId::M10
                | PipelineId::M10_32
                | PipelineId::M9
                | PipelineId::FnaArm64
        )
    }

    pub fn user_selectable_id(self) -> Option<&'static str> {
        match self {
            PipelineId::M12 => Some("m12"),
            PipelineId::D3DMetal => Some("d3dmetal"),
            PipelineId::M11 => Some("m11"),
            PipelineId::M11_32 => Some("m11_32"),
            PipelineId::M10 => Some("m10"),
            PipelineId::M10_32 => Some("m10_32"),
            PipelineId::M9 => Some("m9"),
            PipelineId::FnaArm64 => Some("fna_arm64"),
            _ => None,
        }
    }

    pub fn user_selectable_name(self) -> Option<&'static str> {
        match self {
            PipelineId::M12 => Some("M12"),
            PipelineId::D3DMetal => Some("D3DMetal"),
            PipelineId::M11 => Some("M11"),
            PipelineId::M11_32 => Some("M11(32)"),
            PipelineId::M10 => Some("M10"),
            PipelineId::M10_32 => Some("M10(32)"),
            PipelineId::M9 => Some("M9"),
            PipelineId::FnaArm64 => Some("Mono/FNA"),
            _ => None,
        }
    }

    pub fn from_legacy_method(method: &str) -> Option<PipelineId> {
        match method.trim().to_ascii_lowercase().as_str() {
            "dxmt" => Some(PipelineId::Dxmt),
            "dxmt_metal" | "steam_d3dmetal_perf" | "steam_metalfx" => Some(PipelineId::M11),
            "dxmt_metal12" => Some(PipelineId::M12),
            "d3d9_metal" => Some(PipelineId::M9),
            "wined3d_32" => Some(PipelineId::M32),
            "metalsharp_wine" => Some(PipelineId::WineBare),
            "steam" => Some(PipelineId::Steam),
            "macos_steam" | "mac_steam" | "native_steam" => Some(PipelineId::MacSteam),
            "xna_fna_arm64" | "xna_fna_x86" | "xna_fna" | "fna_mono_xna" | "mono_fna_xna" => Some(PipelineId::FnaArm64),
            _ => None,
        }
    }

    pub fn from_str_flexible(s: &str) -> Option<PipelineId> {
        let normalized = s.trim().to_ascii_lowercase().replace('-', "_");
        if let Some(p) = Self::from_legacy_method(&normalized) {
            return Some(p);
        }
        match normalized.as_str() {
            "dxmt" | "auto_dxmt" | "metalsharp_dxmt" => Some(PipelineId::Dxmt),
            "m11" | "d3d11" | "dx11" | "steam_d3dmetal_perf" | "steam_metalfx" => Some(PipelineId::M11),
            "m11_32" | "d3d11_32" | "dx11_32" => Some(PipelineId::M11_32),
            "m12" | "d3d12" | "dx12" => Some(PipelineId::M12),
            "m13" | "gptk" | "steam_d3dmetal" => Some(PipelineId::M13),
            "d3dmetal" | "d3dmetal_native" => Some(PipelineId::D3DMetal),
            "m10" | "d3d10" | "dx10" => Some(PipelineId::M10),
            "m10_32" | "d3d10_32" | "dx10_32" => Some(PipelineId::M10_32),
            "m9" | "d3d9" | "dx9" => Some(PipelineId::M9),
            "m32" | "m32_w" => Some(PipelineId::M32),
            "fna_arm64" | "fna_x86" | "mono_generic" | "fna_mono_xna" | "mono_fna_xna" => Some(PipelineId::FnaArm64),
            "steam" | "wine_steam" => Some(PipelineId::Steam),
            "macos_steam" | "mac_steam" | "native_steam" => Some(PipelineId::MacSteam),
            "wine_bare" | "m64" => Some(PipelineId::WineBare),
            _ => None,
        }
    }

    pub fn to_legacy_method(self) -> &'static str {
        match self {
            PipelineId::Dxmt
            | PipelineId::M9
            | PipelineId::M10
            | PipelineId::M10_32
            | PipelineId::M11
            | PipelineId::M11_32
            | PipelineId::M12 => "dxmt",
            PipelineId::M13 => "gptk_d3dmetal",
            PipelineId::D3DMetal => "d3dmetal",
            PipelineId::M32 => "wined3d_32",
            PipelineId::FnaArm64 => "xna_fna_arm64",
            PipelineId::Steam => "steam",
            PipelineId::MacSteam => "macos_steam",
            PipelineId::WineBare => "metalsharp_wine",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dxmt_is_primary_user_facing_pipeline() {
        let pipeline_list = pipelines();
        assert_eq!(pipeline_list.first().map(|p| p.id), Some(PipelineId::Dxmt));

        let dxmt = get_pipeline(PipelineId::Dxmt);
        assert_eq!(dxmt.name, "DXMT");
        assert_eq!(dxmt.backend, "dxmt");
        assert_eq!(dxmt.graphics_backend, "dxmt");
        assert!(dxmt.alternatives.contains(&PipelineId::M12));
        assert!(dxmt.alternatives.contains(&PipelineId::M11));
        assert!(dxmt.alternatives.contains(&PipelineId::M10));
        assert!(dxmt.alternatives.contains(&PipelineId::M9));
    }

    #[test]
    fn m12_is_primary_d3d12_vkd3d_profile() {
        let m12 = get_pipeline(PipelineId::M12);
        assert!(!m12.experimental);
        assert_eq!(m12.backend, "vkd3d-proton");
        assert!(m12.launch_args.contains(&"-windowed"));
        assert!(m12.deploy_dlls.iter().any(|dll| dll.filename == "d3d12.dll"));
        assert!(m12.deploy_dlls.iter().any(|dll| dll.filename == "d3d12core.dll"));
        assert!(m12.deploy_dlls.iter().any(|dll| dll.filename == "dxgi.dll"));
        assert_eq!(m12.shader_cache_subdir, Some("m12"));
    }

    #[test]
    fn m12_vkd3d_proton_uses_isolated_lanes() {
        let m12 = get_pipeline(PipelineId::M12);

        for required in ["lib/moltenvk-vkmt", "lib/wine/x86_64-unix"] {
            assert!(m12.dyld_paths.contains(&required));
        }
        assert!(!m12.dyld_paths.contains(&"lib/vkd3d-proton/x86_64-unix"));
        assert!(!m12.dyld_paths.contains(&"lib/dxmt/x86_64-unix"));
        assert!(!m12.dyld_paths.contains(&"lib/dxmt_m12/x86_64-unix"));
        assert!(m12.winedllpath_dirs.is_empty());
        assert!(!m12.winedllpath_dirs.contains(&"lib/dxmt/x86_64-windows"));
        assert!(!m12.winedllpath_dirs.contains(&"lib/dxmt_m12/x86_64-windows"));

        // Deploys vkd3d-proton d3d12/d3d12core + DXVK dxgi; never DXMT.
        let m12_dlls: std::collections::HashSet<_> =
            m12.deploy_dlls.iter().map(|dll| (dll.source_subpath, dll.filename)).collect();
        for required in [
            ("lib/vkd3d-proton/x86_64-windows", "d3d12.dll"),
            ("lib/vkd3d-proton/x86_64-windows", "d3d12core.dll"),
            ("lib/dxvk/x86_64-windows", "dxgi.dll"),
        ] {
            assert!(m12_dlls.contains(&required), "M12 missing vkd3d DLL {:?}", required);
        }
        assert!(!m12.deploy_dlls.iter().any(|dll| dll.source_subpath.starts_with("lib/dxmt")));
        assert!(!m12.deploy_dlls.iter().any(|dll| dll.filename == "winemetal.dll"));
        assert!(!m12.deploy_dlls.iter().any(|dll| dll.filename == "dxgi_dxmt.dll"));
        assert!(!m12.deploy_dlls.iter().any(|dll| dll.filename == "metalsharp_ntdll_hook.dll"));

        let m12_env: std::collections::HashSet<_> = m12.env_vars.iter().map(|env| env.key).collect();
        assert!(m12_env.contains("MVK_PRESENT_MODE"));
        assert!(m12_env.contains("VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT"));
        assert!(!m12_env.iter().any(|key| key.starts_with("DXMT_")));
        let m12_env_values: std::collections::HashMap<_, _> =
            m12.env_vars.iter().map(|env| (env.key, env.value)).collect();
        assert_eq!(m12_env_values.get("MVK_PRESENT_MODE"), Some(&"1"));
        assert_eq!(m12_env_values.get("VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT"), Some(&"1"));

        assert_eq!(
            m12.wine_overrides,
            Some("d3d12,d3d12core,dxgi=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
        );
        assert!(m12.alternatives.contains(&PipelineId::M11));
    }

    #[test]
    fn m12_pipelines_list_has_no_legacy_dxmt_fallback() {
        // M12 is vkd3d-proton only (no DXMT fallback): the pipelines() list
        // must expose exactly the vkd3d-proton node shape.
        let node = pipelines().iter().find(|p| p.id == PipelineId::M12).expect("M12 in list");
        assert_eq!(node.backend, "vkd3d-proton");
        assert_eq!(node.graphics_backend, "vkd3d-proton");
        assert!(!node.winedllpath_dirs.contains(&"lib/dxmt_m12/x86_64-windows"));
        assert!(!node.deploy_dlls.iter().any(|dll| dll.filename == "winemetal.dll"));
        assert!(!node.deploy_dlls.iter().any(|dll| dll.filename == "dxgi_dxmt.dll"));
    }

    #[test]
    fn m11_m10_m9_stay_on_legacy_dxmt_surface() {
        let m11 = get_pipeline(PipelineId::M11);
        assert_eq!(
            m11.wine_overrides,
            Some("winemetal,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
        );

        let m10 = get_pipeline(PipelineId::M10);
        assert_eq!(
            m10.wine_overrides,
            Some("winemetal,d3d10,d3d10_1,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
        );

        let m9 = get_pipeline(PipelineId::M9);
        assert_eq!(m9.wine_overrides, Some("d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"));

        for pipeline in [m11, m10, m9] {
            assert!(
                pipeline.dyld_paths.iter().all(|path| !path.contains("dxmt_m12")),
                "{} should not load the isolated M12 Unix surface",
                pipeline.name
            );
            assert!(
                pipeline.winedllpath_dirs.iter().all(|path| !path.contains("dxmt_m12")),
                "{} should not route PE DLLs through the isolated M12 surface",
                pipeline.name
            );
            assert!(
                pipeline.deploy_dlls.iter().all(|dll| !dll.source_subpath.contains("dxmt_m12")),
                "{} should not deploy isolated M12 DLLs",
                pipeline.name
            );
        }
    }

    #[test]
    fn m10_is_stable_dxmt_d3d10_pipeline() {
        let m10 = get_pipeline(PipelineId::M10);

        assert_eq!(m10.name, "M10");
        assert_eq!(m10.description, "D3D10 -> Metal via DXMT");
        assert_eq!(m10.backend, "dxmt");
        assert!(!m10.experimental);
        assert!(m10.launch_args.is_empty());
        assert_eq!(m10.shader_cache_subdir, Some("m10"));
    }

    #[test]
    fn m10_matches_shared_dxmt_handoff_shape() {
        let m10 = get_pipeline(PipelineId::M10);
        let m11 = get_pipeline(PipelineId::M11);

        assert_eq!(m10.dyld_paths, m11.dyld_paths);
        assert_eq!(
            m10.wine_overrides,
            Some("winemetal,d3d10,d3d10_1,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
        );

        let m10_dlls: std::collections::HashSet<_> = m10.deploy_dlls.iter().map(|dll| dll.filename).collect();
        for required in
            ["d3d10.dll", "d3d10_1.dll", "d3d11.dll", "dxgi.dll", "dxgi_dxmt.dll", "d3d10core.dll", "winemetal.dll"]
        {
            assert!(m10_dlls.contains(required), "M10 missing {}", required);
        }
        assert!(!m10_dlls.contains("d3d12.dll"));

        let m10_env: std::collections::HashSet<_> = m10.env_vars.iter().map(|env| env.key).collect();
        assert!(m10_env.contains("DXMT_ASYNC_PIPELINE_COMPILE"));
        assert!(m10_env.contains("DXMT_METALFX_SPATIAL_SWAPCHAIN"));

        assert!(m10.alternatives.contains(&PipelineId::M11));
        assert!(m10.alternatives.contains(&PipelineId::M9));
        assert!(m10.alternatives.contains(&PipelineId::WineBare));
    }

    #[test]
    fn m11_32_and_m10_32_deploy_from_i386_dxmt_lane() {
        let m11_32 = get_pipeline(PipelineId::M11_32);
        let m10_32 = get_pipeline(PipelineId::M10_32);

        // both are public, non-experimental, dxmt-backed 32-bit options
        for node in [m11_32, m10_32] {
            assert!(node.id.is_user_selectable(), "{:?} should be user-selectable", node.id);
            assert!(!node.experimental);
            assert_eq!(node.backend, "dxmt");
            assert_eq!(node.graphics_backend, "dxmt");

            // every deployed DLL comes from an i386-windows lane, never x86_64-windows
            for dll in &node.deploy_dlls {
                assert!(
                    dll.source_subpath.contains("i386-windows"),
                    "{:?} deploys {} from non-i386 lane {}",
                    node.id,
                    dll.filename,
                    dll.source_subpath
                );
            }
            // must carry the unix sidecar hint validated for 32-bit DXMT launches
            let env_keys: std::collections::HashSet<_> = node.env_vars.iter().map(|e| e.key).collect();
            assert!(env_keys.contains("DXMT_WINEMETAL_UNIXLIB"));
            // dyld path must include the i386-unix lane
            assert!(node.dyld_paths.iter().any(|p| p.contains("i386-unix")));
        }

        // M11(32) ships the D3D11 handoff set; no d3d12 / no nvapi
        let m11_32_dlls: std::collections::HashSet<_> = m11_32.deploy_dlls.iter().map(|d| d.filename).collect();
        for required in ["d3d11.dll", "dxgi.dll", "dxgi_dxmt.dll", "d3d10core.dll", "winemetal.dll"] {
            assert!(m11_32_dlls.contains(required), "M11(32) missing {}", required);
        }
        for forbidden in ["d3d12.dll", "nvapi64.dll", "nvngx.dll"] {
            assert!(!m11_32_dlls.contains(forbidden), "M11(32) must not ship {}", forbidden);
        }

        // M10(32) additionally ships the Wine d3d10/d3d10_1 public entrypoints
        let m10_32_dlls: std::collections::HashSet<_> = m10_32.deploy_dlls.iter().map(|d| d.filename).collect();
        for required in ["d3d10.dll", "d3d10_1.dll", "d3d11.dll", "dxgi.dll", "d3d10core.dll", "winemetal.dll"] {
            assert!(m10_32_dlls.contains(required), "M10(32) missing {}", required);
        }
    }

    #[test]
    fn m9_is_dxmt_family_without_dxvk_or_wined3d_fallbacks() {
        let m9 = get_pipeline(PipelineId::M9);

        assert_eq!(m9.name, "M9");
        assert_eq!(m9.description, "D3D9 -> Metal via DXMT launch family");
        assert_eq!(m9.backend, "dxmt");
        assert!(!m9.experimental);
        assert!(m9.launch_args.is_empty());
        assert_eq!(m9.shader_cache_subdir, Some("m9"));
        assert_eq!(m9.wine_overrides, Some("d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"));

        let m9_dlls: std::collections::HashSet<_> =
            m9.deploy_dlls.iter().map(|dll| (dll.source_subpath, dll.filename)).collect();
        assert!(m9_dlls.contains(&("lib/wine/x86_64-windows", "d3d9.dll")));
        assert!(m9_dlls.contains(&("lib/wine/i386-windows", "d3d9.dll")));
        assert!(m9_dlls.contains(&("lib/wine/i386-windows", "dxgi.dll")));
        assert!(m9_dlls.contains(&("lib/dxmt/x86_64-windows", "nvapi64.dll")));
        assert!(m9.deploy_dlls.iter().all(|dll| !dll.source_subpath.contains("dxvk")));
        assert!(m9.dyld_paths.contains(&"lib/dxmt/x86_64-unix"));

        let m9_env: std::collections::HashSet<_> = m9.env_vars.iter().map(|env| env.key).collect();
        assert!(m9_env.contains("DXMT_ASYNC_PIPELINE_COMPILE"));
        assert!(m9_env.contains("DXMT_METALFX_SPATIAL_SWAPCHAIN"));

        assert!(m9.alternatives.contains(&PipelineId::M11));
        assert!(m9.alternatives.contains(&PipelineId::M10));
        assert!(!m9.alternatives.contains(&PipelineId::M32));
        assert!(!m9.alternatives.contains(&PipelineId::WineBare));
    }

    #[test]
    fn legacy_dxvk_and_wined3d_m9_aliases_are_not_m9() {
        assert_eq!(PipelineId::from_legacy_method("dxvk_metal32"), None);
        assert_eq!(PipelineId::from_str_flexible("m9_gl"), None);
        assert_eq!(PipelineId::from_str_flexible("m32_vk"), None);
    }

    #[test]
    fn user_selectable_pipelines_are_the_public_bottle_options() {
        let selectable: Vec<_> = pipelines()
            .iter()
            .filter(|pipeline| pipeline.id.is_user_selectable())
            .map(|pipeline| pipeline.id)
            .collect();
        assert_eq!(
            selectable,
            vec![
                PipelineId::M12,
                PipelineId::M11,
                PipelineId::M11_32,
                PipelineId::M10,
                PipelineId::M10_32,
                PipelineId::M9,
                PipelineId::D3DMetal,
                PipelineId::FnaArm64
            ]
        );

        let labels: Vec<_> = selectable.iter().map(|pipeline| pipeline.user_selectable_name().unwrap()).collect();
        assert_eq!(labels, vec!["M12", "M11", "M11(32)", "M10", "M10(32)", "M9", "D3DMetal", "Mono/FNA"]);

        for hidden in [
            PipelineId::Dxmt,
            PipelineId::M13,
            PipelineId::M32,
            PipelineId::Steam,
            PipelineId::MacSteam,
            PipelineId::WineBare,
        ] {
            assert!(!hidden.is_user_selectable());
            assert_eq!(hidden.user_selectable_id(), None);
        }
    }

    #[test]
    fn launch_method_parsing_is_case_and_separator_tolerant() {
        assert_eq!(PipelineId::from_str_flexible("dxmt"), Some(PipelineId::Dxmt));
        assert_eq!(PipelineId::from_str_flexible(" M12 "), Some(PipelineId::M12));
        assert_eq!(PipelineId::from_str_flexible("d3d12"), Some(PipelineId::M12));
        assert_eq!(PipelineId::from_str_flexible("dx10"), Some(PipelineId::M10));
        assert_eq!(PipelineId::from_str_flexible("wine-steam"), Some(PipelineId::Steam));
        assert_eq!(PipelineId::from_legacy_method("DXMT_METAL"), Some(PipelineId::M11));
    }

    #[test]
    fn dxmt_family_serializes_to_canonical_launch_method() {
        for pipeline in [PipelineId::Dxmt, PipelineId::M9, PipelineId::M10, PipelineId::M11, PipelineId::M12] {
            assert_eq!(pipeline.to_legacy_method(), "dxmt");
            assert!(pipeline.is_dxmt_family());
        }
    }
}
