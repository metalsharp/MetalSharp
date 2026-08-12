use serde::Serialize;
use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PipelineId {
    Dxmt,
    /// Unified public 32-bit/i386 D3D10/D3D11 DXMT route.
    Dxmt32,
    M9,
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

/// The M12 pipeline node for the vkd3d-proton graphics stack:
/// D3D12 -> vkd3d-proton -> Vulkan -> MoltenVK -> Metal.
///
/// Deploys vkd3d-proton's `d3d12.dll` + `d3d12core.dll` plus DXVK's
/// `dxgi.dll` (vkd3d-proton ships no dxgi of its own) into the game dir,
/// routes D3D12/DXGI through the vkd3d-proton lane, and points the Vulkan
/// loader at the VKMT MoltenVK ICD. M12 has no DXMT mapping: it is the
/// vkd3d-proton stack only.
pub fn pipelines() -> &'static Vec<PipelineNode> {
    PIPELINES.get_or_init(|| {
        vec![
            PipelineNode {
                id: PipelineId::Dxmt,
                name: "DXMT",
                description: "D3D10/D3D11 -> Metal via DXMT",
                backend: "dxmt",
                graphics_backend: "dxmt",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some(
                    "winemetal,d3d10,d3d10_1,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
                ),
                dyld_paths: vec!["lib/wine/x86_64-unix", "lib/dxmt/x86_64-unix"],
                winedllpath_dirs: vec!["lib/wine/x86_64-windows", "lib/dxmt/x86_64-windows"],
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
                        filename: "d3d10core.dll",
                        dest_filename: None,
                    },
                    DllDeploy {
                        source_subpath: "lib/dxmt/x86_64-windows",
                        filename: "winemetal.dll",
                        dest_filename: None,
                    },
                ],
                env_vars: vec![
                    EnvVar { key: "DXMT_METALFX_SPATIAL_SWAPCHAIN", value: "1" },
                    EnvVar { key: "DXMT_ASYNC_PIPELINE_COMPILE", value: "1" },
                    EnvVar { key: "DXMT_CONFIG", value: DXMT_70_PERCENT_UPSCALE_CONFIG },
                ],
                launch_args: vec![],
                alternatives: vec![PipelineId::M12, PipelineId::M9, PipelineId::Steam, PipelineId::MacSteam],
                shader_cache_subdir: Some("dxmt"),
            },
            PipelineNode {
                id: PipelineId::M12,
                name: "M12",
                description: "D3D12 -> Metal via vkd3d-proton (MoltenVK)",
                backend: "vkd3d-proton",
                graphics_backend: "vkd3d-proton",
                experimental: false,
                requires_wine: true,
                wine_overrides: Some("d3d12,d3d12core,dxgi,d3d11=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"),
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
                    DllDeploy { source_subpath: "lib/dxvk/x86_64-windows", filename: "dxgi.dll", dest_filename: None },
                ],
                env_vars: vec![
                    EnvVar { key: "MVK_PRESENT_MODE", value: "1" },
                    EnvVar { key: "VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT", value: "1" },
                    EnvVar { key: "MVK_CONFIG_FORCE_RETAINED_COMMAND_BUFFERS", value: "1" },
                ],
                launch_args: vec!["-windowed", "-ResX=1280", "-ResY=720", "-ForceRes"],
                alternatives: vec![PipelineId::Dxmt, PipelineId::M9, PipelineId::Steam, PipelineId::MacSteam],
                shader_cache_subdir: Some("m12"),
            },
            PipelineNode {
                id: PipelineId::Dxmt32,
                name: "DXMT(32)",
                description: "D3D10/D3D11 -> Metal via DXMT (32-bit / i386)",
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
                    PipelineId::Dxmt,
                    PipelineId::M9,
                    PipelineId::Steam,
                    PipelineId::MacSteam,
                    PipelineId::WineBare,
                ],
                shader_cache_subdir: Some("dxmt_32"),
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
                alternatives: vec![PipelineId::Dxmt, PipelineId::Steam, PipelineId::MacSteam],
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
                alternatives: vec![PipelineId::M12, PipelineId::Dxmt, PipelineId::Steam],
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
                alternatives: vec![PipelineId::M12, PipelineId::Dxmt, PipelineId::M13],
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
                alternatives: vec![PipelineId::M9, PipelineId::Dxmt, PipelineId::Steam, PipelineId::MacSteam],
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
                alternatives: vec![PipelineId::MacSteam, PipelineId::Dxmt, PipelineId::WineBare],
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
                alternatives: vec![PipelineId::Steam, PipelineId::FnaArm64, PipelineId::Dxmt],
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

/// The M12 node. M12 is the vkd3d-proton stack only (D3D12 -> vkd3d-proton
/// -> Vulkan -> MoltenVK); there is no DXMT-backed M12 mapping.
pub fn m12_effective_node() -> &'static PipelineNode {
    pipelines().iter().find(|p| p.id == PipelineId::M12).expect("M12 pipeline not found")
}

impl PipelineId {
    pub fn is_dxmt_family(self) -> bool {
        matches!(self, PipelineId::Dxmt | PipelineId::Dxmt32 | PipelineId::M9 | PipelineId::M12)
    }

    pub fn is_user_selectable(self) -> bool {
        matches!(
            self,
            PipelineId::Dxmt
                | PipelineId::Dxmt32
                | PipelineId::M12
                | PipelineId::D3DMetal
                | PipelineId::M9
                | PipelineId::FnaArm64
        )
    }

    pub fn user_selectable_id(self) -> Option<&'static str> {
        match self {
            PipelineId::Dxmt => Some("dxmt"),
            PipelineId::Dxmt32 => Some("dxmt_32"),
            PipelineId::M12 => Some("m12"),
            PipelineId::D3DMetal => Some("d3dmetal"),
            PipelineId::M9 => Some("m9"),
            PipelineId::FnaArm64 => Some("fna_arm64"),
            _ => None,
        }
    }

    pub fn user_selectable_name(self) -> Option<&'static str> {
        match self {
            PipelineId::Dxmt => Some("DXMT"),
            PipelineId::Dxmt32 => Some("DXMT(32)"),
            PipelineId::M12 => Some("M12"),
            PipelineId::D3DMetal => Some("D3DMetal"),
            PipelineId::M9 => Some("M9"),
            PipelineId::FnaArm64 => Some("Mono/FNA"),
            _ => None,
        }
    }

    pub fn from_legacy_method(method: &str) -> Option<PipelineId> {
        match method.trim().to_ascii_lowercase().as_str() {
            "dxmt" => Some(PipelineId::Dxmt),
            "dxmt_32" | "dxmt32" => Some(PipelineId::Dxmt32),
            "dxmt_metal" | "steam_d3dmetal_perf" | "steam_metalfx" => Some(PipelineId::Dxmt),
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
            "d3d11" | "dx11" | "d3d10" | "dx10" | "steam_d3dmetal_perf" | "steam_metalfx" => Some(PipelineId::Dxmt),
            "dxmt_32" | "dxmt32" | "d3d11_32" | "dx11_32" | "d3d10_32" | "dx10_32" => Some(PipelineId::Dxmt32),
            "m12" | "d3d12" | "dx12" => Some(PipelineId::M12),
            "m13" | "gptk" | "steam_d3dmetal" => Some(PipelineId::M13),
            "d3dmetal" | "d3dmetal_native" => Some(PipelineId::D3DMetal),
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
            PipelineId::Dxmt | PipelineId::Dxmt32 | PipelineId::M9 | PipelineId::M12 => "dxmt",
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
        assert_eq!(m12.dyld_paths.first(), Some(&"lib/moltenvk-vkmt"));
        assert!(!m12.dyld_paths.contains(&"lib/vkd3d-proton/x86_64-unix"));
        assert!(!m12.dyld_paths.contains(&"lib/dxmt/x86_64-unix"));
        assert!(m12.winedllpath_dirs.is_empty());

        // Deploys exactly the vkd3d-proton D3D12 pair plus DXVK dxgi; never
        // DXMT, never a d3d11 handoff, never a MoltenVK PE stub.
        let m12_dlls: std::collections::HashSet<_> =
            m12.deploy_dlls.iter().map(|dll| (dll.source_subpath, dll.filename)).collect();
        assert_eq!(
            m12_dlls,
            [
                ("lib/vkd3d-proton/x86_64-windows", "d3d12.dll"),
                ("lib/vkd3d-proton/x86_64-windows", "d3d12core.dll"),
                ("lib/dxvk/x86_64-windows", "dxgi.dll"),
            ]
            .into_iter()
            .collect(),
            "M12 deploy set must be exactly vkd3d-proton d3d12/d3d12core + DXVK dxgi"
        );
        assert!(!m12.deploy_dlls.iter().any(|dll| dll.source_subpath.starts_with("lib/dxmt")));
        assert!(!m12.deploy_dlls.iter().any(|dll| dll.filename == "d3d11.dll"));
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
            Some("d3d12,d3d12core,dxgi,d3d11=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
        );
        assert!(m12.alternatives.contains(&PipelineId::Dxmt));
    }

    #[test]
    fn m12_pipelines_list_is_vkd3d_proton_only() {
        // M12 is vkd3d-proton only: the pipelines() list must expose exactly
        // the vkd3d-proton node shape with no DXMT surface.
        let node = pipelines().iter().find(|p| p.id == PipelineId::M12).expect("M12 in list");
        assert_eq!(node.backend, "vkd3d-proton");
        assert_eq!(node.graphics_backend, "vkd3d-proton");
        assert!(node.winedllpath_dirs.is_empty());
        assert!(node.dyld_paths.first() == Some(&"lib/moltenvk-vkmt"));
        assert!(!node.deploy_dlls.iter().any(|dll| dll.filename == "winemetal.dll"));
        assert!(!node.deploy_dlls.iter().any(|dll| dll.filename == "dxgi_dxmt.dll"));
        assert!(!node.deploy_dlls.iter().any(|dll| dll.filename == "d3d11.dll"));
    }

    #[test]
    fn dxmt_and_m9_stay_on_legacy_dxmt_surface() {
        let dxmt = get_pipeline(PipelineId::Dxmt);
        assert_eq!(
            dxmt.wine_overrides,
            Some("winemetal,d3d10,d3d10_1,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
        );

        let m9 = get_pipeline(PipelineId::M9);
        assert_eq!(m9.wine_overrides, Some("d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d"));

        for pipeline in [dxmt, get_pipeline(PipelineId::Dxmt32), m9] {
            assert!(
                pipeline.dyld_paths.iter().all(|path| !path.contains("moltenvk-vkmt")),
                "{} should not load the M12 MoltenVK surface",
                pipeline.name
            );
            assert!(
                pipeline.deploy_dlls.iter().all(|dll| !dll.source_subpath.starts_with("lib/vkd3d-proton")),
                "{} should not deploy vkd3d-proton DLLs",
                pipeline.name
            );
        }
    }

    #[test]
    fn dxmt_is_stable_unified_d3d10_d3d11_pipeline() {
        let dxmt = get_pipeline(PipelineId::Dxmt);

        assert_eq!(dxmt.name, "DXMT");
        assert_eq!(dxmt.description, "D3D10/D3D11 -> Metal via DXMT");
        assert_eq!(dxmt.backend, "dxmt");
        assert!(!dxmt.experimental);
        assert!(dxmt.launch_args.is_empty());
        assert_eq!(dxmt.shader_cache_subdir, Some("dxmt"));
    }

    #[test]
    fn dxmt_deploys_exactly_six_x64_route_dlls() {
        let dxmt = get_pipeline(PipelineId::Dxmt);

        let expected: std::collections::HashSet<_> = [
            ("lib/wine/x86_64-windows", "d3d10.dll"),
            ("lib/wine/x86_64-windows", "d3d10_1.dll"),
            ("lib/dxmt/x86_64-windows", "d3d11.dll"),
            ("lib/dxmt/x86_64-windows", "dxgi.dll"),
            ("lib/dxmt/x86_64-windows", "d3d10core.dll"),
            ("lib/dxmt/x86_64-windows", "winemetal.dll"),
        ]
        .into_iter()
        .collect();
        let actual: std::collections::HashSet<_> =
            dxmt.deploy_dlls.iter().map(|dll| (dll.source_subpath, dll.filename)).collect();
        assert_eq!(actual, expected, "DXMT x64 route must deploy exactly the six unified route DLLs");
        assert!(dxmt.deploy_dlls.iter().all(|dll| dll.source_subpath.contains("x86_64-windows")));
        for forbidden in ["dxgi_dxmt.dll", "d3d12.dll", "d3d12core.dll", "nvapi64.dll", "nvngx.dll"] {
            assert!(
                !dxmt.deploy_dlls.iter().any(|dll| dll.filename == forbidden),
                "DXMT must not ship M12/vendor DLL {}",
                forbidden
            );
        }

        let dxmt_env: std::collections::HashSet<_> = dxmt.env_vars.iter().map(|env| env.key).collect();
        assert!(dxmt_env.contains("DXMT_ASYNC_PIPELINE_COMPILE"));
        assert!(dxmt_env.contains("DXMT_METALFX_SPATIAL_SWAPCHAIN"));

        assert!(dxmt.alternatives.contains(&PipelineId::M9));
    }

    #[test]
    fn dxmt32_deploys_exactly_six_i386_route_dlls() {
        let dxmt32 = get_pipeline(PipelineId::Dxmt32);

        assert_eq!(dxmt32.name, "DXMT(32)");
        assert!(dxmt32.id.is_user_selectable());
        assert!(!dxmt32.experimental);
        assert_eq!(dxmt32.backend, "dxmt");
        assert_eq!(dxmt32.graphics_backend, "dxmt");

        let expected: std::collections::HashSet<_> = [
            ("lib/wine/i386-windows", "d3d10.dll"),
            ("lib/wine/i386-windows", "d3d10_1.dll"),
            ("lib/dxmt/i386-windows", "d3d11.dll"),
            ("lib/dxmt/i386-windows", "dxgi.dll"),
            ("lib/dxmt/i386-windows", "d3d10core.dll"),
            ("lib/dxmt/i386-windows", "winemetal.dll"),
        ]
        .into_iter()
        .collect();
        let actual: std::collections::HashSet<_> =
            dxmt32.deploy_dlls.iter().map(|dll| (dll.source_subpath, dll.filename)).collect();
        assert_eq!(actual, expected, "DXMT(32) route must deploy exactly the six unified i386 route DLLs");
        for dll in &dxmt32.deploy_dlls {
            assert!(
                dll.source_subpath.contains("i386-windows"),
                "DXMT(32) deploys {} from non-i386 lane {}",
                dll.filename,
                dll.source_subpath
            );
        }
        for forbidden in ["dxgi_dxmt.dll", "d3d12.dll", "d3d12core.dll", "nvapi64.dll", "nvngx.dll"] {
            assert!(
                !dxmt32.deploy_dlls.iter().any(|dll| dll.filename == forbidden),
                "DXMT(32) must not ship M12/vendor DLL {}",
                forbidden
            );
        }

        let env_keys: std::collections::HashSet<_> = dxmt32.env_vars.iter().map(|e| e.key).collect();
        assert!(env_keys.contains("DXMT_WINEMETAL_UNIXLIB"));
        assert!(dxmt32.dyld_paths.iter().any(|p| p.contains("i386-unix")));
        assert_eq!(dxmt32.shader_cache_subdir, Some("dxmt_32"));
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

        assert!(m9.alternatives.contains(&PipelineId::Dxmt));
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
                PipelineId::Dxmt,
                PipelineId::M12,
                PipelineId::Dxmt32,
                PipelineId::M9,
                PipelineId::D3DMetal,
                PipelineId::FnaArm64
            ]
        );

        let labels: Vec<_> = selectable.iter().map(|pipeline| pipeline.user_selectable_name().unwrap()).collect();
        assert_eq!(labels, vec!["DXMT", "M12", "DXMT(32)", "M9", "D3DMetal", "Mono/FNA"]);

        for hidden in [PipelineId::M13, PipelineId::M32, PipelineId::Steam, PipelineId::MacSteam, PipelineId::WineBare]
        {
            assert!(!hidden.is_user_selectable());
            assert_eq!(hidden.user_selectable_id(), None);
        }
    }

    #[test]
    fn launch_method_parsing_is_case_and_separator_tolerant() {
        assert_eq!(PipelineId::from_str_flexible("dxmt"), Some(PipelineId::Dxmt));
        assert_eq!(PipelineId::from_str_flexible(" M12 "), Some(PipelineId::M12));
        assert_eq!(PipelineId::from_str_flexible("d3d12"), Some(PipelineId::M12));
        assert_eq!(PipelineId::from_str_flexible("dx10"), Some(PipelineId::Dxmt));
        assert_eq!(PipelineId::from_str_flexible("dx10_32"), Some(PipelineId::Dxmt32));
        assert_eq!(PipelineId::from_str_flexible("wine-steam"), Some(PipelineId::Steam));
        assert_eq!(PipelineId::from_legacy_method("DXMT_METAL"), Some(PipelineId::Dxmt));
    }

    #[test]
    fn dxmt_family_serializes_to_canonical_launch_method() {
        for pipeline in [PipelineId::Dxmt, PipelineId::Dxmt32, PipelineId::M9, PipelineId::M12] {
            assert_eq!(pipeline.to_legacy_method(), "dxmt");
            assert!(pipeline.is_dxmt_family());
        }
    }
}
