use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;
#[cfg(target_os = "macos")]
use walkdir::WalkDir;

static INSTALLING: AtomicBool = AtomicBool::new(false);

fn mac_cmd(name: &str) -> Command {
    let path = match name {
        "curl" => "/usr/bin/curl",
        "tar" => "/usr/bin/tar",
        "which" => "/usr/bin/which",
        "shasum" => "/usr/bin/shasum",
        "pgrep" => "/usr/bin/pgrep",
        "softwareupdate" => "/usr/sbin/softwareupdate",
        "pkill" => "/usr/bin/pkill",
        "clang" => "/usr/bin/clang",
        _ => name,
    };
    Command::new(path)
}

pub const DXMT_BUNDLED_RUNTIME_VERSION: &str = concat!(env!("CARGO_PKG_VERSION"), "-m12-isolated-surface-v1");
const DXMT_RUNTIME_MANIFEST: &str = "metalsharp-dxmt-runtime.json";
const DXMT_RUNTIME_SCHEMA: &str = "metalsharp.dxmt-runtime.v1";
const RUNTIME_BUNDLE: &str = "metalsharp-runtime";
const WINE_PACKAGED_DEPENDENCY_ROOT: &str = "/tmp/metalsharp-wine-deps/lib/";
const GRAPHICS_DLL_BUNDLE: &str = "metalsharp-graphics-dll";
const ASSETS_BUNDLE: &str = "metalsharp-assets";
const FNALIBS_BUNDLE: &str = "fnalibs";
const SCRIPTS_TOOLS_BUNDLE: &str = "metalsharp-scripts-tools";
const STEAM_BUNDLE: &str = "metalsharp-steam";
const EAC_RUNTIME_SUBDIR: &str = "eac";
const METALSHARP_NTDLL_HOOK_DLL: &str = "metalsharp_ntdll_hook.dll";
const DXMT_REQUIRED_PE: &[&str] = &[
    "d3d10core.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dxgi.dll",
    "dxgi_dxmt.dll",
    "winemetal.dll",
    "nvapi64.dll",
    "nvngx.dll",
];
const DXMT_REQUIRED_UNIX: &[&str] = &["winemetal.so"];
/// 32-bit (i386) PE DLLs the DXMT bundle ships for M11(32)/M10(32). These stage
/// into `lib/dxmt/i386-windows/` and are surfaced in the runtime manifest so a
/// migration that pulls a newer graphics bundle re-extracts and applies them.
const DXMT_REQUIRED_I386_PE: &[&str] = &["d3d11.dll", "dxgi.dll", "dxgi_dxmt.dll", "d3d10core.dll", "winemetal.dll"];
/// 32-bit (i386) unix sidecar shipped beside the PE set. Stages into
/// `lib/dxmt/i386-unix/`; M11(32)/M10(32) launch sets
/// `DXMT_WINEMETAL_UNIXLIB=winemetal.so` and adds this dir to dyld fallbacks.
const DXMT_REQUIRED_I386_UNIX: &[&str] = &["winemetal.so"];
const DXMT_M12_REQUIRED_UNIX: &[&str] = &["winemetal.so", "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib"];
#[cfg(not(test))]
const DXMT_M12_EXPECTED_HASHES: &[(&str, &str)] = &[
    ("x86_64-windows/d3d10core.dll", "11c9610770cb0e3f6476d2bde2a3b1afa36a41bd00a2fffc6ea61d2e62c6258d"),
    ("x86_64-windows/d3d11.dll", "98ba9581e10414db0273bf1345b5087ee28de0859fcadfb4a6da09579c2020e9"),
    ("x86_64-windows/d3d12.dll", "cce26811c2ff0ab771a15d90e6c927b9e22567c2311b433de143ad3e4d07dd4f"),
    ("x86_64-windows/dxgi.dll", "628998e1ee632eb7a2d601e4bbeb1e28c05f96193ab5fcd349b1f49faaf6131e"),
    ("x86_64-windows/dxgi_dxmt.dll", "6b7ff46182cd1f0be44227f87fe24e7185de43a028ceb189ac3f2190767f8226"),
    ("x86_64-windows/winemetal.dll", "f6844535ce448e6c525884c8c630298895d7cad97c64eade0f85208a804b9003"),
    ("x86_64-windows/nvapi64.dll", "2eeb618e67c0c2a8d8ff0d84bf45cf69828118c15e894881126e2b94e40d1f83"),
    ("x86_64-windows/nvngx.dll", "cc268b8d89eecef4312a010d25cf77d169c1c68c0875ac1b224d2bc118b921e3"),
    ("x86_64-unix/winemetal.so", "fb46317af86ab157d37a5fb8f781368675047614e86786a74f72a5514b8574d9"),
    ("x86_64-unix/libc++.1.dylib", "3f0da0b4025c6fb5e50fc23c8a1feea67c839b40df93baff3b2781089b42ad35"),
    ("x86_64-unix/libc++abi.1.dylib", "9a95b4ce2be40951b688c394db99f79b7e0b81fa2372e5e49615319869e72e49"),
    ("x86_64-unix/libunwind.1.dylib", "964d4e5d6242163e4e8099efd08ba75540f253257b834bf5b7a45f8c84b4ea78"),
];
#[cfg(test)]
const DXMT_M12_EXPECTED_HASHES: &[(&str, &str)] = &[
    ("x86_64-windows/d3d10core.dll", "e2dec232ddf836655d1aabd8600c02b1852a60832715fd2c2adaecfd484fe33f"),
    ("x86_64-windows/d3d11.dll", "c9db49942a544685de29e7119061987cb001100195bddbcd858b7e4bb9d37a66"),
    ("x86_64-windows/d3d12.dll", "383cd81087b22950a3ce4a99bd157e71a0b964950bb7f0bbc8171a405b72b4c8"),
    ("x86_64-windows/dxgi.dll", "9b2fb52b2c2e247db98963e4702091a64d74b47219b9f400aa8470ddb94a50cc"),
    ("x86_64-windows/dxgi_dxmt.dll", "3d47caa6f31ada10a138c7088c5a8335242a2e1acb651f17de36d152ccf513fd"),
    ("x86_64-windows/winemetal.dll", "e104875e15a385f84e9697cfec7ecc6f9f1d3ea4fa94f7f51b09f429448f487e"),
    ("x86_64-windows/nvapi64.dll", "9d60e35c8e6545a07a927ed74d9bb7c7ca7518dcf8a38a84451eaf4071b299a3"),
    ("x86_64-windows/nvngx.dll", "55540a80dd2728cb2ffaa2f565489da1f83b2c3cb5db73eb9fff0ef79777137b"),
    ("x86_64-unix/winemetal.so", "5f489f7f30b2534f01838bbdf4a763d6ceb799854d61c1f0f5212a076231953c"),
    ("x86_64-unix/libc++.1.dylib", "f005326e267412dd6922159b6ce0443373f25b55803d519d0d2752d8dabe5436"),
    ("x86_64-unix/libc++abi.1.dylib", "7687e592454cfd0bfc40ad03f734db734d8b7d7cbfb3f7d5277195d555306651"),
    ("x86_64-unix/libunwind.1.dylib", "90330fb5d68017d4ca75aae86d6202a8313f298c695fce6685584fb131af3b43"),
];
#[cfg(test)]
pub(crate) fn write_dxmt_m12_expected_test_files(dxmt_m12_dir: &Path) {
    for (rel, _) in DXMT_M12_EXPECTED_HASHES {
        let path = dxmt_m12_dir.join(rel);
        fs::create_dir_all(path.parent().expect("M12 test fixture parent")).expect("create M12 test fixture parent");
        fs::write(path, format!("test-m12:{rel}")).expect("write M12 test fixture payload");
    }
}

/// Pinned hashes for the vkd3d-proton M12 lane (production). Sources are the
/// VKMT win64-filtered x86-64 builds (see docs/roadmaps/m12-vkd3d-proton-migration.md).
#[cfg(not(test))]
const VKD3D_PROTON_EXPECTED_HASHES: &[(&str, &str)] = &[
    ("x86_64-windows/d3d12.dll", "7a34f49a8cf309e20df8f5418c133d8e6a00882155de5532eef2bd9b9f094f93"),
    ("x86_64-windows/d3d12core.dll", "8b643bfbdc9acab92aee8c76ce971b9877f0b851cf6fe2aa04bc37cca5ac22e4"),
];
#[cfg(test)]
const VKD3D_PROTON_EXPECTED_HASHES: &[(&str, &str)] = &[
    ("x86_64-windows/d3d12.dll", "941484b218dec5b9467d004be71d90b6077149d94e4640e2fbf236afc62a7b72"),
    ("x86_64-windows/d3d12core.dll", "a27e3a5b3043019702ef34749742ff75cb1e8c08d63f67155ed7ad6bc46dc8fc"),
];
#[cfg(test)]
pub(crate) fn write_vkd3d_proton_expected_test_files(vkd3d_dir: &Path) {
    for (rel, _) in VKD3D_PROTON_EXPECTED_HASHES {
        let path = vkd3d_dir.join(rel);
        fs::create_dir_all(path.parent().expect("vkd3d test fixture parent"))
            .expect("create vkd3d test fixture parent");
        fs::write(path, format!("test-vkd3d:{rel}")).expect("write vkd3d test fixture payload");
    }
}

/// Pinned hash for the VKMT-patched MoltenVK dylib used by M12.
#[cfg(not(test))]
const MOLTENVK_VKMT_EXPECTED_HASHES: &[(&str, &str)] =
    &[("libMoltenVK.dylib", "50e41de23ce85260870c24cec11ac29b225704c6cb0366ce555dcd9ac03417f3")];
#[cfg(test)]
const MOLTENVK_VKMT_EXPECTED_HASHES: &[(&str, &str)] =
    &[("libMoltenVK.dylib", "c0ee2baa9eee1b262c93f30588760835d4262f9bae9d205dce5bc71bcf658b8c")];
#[cfg(test)]
pub(crate) fn write_moltenvk_vkmt_expected_test_files(moltenvk_dir: &Path) {
    for (rel, _) in MOLTENVK_VKMT_EXPECTED_HASHES {
        let path = moltenvk_dir.join(rel);
        fs::create_dir_all(path.parent().expect("MoltenVK test fixture parent"))
            .expect("create MoltenVK test fixture parent");
        fs::write(path, format!("test-moltenvk:{rel}")).expect("write MoltenVK test fixture payload");
    }
}

const RUNTIME_REQUIRED_ARCHIVE_FILES: &[&str] = &[
    "runtime/wine/bin/metalsharp-wine",
    "runtime/metalsharp-backend",
    "runtime/host/manifest.json",
    "runtime/host/HostRuntimeABI.h",
    "runtime/host/libmetalsharp_host_runtime.dylib",
    "runtime/wine/lib/metalsharp/x86_64-windows/metalsharp_ntdll_hook.dll",
    "runtime/wine/lib/metalsharp/i386-windows/metalsharp_ntdll_hook.dll",
    "runtime/wine/lib/metalsharp/x86_64-windows/dinput.dll",
    "runtime/wine/lib/metalsharp/x86_64-windows/dinput8.dll",
    "runtime/wine/lib/metalsharp/x86_64-windows/xinput1_1.dll",
    "runtime/wine/lib/metalsharp/x86_64-windows/xinput1_2.dll",
    "runtime/wine/lib/metalsharp/x86_64-windows/xinput1_3.dll",
    "runtime/wine/lib/metalsharp/x86_64-windows/xinput1_4.dll",
    "runtime/wine/lib/metalsharp/x86_64-windows/xinput9_1_0.dll",
    "runtime/wine/lib/metalsharp/i386-windows/dinput.dll",
    "runtime/wine/lib/metalsharp/i386-windows/dinput8.dll",
    "runtime/wine/lib/metalsharp/i386-windows/xinput1_1.dll",
    "runtime/wine/lib/metalsharp/i386-windows/xinput1_2.dll",
    "runtime/wine/lib/metalsharp/i386-windows/xinput1_3.dll",
    "runtime/wine/lib/metalsharp/i386-windows/xinput1_4.dll",
    "runtime/wine/lib/metalsharp/i386-windows/xinput9_1_0.dll",
];
const GRAPHICS_REQUIRED_ARCHIVE_FILES: &[&str] = &[
    "Graphics/dll/dxmt/x86_64-unix/winemetal.so",
    "Graphics/dll/dxmt/x86_64-windows/d3d10core.dll",
    "Graphics/dll/dxmt/x86_64-windows/d3d11.dll",
    "Graphics/dll/dxmt/x86_64-windows/d3d12.dll",
    "Graphics/dll/dxmt/x86_64-windows/dxgi.dll",
    "Graphics/dll/dxmt/x86_64-windows/dxgi_dxmt.dll",
    "Graphics/dll/dxmt/x86_64-windows/nvapi64.dll",
    "Graphics/dll/dxmt/x86_64-windows/nvngx.dll",
    "Graphics/dll/dxmt/x86_64-windows/winemetal.dll",
    "Graphics/dll/dxmt/i386-unix/winemetal.so",
    "Graphics/dll/dxmt/i386-windows/d3d10core.dll",
    "Graphics/dll/dxmt/i386-windows/d3d11.dll",
    "Graphics/dll/dxmt/i386-windows/dxgi.dll",
    "Graphics/dll/dxmt/i386-windows/dxgi_dxmt.dll",
    "Graphics/dll/dxmt/i386-windows/winemetal.dll",
    "Graphics/dll/dxmt-m12/x86_64-unix/winemetal.so",
    "Graphics/dll/dxmt-m12/x86_64-unix/libc++.1.dylib",
    "Graphics/dll/dxmt-m12/x86_64-unix/libc++abi.1.dylib",
    "Graphics/dll/dxmt-m12/x86_64-unix/libunwind.1.dylib",
    "Graphics/dll/dxmt-m12/x86_64-windows/d3d10core.dll",
    "Graphics/dll/dxmt-m12/x86_64-windows/d3d11.dll",
    "Graphics/dll/dxmt-m12/x86_64-windows/d3d12.dll",
    "Graphics/dll/dxmt-m12/x86_64-windows/dxgi.dll",
    "Graphics/dll/dxmt-m12/x86_64-windows/dxgi_dxmt.dll",
    "Graphics/dll/dxmt-m12/x86_64-windows/nvapi64.dll",
    "Graphics/dll/dxmt-m12/x86_64-windows/nvngx.dll",
    "Graphics/dll/dxmt-m12/x86_64-windows/winemetal.dll",
    // vkd3d-proton lane (M12 default backend): D3D12 -> Vulkan -> MoltenVK.
    // M12 is x86_64-only; i386 vkd3d-proton remains future scope.
    "Graphics/dll/vkd3d-proton/x86_64-windows/d3d12.dll",
    "Graphics/dll/vkd3d-proton/x86_64-windows/d3d12core.dll",
    // DXVK lane: dxgi/d3d11/d3d10/d3d9 surfaces (M12 uses dxgi; M9-M11 use d3d11+).
    "Graphics/dll/dxvk/x86_64-windows/dxgi.dll",
    "Graphics/dll/dxvk/x86_64-windows/d3d11.dll",
    "Graphics/dll/dxvk/x86_64-windows/d3d10core.dll",
    "Graphics/dll/dxvk/x86_64-windows/d3d9.dll",
    "Graphics/dll/dxvk/i386-windows/dxgi.dll",
    "Graphics/dll/dxvk/i386-windows/d3d11.dll",
    "Graphics/dll/dxvk/i386-windows/d3d10core.dll",
    "Graphics/dll/dxvk/i386-windows/d3d9.dll",
    // VKMT MoltenVK lane: patched Vulkan-on-Metal for the vkd3d-proton stack.
    "Graphics/dll/moltenvk-vkmt/libMoltenVK.dylib",
    "Graphics/dll/moltenvk-vkmt/MoltenVK_icd.json",
];
const ASSETS_REQUIRED_ARCHIVE_FILES: &[&str] = &[
    "assets/fna-kickstart/kick.bin.osx",
    "assets/fna-kickstart/FNA.dll",
    "assets/fna-kickstart/mscorlib.dll",
    "assets/fna-kickstart/osx/libmonosgen-2.0.1.dylib",
    "assets/fna-kickstart/osx/libSDL2-2.0.0.dylib",
    "assets/fna-kickstart/osx/libFNA3D.0.dylib",
    "assets/fna-kickstart/osx/libFAudio.0.dylib",
    "assets/fna-kickstart/osx/libMonoPosixHelper.dylib",
    "assets/fnalibs/libFNA3D.0.dylib",
    "assets/fnalibs/libSDL2-2.0.0.dylib",
    "assets/fnalibs/libFAudio.0.dylib",
    "assets/fnalibs/libtheorafile.dylib",
    "assets/fnalibs/fmod/libfmod.dylib",
    "assets/fnalibs/fmod/libfmodstudio.dylib",
    "assets/goldberg/x64/steam_api64.dll",
    "assets/goldberg/x86/steam_api.dll",
    "assets/mono-arm64/bin/mono-sgen",
    "assets/shims/libsteam_api.dylib",
    // XNA 4.0 managed assembly set (improved FNA-compatible payloads).
    "assets/xna/Microsoft.Xna.Framework.dll",
    "assets/xna/Microsoft.Xna.Framework.Game.dll",
    "assets/xna/Microsoft.Xna.Framework.Graphics.dll",
    "assets/xna/Microsoft.Xna.Framework.Audio.dll",
    "assets/xna/Microsoft.Xna.Framework.Input.dll",
    "assets/xna/Microsoft.Xna.Framework.Media.dll",
    "assets/xna/Microsoft.Xna.Framework.Storage.dll",
    // Version-matched Unity Mono runtimes (arm64, per Unity LTS line).
    "assets/unity-mono/manifest.json",
    "assets/unity-mono/2020.3/libmonosgen-2.0.1.dylib",
    "assets/unity-mono/2020.3/mono-sgen",
    "assets/unity-mono/2020.3/MonoBleedingEdge.version",
    "assets/unity-mono/2021.3/libmonosgen-2.0.1.dylib",
    "assets/unity-mono/2021.3/mono-sgen",
    "assets/unity-mono/2021.3/MonoBleedingEdge.version",
    "assets/unity-mono/2022.3/libmonosgen-2.0.1.dylib",
    "assets/unity-mono/2022.3/mono-sgen",
    "assets/unity-mono/2022.3/MonoBleedingEdge.version",
    "assets/unity-mono/6000.0/libmonosgen-2.0.1.dylib",
    "assets/unity-mono/6000.0/mono-sgen",
    "assets/unity-mono/6000.0/MonoBleedingEdge.version",
    // SDL3 (modern FNA/MonoGame/Unity native dependency).
    "assets/sdl3/libSDL3.dylib",
    // Prebuilt launcher/patcher binaries (Terraria launcher, offline patcher,
    // Xact stub) — shipped compiled; the app never compiles at launch.
    "assets/prebuilt-launchers/TerrariaLauncher.exe",
    "assets/prebuilt-launchers/TerrariaOfflinePatcher.exe",
    "assets/prebuilt-launchers/Microsoft.Xna.Framework.Xact.dll",
    // Prebuilt gdiplus/faudio stubs (Terraria lane) — never clang-compiled.
    "assets/shims/libgdiplus.dylib",
    "assets/shims/libFAudio.0.dylib",
];
const FNALIBS_REQUIRED_ARCHIVE_FILES: &[&str] = &[
    "fnalibs/libFNA3D.0.dylib",
    "fnalibs/libSDL2-2.0.0.dylib",
    "fnalibs/libFAudio.0.dylib",
    "fnalibs/libtheorafile.dylib",
    "fnalibs/fmod/libfmod.dylib",
    "fnalibs/fmod/libfmodstudio.dylib",
];
const SCRIPTS_TOOLS_REQUIRED_ARCHIVE_FILES: &[&str] = &[
    "scripts/tools/configs/mtsp-rules.toml",
    "scripts/tools/updater/update.sh",
    "scripts/tools/native/metalsharp_eac_substrate.dylib",
    "scripts/tools/native/metalsharp_eac_libc.so.6",
];
const STEAM_REQUIRED_ARCHIVE_FILES: &[&str] =
    &["steam/SteamSetup.exe", "steam/steamwebhelper.exe", "steam/steamwebhelper-wrapper.c"];

const MAC_RUNTIME_BUNDLE_ASSETS: &[&str] = &[
    "metalsharp-runtime.tar.zst",
    "metalsharp-graphics-dll.tar.zst",
    "metalsharp-assets.tar.zst",
    "fnalibs.tar.zst",
    "metalsharp-scripts-tools.tar.zst",
    "metalsharp-steam.tar.zst",
];

fn progress_path() -> PathBuf {
    crate::platform::metalsharp_home_dir().join("install_progress.json")
}

fn write_progress(step: usize, total: usize, name: &str, status: &str, log_line: &str, error: Option<&str>) {
    let data = json!({
        "step": step,
        "total": total,
        "current": name,
        "status": status,
        "log": log_line,
        "error": error,
    });
    let path = progress_path();
    let _ = fs::write(&path, serde_json::to_string(&data).unwrap_or_default());
}

pub fn is_installing() -> bool {
    INSTALLING.load(Ordering::SeqCst)
}

pub fn read_progress() -> Value {
    let path = progress_path();
    if path.exists() {
        if let Ok(contents) = fs::read_to_string(&path) {
            if let Ok(v) = serde_json::from_str::<Value>(&contents) {
                return v;
            }
        }
    }
    json!({
        "step": 0,
        "total": 0,
        "current": "",
        "status": "idle",
        "log": "",
        "error": null,
    })
}

pub fn start_install_all() -> Result<Value, Box<dyn std::error::Error>> {
    if INSTALLING.load(Ordering::SeqCst) {
        return Ok(json!({"ok": false, "error": "installation already in progress"}));
    }

    if INSTALLING.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_err() {
        return Ok(json!({"ok": false, "error": "installation already in progress"}));
    }

    std::thread::spawn(|| {
        run_install_all();
        INSTALLING.store(false, Ordering::SeqCst);
    });

    Ok(json!({"ok": true}))
}

fn run_install_all() {
    if crate::platform::current() != crate::platform::HostPlatform::Macos {
        write_progress(
            0,
            0,
            "Unsupported Platform",
            "error",
            "MetalSharp runtime installation is Apple Silicon macOS-only.",
            Some("unsupported_platform"),
        );
        return;
    }

    let home = match dirs::home_dir() {
        Some(h) => h,
        None => {
            write_progress(0, 0, "", "error", "no home directory", Some("no home directory"));
            return;
        },
    };

    let steps = install_steps();
    let total = steps.len();

    write_progress(0, total, "Starting...", "starting", "Verifying prerequisites...", None);
    if !check_command("tar") {
        write_progress(
            0,
            total,
            "Prerequisites",
            "error",
            "tar not found — macOS should have this. Is your system intact?",
            Some("tar command not found"),
        );
        return;
    }

    if !check_command("curl") {
        write_progress(
            0,
            total,
            "Prerequisites",
            "error",
            "curl not found — install curl before installing runtime assets.",
            Some("curl command not found"),
        );
        return;
    }

    if !check_command("brew") {
        write_progress(
            0,
            total,
            "Homebrew",
            "error",
            "Homebrew is required but not installed. Please install it first from the setup wizard.",
            Some("Homebrew not installed — install from https://brew.sh"),
        );
        return;
    }

    for (i, (name, installer)) in steps.iter().enumerate() {
        let step_num = i + 1;
        write_progress(step_num, total, name, "installing", &format!("Installing {}...", name), None);

        match installer(&home) {
            Ok(false) => {
                write_progress(step_num, total, name, "done", &format!("{} ready", name), None);
            },
            Ok(true) => {
                write_progress(step_num, total, name, "done", &format!("{} installed", name), None);
            },
            Err(e) => {
                write_progress(step_num, total, name, "error", &format!("{} failed: {}", name, e), Some(&e));
                return;
            },
        }

        std::thread::sleep(Duration::from_millis(200));
    }

    // Fresh install: the default m12Backend is vkd3d-proton, but if the
    // vkd3d/DXVK/MoltenVK lanes failed to stage (old bundle, offline, hash
    // mismatch) the default must fall back to DXMT in config so it never
    // points at a missing runtime. Migration does this in reconcile_m12_backend;
    // install needs the same guard.
    reconcile_m12_backend_for_home(&home);

    write_progress(total, total, "Complete", "complete", "All assets installed!", None);
}

/// Keep the M12 backend pointing at a real runtime after a fresh install:
/// leave the vkd3d-proton default when its lanes staged, otherwise pin DXMT in
/// config. Mirrors migrate.rs `reconcile_m12_backend` for the install path.
pub fn reconcile_m12_backend_for_home(home: &Path) {
    if vkd3d_proton_runtime_current_for_home(home)
        && moltenvk_vkmt_runtime_ready_for_home(home)
        && dxvk_runtime_ready_for_home(home)
    {
        return;
    }
    let mut body = serde_json::Map::new();
    body.insert("m12Backend".into(), json!("dxmt"));
    if crate::launch::set_config_for_home(home, &body).is_ok() {
        eprintln!("M12 vkd3d-proton lanes missing after install — fell back to DXMT backend");
    }
}

type InstallStep = (&'static str, Box<dyn Fn(&PathBuf) -> Result<bool, String>>);

fn install_steps() -> Vec<InstallStep> {
    vec![
        ("System Tools", Box::new(|_| install_xcode_cli())),
        ("Rosetta 2", Box::new(|_| install_rosetta())),
        ("Extract Tools (zstd)", Box::new(|_| ensure_zstd())),
        ("Runtime Bundle Downloads", Box::new(ensure_runtime_bundle_assets)),
        ("Runtime Assets", Box::new(install_metalsharp_bundle)),
        ("Host Runtime ABI", Box::new(install_host_runtime)),
        ("Support Assets", Box::new(install_split_assets_bundle)),
        ("Scripts and Tools", Box::new(install_scripts_tools_bundle)),
        ("EAC Substrate", Box::new(ensure_eac_substrate_runtime_ready)),
        ("DXMT Graphics Runtimes", Box::new(|home| ensure_graphics_runtimes_ready(home))),
        ("Goldberg Steam Emulator", Box::new(install_goldberg)),
        ("Steam Bridge Shim", Box::new(install_steam_bridge)),
        ("Pipeline Rules", Box::new(install_mtsp_rules)),
        ("Mono Configs", Box::new(install_mono_configs)),
        ("Runtime Support", Box::new(|_| install_mono_arm64())),
        ("FNA Shim Precompile", Box::new(|_| crate::mtsp::launcher::precompile_all_fna_shims().map(|_| true))),
    ]
}

fn runtime_bundle_assets_for_host() -> &'static [&'static str] {
    MAC_RUNTIME_BUNDLE_ASSETS
}

fn ensure_runtime_bundle_assets(_home: &PathBuf) -> Result<bool, String> {
    let mut downloaded = false;
    let mut missing = Vec::new();

    for asset in runtime_bundle_assets_for_host() {
        let had_local = bundled_file_valid_exists(asset);
        if had_local {
            continue;
        }

        write_progress(3, 14, "Runtime Bundle Downloads", "downloading", &format!("Downloading {}...", asset), None);
        match download_bundled_file(asset) {
            Some(path) if file_nonempty(&path) && bundled_artifact_valid(asset, &path) => {
                downloaded = true;
                write_progress(3, 14, "Runtime Bundle Downloads", "done", &format!("Downloaded {}", asset), None);
            },
            _ => {
                missing.push(*asset);
                write_progress(
                    3,
                    14,
                    "Runtime Bundle Downloads",
                    "error",
                    &format!("Failed to download {}", asset),
                    Some(&format!("Missing bundle: {}", asset)),
                );
            },
        }
    }

    if missing.is_empty() {
        Ok(downloaded)
    } else {
        Err(format!(
            "Missing required runtime bundle asset(s) that could not be downloaded: {}. Please check your internet connection and try again.",
            missing.join(", ")
        ))
    }
}

fn bundled_file_valid_exists(name: &str) -> bool {
    let mut candidates = Vec::new();

    if let Some(resources) = crate::platform::app_resources_dir() {
        candidates.push(resources.join("bundles").join(name));
    }

    candidates.push(PathBuf::from("app/bundles").join(name));

    if let Some(home) = dirs::home_dir() {
        candidates.push(crate::platform::metalsharp_home_dir_for(&home).join("cache").join("bundles").join(name));
    }

    bundled_file_valid_exists_in_candidates(name, candidates)
}

fn bundled_file_valid_exists_in_candidates(name: &str, candidates: impl IntoIterator<Item = PathBuf>) -> bool {
    candidates.into_iter().any(|path| bundled_artifact_valid(name, &path))
}

fn install_rosetta() -> Result<bool, String> {
    let plist = PathBuf::from("/Library/Apple/System/Library/LaunchDaemons/com.apple.oahd.plist");
    if plist.exists() {
        return Ok(false);
    }
    let running = mac_cmd("pgrep").args(["-q", "oahd"]).status().map(|s| s.success()).unwrap_or(false);
    if running {
        return Ok(false);
    }

    let output = mac_cmd("softwareupdate")
        .args(["--install-rosetta", "--agree-to-license"])
        .output()
        .map_err(|e| format!("failed to run softwareupdate: {}", e))?;

    if output.status.success() || String::from_utf8_lossy(&output.stderr).contains("already installed") {
        Ok(true)
    } else {
        Err(format!("rosetta install failed: {}", String::from_utf8_lossy(&output.stderr)))
    }
}

fn install_xcode_cli() -> Result<bool, String> {
    if xcode_cli_functional() {
        return Ok(false);
    }

    let output = Command::new("/usr/bin/xcode-select")
        .args(["--install"])
        .output()
        .map_err(|e| format!("failed to run xcode-select: {}", e))?;

    let stderr = String::from_utf8_lossy(&output.stderr);
    if (stderr.contains("already installed") || stderr.contains("command line tools are already installed"))
        && xcode_cli_functional()
    {
        return Ok(false);
    }

    for _ in 0..120 {
        std::thread::sleep(Duration::from_secs(5));
        if xcode_cli_functional() {
            return Ok(true);
        }
    }

    install_xcode_cli_softwareupdate()?;

    if xcode_cli_functional() {
        Ok(true)
    } else {
        Err("Xcode CLI tools installation failed — install manually with: xcode-select --install".into())
    }
}

fn xcode_cli_functional() -> bool {
    let clang = match find_system_command("clang") {
        Some(p) => p,
        None => return false,
    };
    Command::new(&clang)
        .args(["-x", "c", "-c", "-o", "/dev/null", "-"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn install_xcode_cli_softwareupdate() -> Result<(), String> {
    let list_output = Command::new("/usr/sbin/softwareupdate")
        .args(["--list"])
        .output()
        .map_err(|e| format!("softwareupdate --list failed: {}", e))?;

    let list = String::from_utf8_lossy(&list_output.stdout);
    let label = list
        .lines()
        .find(|line| line.to_lowercase().contains("command line tools") || line.to_lowercase().contains("xcode"))
        .and_then(|line| {
            line.split_whitespace()
                .find(|word| word.starts_with('*') || word.contains("CLTools") || word.contains("Xcode"))
                .map(|w| w.trim_start_matches('*').trim_start_matches('"').trim_end_matches('"').to_string())
                .or_else(|| {
                    let parts: Vec<&str> = line.splitn(2, ',').collect();
                    parts.first().map(|s| {
                        s.trim().trim_start_matches('*').trim_start_matches('"').trim_end_matches('"').to_string()
                    })
                })
        });

    let install_target = match label {
        Some(l) => l,
        None => "*Command Line Tools*".to_string(),
    };

    let install_output = Command::new("/usr/sbin/softwareupdate")
        .args(["--install", &install_target])
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .output()
        .map_err(|e| format!("softwareupdate --install failed: {}", e))?;

    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&install_output.stdout),
        String::from_utf8_lossy(&install_output.stderr)
    );
    if !install_output.status.success() && !combined.contains("No updates") && !combined.contains("already installed") {
        return Err(format!("softwareupdate install failed: {}", combined.lines().last().unwrap_or("unknown error")));
    }

    Ok(())
}

fn install_metalsharp_bundle(home: &PathBuf) -> Result<bool, String> {
    let runtime_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime");
    let _ = fs::create_dir_all(&runtime_dir);

    let bundle = find_bundled_archive(RUNTIME_BUNDLE);
    let ms_wine = metalsharp_wine_binary(home);
    let host_dir = runtime_dir.join("host");
    let backend = runtime_dir.join("metalsharp-backend");
    if ms_wine.exists()
        && host_runtime_ready(&host_dir)
        && file_nonempty(&backend)
        && metalsharp_runtime_lib_ready(&runtime_dir.join("wine"))
        && bundle.as_ref().is_some_and(|archive| split_bundle_current(home, RUNTIME_BUNDLE, archive))
    {
        // Older runtime bundles were published with build-machine absolute
        // GnuTLS install names.  Repair this even on the currency fast path;
        // otherwise an already-current bundle can keep failing HTTPS during
        // protected-launch module download forever.
        return repair_wine_packaged_dependencies(&runtime_dir.join("wine"));
    }

    if let Some(archive) = bundle {
        let tmp_extract = std::env::temp_dir().join("metalsharp-bundle-extract");
        let _ = fs::remove_dir_all(&tmp_extract);
        let _ = fs::create_dir_all(&tmp_extract);
        extract_zst(&archive, &tmp_extract, RUNTIME_BUNDLE)?;

        let wine_dir = runtime_dir.join("wine");
        let source = tmp_extract.join("runtime").join("wine");
        if source.exists() {
            let preserved_graphics = preserve_graphics_runtime_surfaces(&wine_dir, &tmp_extract)?;
            let _ = fs::remove_dir_all(&wine_dir);
            copy_dir_recursive(&source, &wine_dir)?;
            restore_preserved_graphics_runtime_surfaces(&wine_dir, &preserved_graphics)?;
        }

        let source_host = tmp_extract.join("runtime").join("host");
        if source_host.exists() {
            let _ = fs::remove_dir_all(&host_dir);
            copy_dir_recursive(&source_host, &host_dir)?;
        }

        let source_backend = tmp_extract.join("runtime").join("metalsharp-backend");
        if source_backend.exists() {
            fs::copy(&source_backend, &backend).map_err(|e| format!("copy runtime backend: {}", e))?;
            make_executable(&backend);
        }
        let _ = fs::remove_dir_all(&tmp_extract);

        let ms_wine = metalsharp_wine_binary(home);
        if ms_wine.exists() {
            if !host_runtime_ready(&host_dir) {
                return Err("MetalSharp runtime bundle installed but host runtime ABI assets are missing".into());
            }
            if !file_nonempty(&backend) {
                return Err("MetalSharp runtime bundle installed but backend executable is missing".into());
            }
            if !metalsharp_runtime_lib_ready(&runtime_dir.join("wine")) {
                return Err("MetalSharp runtime bundle installed but MetalSharp hook DLL is missing".into());
            }

            let wine_check = Command::new(&ms_wine).arg("--version").output();
            match wine_check {
                Ok(o) if o.status.success() => {
                    fix_moltenvk_icd_paths(&runtime_dir.join("wine"));
                    repair_wine_packaged_dependencies(&runtime_dir.join("wine"))?;
                    mark_split_bundle_installed(home, RUNTIME_BUNDLE, &archive);
                    return Ok(true);
                },
                Ok(o) => {
                    return Err(format!(
                        "Wine binary exists but --version failed: {}",
                        String::from_utf8_lossy(&o.stderr)
                    ))
                },
                Err(e) => return Err(format!("Wine binary exists but cannot execute: {}", e)),
            }
        }
    }

    Err("MetalSharp runtime not found — no bundled metalsharp-runtime.tar.zst available".into())
}

/// Rewrite build-machine absolute dylib names in the Wine bundle to paths
/// relative to the loading dylib.  The macOS Wine bundle carries GnuTLS and
/// its crypto closure in a `lib/wine/*-unix` tree; older bundle builds
/// retained `/tmp/metalsharp-wine-deps/...` from the staging machine, so dyld
/// could not load Schannel even though the files were present.
///
/// This deliberately changes only that exact private staging prefix.  System
/// frameworks, SDK libraries, and vendor/runtime assets keep their original
/// install names.  The operation is idempotent and re-signs only binaries it
/// actually changes.
fn repair_wine_packaged_dependencies(wine_dir: &Path) -> Result<bool, String> {
    #[cfg(target_os = "macos")]
    {
        let wine_lib_root = wine_dir.join("lib").join("wine");
        if !wine_lib_root.is_dir() {
            return Ok(false);
        }

        let mut changed = false;
        for entry in WalkDir::new(&wine_lib_root).follow_links(false).into_iter().filter_map(Result::ok) {
            let path = entry.path();
            let is_macho_candidate =
                matches!(path.extension().and_then(|ext| ext.to_str()), Some("dylib") | Some("so"));
            if !is_macho_candidate || !entry.file_type().is_file() {
                continue;
            }

            let output = Command::new("/usr/bin/otool")
                .arg("-L")
                .arg(path)
                .output()
                .map_err(|e| format!("inspect Wine dylib {}: {}", path.display(), e))?;
            if !output.status.success() {
                // The runtime tree can contain a non-Mach-O file with a
                // dylib suffix from a third-party payload.  It is not part of
                // this repair surface; let Wine's normal validation report it.
                continue;
            }

            let dependencies = String::from_utf8_lossy(&output.stdout)
                .lines()
                .skip(1)
                .filter_map(|line| line.split_whitespace().next())
                .filter_map(packaged_dependency_target)
                .collect::<Vec<_>>();
            let current_id = Command::new("/usr/bin/otool").arg("-D").arg(path).output().ok().and_then(|id| {
                if !id.status.success() {
                    return None;
                }
                String::from_utf8_lossy(&id.stdout).lines().nth(1).map(str::trim).map(str::to_string)
            });
            let id_target = current_id.as_deref().and_then(packaged_dependency_target);

            let mut file_changed = false;
            for target in dependencies {
                let old = format!("{}{}", WINE_PACKAGED_DEPENDENCY_ROOT, target.trim_start_matches("@loader_path/"));
                run_install_name_tool(&["-change", &old, &target], path)?;
                file_changed = true;
            }
            if let Some(target) = id_target {
                run_install_name_tool(&["-id", &target], path)?;
                file_changed = true;
            }

            if file_changed {
                let sign = Command::new("/usr/bin/codesign")
                    .args(["--force", "--sign", "-"])
                    .arg(path)
                    .output()
                    .map_err(|e| format!("ad-hoc sign repaired Wine dylib {}: {}", path.display(), e))?;
                if !sign.status.success() {
                    return Err(format!(
                        "ad-hoc sign repaired Wine dylib {} failed: {}",
                        path.display(),
                        String::from_utf8_lossy(&sign.stderr).trim()
                    ));
                }
                changed = true;
            }
        }

        Ok(changed)
    }

    #[cfg(not(target_os = "macos"))]
    {
        let _ = wine_dir;
        Ok(false)
    }
}

#[cfg(target_os = "macos")]
fn packaged_dependency_target(path: &str) -> Option<String> {
    let basename = path.strip_prefix(WINE_PACKAGED_DEPENDENCY_ROOT)?;
    if basename.is_empty() || basename.contains('/') {
        return None;
    }
    Some(format!("@loader_path/{}", basename))
}

#[cfg(target_os = "macos")]
fn run_install_name_tool(args: &[&str], path: &Path) -> Result<(), String> {
    let output = Command::new("/usr/bin/install_name_tool")
        .args(args)
        .arg(path)
        .output()
        .map_err(|e| format!("rewrite Wine dylib {}: {}", path.display(), e))?;
    if output.status.success() {
        Ok(())
    } else {
        Err(format!("rewrite Wine dylib {} failed: {}", path.display(), String::from_utf8_lossy(&output.stderr).trim()))
    }
}

const GRAPHICS_RUNTIME_SURFACES: &[&str] = &["dxmt", "dxmt_m12", "vkd3d-proton", "dxvk", "moltenvk-vkmt"];

fn preserve_graphics_runtime_surfaces(wine_dir: &Path, tmp_extract: &Path) -> Result<PathBuf, String> {
    let preserve_dir = tmp_extract.join("preserved-graphics-runtimes");
    let lib_dir = wine_dir.join("lib");
    for surface in GRAPHICS_RUNTIME_SURFACES {
        let src = lib_dir.join(surface);
        if src.exists() {
            copy_dir_recursive(&src, &preserve_dir.join(surface))?;
        }
    }
    Ok(preserve_dir)
}

fn restore_preserved_graphics_runtime_surfaces(wine_dir: &Path, preserve_dir: &Path) -> Result<(), String> {
    for surface in GRAPHICS_RUNTIME_SURFACES {
        let preserved = preserve_dir.join(surface);
        let dst = wine_dir.join("lib").join(surface);
        if preserved.exists() && !dst.exists() {
            copy_dir_recursive(&preserved, &dst)?;
        }
    }
    Ok(())
}

fn metalsharp_wine_binary(home: &Path) -> PathBuf {
    crate::platform::runtime_wine_binary(&crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine"))
}

fn install_host_runtime(home: &PathBuf) -> Result<bool, String> {
    let dest = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("host");
    if host_runtime_ready(&dest) {
        return Ok(false);
    }

    let Some(source) = find_packaged_host_runtime() else {
        return install_host_runtime_from_runtime_bundle(&dest);
    };

    install_host_runtime_from_dir(&source, &dest)
}

fn install_host_runtime_from_dir(source: &Path, dest: &Path) -> Result<bool, String> {
    let _ = fs::remove_dir_all(dest);
    fs::create_dir_all(dest).map_err(|e| format!("create host runtime dir: {}", e))?;
    copy_dir_recursive(source, dest)?;

    if host_runtime_ready(dest) {
        Ok(true)
    } else {
        Err("MetalSharp host runtime copied but required ABI files are missing".into())
    }
}

fn install_host_runtime_from_runtime_bundle(dest: &Path) -> Result<bool, String> {
    let archive = find_bundled_archive(RUNTIME_BUNDLE)
        .ok_or_else(|| "MetalSharp host runtime not found — packaged runtime/host assets are missing".to_string())?;
    let tmp_extract = std::env::temp_dir().join("metalsharp-host-runtime-extract");
    let _ = fs::remove_dir_all(&tmp_extract);
    let _ = fs::create_dir_all(&tmp_extract);
    extract_zst(&archive, &tmp_extract, RUNTIME_BUNDLE)?;

    let source = tmp_extract.join("runtime").join("host");
    if !host_runtime_ready(&source) {
        let _ = fs::remove_dir_all(&tmp_extract);
        return Err("MetalSharp host runtime not found in bundled metalsharp-runtime.tar.zst".into());
    }

    let result = install_host_runtime_from_dir(&source, dest);
    let _ = fs::remove_dir_all(&tmp_extract);
    result
}

fn host_runtime_ready(dir: &Path) -> bool {
    file_nonempty(&dir.join("manifest.json"))
        && file_nonempty(&dir.join("HostRuntimeABI.h"))
        && (file_nonempty(&dir.join("libmetalsharp_host_runtime.dylib"))
            || file_nonempty(&dir.join("libmetalsharp_host_runtime.so"))
            || file_nonempty(&dir.join("metalsharp_host_runtime.dll")))
}

fn file_nonempty(path: &Path) -> bool {
    path.metadata().map(|meta| meta.is_file() && meta.len() > 0).unwrap_or(false)
}

pub(crate) fn metalsharp_runtime_lib_ready(wine_dir: &Path) -> bool {
    file_nonempty(&wine_dir.join("lib").join("metalsharp").join("x86_64-windows").join(METALSHARP_NTDLL_HOOK_DLL))
        && file_nonempty(&wine_dir.join("lib").join("metalsharp").join("i386-windows").join(METALSHARP_NTDLL_HOOK_DLL))
}

pub fn moltenvk_ready(wine_dir: &Path) -> bool {
    wine_dir.join("lib").join("wine").join("x86_64-unix").join("libMoltenVK.dylib").is_file()
}

/// True when the hash-pinned VKMT MoltenVK lane is installed (preferred for
/// the vkd3d-proton M12 stack). Falls back to the stock runtime dylib.
pub fn moltenvk_vkmt_ready(wine_dir: &Path) -> bool {
    let dir = wine_dir.join("lib").join("moltenvk-vkmt");
    MOLTENVK_VKMT_EXPECTED_HASHES
        .iter()
        .all(|(rel, expected)| crate::diagnostics::file_sha256(&dir.join(rel)).as_deref() == Some(*expected))
}

/// Resolve the MoltenVK dylib to prefer: VKMT lane first, then the stock
/// runtime location.
fn moltenvk_library_path(wine_dir: &Path) -> PathBuf {
    let vkmt = wine_dir.join("lib").join("moltenvk-vkmt").join("libMoltenVK.dylib");
    if moltenvk_vkmt_ready(wine_dir) {
        return vkmt;
    }
    wine_dir.join("lib").join("wine").join("x86_64-unix").join("libMoltenVK.dylib")
}

fn fix_moltenvk_icd_paths(wine_dir: &Path) {
    let actual_lib = moltenvk_library_path(wine_dir);
    if !actual_lib.exists() {
        eprintln!("moltenvk: libMoltenVK.dylib not found in runtime — skipping ICD fix");
        return;
    }

    let icd_dir = wine_dir.join("etc").join("vulkan").join("icd.d");
    if !icd_dir.is_dir() {
        eprintln!("moltenvk: ICD directory not found — skipping");
        return;
    }

    // The VKMT package ships its own ICD json; stage it into the runtime
    // icd.d so the Vulkan loader resolves the patched dylib.
    let vkmt_icd = wine_dir.join("lib").join("moltenvk-vkmt").join("MoltenVK_icd.json");
    if vkmt_icd.is_file() {
        let target = icd_dir.join("MoltenVK_icd.json");
        if let Ok(data) = fs::read_to_string(&vkmt_icd) {
            let _ = fs::write(&target, data);
        }
    }

    let correct_path = format!("{}", actual_lib.to_string_lossy());

    for entry in std::fs::read_dir(&icd_dir).unwrap_or_else(|_| panic!("read_dir")).flatten() {
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();
        if !name.starts_with("MoltenVK") || !name.ends_with(".json") {
            continue;
        }
        let Ok(data) = fs::read_to_string(&path) else { continue };
        let Ok(mut v) = serde_json::from_str::<serde_json::Value>(&data) else { continue };
        if let Some(icd) = v.get_mut("ICD") {
            if let Some(lib_path) = icd.get_mut("library_path") {
                let current = lib_path.as_str().unwrap_or("");
                if current != correct_path {
                    eprintln!("moltenvk: fixing {} ICD path {} -> {}", name, current, correct_path);
                    *lib_path = serde_json::Value::String(correct_path.clone());
                    if let Err(e) = fs::write(&path, serde_json::to_string_pretty(&v).unwrap_or_default()) {
                        eprintln!("moltenvk: failed to write {}: {}", path.display(), e);
                    }
                }
            }
        }
    }
}

fn split_bundle_marker_dir(home: &Path) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("bundle-state")
}

fn split_bundle_marker_path(home: &Path, bundle: &str) -> PathBuf {
    split_bundle_marker_dir(home).join(format!("{}.sha256", bundle))
}

fn split_bundle_current(home: &Path, bundle: &str, archive: &Path) -> bool {
    if bundle == ASSETS_BUNDLE && !fna_support_assets_current(home) {
        return false;
    }
    let Some(hash) = archive_sha256(archive) else {
        return false;
    };
    fs::read_to_string(split_bundle_marker_path(home, bundle)).map(|existing| existing.trim() == hash).unwrap_or(false)
}

fn fna_support_assets_current(home: &Path) -> bool {
    let runtime = crate::platform::metalsharp_home_dir_for(home).join("runtime");
    let fna3d = runtime.join("fnalibs").join("libFNA3D.0.dylib");
    let kick_fna3d = runtime.join("fna-kickstart").join("osx").join("libFNA3D.0.dylib");
    let sdl2 = runtime.join("fnalibs").join("libSDL2-2.0.0.dylib");
    let faudio = runtime.join("fnalibs").join("libFAudio.0.dylib");
    let kick_faudio = runtime.join("fna-kickstart").join("osx").join("libFAudio.0.dylib");
    let fmod = runtime.join("fnalibs").join("fmod").join("libfmod.dylib");
    let fmodstudio = runtime.join("fnalibs").join("fmod").join("libfmodstudio.dylib");
    fna_dylib_uses_sdl2(&fna3d)
        && fna_dylib_uses_sdl2(&kick_fna3d)
        && fna_dylib_uses_sdl2(&faudio)
        && fna_dylib_uses_sdl2(&kick_faudio)
        && sdl2.exists()
        && fmod_dylib_has_payload(&fmod)
        && fmod_dylib_has_payload(&fmodstudio)
}

fn fna_dylib_uses_sdl2(path: &Path) -> bool {
    if !path.exists() {
        return false;
    }
    if crate::platform::current() != crate::platform::HostPlatform::Macos {
        return true;
    }
    let Ok(output) = Command::new("/usr/bin/otool").args(["-L", "-arch", "x86_64"]).arg(path).output() else {
        return false;
    };
    if !output.status.success() {
        return false;
    }
    let deps = String::from_utf8_lossy(&output.stdout);
    deps.contains("libSDL2") && !deps.contains("libSDL3")
}

fn fmod_dylib_has_payload(path: &Path) -> bool {
    path.metadata().map(|metadata| metadata.len() >= 256 * 1024).unwrap_or(false)
}

pub(crate) fn repair_fna_support_assets() -> Result<usize, String> {
    let home = dirs::home_dir().ok_or_else(|| "no home dir".to_string())?;
    let runtime_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime");

    if let Some(archive) = find_bundled_archive(FNALIBS_BUNDLE) {
        return repair_fna_support_assets_from_fnalibs_archive(&archive, &runtime_dir);
    }

    let archive = find_bundled_archive(ASSETS_BUNDLE).ok_or_else(|| {
        "FNA support assets not found — fnalibs.tar.zst or metalsharp-assets.tar.zst is missing".to_string()
    })?;
    repair_fna_support_assets_from_assets_archive(&archive, &runtime_dir)
}

fn repair_fna_support_assets_from_fnalibs_archive(archive: &Path, runtime_dir: &Path) -> Result<usize, String> {
    let tmp = std::env::temp_dir().join("metalsharp-fnalibs-repair");
    let _ = fs::remove_dir_all(&tmp);
    fs::create_dir_all(&tmp).map_err(|e| format!("create {}: {}", tmp.display(), e))?;
    extract_zst(&archive.to_path_buf(), &tmp, FNALIBS_BUNDLE)?;
    let copied = refresh_fna_support_assets_from_fnalibs_dir(&tmp.join("fnalibs"), runtime_dir)?;
    let _ = fs::remove_dir_all(&tmp);
    Ok(copied)
}

fn repair_fna_support_assets_from_assets_archive(archive: &Path, runtime_dir: &Path) -> Result<usize, String> {
    let tmp = std::env::temp_dir().join("metalsharp-assets-fna-repair");
    let _ = fs::remove_dir_all(&tmp);
    fs::create_dir_all(&tmp).map_err(|e| format!("create {}: {}", tmp.display(), e))?;
    extract_zst(&archive.to_path_buf(), &tmp, ASSETS_BUNDLE)?;
    let assets = tmp.join("assets");
    let mut copied = refresh_fna_support_assets_from_fnalibs_dir(&assets.join("fnalibs"), runtime_dir)?;
    copied += refresh_fna_kickstart_from_dir(&assets.join("fna-kickstart").join("osx"), runtime_dir)?;
    let _ = fs::remove_dir_all(&tmp);
    Ok(copied)
}

fn refresh_fna_support_assets_from_fnalibs_dir(source: &Path, runtime_dir: &Path) -> Result<usize, String> {
    if !fna_support_source_dir_valid(source) {
        return Err(format!("FNA support source is invalid: {}", source.display()));
    }

    let fnalibs_dir = runtime_dir.join("fnalibs");
    let fmod_dir = fnalibs_dir.join("fmod");
    fs::create_dir_all(&fmod_dir).map_err(|e| format!("create {}: {}", fmod_dir.display(), e))?;

    let mut copied = 0usize;
    for name in ["libFNA3D.0.dylib", "libSDL2-2.0.0.dylib", "libFAudio.0.dylib", "libtheorafile.dylib"] {
        copy_file_overwrite(&source.join(name), &fnalibs_dir.join(name))?;
        copied += 1;
    }
    for name in ["libfmod.dylib", "libfmodstudio.dylib"] {
        copy_file_overwrite(&source.join("fmod").join(name), &fmod_dir.join(name))?;
        copied += 1;
    }

    let kick_osx = runtime_dir.join("fna-kickstart").join("osx");
    fs::create_dir_all(&kick_osx).map_err(|e| format!("create {}: {}", kick_osx.display(), e))?;
    for name in ["libFNA3D.0.dylib", "libSDL2-2.0.0.dylib", "libFAudio.0.dylib", "libtheorafile.dylib"] {
        copy_file_overwrite(&source.join(name), &kick_osx.join(name))?;
        copied += 1;
    }

    Ok(copied)
}

fn refresh_fna_kickstart_from_dir(source: &Path, runtime_dir: &Path) -> Result<usize, String> {
    if !fna_dylib_uses_sdl2(&source.join("libFNA3D.0.dylib"))
        || !fna_dylib_uses_sdl2(&source.join("libFAudio.0.dylib"))
        || !source.join("libSDL2-2.0.0.dylib").exists()
    {
        return Ok(0);
    }

    let kick_osx = runtime_dir.join("fna-kickstart").join("osx");
    fs::create_dir_all(&kick_osx).map_err(|e| format!("create {}: {}", kick_osx.display(), e))?;
    let mut copied = 0usize;
    for name in ["libFNA3D.0.dylib", "libSDL2-2.0.0.dylib", "libFAudio.0.dylib"] {
        copy_file_overwrite(&source.join(name), &kick_osx.join(name))?;
        copied += 1;
    }
    Ok(copied)
}

fn fna_support_source_dir_valid(source: &Path) -> bool {
    fna_dylib_uses_sdl2(&source.join("libFNA3D.0.dylib"))
        && fna_dylib_uses_sdl2(&source.join("libFAudio.0.dylib"))
        && source.join("libSDL2-2.0.0.dylib").exists()
        && fmod_dylib_has_payload(&source.join("fmod").join("libfmod.dylib"))
        && fmod_dylib_has_payload(&source.join("fmod").join("libfmodstudio.dylib"))
}

fn copy_file_overwrite(src: &Path, dst: &Path) -> Result<(), String> {
    if let Some(parent) = dst.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
    }
    fs::copy(src, dst).map(|_| ()).map_err(|e| format!("copy {} to {}: {}", src.display(), dst.display(), e))
}

fn mark_split_bundle_installed(home: &Path, bundle: &str, archive: &Path) {
    let Some(hash) = archive_sha256(archive) else {
        return;
    };
    let marker_dir = split_bundle_marker_dir(home);
    if fs::create_dir_all(&marker_dir).is_ok() {
        let _ = fs::write(marker_dir.join(format!("{}.sha256", bundle)), hash);
    }
}

fn archive_sha256(path: &Path) -> Option<String> {
    for (cmd, args) in [("/usr/bin/shasum", vec!["-a", "256"]), ("sha256sum", Vec::new())] {
        let Ok(output) = Command::new(cmd).args(args).arg(path).output() else {
            continue;
        };
        if !output.status.success() {
            continue;
        }
        let stdout = String::from_utf8_lossy(&output.stdout);
        let hash = stdout.split_whitespace().next()?.to_string();
        if hash.len() == 64 && hash.chars().all(|c| c.is_ascii_hexdigit()) {
            return Some(hash);
        }
    }
    None
}

fn find_packaged_host_runtime() -> Option<PathBuf> {
    if let Some(resources) = crate::platform::app_resources_dir() {
        let dir = resources.join("runtime").join("host");
        if host_runtime_ready(&dir) {
            return Some(dir);
        }
    }

    let dev = PathBuf::from("app/native/host");
    if host_runtime_ready(&dev) {
        return Some(dev);
    }

    if let Ok(exe) = std::env::current_exe() {
        let dev = exe.parent()?.parent()?.parent()?.parent()?.join("native").join("host");
        if host_runtime_ready(&dev) {
            return Some(dev);
        }
    }

    None
}

fn install_split_assets_bundle(home: &PathBuf) -> Result<bool, String> {
    let archive = find_bundled_archive(ASSETS_BUNDLE)
        .ok_or_else(|| "Support assets not found — metalsharp-assets.tar.zst is missing".to_string())?;
    let runtime_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime");
    let _ = fs::remove_dir_all(runtime_dir.join("eac-toggle"));
    if split_bundle_current(home, ASSETS_BUNDLE, &archive) {
        return Ok(false);
    }
    let tmp = std::env::temp_dir().join("metalsharp-assets-extract");
    let _ = fs::remove_dir_all(&tmp);
    let _ = fs::create_dir_all(&tmp);
    extract_zst(&archive, &tmp, ASSETS_BUNDLE)?;
    let assets = tmp.join("assets");

    let mut changed = false;
    for (src_name, dst_path) in [
        ("mono-x86", runtime_dir.join("mono-x86")),
        ("mono-arm64", runtime_dir.join("mono-arm64")),
        ("dxvk-1.10.3", runtime_dir.join("dxvk-1.10.3")),
        ("goldberg", runtime_dir.join("goldberg")),
        ("shims", runtime_dir.join("shims")),
        ("fnalibs", runtime_dir.join("fnalibs")),
        ("fna-kickstart", runtime_dir.join("fna-kickstart")),
        // Mono-route payloads (Phase 3): version-matched Unity Mono runtimes,
        // XNA 4.0 assembly set, SDL3, prebuilt launcher/patcher binaries.
        ("xna", runtime_dir.join("xna")),
        ("unity-mono", runtime_dir.join("unity-mono")),
        ("sdl3", runtime_dir.join("sdl3")),
        ("prebuilt-launchers", runtime_dir.join("prebuilt-launchers")),
    ] {
        let src = assets.join(src_name);
        if src.exists() {
            let _ = fs::remove_dir_all(&dst_path);
            copy_dir_recursive(&src, &dst_path)?;
            changed = true;
        }
    }

    // GPTK is Homebrew-owned. Ignore stale assets/gptk payloads that may exist
    // in old cached assets bundles so MetalSharp never stages or mixes GPTK
    // route DLLs/frameworks with Homebrew GPTK Wine.
    let _ = fs::remove_dir_all(runtime_dir.join("wine").join("lib").join("gptk"));
    let _ = fs::remove_dir_all(runtime_dir.join("wine").join("lib").join("external").join("D3DMetal.framework"));
    let _ = fs::remove_file(runtime_dir.join("wine").join("lib").join("external").join("libd3dshared.dylib"));

    let wine_etc = assets.join("wine").join("etc");
    if wine_etc.exists() {
        copy_dir_recursive(&wine_etc, &runtime_dir.join("wine").join("etc"))?;
        changed = true;
    }

    let shader_cache = assets.join("shader-cache");
    if shader_cache.exists() {
        copy_dir_recursive(&shader_cache, &crate::platform::metalsharp_home_dir_for(&home).join("shader-cache"))?;
        changed = true;
    }

    let _ = fs::remove_dir_all(&tmp);
    if changed {
        mark_split_bundle_installed(home, ASSETS_BUNDLE, &archive);
    }
    Ok(changed)
}

fn install_scripts_tools_bundle(home: &PathBuf) -> Result<bool, String> {
    let archive = find_bundled_archive(SCRIPTS_TOOLS_BUNDLE)
        .ok_or_else(|| "Scripts/tools bundle not found — metalsharp-scripts-tools.tar.zst is missing".to_string())?;
    if split_bundle_current(home, SCRIPTS_TOOLS_BUNDLE, &archive) {
        return Ok(false);
    }
    let dest = crate::platform::metalsharp_home_dir_for(&home).join("scripts").join("tools");
    let tmp = std::env::temp_dir().join("metalsharp-scripts-tools-extract");
    let _ = fs::remove_dir_all(&tmp);
    let _ = fs::create_dir_all(&tmp);
    extract_zst(&archive, &tmp, SCRIPTS_TOOLS_BUNDLE)?;
    let src = tmp.join("scripts").join("tools");
    let _ = fs::remove_dir_all(&dest);
    copy_dir_recursive(&src, &dest)?;
    let _ = fs::remove_dir_all(&tmp);
    mark_split_bundle_installed(home, SCRIPTS_TOOLS_BUNDLE, &archive);
    Ok(true)
}

fn eac_substrate_file_valid(path: &Path, elf: bool) -> bool {
    let Ok(bytes) = fs::read(path) else {
        return false;
    };
    if bytes.is_empty() {
        return false;
    }
    if elf {
        bytes.len() >= 4 && bytes.starts_with(b"\x7fELF")
    } else {
        macho_contains_x86_64(&bytes)
    }
}

fn macho_contains_x86_64(bytes: &[u8]) -> bool {
    if bytes.len() < 8 {
        return false;
    }

    match bytes[..4] {
        // MH_MAGIC_64: little-endian x86_64 thin Mach-O.
        [0xcf, 0xfa, 0xed, 0xfe] => bytes[4..8] == [0x07, 0x00, 0x00, 0x01],
        // MH_CIGAM_64: big-endian x86_64 thin Mach-O.
        [0xfe, 0xed, 0xfa, 0xcf] => bytes[4..8] == [0x01, 0x00, 0x00, 0x07],
        // FAT_MAGIC / FAT_CIGAM.  Each fat_arch starts with cputype.
        [0xca, 0xfe, 0xba, 0xbe] => fat_macho_contains_x86_64(bytes, false),
        [0xbe, 0xba, 0xfe, 0xca] => fat_macho_contains_x86_64(bytes, true),
        _ => false,
    }
}

fn fat_macho_contains_x86_64(bytes: &[u8], little_endian: bool) -> bool {
    let read_u32 = |offset: usize| -> Option<u32> {
        let field = bytes.get(offset..offset.checked_add(4)?)?;
        Some(if little_endian {
            u32::from_le_bytes(field.try_into().ok()?)
        } else {
            u32::from_be_bytes(field.try_into().ok()?)
        })
    };
    let Some(architecture_count) = read_u32(4) else {
        return false;
    };
    let max_architectures = (bytes.len().saturating_sub(8) / 20) as u32;
    if architecture_count > max_architectures {
        return false;
    }
    (0..architecture_count).any(|index| read_u32(8 + index as usize * 20) == Some(0x0100_0007))
}

pub(crate) fn eac_substrate_runtime_ready_for_ms_dir(ms_dir: &Path) -> bool {
    let eac_dir = ms_dir.join("runtime").join(EAC_RUNTIME_SUBDIR);
    eac_substrate_file_valid(&eac_dir.join(crate::anticheat::EAC_SUBSTRATE_FILENAME), false)
        && eac_substrate_file_valid(&eac_dir.join(crate::anticheat::EAC_SYMBOL_IMAGE_FILENAME), true)
}

pub(crate) fn eac_substrate_runtime_ready_for_home(home: &Path) -> bool {
    eac_substrate_runtime_ready_for_ms_dir(&crate::platform::metalsharp_home_dir_for(home))
}

fn eac_path_exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

fn remove_eac_path(path: &Path) {
    let Ok(metadata) = fs::symlink_metadata(path) else {
        return;
    };
    if metadata.file_type().is_symlink() || metadata.is_file() {
        let _ = fs::remove_file(path);
    } else if metadata.is_dir() {
        let _ = fs::remove_dir_all(path);
    }
}

fn install_eac_substrate_from_sources(
    home: &Path,
    substrate_source: &Path,
    symbol_source: &Path,
) -> Result<bool, String> {
    if !eac_substrate_file_valid(substrate_source, false) {
        return Err(format!("invalid MetalSharp EAC substrate dylib: {}", substrate_source.display()));
    }
    if !eac_substrate_file_valid(symbol_source, true) {
        return Err(format!("invalid MetalSharp EAC Linux symbol image: {}", symbol_source.display()));
    }

    let ms_dir = crate::platform::metalsharp_home_dir_for(home);
    let runtime_dir = ms_dir.join("runtime");
    let eac_dir = runtime_dir.join(EAC_RUNTIME_SUBDIR);
    let substrate_dest = eac_dir.join(crate::anticheat::EAC_SUBSTRATE_FILENAME);
    let symbol_dest = eac_dir.join(crate::anticheat::EAC_SYMBOL_IMAGE_FILENAME);
    let sources_match = crate::diagnostics::file_sha256(substrate_source)
        .zip(crate::diagnostics::file_sha256(symbol_source))
        .zip(crate::diagnostics::file_sha256(&substrate_dest).zip(crate::diagnostics::file_sha256(&symbol_dest)))
        .is_some_and(|((source_substrate, source_symbol), (dest_substrate, dest_symbol))| {
            source_substrate == dest_substrate && source_symbol == dest_symbol
        });
    if eac_substrate_runtime_ready_for_ms_dir(&ms_dir) && sources_match {
        return Ok(false);
    }

    fs::create_dir_all(&runtime_dir).map_err(|error| format!("create EAC runtime parent: {}", error))?;
    let unique = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or(0);
    let staging_dir = runtime_dir.join(format!(".eac-staging-{}-{}", std::process::id(), unique));
    let backup_dir = runtime_dir.join(format!(".eac-backup-{}-{}", std::process::id(), unique));
    remove_eac_path(&staging_dir);
    remove_eac_path(&backup_dir);
    fs::create_dir_all(&staging_dir).map_err(|error| format!("create EAC staging directory: {}", error))?;

    let staging_substrate = staging_dir.join(crate::anticheat::EAC_SUBSTRATE_FILENAME);
    let staging_symbol = staging_dir.join(crate::anticheat::EAC_SYMBOL_IMAGE_FILENAME);
    let stage_result = (|| {
        fs::copy(substrate_source, &staging_substrate).map_err(|error| format!("stage EAC substrate: {}", error))?;
        fs::copy(symbol_source, &staging_symbol).map_err(|error| format!("stage EAC symbol image: {}", error))?;
        if !eac_substrate_file_valid(&staging_substrate, false) || !eac_substrate_file_valid(&staging_symbol, true) {
            return Err("staged EAC substrate artifacts failed validation".to_string());
        }
        Ok(())
    })();
    if let Err(error) = stage_result {
        remove_eac_path(&staging_dir);
        return Err(error);
    }

    // Replace the pair as one directory.  Migration removes the whole runtime
    // tree, and an update can refresh one or both files; keeping the old
    // directory until the complete staged pair is ready prevents a half-new,
    // half-old EAC surface if either copy or validation fails.
    let had_previous = eac_path_exists(&eac_dir);
    if had_previous {
        if let Err(error) = fs::rename(&eac_dir, &backup_dir) {
            remove_eac_path(&staging_dir);
            return Err(format!("stage existing EAC runtime for replacement: {}", error));
        }
    }
    if let Err(error) = fs::rename(&staging_dir, &eac_dir) {
        if had_previous {
            let _ = fs::rename(&backup_dir, &eac_dir);
        }
        remove_eac_path(&staging_dir);
        return Err(format!("commit EAC runtime directory: {}", error));
    }

    if !eac_substrate_runtime_ready_for_ms_dir(&ms_dir) {
        remove_eac_path(&eac_dir);
        if had_previous {
            let _ = fs::rename(&backup_dir, &eac_dir);
        }
        return Err("EAC substrate installation completed but the durable artifacts are incomplete".to_string());
    }
    if had_previous {
        remove_eac_path(&backup_dir);
    }
    Ok(true)
}

/// Stage the two MetalSharp-owned EAC boundary artifacts into the durable
/// runtime tree.  The packaged app also carries them in Resources, but a
/// runtime copy makes first install, DMG replacement, and migration converge
/// on the same asset layout and lets the backend survive a missing resource
/// lookup after an update.
pub(crate) fn ensure_eac_substrate_runtime_ready(home: &PathBuf) -> Result<bool, String> {
    if !cfg!(target_os = "macos") {
        return Ok(false);
    }

    let substrate_source = crate::anticheat::eac_packaged_asset_path(crate::anticheat::EAC_SUBSTRATE_FILENAME)
        .ok_or_else(|| "MetalSharp EAC substrate dylib is missing from the packaged native assets".to_string())?;
    let symbol_source = crate::anticheat::eac_packaged_asset_path(crate::anticheat::EAC_SYMBOL_IMAGE_FILENAME)
        .ok_or_else(|| "MetalSharp EAC Linux symbol image is missing from the packaged native assets".to_string())?;
    install_eac_substrate_from_sources(home, &substrate_source, &symbol_source)
}

fn install_mono_x86_fallback(home: &PathBuf) -> Result<bool, String> {
    let mono_x86 =
        crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("mono-x86").join("bin").join("mono");
    if mono_x86.exists() {
        return Ok(false);
    }
    let bundled = find_bundled_archive("mono-x86");
    let runtime_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime");
    if let Some(archive) = bundled {
        extract_zst(&archive, &runtime_dir, "mono-x86")?;
        if mono_x86.exists() {
            return Ok(true);
        }
    }
    Err("mono x86 fallback not found".into())
}

fn install_dxvk_fallback(home: &PathBuf) -> Result<bool, String> {
    let dxvk_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("dxvk-1.10.3");
    if dxvk_dir.join("x32").join("d3d11.dll").exists() {
        return Ok(false);
    }
    let _ = fs::create_dir_all(&dxvk_dir);
    let bundled = find_bundled_archive("dxvk");
    if let Some(archive) = bundled {
        let tmp = std::env::temp_dir().join("metalsharp-dxvk-extract");
        let _ = fs::remove_dir_all(&tmp);
        let _ = fs::create_dir_all(&tmp);
        extract_zst(&archive, &tmp, "dxvk")?;
        let src = tmp.join("dxvk-1.10.3");
        if src.exists() {
            for subdir in &["x32", "x64"] {
                let s = src.join(subdir);
                if s.exists() {
                    let _ = fs::create_dir_all(dxvk_dir.join(subdir));
                    for entry in fs::read_dir(&s).map_err(|e| format!("read {}: {}", subdir, e))? {
                        let entry = entry.map_err(|e| e.to_string())?;
                        let _ = fs::copy(entry.path(), dxvk_dir.join(subdir).join(entry.file_name()));
                    }
                }
            }
        }
        let _ = fs::remove_dir_all(&tmp);
        if dxvk_dir.join("x32").join("d3d11.dll").exists() {
            return Ok(true);
        }
    }
    Err("DXVK fallback not found".into())
}

fn install_metalsharp_wine(home: &PathBuf) -> Result<bool, String> {
    let ms_wine = metalsharp_wine_binary(home);
    if ms_wine.exists() {
        return Ok(false);
    }

    let wine_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let _ = fs::create_dir_all(&wine_dir);

    let bundled = find_bundled_archive("wine");
    if let Some(archive) = bundled {
        extract_zst(&archive, &wine_dir, "wine")?;
        let ms_wine = metalsharp_wine_binary(home);
        if ms_wine.exists() {
            return Ok(true);
        }
    }

    Err("MetalSharp Wine not found — no bundled wine.tar.zst available".into())
}

pub fn ensure_dxmt_runtime_ready(home: &Path) -> Result<bool, String> {
    let dxmt_dir = dxmt_runtime_dir_for_home(home);
    if dxmt_runtime_current_for_dir(&dxmt_dir) {
        return Ok(false);
    }

    let home_buf = home.to_path_buf();
    let mut changed = false;
    changed |= ensure_runtime_bundle_assets(&home_buf)?;
    changed |= install_metalsharp_bundle(&home_buf)?;
    changed |= install_host_runtime(&home_buf)?;
    changed |= install_scripts_tools_bundle(&home_buf)?;
    changed |= install_dxmt_runtime(&home_buf)?;

    if dxmt_runtime_current_for_dir(&dxmt_dir) {
        Ok(changed)
    } else {
        Err(format!(
            "DXMT runtime {} is not ready after setup; missing files under {}",
            DXMT_BUNDLED_RUNTIME_VERSION,
            dxmt_dir.display()
        ))
    }
}

pub fn ensure_dxmt_m12_runtime_ready(home: &Path) -> Result<bool, String> {
    let dxmt_m12_dir = dxmt_m12_runtime_dir_for_home(home);
    if dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir) {
        return Ok(false);
    }

    let home_buf = home.to_path_buf();
    let mut changed = false;
    changed |= ensure_runtime_bundle_assets(&home_buf)?;
    changed |= install_metalsharp_bundle(&home_buf)?;
    changed |= install_host_runtime(&home_buf)?;
    changed |= install_scripts_tools_bundle(&home_buf)?;
    changed |= install_dxmt_m12_runtime(&home_buf)?;

    if dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir) {
        Ok(changed)
    } else {
        Err(format!(
            "M12 DXMT runtime {} is not ready after setup; missing files under {}",
            DXMT_BUNDLED_RUNTIME_VERSION,
            dxmt_m12_dir.display()
        ))
    }
}

pub fn ensure_graphics_runtimes_ready(home: &Path) -> Result<bool, String> {
    let dxmt_dir = dxmt_runtime_dir_for_home(home);
    let dxmt_m12_dir = dxmt_m12_runtime_dir_for_home(home);
    // Skip only when BOTH staged surfaces are current AND the bundled
    // metalsharp-graphics-dll.tar.zst has not changed since the last stage.
    // If the bundle carries new infrastructure (e.g. the i386 DXMT lanes for
    // M11(32)/M10(32)) the staged surface can look "current" by version alone
    // while still missing the new lanes, so we must re-extract (zst) and
    // re-stage to apply it during a migration update.
    if dxmt_runtime_current_for_dir(&dxmt_dir)
        && dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir)
        && !graphics_bundle_has_update(home)
        && m12_vulkan_runtime_ready_for_home(home)
    {
        // Legacy DXMT currency alone is insufficient: older installations can
        // have a current graphics marker yet lack the later vkd3d/DXVK/VKMT
        // M12 lanes. Run the M12 ensure on the fast path so it also repairs
        // Wine's direct-load MoltenVK mirror without re-staging healthy lanes.
        return match ensure_vkd3d_proton_runtime_ready(home) {
            Ok(changed) => Ok(changed),
            Err(err) => {
                eprintln!("setup: vkd3d-proton lanes not staged (DXMT fallback remains active): {}", err);
                Ok(false)
            },
        };
    }

    let home_buf = home.to_path_buf();
    let mut changed = false;
    // Only the DXMT surfaces consume metalsharp-graphics-dll.tar.zst, so when
    // we fall through here because the *graphics bundle* changed we re-stage
    // just those two — not the runtime/host/scripts bundles, which have their
    // own currency gates and would otherwise be needlessly re-extracted (the
    // UI stalls on "DXMT Graphics Runtimes" for ~minutes while they rerun).
    // The unrelated installers still run when the surfaces themselves are not
    // current (e.g. a fresh install or a version bump).
    let dxmt_current = dxmt_runtime_current_for_dir(&dxmt_dir);
    let m12_current = dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir);
    if !dxmt_current || !m12_current {
        changed |= ensure_runtime_bundle_assets(&home_buf)?;
        changed |= install_metalsharp_bundle(&home_buf)?;
        changed |= install_host_runtime(&home_buf)?;
        changed |= install_scripts_tools_bundle(&home_buf)?;
    }
    changed |= install_dxmt_runtime(&home_buf)?;
    changed |= install_dxmt_m12_runtime(&home_buf)?;

    // Stage the vkd3d-proton M12 stack (vkd3d-proton + dxvk + VKMT MoltenVK)
    // from the graphics bundle. Best-effort: if the bundle does not yet carry
    // the new lanes, DXMT remains the M12 fallback and setup still succeeds.
    if let Err(err) = ensure_vkd3d_proton_runtime_ready(home) {
        eprintln!("setup: vkd3d-proton lanes not staged (DXMT fallback remains active): {}", err);
    }

    if dxmt_runtime_current_for_dir(&dxmt_dir) && dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir) {
        Ok(changed)
    } else {
        Err(format!(
            "DXMT graphics runtimes {} are not ready after setup; legacy={} m12={}",
            DXMT_BUNDLED_RUNTIME_VERSION,
            dxmt_dir.display(),
            dxmt_m12_dir.display()
        ))
    }
}

pub fn ensure_m12_runtime_ready(home: &Path) -> Result<bool, String> {
    ensure_dxmt_m12_runtime_ready(home)
}

/// True when a bundled `metalsharp-graphics-dll.tar.zst` exists whose sha256
/// differs from the marker written by the last successful stage — i.e. the
/// bundle carries new content (such as the i386 DXMT lanes) that the staged
/// runtime surface has not yet absorbed. Returns false when no bundle is
/// present so the no-bundle fallback path keeps its existing behavior.
fn graphics_bundle_has_update(home: &Path) -> bool {
    match find_bundled_archive(GRAPHICS_DLL_BUNDLE) {
        Some(archive) => bundle_archive_has_update(home, GRAPHICS_DLL_BUNDLE, &archive),
        None => false,
    }
}

/// Pure, testable core of [`graphics_bundle_has_update`]: true when the given
/// archive's sha256 does not match the staged marker for `bundle`.
fn bundle_archive_has_update(home: &Path, bundle: &str, archive: &Path) -> bool {
    !split_bundle_current(home, bundle, archive)
}

pub fn ensure_gptk_runtime_ready(home: &Path) -> Result<bool, String> {
    install_gptk_runtime(&home.to_path_buf())
}

fn install_dxmt_runtime(home: &PathBuf) -> Result<bool, String> {
    let dxmt_dir = dxmt_runtime_dir_for_home(home);
    let bundled = find_bundled_archive(GRAPHICS_DLL_BUNDLE);
    if dxmt_runtime_current_for_dir(&dxmt_dir)
        && bundled.as_ref().is_some_and(|archive| split_bundle_current(home, GRAPHICS_DLL_BUNDLE, archive))
    {
        return Ok(false);
    }

    install_graphics_runtime_surface(
        home,
        "dxmt",
        &dxmt_dir,
        |dir| dxmt_runtime_ready(dir),
        |dir| dxmt_runtime_current_for_dir(dir),
        "fallback:~/metalsharp/runtime/dxmt",
    )
}

fn install_dxmt_m12_runtime(home: &PathBuf) -> Result<bool, String> {
    let dxmt_m12_dir = dxmt_m12_runtime_dir_for_home(home);
    let bundled = find_bundled_archive(GRAPHICS_DLL_BUNDLE);
    if dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir)
        && bundled.as_ref().is_some_and(|archive| split_bundle_current(home, GRAPHICS_DLL_BUNDLE, archive))
    {
        return Ok(false);
    }

    install_graphics_runtime_surface(
        home,
        "dxmt-m12",
        &dxmt_m12_dir,
        |dir| dxmt_m12_runtime_ready(dir),
        |dir| dxmt_m12_runtime_current_for_dir(dir),
        "fallback:~/metalsharp/runtime/dxmt-m12",
    )
}

fn install_graphics_runtime_surface(
    home: &PathBuf,
    bundle_surface: &str,
    dst_dir: &Path,
    files_ready: fn(&Path) -> bool,
    current: fn(&Path) -> bool,
    fallback_source: &str,
) -> Result<bool, String> {
    let _ = fs::create_dir_all(dst_dir.join("x86_64-unix"));
    let _ = fs::create_dir_all(dst_dir.join("x86_64-windows"));
    let _ = fs::create_dir_all(dst_dir.join("i386-unix"));
    let _ = fs::create_dir_all(dst_dir.join("i386-windows"));

    if let Some(archive) = find_bundled_archive(GRAPHICS_DLL_BUNDLE) {
        let tmp = std::env::temp_dir().join(format!("metalsharp-{}-extract", bundle_surface));
        let _ = fs::remove_dir_all(&tmp);
        let _ = fs::create_dir_all(&tmp);
        extract_zst(&archive, &tmp, GRAPHICS_DLL_BUNDLE)?;

        let src_root = tmp.join("Graphics").join("dll").join(bundle_surface);
        copy_graphics_runtime_surface(&src_root, dst_dir)?;
        ensure_dxmt_runtime_compat_files(dst_dir)?;
        write_dxmt_runtime_manifest(dst_dir, "bundled:metalsharp-graphics-dll.tar.zst")?;
        mark_split_bundle_installed(home, GRAPHICS_DLL_BUNDLE, &archive);
        let _ = fs::remove_dir_all(&tmp);
    } else {
        let fallback_surface = if bundle_surface == "dxmt-m12" { "dxmt-m12" } else { "dxmt" };
        let src_root = home.join("metalsharp").join("runtime").join(fallback_surface);
        if src_root.exists() {
            copy_graphics_runtime_surface(&src_root, dst_dir)?;
            ensure_dxmt_runtime_compat_files(dst_dir)?;
            if files_ready(dst_dir) {
                write_dxmt_runtime_manifest(dst_dir, fallback_source)?;
            }
        }
    }

    if current(dst_dir) {
        Ok(true)
    } else {
        Err(format!(
            "DXMT runtime surface {} {} not installed — bundle metalsharp-graphics-dll.tar.zst or place files in ~/.metalsharp/runtime/{}/",
            bundle_surface,
            DXMT_BUNDLED_RUNTIME_VERSION,
            bundle_surface
        ))
    }
}

fn dxmt_runtime_dir_for_home(home: &Path) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine").join("lib").join("dxmt")
}

fn dxmt_m12_runtime_dir_for_home(home: &Path) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine").join("lib").join("dxmt_m12")
}

fn dxmt_m12_runtime_dir_from_dxmt_dir(dxmt_dir: &Path) -> PathBuf {
    dxmt_dir.parent().unwrap_or(dxmt_dir).join("dxmt_m12")
}

pub fn dxmt_runtime_current_for_home(home: &Path) -> bool {
    dxmt_runtime_current_for_dir(&dxmt_runtime_dir_for_home(home))
}

pub fn dxmt_m12_runtime_current_for_home(home: &Path) -> bool {
    dxmt_m12_runtime_current_for_dir(&dxmt_m12_runtime_dir_for_home(home))
}

pub fn dxmt_m12_runtime_artifact_valid_for_home(home: &Path, rel: &str) -> bool {
    let Some((_, expected)) = DXMT_M12_EXPECTED_HASHES.iter().find(|(candidate, _)| *candidate == rel) else {
        return false;
    };
    crate::diagnostics::file_sha256(&dxmt_m12_runtime_dir_for_home(home).join(rel)).as_deref() == Some(*expected)
}

/// Mirror the VKMT MoltenVK lane into Wine's direct-load location. Wine's
/// Vulkan driver bypasses the route ICD/DYLD configuration and loads these
/// names from `lib/wine/x86_64-unix`, so they must be byte-identical to the
/// selected VKMT lane after both a fresh install and an upgrade.
fn sync_vkmt_moltenvk_into_wine_tree(wine_dir: &Path) -> Result<bool, String> {
    let source = wine_dir.join("lib").join("moltenvk-vkmt").join("libMoltenVK.dylib");
    if !source.is_file() {
        return Ok(false);
    }
    let source_bytes = fs::read(&source).map_err(|e| format!("read VKMT MoltenVK {}: {}", source.display(), e))?;
    let unix_dir = wine_dir.join("lib").join("wine").join("x86_64-unix");
    fs::create_dir_all(&unix_dir)
        .map_err(|e| format!("create Wine MoltenVK directory {}: {}", unix_dir.display(), e))?;

    let mut changed = false;
    for name in ["libMoltenVK.dylib", "libMoltenVK.1.dylib"] {
        let target = unix_dir.join(name);
        let matches_source = fs::read(&target).map(|bytes| bytes == source_bytes).unwrap_or(false);
        if !matches_source {
            fs::write(&target, &source_bytes)
                .map_err(|e| format!("sync VKMT MoltenVK {} -> {}: {}", source.display(), target.display(), e))?;
            changed = true;
        }
    }
    Ok(changed)
}

/// Stage the vkd3d-proton M12 stack from the graphics bundle:
/// `Graphics/dll/{vkd3d-proton,dxvk,moltenvk-vkmt}` -> the runtime wine lib
/// lanes. Installs when the bundle carries the lanes; returns Ok(true) when
/// the lanes are present and hash-valid afterwards.
pub fn ensure_vkd3d_proton_runtime_ready(home: &Path) -> Result<bool, String> {
    // Cheap read-only currency gate: only re-extract when a shipped lane
    // artifact is missing or hash-mismatched. The extraction below zstd-
    // decompresses the ENTIRE graphics bundle and rm -rf + re-copies the
    // vkd3d-proton/dxvk/moltenvk-vkmt dirs, and it runs on the backend's
    // single main thread — an ungated run freezes the app (Steam status
    // polls, launch requests) for the whole duration. Every M12 bottle save
    // and health check hits this; with current lanes it must be a no-op.
    let wine_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let vkd3d_dir = wine_dir.join("lib").join("vkd3d-proton");
    let dxvk_dir = wine_dir.join("lib").join("dxvk");
    let moltenvk_dir = wine_dir.join("lib").join("moltenvk-vkmt");

    // Wine's Vulkan driver bypasses the route ICD/DYLD configuration, so heal
    // any stale direct-load MoltenVK copy before checking whether the VKMT
    // lanes are current. This is content-based rather than size-based.
    sync_vkmt_moltenvk_into_wine_tree(&wine_dir)?;

    // Cheap read-only currency gate: only re-extract when a shipped lane
    // artifact is missing or hash-mismatched. The extraction below zstd-
    // decompresses the ENTIRE graphics bundle and rm -rf + re-copies the
    // vkd3d-proton/dxvk/moltenvk-vkmt dirs, and it runs on the backend's
    // single main thread — an ungated run freezes the app (Steam status
    // polls, launch requests) for the whole duration. Every M12 bottle save
    // and health check hits this; with current lanes it must be a no-op.
    if vkd3d_proton_runtime_current_for_home(home)
        && moltenvk_vkmt_runtime_ready_for_home(home)
        && dxvk_runtime_ready_for_home(home)
    {
        return Ok(false);
    }

    if let Some(archive) = find_bundled_archive(GRAPHICS_DLL_BUNDLE) {
        let tmp = std::env::temp_dir().join("metalsharp-vkd3d-extract");
        let _ = fs::remove_dir_all(&tmp);
        let _ = fs::create_dir_all(&tmp);
        extract_zst(&archive, &tmp, GRAPHICS_DLL_BUNDLE)?;

        let dll_root = tmp.join("Graphics").join("dll");
        for (bundle_surface, dst) in
            [("vkd3d-proton", &vkd3d_dir), ("dxvk", &dxvk_dir), ("moltenvk-vkmt", &moltenvk_dir)]
        {
            let src = dll_root.join(bundle_surface);
            if src.exists() {
                let _ = fs::remove_dir_all(dst);
                copy_dir_recursive(&src, dst)?;
            }
        }
        let _ = fs::remove_dir_all(&tmp);
        // Fresh installs and upgrades only have the new VKMT lane after the
        // copy above; synchronize again so Wine's direct-load path cannot
        // retain the old bundled MoltenVK.
        sync_vkmt_moltenvk_into_wine_tree(&wine_dir)?;
        mark_split_bundle_installed(home, GRAPHICS_DLL_BUNDLE, &archive);
    }

    ensure_moltenvk_vkmt_loader_alias(&moltenvk_dir)?;
    fix_moltenvk_icd_paths(&wine_dir);

    if vkd3d_proton_runtime_current_for_home(home)
        && moltenvk_vkmt_runtime_ready_for_home(home)
        && dxvk_runtime_ready_for_home(home)
    {
        Ok(true)
    } else {
        Err("vkd3d-proton M12 runtime lanes not installed — refresh the metalsharp-graphics-dll bundle".into())
    }
}

fn ensure_moltenvk_vkmt_loader_alias(moltenvk_dir: &Path) -> Result<(), String> {
    let source = moltenvk_dir.join("libMoltenVK.dylib");
    let versioned = moltenvk_dir.join("libMoltenVK.1.dylib");
    if !source.is_file() {
        return Err(format!("VKMT MoltenVK runtime missing {}", source.display()));
    }
    fs::copy(&source, &versioned)
        .map(|_| ())
        .map_err(|e| format!("failed to create VKMT MoltenVK loader alias {}: {}", versioned.display(), e))
}

pub fn dxmt_m12_runtime_artifact_path_for_home(home: &Path, rel: &str) -> PathBuf {
    dxmt_m12_runtime_dir_for_home(home).join(rel)
}

pub fn vkd3d_proton_runtime_dir_for_home(home: &Path) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine").join("lib").join("vkd3d-proton")
}

pub fn moltenvk_vkmt_runtime_dir_for_home(home: &Path) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine").join("lib").join("moltenvk-vkmt")
}

pub fn dxvk_runtime_dir_for_home(home: &Path) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine").join("lib").join("dxvk")
}

/// vkd3d-proton lane is current when every SHIPPED pinned artifact matches
/// its hash. Only the x86_64-windows surface is verified: the pinned i386
/// entries are aspirational (the vkd3d-proton bundle ships Windows DLLs for
/// x86_64-windows only — no i386, no unix sidecar), so requiring them would
/// make "current" permanently false and force an ungated full-bundle
/// re-extraction on every M12 bottle save.
pub fn vkd3d_proton_runtime_current_for_home(home: &Path) -> bool {
    let dir = vkd3d_proton_runtime_dir_for_home(home);
    VKD3D_PROTON_EXPECTED_HASHES
        .iter()
        .filter(|(rel, _)| rel.starts_with("x86_64-windows/"))
        .all(|(rel, expected)| crate::diagnostics::file_sha256(&dir.join(rel)).as_deref() == Some(*expected))
}

pub fn vkd3d_proton_runtime_artifact_valid_for_home(home: &Path, rel: &str) -> bool {
    let Some((_, expected)) = VKD3D_PROTON_EXPECTED_HASHES.iter().find(|(candidate, _)| *candidate == rel) else {
        return false;
    };
    crate::diagnostics::file_sha256(&vkd3d_proton_runtime_dir_for_home(home).join(rel)).as_deref() == Some(*expected)
}

pub fn vkd3d_proton_runtime_artifact_path_for_home(home: &Path, rel: &str) -> PathBuf {
    vkd3d_proton_runtime_dir_for_home(home).join(rel)
}

/// The VKMT MoltenVK lane is present when the patched dylib, Wine's versioned
/// loader alias, and the ICD exist.
pub fn moltenvk_vkmt_runtime_ready_for_home(home: &Path) -> bool {
    let wine_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let dir = wine_dir.join("lib").join("moltenvk-vkmt");
    moltenvk_vkmt_ready(&wine_dir)
        && dir.join("libMoltenVK.1.dylib").is_file()
        && dir.join("MoltenVK_icd.json").is_file()
}

/// The DXVK lane (dxgi/d3d11/d3d10/d3d9) is present for the x86_64 surface.
pub fn dxvk_runtime_ready_for_home(home: &Path) -> bool {
    let dir = dxvk_runtime_dir_for_home(home).join("x86_64-windows");
    ["dxgi.dll", "d3d11.dll", "d3d10core.dll", "d3d9.dll"].iter().all(|dll| dir.join(dll).is_file())
}

fn m12_vulkan_runtime_ready_for_home(home: &Path) -> bool {
    vkd3d_proton_runtime_current_for_home(home)
        && moltenvk_vkmt_runtime_ready_for_home(home)
        && dxvk_runtime_ready_for_home(home)
}

pub fn dxmt_runtime_current_for_ms_dir(ms_dir: &Path) -> bool {
    dxmt_runtime_current_for_dir(&ms_dir.join("runtime").join("wine").join("lib").join("dxmt"))
}

pub fn dxmt_m12_runtime_current_for_ms_dir(ms_dir: &Path) -> bool {
    dxmt_m12_runtime_current_for_dir(&ms_dir.join("runtime").join("wine").join("lib").join("dxmt_m12"))
}

pub fn dxmt_graphics_runtimes_current_for_ms_dir(ms_dir: &Path) -> bool {
    dxmt_runtime_current_for_ms_dir(ms_dir) && dxmt_m12_runtime_current_for_ms_dir(ms_dir)
}

pub fn dxmt_runtime_status() -> Value {
    let home = dirs::home_dir().unwrap_or_default();
    let dxmt_dir = dxmt_runtime_dir_for_home(&home);
    let dxmt_m12_dir = dxmt_m12_runtime_dir_for_home(&home);
    let installed_version = dxmt_runtime_installed_version(&dxmt_dir);
    let m12_installed_version = dxmt_runtime_installed_version(&dxmt_m12_dir);
    let files_ready = dxmt_runtime_ready(&dxmt_dir);
    let m12_files_ready = dxmt_m12_runtime_ready(&dxmt_m12_dir);
    let legacy_current = files_ready && installed_version.as_deref() == Some(DXMT_BUNDLED_RUNTIME_VERSION);
    let m12_current = m12_files_ready && m12_installed_version.as_deref() == Some(DXMT_BUNDLED_RUNTIME_VERSION);

    json!({
        "current": legacy_current,
        "filesReady": files_ready,
        "m12Current": m12_current,
        "m12FilesReady": m12_files_ready,
        "installedVersion": installed_version,
        "m12InstalledVersion": m12_installed_version,
        "requiredVersion": DXMT_BUNDLED_RUNTIME_VERSION,
        "manifestPath": dxmt_dir.join(DXMT_RUNTIME_MANIFEST).to_string_lossy(),
        "m12ManifestPath": dxmt_m12_dir.join(DXMT_RUNTIME_MANIFEST).to_string_lossy(),
        "path": dxmt_dir.to_string_lossy(),
        "m12Path": dxmt_m12_dir.to_string_lossy(),
        "dxmt": {
            "current": legacy_current,
            "filesReady": files_ready,
            "installedVersion": installed_version,
            "requiredVersion": DXMT_BUNDLED_RUNTIME_VERSION,
            "manifestPath": dxmt_dir.join(DXMT_RUNTIME_MANIFEST).to_string_lossy(),
            "path": dxmt_dir.to_string_lossy(),
        },
        "dxmt_m12": {
            "current": m12_current,
            "filesReady": m12_files_ready,
            "installedVersion": m12_installed_version,
            "requiredVersion": DXMT_BUNDLED_RUNTIME_VERSION,
            "manifestPath": dxmt_m12_dir.join(DXMT_RUNTIME_MANIFEST).to_string_lossy(),
            "path": dxmt_m12_dir.to_string_lossy(),
        },
    })
}

fn dxmt_runtime_current_for_dir(dxmt_dir: &Path) -> bool {
    dxmt_runtime_ready(dxmt_dir)
        && dxmt_runtime_installed_version(dxmt_dir).as_deref() == Some(DXMT_BUNDLED_RUNTIME_VERSION)
}

fn dxmt_m12_runtime_current_for_dir(dxmt_m12_dir: &Path) -> bool {
    dxmt_m12_runtime_ready(dxmt_m12_dir)
        && dxmt_m12_runtime_hashes_current(dxmt_m12_dir)
        && dxmt_runtime_installed_version(dxmt_m12_dir).as_deref() == Some(DXMT_BUNDLED_RUNTIME_VERSION)
}

fn dxmt_m12_runtime_hashes_current(dxmt_m12_dir: &Path) -> bool {
    DXMT_M12_EXPECTED_HASHES
        .iter()
        .all(|(rel, expected)| crate::diagnostics::file_sha256(&dxmt_m12_dir.join(rel)).as_deref() == Some(*expected))
}

fn dxmt_runtime_installed_version(dxmt_dir: &Path) -> Option<String> {
    let manifest = fs::read_to_string(dxmt_dir.join(DXMT_RUNTIME_MANIFEST)).ok()?;
    let value: Value = serde_json::from_str(&manifest).ok()?;
    let schema = value.get("schema").and_then(|v| v.as_str())?;
    if schema != DXMT_RUNTIME_SCHEMA {
        return None;
    }
    value.get("version").and_then(|v| v.as_str()).map(str::to_string)
}

fn write_dxmt_runtime_manifest(dxmt_dir: &Path, source: &str) -> Result<(), String> {
    let installed_at =
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or_default();
    let manifest = json!({
        "schema": DXMT_RUNTIME_SCHEMA,
        "version": DXMT_BUNDLED_RUNTIME_VERSION,
        "source": source,
        "installedAtUnix": installed_at,
        "requiredFiles": {
            "dxmt/x86_64-unix": DXMT_REQUIRED_UNIX,
            "x86_64-windows": DXMT_REQUIRED_PE,
            "dxmt/i386-unix": DXMT_REQUIRED_I386_UNIX,
            "i386-windows": DXMT_REQUIRED_I386_PE,
            "dxmt_m12/x86_64-unix": DXMT_M12_REQUIRED_UNIX,
            "dxmt_m12/x86_64-windows": DXMT_REQUIRED_PE,
        },
    });
    fs::write(dxmt_dir.join(DXMT_RUNTIME_MANIFEST), serde_json::to_string_pretty(&manifest).unwrap_or_default())
        .map_err(|e| format!("write DXMT runtime manifest: {}", e))
}

fn ensure_dxmt_runtime_compat_files(dxmt_dir: &Path) -> Result<(), String> {
    for lane in ["x86_64-windows", "i386-windows"] {
        let pe_dir = dxmt_dir.join(lane);
        let dxgi = pe_dir.join("dxgi.dll");
        let dxgi_dxmt = pe_dir.join("dxgi_dxmt.dll");

        if !file_nonempty(&dxgi_dxmt) && file_nonempty(&dxgi) {
            fs::copy(&dxgi, &dxgi_dxmt).map_err(|e| {
                format!(
                    "copy legacy DXMT dxgi.dll to dxgi_dxmt.dll: {} -> {}: {}",
                    dxgi.display(),
                    dxgi_dxmt.display(),
                    e
                )
            })?;
        }
    }

    Ok(())
}

fn copy_graphics_runtime_surface(src_root: &Path, dst_root: &Path) -> Result<(), String> {
    // Each lane is copied only if present in the bundle surface, so x86_64-only
    // bundles keep working and i386 lanes stage when shipped.
    for lane in ["x86_64-unix", "x86_64-windows", "i386-unix", "i386-windows"] {
        let src_lane = src_root.join(lane);
        if !src_lane.exists() {
            continue;
        }
        let dst_lane = dst_root.join(lane);
        fs::create_dir_all(&dst_lane).map_err(|e| format!("create DXMT lane dir {}: {}", dst_lane.display(), e))?;
        for entry in fs::read_dir(&src_lane).map_err(|e| format!("read {}: {}", src_lane.display(), e))? {
            let entry = entry.map_err(|e| e.to_string())?;
            fs::copy(entry.path(), dst_lane.join(entry.file_name())).map_err(|e| {
                format!(
                    "copy graphics lane file {} to {}: {}",
                    entry.path().display(),
                    dst_lane.join(entry.file_name()).display(),
                    e
                )
            })?;
        }
    }

    Ok(())
}

fn dxmt_runtime_ready(dxmt_dir: &Path) -> bool {
    let pe_dir = dxmt_dir.join("x86_64-windows");
    DXMT_REQUIRED_UNIX.iter().all(|name| file_nonempty(&dxmt_dir.join("x86_64-unix").join(name)))
        && DXMT_REQUIRED_PE.iter().all(|dll| file_nonempty(&pe_dir.join(dll)))
}

/// Phase 7: per-artifact verification report. Goes beyond the existing
/// `file_nonempty` presence checks by also recording sha256 and size, and by
/// reporting EACH required file individually (so a missing M12 sidecar is
/// visible by name, not a single boolean). Used by the runtime-verification
/// gate so a missing DLL/dylib/so sidecar is caught before gameplay.
pub fn runtime_artifact_report() -> Value {
    match dirs::home_dir() {
        Some(home) => runtime_artifact_report_for(&home),
        None => json!({"ok": false, "error": "home directory could not be resolved"}),
    }
}

/// Explicit-home variant used by tests so they never mutate the process-global
/// METALSHARP_HOME (which would race with other parallel tests).
pub fn runtime_artifact_report_for(home: &Path) -> Value {
    let dxmt_dir = dxmt_runtime_dir_for_home(home);
    let dxmt_m12_dir = dxmt_m12_runtime_dir_for_home(home);
    let m11 = verify_required_files("dxmt", &dxmt_dir, DXMT_REQUIRED_UNIX, DXMT_REQUIRED_PE);
    let m12 = verify_required_files_with_unix("dxmt_m12", &dxmt_m12_dir, DXMT_M12_REQUIRED_UNIX, DXMT_REQUIRED_PE);
    let ok = m11.get("all_present").and_then(|v| v.as_bool()).unwrap_or(false)
        && m12.get("all_present").and_then(|v| v.as_bool()).unwrap_or(false);
    json!({
        "ok": ok,
        "schema_version": 1,
        "dxmt": m11,
        "dxmt_m12": m12,
    })
}

fn verify_required_files(label: &str, runtime_dir: &Path, unix_required: &[&str], pe_required: &[&str]) -> Value {
    let mut entries = Vec::new();
    let mut all_present = true;
    for name in unix_required {
        let path = runtime_dir.join("x86_64-unix").join(name);
        let present = file_nonempty(&path);
        all_present &= present;
        entries.push(artifact_entry(label, "x86_64-unix", name, &path, present));
    }
    for dll in pe_required {
        let path = runtime_dir.join("x86_64-windows").join(dll);
        let present = file_nonempty(&path);
        all_present &= present;
        entries.push(artifact_entry(label, "x86_64-windows", dll, &path, present));
    }
    json!({
        "all_present": all_present,
        "entries": entries,
    })
}

fn verify_required_files_with_unix(
    label: &str,
    runtime_dir: &Path,
    unix_required: &[&str],
    pe_required: &[&str],
) -> Value {
    // M12 lane has its OWN required unix set (winemetal.so + libc++ dylibs +
    // libunwind). This is the same shape as verify_required_files but takes the
    // M12 unix list explicitly so the report names each sidecar.
    verify_required_files(label, runtime_dir, unix_required, pe_required)
}

fn artifact_entry(label: &str, subdir: &str, name: &str, path: &Path, present: bool) -> Value {
    let sha = if present { crate::diagnostics::file_sha256(path) } else { None };
    let size = if present { fs::metadata(path).ok().map(|m| m.len()) } else { None };
    json!({
        "label": label,
        "subdir": subdir,
        "filename": name,
        "path": path.to_string_lossy(),
        "present": present,
        "sha256": sha,
        "size_bytes": size,
    })
}

/// Phase 7: explicitly named missing M12 sidecars, for the regression test
/// ("runtime verification catches missing M12 sidecars before gameplay").
pub fn missing_m12_sidecars() -> Vec<String> {
    dirs::home_dir().map(|home| missing_m12_sidecars_for(&home)).unwrap_or_default()
}

/// Explicit-home variant used by tests.
pub fn missing_m12_sidecars_for(home: &Path) -> Vec<String> {
    let dxmt_m12_dir = dxmt_m12_runtime_dir_for_home(home);
    let pe_dir = dxmt_m12_dir.join("x86_64-windows");
    let unix_dir = dxmt_m12_dir.join("x86_64-unix");
    let mut missing = Vec::new();
    for name in DXMT_M12_REQUIRED_UNIX {
        if !file_nonempty(&unix_dir.join(name)) {
            missing.push(format!("dxmt_m12/x86_64-unix/{}", name));
        }
    }
    for dll in DXMT_REQUIRED_PE {
        if !file_nonempty(&pe_dir.join(dll)) {
            missing.push(format!("dxmt_m12/x86_64-windows/{}", dll));
        }
    }
    missing
}

fn dxmt_m12_runtime_ready(dxmt_m12_dir: &Path) -> bool {
    let pe_dir = dxmt_m12_dir.join("x86_64-windows");
    DXMT_M12_REQUIRED_UNIX.iter().all(|name| file_nonempty(&dxmt_m12_dir.join("x86_64-unix").join(name)))
        && DXMT_REQUIRED_PE.iter().all(|dll| file_nonempty(&pe_dir.join(dll)))
}

fn install_gptk_runtime(_home: &PathBuf) -> Result<bool, String> {
    let was_installed = crate::platform::gptk_homebrew_installed();
    if !was_installed {
        brew_trust_cask("gcenx/wine/game-porting-toolkit")?;
        brew_install("game-porting-toolkit")?;
    }
    if !crate::platform::gptk_homebrew_installed() {
        return Err("GPTK installed via Homebrew but wine64/wineserver were not found under /Applications/Game Porting Toolkit.app".into());
    }
    let wine_root = crate::platform::gptk_homebrew_wine_root();
    let pe_dir = wine_root.join("lib").join("wine").join("x86_64-windows");
    let framework = wine_root.join("lib").join("external").join("D3DMetal.framework");
    if !gptk_runtime_ready(&pe_dir, &framework) {
        return Err("Homebrew GPTK payload is incomplete; reinstall game-porting-toolkit".into());
    }
    Ok(!was_installed)
}

fn gptk_runtime_ready(pe_dir: &Path, framework: &Path) -> bool {
    let required_pe = ["d3d10.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvapi64.dll", "nvngx-on-metalfx.dll"];
    required_pe.iter().all(|dll| file_nonempty(&pe_dir.join(dll)))
        && file_nonempty(&framework.join("Versions").join("A").join("D3DMetal"))
        && framework_has_resource_dylib(framework)
}

fn framework_has_resource_dylib(framework: &Path) -> bool {
    for resources_dir in [framework.join("Resources"), framework.join("Versions").join("A").join("Resources")] {
        if let Ok(entries) = fs::read_dir(resources_dir) {
            if entries.flatten().any(|entry| {
                entry.path().extension().and_then(|ext| ext.to_str()) == Some("dylib") && file_nonempty(&entry.path())
            }) {
                return true;
            }
        }
    }
    false
}

fn install_goldberg(home: &PathBuf) -> Result<bool, String> {
    let goldberg_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("goldberg");
    let x86_dll = goldberg_dir.join("x86").join("steam_api.dll");
    let x64_dll = goldberg_dir.join("x64").join("steam_api64.dll");
    let sc64_dll = goldberg_dir.join("steamclient").join("steamclient64.dll");

    if x86_dll.exists() && x64_dll.exists() {
        // steamclient is optional — only check core DLLs.
        return Ok(false);
    }

    let _ = fs::create_dir_all(goldberg_dir.join("x86"));
    let _ = fs::create_dir_all(goldberg_dir.join("x64"));
    let _ = fs::create_dir_all(goldberg_dir.join("steamclient"));

    let bundled = find_bundled_archive("goldberg");
    if let Some(archive) = bundled {
        let tmp = std::env::temp_dir().join("metalsharp-goldberg-extract");
        let _ = fs::remove_dir_all(&tmp);
        let _ = fs::create_dir_all(&tmp);
        extract_zst(&archive, &tmp, "goldberg")?;

        let src_x86 = tmp.join("x86");
        let src_x64 = tmp.join("x64");
        let src_sc = tmp.join("steamclient");

        if src_x86.exists() {
            for entry in fs::read_dir(&src_x86).map_err(|e| format!("read x86: {}", e))? {
                let entry = entry.map_err(|e| e.to_string())?;
                let _ = fs::copy(entry.path(), goldberg_dir.join("x86").join(entry.file_name()));
            }
        }
        if src_x64.exists() {
            for entry in fs::read_dir(&src_x64).map_err(|e| format!("read x64: {}", e))? {
                let entry = entry.map_err(|e| e.to_string())?;
                let _ = fs::copy(entry.path(), goldberg_dir.join("x64").join(entry.file_name()));
            }
        }
        if src_sc.exists() {
            for entry in fs::read_dir(&src_sc).map_err(|e| format!("read steamclient: {}", e))? {
                let entry = entry.map_err(|e| e.to_string())?;
                let _ = fs::copy(entry.path(), goldberg_dir.join("steamclient").join(entry.file_name()));
            }
        }

        let _ = fs::remove_dir_all(&tmp);
    }

    if goldberg_dir.join("x86").join("steam_api.dll").exists()
        && goldberg_dir.join("x64").join("steam_api64.dll").exists()
    {
        Ok(true)
    } else {
        Err("Goldberg Steam emulator not found — goldberg.tar.zst missing from bundles".into())
    }
}

fn install_steam_bridge(home: &PathBuf) -> Result<bool, String> {
    let bridge_dir = crate::platform::metalsharp_home_dir_for(home).join("runtime").join("steam-bridge");
    let shim_dst = bridge_dir.join("libsteam_api.dylib");

    let wine_dir = crate::platform::metalsharp_home_dir_for(home).join("runtime").join("wine");
    fix_moltenvk_icd_paths(&wine_dir);
    if !moltenvk_ready(&wine_dir) {
        eprintln!("steam-bridge: warning — MoltenVK not found in Wine runtime, CEF webhelper may not render");
    }

    if shim_dst.exists() {
        return Ok(false);
    }

    let _ = fs::create_dir_all(&bridge_dir);

    let shims_dylib =
        crate::platform::metalsharp_home_dir_for(home).join("runtime").join("shims").join("libsteam_api.dylib");
    if shims_dylib.exists() {
        fs::copy(&shims_dylib, &shim_dst).map_err(|e| format!("copy steam bridge shim: {}", e))?;
    }

    if shim_dst.exists() {
        Ok(true)
    } else {
        Ok(false)
    }
}

fn install_mtsp_rules(home: &PathBuf) -> Result<bool, String> {
    let dest = crate::platform::metalsharp_home_dir_for(&home).join("configs").join("mtsp-rules.toml");
    let mut candidates = vec![
        PathBuf::from("configs/mtsp-rules.toml"),
        crate::platform::metalsharp_home_dir_for(&home)
            .join("scripts")
            .join("tools")
            .join("configs")
            .join("mtsp-rules.toml"),
        home.join("metalsharp").join("configs").join("mtsp-rules.toml"),
        home.join("repos").join("metalsharp").join("configs").join("mtsp-rules.toml"),
    ];

    if let Ok(exe) = std::env::current_exe() {
        if let Some(mut dir) = exe.parent() {
            for _ in 0..8 {
                candidates.push(dir.join("configs").join("mtsp-rules.toml"));
                candidates.push(dir.join("scripts").join("tools").join("configs").join("mtsp-rules.toml"));
                match dir.parent() {
                    Some(p) => dir = p,
                    None => break,
                }
            }
        }
    }

    install_mtsp_rules_from_candidates(&dest, &candidates)
}

fn install_mtsp_rules_from_candidates(dest: &Path, candidates: &[PathBuf]) -> Result<bool, String> {
    for src in candidates {
        if src.exists() {
            if let Ok(contents) = fs::read_to_string(src) {
                if let Ok(existing) = fs::read_to_string(dest) {
                    if existing == contents {
                        return Ok(false);
                    }
                    let backup = dest.with_extension("toml.bak");
                    let _ = fs::write(&backup, existing);
                }
                fs::create_dir_all(dest.parent().unwrap()).map_err(|e| format!("create MTSP config dir: {}", e))?;
                fs::write(dest, &contents).map_err(|e| format!("write mtsp-rules.toml: {}", e))?;
                if fs::read_to_string(dest).ok().as_deref() == Some(contents.as_str()) {
                    return Ok(true);
                }
                return Err("mtsp-rules.toml was written but could not be verified".into());
            }
        }
    }

    Ok(false)
}

fn install_mono_configs(home: &PathBuf) -> Result<bool, String> {
    let configs_dir = crate::platform::metalsharp_home_dir_for(&home).join("configs");
    let _ = fs::create_dir_all(&configs_dir);

    let config_files =
        ["terraria-mono.config", "celeste-x86-mono.config", "stardew-mono.config", "generic-fna-mono.config"];
    let mut any_installed = false;

    for name in &config_files {
        let dest = configs_dir.join(name);
        if dest.exists() {
            continue;
        }

        let mut candidates = vec![
            PathBuf::from(format!("configs/{}", name)),
            crate::platform::metalsharp_home_dir_for(&home).join("configs").join(name),
            home.join("repos").join("metalsharp").join("configs").join(name),
        ];

        if let Ok(exe) = std::env::current_exe() {
            if let Some(mut dir) = exe.parent() {
                for _ in 0..8 {
                    candidates.push(dir.join("configs").join(name));
                    candidates.push(dir.join("scripts").join("tools").join("configs").join(name));
                    match dir.parent() {
                        Some(p) => dir = p,
                        None => break,
                    }
                }
            }
        }

        for src in &candidates {
            if src.exists() {
                if let Ok(contents) = fs::read_to_string(src) {
                    let _ = fs::write(&dest, &contents);
                    if dest.exists() {
                        any_installed = true;
                        break;
                    }
                }
            }
        }
    }

    Ok(any_installed)
}

fn install_windows_steam(home: &PathBuf) -> Result<bool, String> {
    let steam_exe = home
        .join(".metalsharp")
        .join("prefix-steam")
        .join("drive_c")
        .join("Program Files (x86)")
        .join("Steam")
        .join("Steam.exe");
    if steam_exe.exists() {
        return Ok(false);
    }

    let ms_wine = metalsharp_wine_binary(home);
    if !ms_wine.exists() {
        return Err("MetalSharp Wine not found — cannot install Steam".into());
    }

    let installer = crate::platform::metalsharp_home_dir_for(&home).join("SteamSetup.exe");

    let _ = fs::remove_file(&installer);
    crate::steam::stage_verified_steam_setup(&installer)?;

    let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
    let _ = fs::create_dir_all(&prefix);

    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let mut wineboot_cmd = Command::new(&ms_wine);
    wineboot_cmd
        .env("WINEPREFIX", prefix.to_string_lossy().to_string())
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .arg("wineboot")
        .arg("--init")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut wineboot_cmd, &ms_root);
    let _ = wineboot_cmd.status();
    crate::steam::stage_verified_steam_setup(&installer)?;

    let mut install_cmd = Command::new(&ms_wine);
    install_cmd
        .env("WINEPREFIX", prefix.to_string_lossy().to_string())
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .arg(&installer)
        .args(["/S"])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut install_cmd, &ms_root);
    let _ = install_cmd.spawn().map_err(|e| format!("Steam install spawn failed: {}", e))?;

    for _ in 0..90 {
        std::thread::sleep(Duration::from_secs(2));
        if steam_exe.exists() {
            let steam_dir = crate::platform::metalsharp_home_dir_for(&home)
                .join("prefix-steam")
                .join("drive_c")
                .join("Program Files (x86)")
                .join("Steam");
            crate::steam::deploy_steamwebhelper_wrapper(&steam_dir);
            return Ok(true);
        }
    }

    if steam_exe.exists() {
        let steam_dir = crate::platform::metalsharp_home_dir_for(&home)
            .join("prefix-steam")
            .join("drive_c")
            .join("Program Files (x86)")
            .join("Steam");
        crate::steam::deploy_steamwebhelper_wrapper(&steam_dir);
        Ok(true)
    } else {
        Err("Steam.exe not found after installation — may need manual install".into())
    }
}

fn check_command(cmd: &str) -> bool {
    find_system_command(cmd).is_some()
}

fn find_system_command(cmd: &str) -> Option<PathBuf> {
    let candidates = match cmd {
        "which" => vec![PathBuf::from("/usr/bin/which")],
        "brew" => vec![PathBuf::from("/opt/homebrew/bin/brew"), PathBuf::from("/usr/local/bin/brew")],
        "mono" => vec![
            PathBuf::from("/opt/homebrew/bin/mono"),
            PathBuf::from("/usr/local/bin/mono"),
            PathBuf::from("/usr/bin/mono"),
        ],
        "wine" => vec![PathBuf::from("/usr/bin/wine"), PathBuf::from("/usr/local/bin/wine")],
        "innoextract" => {
            vec![PathBuf::from("/opt/homebrew/bin/innoextract"), PathBuf::from("/usr/local/bin/innoextract")]
        },
        _ => vec![PathBuf::from(cmd)],
    };
    for c in &candidates {
        if c.exists() {
            return Some(c.clone());
        }
    }
    Command::new("/usr/bin/which")
        .arg(cmd)
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
        .then(|| {
            let output = Command::new("/usr/bin/which").arg(cmd).output().ok()?;
            let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
            (!path.is_empty()).then_some(PathBuf::from(path))
        })
        .flatten()
}

fn make_executable(path: &PathBuf) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if let Ok(metadata) = fs::metadata(path) {
            let mut permissions = metadata.permissions();
            permissions.set_mode(0o755);
            let _ = fs::set_permissions(path, permissions);
        }
    }
}

pub fn ensure_zstd() -> Result<bool, String> {
    if check_command("unzstd") {
        return Ok(false);
    }
    brew_install("zstd")
}

pub fn innoextract_binary() -> Result<PathBuf, String> {
    if let Some(path) = find_system_command("innoextract") {
        return Ok(path);
    }
    brew_install("innoextract")?;
    find_system_command("innoextract").ok_or_else(|| "innoextract was installed but its binary was not found".into())
}

fn install_mono_arm64() -> Result<bool, String> {
    if check_command("mono") {
        return Ok(false);
    }

    let home = dirs::home_dir().ok_or("Cannot find home directory")?;
    let mono_arm64 =
        crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("mono-arm64").join("bin").join("mono");
    if mono_arm64.exists() {
        return Ok(false);
    }

    let bundled = find_bundled_archive("mono-arm64");
    let runtime_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime");
    let _ = fs::create_dir_all(&runtime_dir);

    if let Some(archive) = bundled {
        extract_zst(&archive, &runtime_dir, "mono-arm64")?;
        if mono_arm64.exists() {
            return Ok(true);
        }
    }

    brew_install("mono")
}

fn install_moltenvk() -> Result<bool, String> {
    let icd = PathBuf::from("/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json");
    if icd.exists() {
        return Ok(false);
    }

    let bundled = find_bundled_archive("moltenvk");
    if let Some(archive) = bundled {
        let cellar = PathBuf::from("/opt/homebrew/Cellar/molten-vk");
        let _ = fs::create_dir_all(&cellar);
        extract_zst(&archive, &cellar, "moltenvk")?;
        if icd.exists() {
            return Ok(true);
        }
    }

    brew_install("molten-vk")
}

fn find_brew() -> Result<PathBuf, String> {
    let candidates = [PathBuf::from("/opt/homebrew/bin/brew"), PathBuf::from("/usr/local/bin/brew")];
    for c in &candidates {
        if c.exists() {
            return Ok(c.clone());
        }
    }
    let output = mac_cmd("which").arg("brew").output().ok();
    if let Some(o) = output {
        if o.status.success() {
            let path = String::from_utf8_lossy(&o.stdout).trim().to_string();
            if !path.is_empty() {
                return Ok(PathBuf::from(path));
            }
        }
    }
    Err("Homebrew not found — install it first".into())
}

fn brew_install(package: &str) -> Result<bool, String> {
    let brew = find_brew()?;
    let output = Command::new(&brew).args(["install", package]).output().map_err(|e| format!("brew failed: {}", e))?;

    let combined = format!("{}{}", String::from_utf8_lossy(&output.stdout), String::from_utf8_lossy(&output.stderr));

    if output.status.success() || combined.contains("already installed") {
        Ok(true)
    } else {
        Err(combined.lines().last().unwrap_or("brew install failed").into())
    }
}

fn brew_trust_cask(cask: &str) -> Result<bool, String> {
    let brew = find_brew()?;
    let output = Command::new(&brew)
        .args(["trust", "--cask", cask])
        .output()
        .map_err(|e| format!("brew trust failed: {}", e))?;
    let combined = format!("{}{}", String::from_utf8_lossy(&output.stdout), String::from_utf8_lossy(&output.stderr));
    if output.status.success() || combined.contains("Trusted cask") || combined.contains("already trusted") {
        Ok(true)
    } else {
        Err(combined.lines().last().unwrap_or("brew trust failed").into())
    }
}

fn find_bundled_archive(name: &str) -> Option<PathBuf> {
    let candidates = [find_in_resources(name), find_in_dev_path(name)];

    if let Some(found) =
        candidates.into_iter().find(|c| c.as_ref().is_some_and(|path| bundled_artifact_valid(name, path))).flatten()
    {
        return Some(found);
    }

    download_from_github_release(&format!("{}.tar.zst", name))
}

fn download_bundled_file(name: &str) -> Option<PathBuf> {
    let cache_dir = crate::platform::metalsharp_home_dir().join("cache").join("bundles");
    let _ = fs::create_dir_all(&cache_dir);
    let cached = cache_dir.join(name);
    let tmp = cache_dir.join(format!("{}.download", name));

    if file_nonempty(&cached) && bundled_artifact_valid(name, &cached) {
        return Some(cached);
    }

    let url = format!("https://github.com/aaf2tbz/metalsharp/releases/download/bundles/{}", name);

    let _ = fs::remove_file(&tmp);

    for retry in 0..3 {
        let output = mac_cmd("curl")
            .args([
                "--fail",
                "--location",
                "--silent",
                "--show-error",
                "--retry",
                "2",
                "--connect-timeout",
                "30",
                "--max-time",
                "600",
                "-o",
            ])
            .arg(&tmp)
            .arg(&url)
            .output();

        match output {
            Ok(o) if o.status.success() && file_nonempty(&tmp) => {
                if bundled_artifact_valid(name, &tmp) {
                    if fs::rename(&tmp, &cached).or_else(|_| fs::copy(&tmp, &cached).map(|_| ())).is_ok() {
                        let _ = fs::remove_file(&tmp);
                        return Some(cached);
                    }
                    let _ = fs::remove_file(&tmp);
                    return None;
                } else {
                    let _ = fs::remove_file(&tmp);
                    if retry < 2 {
                        std::thread::sleep(std::time::Duration::from_secs(1));
                    }
                }
            },
            Ok(_) => {
                let _ = fs::remove_file(&tmp);
                if retry < 2 {
                    std::thread::sleep(std::time::Duration::from_secs(2));
                }
            },
            Err(_) => {
                let _ = fs::remove_file(&tmp);
                if retry < 2 {
                    std::thread::sleep(std::time::Duration::from_secs(2));
                }
            },
        }
    }

    None
}

fn download_from_github_release(filename: &str) -> Option<PathBuf> {
    download_bundled_file(filename)
}

fn bundled_artifact_valid(name: &str, path: &Path) -> bool {
    if !file_nonempty(path) {
        return false;
    }

    if name == RUNTIME_BUNDLE || name == "metalsharp-runtime.tar.zst" {
        return archive_required_files_valid(path, RUNTIME_REQUIRED_ARCHIVE_FILES);
    }

    if name == GRAPHICS_DLL_BUNDLE || name == "metalsharp-graphics-dll.tar.zst" {
        return archive_required_files_valid(path, GRAPHICS_REQUIRED_ARCHIVE_FILES) && archive_m12_hashes_valid(path);
    }

    if name == ASSETS_BUNDLE || name == "metalsharp-assets.tar.zst" {
        return archive_required_files_valid(path, ASSETS_REQUIRED_ARCHIVE_FILES)
            && archive_fna_support_payloads_valid(path, "assets/fnalibs")
            && archive_fna_kickstart_payloads_valid(path, "assets/fna-kickstart/osx");
    }

    if name == FNALIBS_BUNDLE || name == "fnalibs.tar.zst" {
        return archive_required_files_valid(path, FNALIBS_REQUIRED_ARCHIVE_FILES)
            && archive_fna_support_payloads_valid(path, "fnalibs");
    }

    if name == SCRIPTS_TOOLS_BUNDLE || name == "metalsharp-scripts-tools.tar.zst" {
        return archive_required_files_valid(path, SCRIPTS_TOOLS_REQUIRED_ARCHIVE_FILES);
    }

    if name == STEAM_BUNDLE || name == "metalsharp-steam.tar.zst" {
        return archive_required_files_valid(path, STEAM_REQUIRED_ARCHIVE_FILES);
    }

    true
}

fn archive_m12_hashes_valid(path: &Path) -> bool {
    let tmp = std::env::temp_dir().join(format!(
        "metalsharp-m12-hash-validate-{}-{}",
        std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
    ));
    let _ = fs::remove_dir_all(&tmp);
    if fs::create_dir_all(&tmp).is_err() {
        return false;
    }

    let hash_sets: &[(&str, &[(&str, &str)])] = &[
        ("dxmt-m12", DXMT_M12_EXPECTED_HASHES),
        ("vkd3d-proton", VKD3D_PROTON_EXPECTED_HASHES),
        ("moltenvk-vkmt", MOLTENVK_VKMT_EXPECTED_HASHES),
    ];
    let archive_paths: Vec<String> = hash_sets
        .iter()
        .flat_map(|(lane, expected_hashes)| {
            expected_hashes.iter().map(move |(rel, _)| format!("Graphics/dll/{lane}/{rel}"))
        })
        .collect();
    let archive_args: Vec<&str> = archive_paths.iter().map(String::as_str).collect();
    let extracted = extract_archive_files(path, &tmp, &archive_args);
    let valid = extracted
        && hash_sets.iter().all(|(lane, expected_hashes)| {
            expected_hashes.iter().all(|(rel, expected)| {
                let extracted_path = tmp.join("Graphics").join("dll").join(lane).join(rel);
                crate::diagnostics::file_sha256(&extracted_path).as_deref() == Some(*expected)
            })
        });

    let _ = fs::remove_dir_all(&tmp);
    valid
}

fn archive_required_files_valid(path: &Path, required_files: &[&str]) -> bool {
    let tmp = std::env::temp_dir().join(format!(
        "metalsharp-archive-validate-{}-{}",
        std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
    ));
    let _ = fs::remove_dir_all(&tmp);
    if fs::create_dir_all(&tmp).is_err() {
        return false;
    }

    let file = match fs::File::open(path) {
        Ok(f) => f,
        Err(_) => {
            let _ = fs::remove_dir_all(&tmp);
            return false;
        },
    };
    let mut decoder = match zstd::Decoder::new(file) {
        Ok(d) => d,
        Err(_) => {
            let _ = fs::remove_dir_all(&tmp);
            return false;
        },
    };

    let mut tar_cmd = match mac_cmd("tar")
        .args(["-xf", "-"])
        .arg("-C")
        .arg(&tmp)
        .args(required_files)
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            let _ = fs::remove_dir_all(&tmp);
            return false;
        },
    };

    if let Some(mut stdin) = tar_cmd.stdin.take() {
        let _ = std::io::copy(&mut decoder, &mut stdin);
    }

    let ready = tar_cmd.wait().map(|s| s.success()).unwrap_or(false)
        && required_files.iter().all(|required| file_nonempty(&tmp.join(required)));
    let _ = fs::remove_dir_all(&tmp);
    ready
}

fn archive_fna_support_payloads_valid(path: &Path, root: &str) -> bool {
    let tmp = std::env::temp_dir().join(format!(
        "metalsharp-fna-validate-{}-{}",
        std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
    ));
    let _ = fs::remove_dir_all(&tmp);
    if fs::create_dir_all(&tmp).is_err() {
        return false;
    }

    let files = [
        format!("{}/libFNA3D.0.dylib", root),
        format!("{}/libFAudio.0.dylib", root),
        format!("{}/libSDL2-2.0.0.dylib", root),
        format!("{}/fmod/libfmod.dylib", root),
        format!("{}/fmod/libfmodstudio.dylib", root),
    ];
    let file_args: Vec<&str> = files.iter().map(String::as_str).collect();
    let extracted = extract_archive_files(path, &tmp, &file_args);
    let root_path = tmp.join(root);
    let valid = extracted
        && fna_dylib_uses_sdl2(&root_path.join("libFNA3D.0.dylib"))
        && fna_dylib_uses_sdl2(&root_path.join("libFAudio.0.dylib"))
        && root_path.join("libSDL2-2.0.0.dylib").exists()
        && fmod_dylib_has_payload(&root_path.join("fmod").join("libfmod.dylib"))
        && fmod_dylib_has_payload(&root_path.join("fmod").join("libfmodstudio.dylib"));

    let _ = fs::remove_dir_all(&tmp);
    valid
}

fn archive_fna_kickstart_payloads_valid(path: &Path, root: &str) -> bool {
    let tmp = std::env::temp_dir().join(format!(
        "metalsharp-fna-kick-validate-{}-{}",
        std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
    ));
    let _ = fs::remove_dir_all(&tmp);
    if fs::create_dir_all(&tmp).is_err() {
        return false;
    }

    let files = [
        format!("{}/libFNA3D.0.dylib", root),
        format!("{}/libFAudio.0.dylib", root),
        format!("{}/libSDL2-2.0.0.dylib", root),
    ];
    let file_args: Vec<&str> = files.iter().map(String::as_str).collect();
    let extracted = extract_archive_files(path, &tmp, &file_args);
    let root_path = tmp.join(root);
    let valid = extracted
        && fna_dylib_uses_sdl2(&root_path.join("libFNA3D.0.dylib"))
        && fna_dylib_uses_sdl2(&root_path.join("libFAudio.0.dylib"))
        && root_path.join("libSDL2-2.0.0.dylib").exists();

    let _ = fs::remove_dir_all(&tmp);
    valid
}

fn extract_archive_files(path: &Path, dest: &Path, files: &[&str]) -> bool {
    let file = match fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return false,
    };
    let mut decoder = match zstd::Decoder::new(file) {
        Ok(d) => d,
        Err(_) => return false,
    };

    let mut tar_cmd = match mac_cmd("tar")
        .args(["-xf", "-"])
        .arg("-C")
        .arg(dest)
        .args(files)
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
    {
        Ok(cmd) => cmd,
        Err(_) => return false,
    };

    if let Some(mut stdin) = tar_cmd.stdin.take() {
        if std::io::copy(&mut decoder, &mut stdin).is_err() {
            let _ = tar_cmd.kill();
            let _ = tar_cmd.wait();
            return false;
        }
        drop(stdin);
    }

    tar_cmd.wait().map(|status| status.success()).unwrap_or(false)
}

fn find_in_resources(name: &str) -> Option<PathBuf> {
    if let Some(resources) = crate::platform::app_resources_dir() {
        let archive = resources.join(format!("bundles/{}.tar.zst", name));
        if archive.exists() {
            return Some(archive);
        }
    }
    None
}

fn find_in_dev_path(name: &str) -> Option<PathBuf> {
    let archive = PathBuf::from(format!("app/bundles/{}.tar.zst", name));
    if archive.exists() {
        return Some(archive);
    }

    if let Ok(exe) = std::env::current_exe() {
        let dev = exe.parent()?.parent()?.parent()?.parent()?.join("bundles");
        let archive = dev.join(format!("{}.tar.zst", name));
        if archive.exists() {
            return Some(archive);
        }
    }
    None
}

fn copy_dir_recursive(src: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| format!("create {}: {}", dst.display(), e))?;
    for entry in fs::read_dir(src).map_err(|e| format!("read {}: {}", src.display(), e))? {
        let entry = entry.map_err(|e| format!("read entry in {}: {}", src.display(), e))?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        let file_type =
            fs::symlink_metadata(&src_path).map_err(|e| format!("metadata {}: {}", src_path.display(), e))?.file_type();
        if file_type.is_symlink() {
            copy_symlink_or_target(&src_path, &dst_path)?;
        } else if file_type.is_dir() {
            copy_dir_recursive(&src_path, &dst_path)?;
        } else {
            fs::copy(&src_path, &dst_path)
                .map_err(|e| format!("copy {} to {}: {}", src_path.display(), dst_path.display(), e))?;
        }
    }
    Ok(())
}

#[cfg(unix)]
fn copy_symlink_or_target(src: &Path, dst: &Path) -> Result<(), String> {
    let target = fs::read_link(src).map_err(|e| format!("read symlink {}: {}", src.display(), e))?;
    let _ = fs::remove_file(dst);
    std::os::unix::fs::symlink(&target, dst)
        .map_err(|e| format!("copy symlink {} to {}: {}", src.display(), dst.display(), e))
}

#[cfg(not(unix))]
fn copy_symlink_or_target(src: &Path, dst: &Path) -> Result<(), String> {
    fs::copy(src, dst).map(|_| ()).map_err(|e| format!("copy {} to {}: {}", src.display(), dst.display(), e))
}

fn extract_zst(archive: &PathBuf, dest: &PathBuf, name: &str) -> Result<(), String> {
    let _ = fs::create_dir_all(dest);

    let file = fs::File::open(archive).map_err(|e| format!("cannot open archive: {}", e))?;

    let mut decoder = zstd::Decoder::new(file).map_err(|e| format!("zstd decode error: {}", e))?;

    let mut tar_cmd = mac_cmd("tar")
        .args(["-xf", "-"])
        .arg("-C")
        .arg(dest)
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("tar spawn failed: {}", e))?;

    if let Some(mut stdin) = tar_cmd.stdin.take() {
        std::io::copy(&mut decoder, &mut stdin).map_err(|e| format!("zstd decompression failed: {}", e))?;
        drop(stdin);
    }

    let status = tar_cmd.wait().map_err(|e| format!("tar wait failed: {}", e))?;

    if !status.success() {
        return Err(format!("tar extraction failed for {}", name));
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(target_os = "macos")]
    #[test]
    fn wine_dependency_repair_only_rewrites_the_private_staging_prefix() {
        assert_eq!(
            packaged_dependency_target("/tmp/metalsharp-wine-deps/lib/libgnutls.30.dylib"),
            Some("@loader_path/libgnutls.30.dylib".to_string())
        );
        assert_eq!(
            packaged_dependency_target("/tmp/metalsharp-wine-deps/lib/libgmp.10.dylib"),
            Some("@loader_path/libgmp.10.dylib".to_string())
        );
        assert_eq!(packaged_dependency_target("/opt/homebrew/lib/libgnutls.30.dylib"), None);
        assert_eq!(packaged_dependency_target("/tmp/metalsharp-wine-deps/lib/nested/libfoo.dylib"), None);
        assert_eq!(packaged_dependency_target("@loader_path/libgnutls.30.dylib"), None);
    }

    #[test]
    fn assets_required_files_cover_fna_unity_payloads() {
        // Phase: the mono route's version-matched payloads must be required by
        // the installer so a bundle without them is rejected before a game
        // tries to deploy a missing runtime.
        for rel in [
            "assets/xna/Microsoft.Xna.Framework.dll",
            "assets/xna/Microsoft.Xna.Framework.Game.dll",
            "assets/xna/Microsoft.Xna.Framework.Graphics.dll",
            "assets/xna/Microsoft.Xna.Framework.Audio.dll",
            "assets/xna/Microsoft.Xna.Framework.Input.dll",
            "assets/xna/Microsoft.Xna.Framework.Media.dll",
            "assets/xna/Microsoft.Xna.Framework.Storage.dll",
            "assets/unity-mono/manifest.json",
            "assets/unity-mono/2020.3/libmonosgen-2.0.1.dylib",
            "assets/unity-mono/2021.3/libmonosgen-2.0.1.dylib",
            "assets/unity-mono/2022.3/libmonosgen-2.0.1.dylib",
            "assets/unity-mono/6000.0/libmonosgen-2.0.1.dylib",
            "assets/sdl3/libSDL3.dylib",
            "assets/prebuilt-launchers/TerrariaLauncher.exe",
            "assets/prebuilt-launchers/TerrariaOfflinePatcher.exe",
            "assets/prebuilt-launchers/Microsoft.Xna.Framework.Xact.dll",
            "assets/shims/libgdiplus.dylib",
            "assets/shims/libFAudio.0.dylib",
        ] {
            assert!(ASSETS_REQUIRED_ARCHIVE_FILES.contains(&rel), "ASSETS_REQUIRED_ARCHIVE_FILES must require {rel}");
        }
    }

    #[test]
    fn missing_m12_sidecars_lists_each_absent_file_by_name() {
        // Phase 7: runtime verification must catch missing M12 sidecars
        // (DLL/dylib/so) by name before gameplay. With an empty home, every
        // required M12 file is missing and must be named explicitly. Uses the
        // explicit-home variant so no global env is mutated.
        let home = test_home("missing-m12-sidecars");

        let missing = missing_m12_sidecars_for(&home);
        // Every required unix sidecar and PE DLL must be named.
        for name in DXMT_M12_REQUIRED_UNIX {
            assert!(
                missing.iter().any(|m| m.ends_with(&format!("/x86_64-unix/{}", name))),
                "missing M12 unix sidecar {} must be reported: {:?}",
                name,
                missing
            );
        }
        for dll in DXMT_REQUIRED_PE {
            assert!(
                missing.iter().any(|m| m.ends_with(&format!("/x86_64-windows/{}", dll))),
                "missing M12 PE DLL {} must be reported: {:?}",
                dll,
                missing
            );
        }
    }

    #[test]
    fn runtime_artifact_report_names_each_file_with_presence_and_hash() {
        // Phase 7: the artifact report must name each file with presence +
        // sha256 so a stale/missing artifact is observable by name. Explicit
        // home so no global env mutation.
        let home = test_home("artifact-report-empty");

        let report = runtime_artifact_report_for(&home);
        assert_eq!(report.get("schema_version").and_then(|v| v.as_u64()), Some(1));
        assert_eq!(report.get("ok").and_then(|v| v.as_bool()), Some(false), "empty home must report ok=false");
        let m12 = report.get("dxmt_m12").unwrap();
        let entries = m12.get("entries").and_then(|v| v.as_array()).unwrap();
        // Every entry must carry filename, present=false, sha256=null.
        for entry in entries {
            assert!(entry.get("filename").and_then(|v| v.as_str()).is_some());
            assert_eq!(entry.get("present").and_then(|v| v.as_bool()), Some(false));
            assert_eq!(entry.get("sha256").and_then(|v| v.as_str()), None);
        }
    }

    #[test]
    fn metalsharp_wine_binary_accepts_renamed_runtime_binary() {
        let home = test_home("renamed-runtime-binary");
        let bin = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine").join("bin");
        fs::create_dir_all(&bin).expect("create runtime bin");
        fs::write(bin.join("metalsharp-wine"), b"#!/bin/sh\n").expect("write renamed wine");

        assert_eq!(metalsharp_wine_binary(&home), bin.join("metalsharp-wine"));
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn gptk_runtime_readiness_requires_framework_contents() {
        let home = test_home("gptk-readiness");
        let gptk_dir = home.join("gptk");
        let pe_dir = gptk_dir.join("x86_64-windows");
        let framework = home.join("external").join("D3DMetal.framework");
        let resources = framework.join("Versions").join("A").join("Resources");
        fs::create_dir_all(&pe_dir).expect("create GPTK PE dir");
        fs::create_dir_all(&resources).expect("create framework resources");
        for dll in ["d3d10.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvapi64.dll", "nvngx-on-metalfx.dll"] {
            fs::write(pe_dir.join(dll), b"dll").expect("write GPTK DLL");
        }

        assert!(!gptk_runtime_ready(&pe_dir, &framework));

        fs::write(framework.join("Versions").join("A").join("D3DMetal"), b"framework").expect("write framework binary");
        fs::write(resources.join("libD3DMetalHelper.dylib"), b"dylib").expect("write framework resource dylib");

        assert!(gptk_runtime_ready(&pe_dir, &framework));
        fs::remove_file(pe_dir.join("nvngx-on-metalfx.dll")).expect("remove MetalFX NVNGX bridge");
        assert!(!gptk_runtime_ready(&pe_dir, &framework));
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn runtime_bundle_preflight_knows_beta7_assets() {
        let mac_assets = MAC_RUNTIME_BUNDLE_ASSETS;
        for expected in [
            "metalsharp-runtime.tar.zst",
            "metalsharp-graphics-dll.tar.zst",
            "metalsharp-assets.tar.zst",
            "fnalibs.tar.zst",
            "metalsharp-scripts-tools.tar.zst",
            "metalsharp-steam.tar.zst",
        ] {
            assert!(mac_assets.contains(&expected), "missing mac bundle asset {}", expected);
        }
    }

    #[test]
    fn install_order_runs_xcode_cli_before_rosetta() {
        let names: Vec<&str> = install_steps().into_iter().map(|(name, _)| name).collect();

        let xcode_idx = names.iter().position(|name| *name == "System Tools").expect("system tools step");
        let rosetta_idx = names.iter().position(|name| *name == "Rosetta 2").expect("rosetta step");

        assert!(xcode_idx < rosetta_idx);
    }

    #[test]
    fn install_steps_include_eac_substrate_without_installing_eac_toggle_or_gptk() {
        let names: Vec<&str> = install_steps().into_iter().map(|(name, _)| name).collect();

        assert!(names.contains(&"DXMT Graphics Runtimes"));
        let scripts_idx = names.iter().position(|name| *name == "Scripts and Tools").expect("scripts/tools step");
        let eac_idx = names.iter().position(|name| *name == "EAC Substrate").expect("EAC substrate step");
        assert!(scripts_idx < eac_idx, "the EAC step must consume the installed scripts/tools bundle");
        assert!(!names.contains(&"Offline EAC Mode"));
        assert!(
            names.iter().all(|name| !name.to_ascii_lowercase().contains("gptk")),
            "first-time setup must not install GPTK; D3DMetal bottles own Homebrew GPTK setup: {:?}",
            names
        );
    }

    #[test]
    fn scripts_tools_bundle_requires_both_eac_native_assets() {
        assert!(SCRIPTS_TOOLS_REQUIRED_ARCHIVE_FILES.contains(&"scripts/tools/native/metalsharp_eac_substrate.dylib"));
        assert!(SCRIPTS_TOOLS_REQUIRED_ARCHIVE_FILES.contains(&"scripts/tools/native/metalsharp_eac_libc.so.6"));
    }

    #[test]
    fn eac_macho_validation_requires_the_wine_architecture() {
        assert!(macho_contains_x86_64(&[0xcf, 0xfa, 0xed, 0xfe, 0x07, 0x00, 0x00, 0x01]));
        assert!(!macho_contains_x86_64(&[0xcf, 0xfa, 0xed, 0xfe, 0x0c, 0x00, 0x00, 0x01]));

        let mut universal = vec![0xca, 0xfe, 0xba, 0xbe, 0x00, 0x00, 0x00, 0x01];
        universal.extend_from_slice(&[0x01, 0x00, 0x00, 0x07]);
        universal.extend_from_slice(&[0; 16]);
        assert!(macho_contains_x86_64(&universal));
    }

    #[test]
    fn eac_runtime_readiness_requires_a_macho_and_an_elf_image() {
        let home = test_home("eac-readiness");
        let ms_dir = crate::platform::metalsharp_home_dir_for(&home);
        let eac_dir = ms_dir.join("runtime").join(EAC_RUNTIME_SUBDIR);
        fs::create_dir_all(&eac_dir).expect("create EAC runtime dir");

        fs::write(
            eac_dir.join(crate::anticheat::EAC_SUBSTRATE_FILENAME),
            [0xcf, 0xfa, 0xed, 0xfe, 0x07, 0x00, 0x00, 0x01],
        )
        .expect("write Mach-O fixture");
        assert!(!eac_substrate_runtime_ready_for_home(&home));

        fs::write(eac_dir.join(crate::anticheat::EAC_SYMBOL_IMAGE_FILENAME), b"\x7fELF\x02\x01")
            .expect("write ELF fixture");
        assert!(eac_substrate_runtime_ready_for_home(&home));

        fs::write(eac_dir.join(crate::anticheat::EAC_SUBSTRATE_FILENAME), b"not a Mach-O").expect("poison Mach-O");
        assert!(!eac_substrate_runtime_ready_for_home(&home));
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn eac_runtime_install_is_idempotent_and_refreshes_as_a_pair() {
        let home = test_home("eac-install");
        let source_dir = test_home("eac-install-source");
        fs::create_dir_all(&source_dir).expect("create source dir");
        let substrate_source = source_dir.join(crate::anticheat::EAC_SUBSTRATE_FILENAME);
        let symbol_source = source_dir.join(crate::anticheat::EAC_SYMBOL_IMAGE_FILENAME);
        fs::write(&substrate_source, [0xcf, 0xfa, 0xed, 0xfe, 0x07, 0x00, 0x00, 0x01]).expect("write substrate source");
        fs::write(&symbol_source, b"\x7fELF\x02\x01-v1").expect("write symbol source");

        assert_eq!(install_eac_substrate_from_sources(&home, &substrate_source, &symbol_source), Ok(true));
        let ms_dir = crate::platform::metalsharp_home_dir_for(&home);
        assert!(eac_substrate_runtime_ready_for_ms_dir(&ms_dir));
        assert_eq!(
            install_eac_substrate_from_sources(&home, &substrate_source, &symbol_source),
            Ok(false),
            "an unchanged update must not rewrite the durable EAC pair"
        );

        fs::write(&substrate_source, [0xfe, 0xed, 0xfa, 0xcf, 0x01, 0x00, 0x00, 0x07, 0x02])
            .expect("refresh substrate source");
        fs::write(&symbol_source, b"\x7fELF\x02\x01-v2").expect("refresh symbol source");
        assert_eq!(install_eac_substrate_from_sources(&home, &substrate_source, &symbol_source), Ok(true));
        assert_eq!(
            fs::read(ms_dir.join("runtime").join(EAC_RUNTIME_SUBDIR).join(crate::anticheat::EAC_SYMBOL_IMAGE_FILENAME))
                .expect("read installed symbol image"),
            b"\x7fELF\x02\x01-v2"
        );

        fs::write(&substrate_source, b"invalid substrate").expect("poison source");
        assert!(install_eac_substrate_from_sources(&home, &substrate_source, &symbol_source).is_err());
        assert!(eac_substrate_runtime_ready_for_ms_dir(&ms_dir), "a failed update must retain the previous pair");

        let _ = fs::remove_dir_all(home);
        let _ = fs::remove_dir_all(source_dir);
    }

    #[test]
    fn graphics_bundle_layout_matches_release_manifest() {
        let manifest = include_str!("../../../tools/bundles/asset-manifest.tsv");
        let graphics_row = manifest
            .lines()
            .find(|line| line.starts_with("metalsharp-graphics-dll.tar.zst\t"))
            .expect("metalsharp-graphics-dll.tar.zst release manifest row");
        let fields: Vec<&str> = graphics_row.split('\t').collect();

        assert_eq!(fields.get(1).copied(), Some("Graphics/dll"));
    }

    #[test]
    fn bundle_validation_rejects_stale_known_archives() {
        let home = test_home("stale-known-bundle");
        fs::create_dir_all(&home).expect("create test dir");
        let stale = home.join("metalsharp-runtime.tar.zst");
        fs::write(&stale, b"old runtime bundle").expect("write stale archive");

        assert!(!bundled_artifact_valid("metalsharp-runtime", &stale));
        assert!(!bundled_artifact_valid("metalsharp-runtime.tar.zst", &stale));
        assert!(!bundled_artifact_valid("metalsharp-graphics-dll", &stale));
        assert!(!bundled_artifact_valid("metalsharp-graphics-dll.tar.zst", &stale));
        assert!(!bundled_artifact_valid("metalsharp-assets", &stale));
        assert!(!bundled_artifact_valid("metalsharp-assets.tar.zst", &stale));
        assert!(!bundled_artifact_valid("fnalibs", &stale));
        assert!(!bundled_artifact_valid("fnalibs.tar.zst", &stale));
        assert!(!bundled_artifact_valid("metalsharp-scripts-tools", &stale));
        assert!(!bundled_artifact_valid("metalsharp-scripts-tools.tar.zst", &stale));
        assert!(!bundled_artifact_valid("metalsharp-steam", &stale));
        assert!(!bundled_artifact_valid("metalsharp-steam.tar.zst", &stale));
        assert!(bundled_artifact_valid("unmanaged-test-asset.bin", &stale));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn dxmt_readiness_requires_current_bundled_manifest() {
        let home = test_home("dxmt-current-manifest");
        let dxmt_dir = dxmt_runtime_dir_for_home(&home);
        write_dxmt_runtime_files(&dxmt_dir);

        assert!(dxmt_runtime_ready(&dxmt_dir));
        assert!(!dxmt_runtime_current_for_dir(&dxmt_dir));

        write_dxmt_runtime_manifest(&dxmt_dir, "test").expect("write current DXMT manifest");
        assert!(dxmt_runtime_current_for_dir(&dxmt_dir));

        fs::write(dxmt_dir.join(DXMT_RUNTIME_MANIFEST), br#"{"schema":"metalsharp.dxmt-runtime.v1","version":"old"}"#)
            .expect("write stale manifest");
        assert!(!dxmt_runtime_current_for_dir(&dxmt_dir));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn dxmt_runtime_current_does_not_require_dxmt_m12_lane() {
        let home = test_home("dxmt-current-no-m12");
        let dxmt_dir = dxmt_runtime_dir_for_home(&home);
        write_dxmt_runtime_files_only(&dxmt_dir);
        write_dxmt_runtime_manifest(&dxmt_dir, "test").expect("write current DXMT manifest");

        assert!(dxmt_runtime_current_for_dir(&dxmt_dir));
        assert!(!dxmt_m12_runtime_current_for_dir(&dxmt_m12_runtime_dir_for_home(&home)));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn dxmt_m12_runtime_current_requires_manifest_sidecars_and_expected_hashes() {
        let home = test_home("dxmt-m12-own-manifest");
        let dxmt_dir = dxmt_runtime_dir_for_home(&home);
        let dxmt_m12_dir = dxmt_m12_runtime_dir_for_home(&home);
        write_dxmt_runtime_files_only(&dxmt_dir);
        write_dxmt_m12_runtime_files_only(&dxmt_m12_dir);
        write_dxmt_runtime_manifest(&dxmt_dir, "legacy-test").expect("write legacy manifest");

        assert!(dxmt_runtime_current_for_dir(&dxmt_dir));
        assert!(!dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir));

        write_dxmt_runtime_manifest(&dxmt_m12_dir, "m12-test").expect("write m12 manifest");
        assert!(dxmt_m12_runtime_ready(&dxmt_m12_dir));
        assert!(
            !dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir),
            "dummy test DLL contents must not satisfy confirmed-good M12 hash guard"
        );

        fs::remove_file(dxmt_m12_dir.join("x86_64-unix").join("winemetal.so")).expect("remove m12 winemetal.so");
        assert!(!dxmt_m12_runtime_ready(&dxmt_m12_dir));
        assert!(!dxmt_m12_runtime_current_for_dir(&dxmt_m12_dir));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn dxmt_install_normalizes_legacy_bundle_dxgi_bridge() {
        let home = test_home("dxmt-legacy-dxgi-bridge");
        let dxmt_dir = dxmt_runtime_dir_for_home(&home);
        let unix_dir = dxmt_dir.join("x86_64-unix");
        let pe_dir = dxmt_dir.join("x86_64-windows");
        fs::create_dir_all(&unix_dir).expect("create DXMT unix dir");
        fs::create_dir_all(&pe_dir).expect("create DXMT PE dir");
        fs::write(unix_dir.join("winemetal.so"), b"so").expect("write winemetal");
        for dll in DXMT_REQUIRED_PE.iter().copied().filter(|dll| *dll != "dxgi_dxmt.dll") {
            fs::write(pe_dir.join(dll), dll.as_bytes()).expect("write DXMT DLL");
        }

        assert!(!dxmt_runtime_ready(&dxmt_dir));

        ensure_dxmt_runtime_compat_files(&dxmt_dir).expect("normalize legacy DXMT bundle");

        assert!(dxmt_runtime_ready(&dxmt_dir));
        assert_eq!(
            fs::read(pe_dir.join("dxgi_dxmt.dll")).expect("read dxgi_dxmt"),
            fs::read(pe_dir.join("dxgi.dll")).expect("read dxgi")
        );
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn install_mtsp_rules_refreshes_stale_installed_copy() {
        let repo = test_home("mtsp-rules-source");
        let home = test_home("mtsp-rules-home");
        let source_dir = repo.join("configs");
        let dest_dir = crate::platform::metalsharp_home_dir_for(&home).join("configs");
        let dest = dest_dir.join("mtsp-rules.toml");
        fs::create_dir_all(&source_dir).expect("create source config dir");
        fs::create_dir_all(&dest_dir).expect("create dest config dir");
        fs::write(source_dir.join("mtsp-rules.toml"), "# new rules\n[overrides]\n").expect("write source rules");
        fs::write(&dest, "# stale rules\n").expect("write stale rules");

        let result = install_mtsp_rules_from_candidates(&dest, &[source_dir.join("mtsp-rules.toml")]);

        assert_eq!(result, Ok(true));
        assert_eq!(fs::read_to_string(&dest).expect("read rules"), "# new rules\n[overrides]\n");
        assert_eq!(fs::read_to_string(dest_dir.join("mtsp-rules.toml.bak")).expect("read backup"), "# stale rules\n");
        let _ = fs::remove_dir_all(repo);
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn install_mtsp_rules_uses_installed_scripts_tools_bundle_copy() {
        let home = test_home("mtsp-rules-installed-tools");
        let source_dir = crate::platform::metalsharp_home_dir_for(&home).join("scripts").join("tools").join("configs");
        let dest_dir = crate::platform::metalsharp_home_dir_for(&home).join("configs");
        fs::create_dir_all(&source_dir).expect("create installed scripts tools config dir");
        fs::write(source_dir.join("mtsp-rules.toml"), "# installed rules\n[profiles]\n").expect("write source rules");

        let result = install_mtsp_rules(&home);

        assert_eq!(result, Ok(true));
        assert_eq!(
            fs::read_to_string(dest_dir.join("mtsp-rules.toml")).expect("read rules"),
            "# installed rules\n[profiles]\n"
        );
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn install_mtsp_rules_allows_missing_optional_rules() {
        let home = test_home("mtsp-rules-missing-home");
        let dest = crate::platform::metalsharp_home_dir_for(&home).join("configs").join("mtsp-rules.toml");

        let result = install_mtsp_rules_from_candidates(&dest, &[]);

        assert_eq!(result, Ok(false));
        assert!(!dest.exists());
        let _ = fs::remove_dir_all(home);
    }

    fn test_home(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "metalsharp-installer-{}-{}-{}",
            name,
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).expect("system time").as_nanos()
        ))
    }

    #[test]
    fn bundled_file_valid_exists_rejects_empty_files() {
        let home = test_home("empty-file-validation");
        let ms_dir = crate::platform::metalsharp_home_dir_for(&home);
        let bundles_dir = ms_dir.join("cache").join("bundles");
        fs::create_dir_all(&bundles_dir).expect("create bundles dir");

        let empty_file = bundles_dir.join("metalsharp-runtime.tar.zst");
        fs::write(&empty_file, b"").expect("create empty file");

        assert!(!bundled_file_valid_exists_in_candidates("metalsharp-runtime.tar.zst", [empty_file],));
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn bundled_file_valid_exists_rejects_nonexistent_files() {
        let nonexistent = test_home("nonexistent-file-validation").join("metalsharp-runtime.tar.zst");
        assert!(!bundled_file_valid_exists_in_candidates("metalsharp-runtime.tar.zst", [nonexistent],));
    }

    #[test]
    fn bundled_file_valid_exists_rejects_invalid_archives() {
        let home = test_home("invalid-archive-validation");
        let ms_dir = crate::platform::metalsharp_home_dir_for(&home);
        let bundles_dir = ms_dir.join("cache").join("bundles");
        fs::create_dir_all(&bundles_dir).expect("create bundles dir");

        let invalid_file = bundles_dir.join("metalsharp-runtime.tar.zst");
        fs::write(&invalid_file, b"not a valid zst archive").expect("create invalid archive");

        assert!(!bundled_file_valid_exists_in_candidates("metalsharp-runtime.tar.zst", [invalid_file],));
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn bundled_artifact_valid_accepts_non_bundle_files() {
        let home = test_home("non-bundle-file");
        let ms_dir = crate::platform::metalsharp_home_dir_for(&home);
        let bundles_dir = ms_dir.join("cache").join("bundles");
        fs::create_dir_all(&bundles_dir).expect("create bundles dir");

        let non_bundle = bundles_dir.join("other-file.bin");
        fs::write(&non_bundle, b"some content").expect("create non-bundle file");

        assert!(bundled_artifact_valid("other-file.bin", &non_bundle));
        let _ = fs::remove_dir_all(home);
    }

    fn write_dxmt_runtime_files(dxmt_dir: &Path) {
        write_dxmt_runtime_files_only(dxmt_dir);
        write_dxmt_m12_runtime_files_only(&dxmt_m12_runtime_dir_from_dxmt_dir(dxmt_dir));
    }

    fn write_dxmt_runtime_files_only(dxmt_dir: &Path) {
        let unix_dir = dxmt_dir.join("x86_64-unix");
        let pe_dir = dxmt_dir.join("x86_64-windows");
        fs::create_dir_all(&unix_dir).expect("create DXMT unix dir");
        fs::create_dir_all(&pe_dir).expect("create DXMT PE dir");
        fs::write(unix_dir.join("winemetal.so"), b"so").expect("write winemetal");
        for dll in DXMT_REQUIRED_PE {
            fs::write(pe_dir.join(dll), b"dll").expect("write DXMT DLL");
        }
    }

    fn write_dxmt_m12_runtime_files_only(dxmt_m12_dir: &Path) {
        let m12_unix_dir = dxmt_m12_dir.join("x86_64-unix");
        let m12_pe_dir = dxmt_m12_dir.join("x86_64-windows");
        fs::create_dir_all(&m12_unix_dir).expect("create M12 Unix dir");
        fs::create_dir_all(&m12_pe_dir).expect("create M12 PE dir");
        for lib in DXMT_M12_REQUIRED_UNIX {
            fs::write(m12_unix_dir.join(lib), b"lib").expect("write M12 Unix sidecar");
        }
        for dll in DXMT_REQUIRED_PE {
            fs::write(m12_pe_dir.join(dll), b"dll").expect("write M12 DLL");
        }
    }

    #[test]
    fn bundle_archive_has_update_detects_new_graphics_bundle_content() {
        // The migration gate must treat a graphics bundle whose sha256 differs
        // from the last-staged marker as carrying new infrastructure (e.g. the
        // i386 DXMT lanes), so ensure_graphics_runtimes_ready re-extracts it
        // instead of early-returning on a version-matching x86_64 surface.
        let home = test_home("graphics-bundle-update-detector");
        let marker_dir = split_bundle_marker_dir(&home);
        fs::create_dir_all(&marker_dir).expect("create marker dir");
        let archive = std::env::temp_dir().join(format!(
            "metalsharp-graphics-bundle-update-{}-{}.tar.zst",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).expect("system time").as_nanos()
        ));
        fs::write(&archive, b"graphics-bundle-v2-with-i386-lanes").expect("write synthetic bundle");

        // No marker yet -> the staged surface has never absorbed this bundle.
        assert!(
            bundle_archive_has_update(&home, GRAPHICS_DLL_BUNDLE, &archive),
            "missing marker should signal an update is pending"
        );

        // Stale marker (wrong hash) -> bundle carries new content.
        fs::write(split_bundle_marker_path(&home, GRAPHICS_DLL_BUNDLE), b"deadbeef").expect("write stale marker");
        assert!(
            bundle_archive_has_update(&home, GRAPHICS_DLL_BUNDLE, &archive),
            "a sha mismatch should signal the bundle has new infrastructure"
        );

        // Current marker (matching hash) -> no update pending.
        let hash = archive_sha256(&archive).expect("compute archive sha256 for test");
        fs::write(split_bundle_marker_path(&home, GRAPHICS_DLL_BUNDLE), &hash).expect("write current marker");
        assert!(
            !bundle_archive_has_update(&home, GRAPHICS_DLL_BUNDLE, &archive),
            "a matching marker should not signal an update"
        );

        let _ = fs::remove_file(&archive);
        let _ = fs::remove_dir_all(&home);
    }

    #[test]
    fn runtime_bundle_refresh_preserves_every_graphics_surface() {
        let home = test_home("preserve-graphics-surfaces");
        let wine_dir = home.join("runtime").join("wine");
        let lib_dir = wine_dir.join("lib");
        let extract_dir = home.join("extract");

        for surface in GRAPHICS_RUNTIME_SURFACES {
            let marker = lib_dir.join(surface).join("marker");
            fs::create_dir_all(marker.parent().expect("surface parent")).expect("create graphics surface");
            fs::write(&marker, surface.as_bytes()).expect("write graphics marker");
        }

        let preserved = preserve_graphics_runtime_surfaces(&wine_dir, &extract_dir).expect("preserve graphics lanes");
        fs::remove_dir_all(&lib_dir).expect("simulate runtime bundle replacement");
        fs::create_dir_all(&lib_dir).expect("recreate runtime lib directory");
        restore_preserved_graphics_runtime_surfaces(&wine_dir, &preserved).expect("restore graphics lanes");

        for surface in GRAPHICS_RUNTIME_SURFACES {
            assert_eq!(
                fs::read_to_string(lib_dir.join(surface).join("marker")).expect("read restored marker"),
                *surface
            );
        }

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn vkd3d_proton_runtime_current_requires_shipped_x86_64_hashes() {
        let home = test_home("vkd3d-current");
        let dir = vkd3d_proton_runtime_dir_for_home(&home);
        write_vkd3d_proton_expected_test_files(&dir);

        // All fixtures present + matching test hashes -> current.
        assert!(vkd3d_proton_runtime_current_for_home(&home));

        // The bundle ships x86_64-windows only: missing i386 lanes are
        // phantom pins and must NOT block currency — requiring them would
        // keep "current" permanently false and force a full-bundle zstd
        // re-extraction on every M12 bottle save (backend freeze).
        assert!(!dir.join("i386-windows").exists(), "the test fixture must remain x86_64-only");
        assert!(vkd3d_proton_runtime_current_for_home(&home), "phantom i386 lanes must not block currency");

        // Corrupt one artifact -> not current.
        let artifact = dir.join("x86_64-windows/d3d12core.dll");
        fs::write(&artifact, b"corrupted").expect("corrupt artifact");
        assert!(!vkd3d_proton_runtime_current_for_home(&home));
        assert!(!vkd3d_proton_runtime_artifact_valid_for_home(&home, "x86_64-windows/d3d12core.dll"));
        assert!(vkd3d_proton_runtime_artifact_valid_for_home(&home, "x86_64-windows/d3d12.dll"));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn vkd3d_proton_unknown_artifact_is_never_valid() {
        let home = test_home("vkd3d-unknown");
        assert!(!vkd3d_proton_runtime_artifact_valid_for_home(&home, "x86_64-windows/nope.dll"));
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn vkmt_moltenvk_sync_replaces_equal_size_stale_wine_direct_load_copies() {
        let home = test_home("vkmt-moltenvk-sync");
        let wine_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
        let lane = wine_dir.join("lib").join("moltenvk-vkmt");
        let unix = wine_dir.join("lib").join("wine").join("x86_64-unix");
        fs::create_dir_all(&lane).expect("create VKMT lane");
        fs::create_dir_all(&unix).expect("create Wine unix lane");
        fs::write(lane.join("libMoltenVK.dylib"), b"vkmt-new-1.4.2").expect("write VKMT MoltenVK");
        for name in ["libMoltenVK.dylib", "libMoltenVK.1.dylib"] {
            fs::write(unix.join(name), b"stock-old-1.4.").expect("write stale stock MoltenVK");
        }

        assert!(sync_vkmt_moltenvk_into_wine_tree(&wine_dir).expect("synchronize VKMT MoltenVK"));
        for name in ["libMoltenVK.dylib", "libMoltenVK.1.dylib"] {
            assert_eq!(fs::read(unix.join(name)).unwrap(), b"vkmt-new-1.4.2", "{name} must match VKMT lane");
        }
        assert!(!sync_vkmt_moltenvk_into_wine_tree(&wine_dir).expect("idempotent synchronization"));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn moltenvk_vkmt_and_dxvk_lanes_ready_only_when_files_present() {
        let home = test_home("vkmt-lanes");
        assert!(!moltenvk_vkmt_runtime_ready_for_home(&home));
        assert!(!dxvk_runtime_ready_for_home(&home));

        let mvk = moltenvk_vkmt_runtime_dir_for_home(&home);
        fs::create_dir_all(&mvk).expect("create moltenvk lane");
        write_moltenvk_vkmt_expected_test_files(&mvk);
        assert!(!moltenvk_vkmt_runtime_ready_for_home(&home), "ICD still missing");
        fs::write(mvk.join("MoltenVK_icd.json"), b"{}").expect("write icd");
        assert!(!moltenvk_vkmt_runtime_ready_for_home(&home), "versioned loader alias still missing");
        ensure_moltenvk_vkmt_loader_alias(&mvk).expect("create loader alias");
        assert!(moltenvk_vkmt_runtime_ready_for_home(&home));
        fs::write(mvk.join("libMoltenVK.dylib"), b"corrupted").expect("corrupt MoltenVK dylib");
        assert!(!moltenvk_vkmt_runtime_ready_for_home(&home), "hash-mismatched MoltenVK must not be ready");
        write_moltenvk_vkmt_expected_test_files(&mvk);
        ensure_moltenvk_vkmt_loader_alias(&mvk).expect("restore loader alias");
        assert!(moltenvk_vkmt_runtime_ready_for_home(&home));

        let dxvk = dxvk_runtime_dir_for_home(&home).join("x86_64-windows");
        fs::create_dir_all(&dxvk).expect("create dxvk lane");
        fs::write(dxvk.join("dxgi.dll"), b"dxgi").expect("write dxgi");
        assert!(!dxvk_runtime_ready_for_home(&home), "d3d11 still missing");
        for dll in ["d3d11.dll", "d3d10core.dll", "d3d9.dll"] {
            fs::write(dxvk.join(dll), dll.as_bytes()).expect("write dll");
        }
        assert!(dxvk_runtime_ready_for_home(&home));
        assert!(!m12_vulkan_runtime_ready_for_home(&home), "vkd3d-proton lane still missing");

        write_vkd3d_proton_expected_test_files(&vkd3d_proton_runtime_dir_for_home(&home));
        assert!(m12_vulkan_runtime_ready_for_home(&home));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn moltenvk_library_path_prefers_vkmt_lane_over_stock() {
        let wine_dir = test_home("moltenvk-preference").join("runtime").join("wine");
        let stock = wine_dir.join("lib").join("wine").join("x86_64-unix").join("libMoltenVK.dylib");
        let vkmt = wine_dir.join("lib").join("moltenvk-vkmt").join("libMoltenVK.dylib");
        let vkmt_icd = vkmt.parent().unwrap().join("MoltenVK_icd.json");
        let runtime_icd = wine_dir.join("etc").join("vulkan").join("icd.d").join("MoltenVK_icd.json");
        fs::create_dir_all(stock.parent().unwrap()).expect("stock parent");
        fs::create_dir_all(vkmt.parent().unwrap()).expect("vkmt parent");
        fs::create_dir_all(runtime_icd.parent().unwrap()).expect("ICD parent");
        fs::write(&stock, b"stock").expect("stock dylib");
        assert_eq!(moltenvk_library_path(&wine_dir), stock);

        write_moltenvk_vkmt_expected_test_files(vkmt.parent().unwrap());
        fs::write(
            &vkmt_icd,
            r#"{"file_format_version":"1.0.0","ICD":{"library_path":"./libMoltenVK.dylib","api_version":"1.4.0"}}"#,
        )
        .expect("vkmt ICD");
        assert_eq!(moltenvk_library_path(&wine_dir), vkmt);
        assert!(moltenvk_vkmt_ready(&wine_dir));

        fix_moltenvk_icd_paths(&wine_dir);
        let runtime_manifest: serde_json::Value =
            serde_json::from_str(&fs::read_to_string(&runtime_icd).expect("runtime ICD")).expect("parse runtime ICD");
        assert_eq!(runtime_manifest["ICD"]["library_path"], vkmt.to_string_lossy().as_ref());

        let _ = fs::remove_dir_all(wine_dir.parent().unwrap());
    }

    #[test]
    fn install_reconcile_keeps_vkd3d_default_when_lanes_present() {
        let home = test_home("install-reconcile-vkd3d");
        write_vkd3d_proton_expected_test_files(&vkd3d_proton_runtime_dir_for_home(&home));
        let mvk = moltenvk_vkmt_runtime_dir_for_home(&home);
        fs::create_dir_all(&mvk).expect("mvk dir");
        write_moltenvk_vkmt_expected_test_files(&mvk);
        ensure_moltenvk_vkmt_loader_alias(&mvk).expect("mvk loader alias");
        fs::write(mvk.join("MoltenVK_icd.json"), b"icd").expect("mvk icd");
        let dxvk = dxvk_runtime_dir_for_home(&home).join("x86_64-windows");
        fs::create_dir_all(&dxvk).expect("dxvk dir");
        for dll in ["dxgi.dll", "d3d11.dll", "d3d10core.dll", "d3d9.dll"] {
            fs::write(dxvk.join(dll), dll.as_bytes()).expect("write dxvk dll");
        }

        reconcile_m12_backend_for_home(&home);

        // Lanes present -> default stays vkd3d-proton, no config pin written.
        let configs = home.join(".metalsharp").join("configs");
        assert!(
            !configs.join("config.json").exists(),
            "vkd3d-proton default must not be pinned to config when lanes are present"
        );
        assert_eq!(crate::launch::m12_backend_mode_for(&home), "vkd3d-proton");
        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn install_reconcile_pins_dxmt_when_lanes_missing() {
        let home = test_home("install-reconcile-dxmt-fallback");

        // Empty home: no vkd3d/MoltenVK/DXVK lanes staged -> must pin DXMT so
        // the default never points at a missing runtime.
        reconcile_m12_backend_for_home(&home);

        assert_eq!(crate::launch::m12_backend_mode_for(&home), "dxmt");
        let _ = fs::remove_dir_all(home);
    }
}
