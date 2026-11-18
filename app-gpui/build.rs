//! With the `sparkle` feature: compiles sparkle/MSSparkleBridge.m and links Sparkle.framework, which MetalSharp.app
//! carries in Contents/Frameworks. SPARKLE_FRAMEWORK_DIR names the folder holding Sparkle.framework; by default it is
//! the one tools/sparkle/fetch-sparkle.sh unpacks.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=sparkle/MSSparkleBridge.m");
    println!("cargo:rerun-if-changed=glass/MSGlass.swift");
    println!("cargo:rerun-if-env-changed=SPARKLE_FRAMEWORK_DIR");
    let macos = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos");
    if macos {
        build_glass();
    }
    if !macos || std::env::var_os("CARGO_FEATURE_SPARKLE").is_none() {
        return;
    }
    let manifest = std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let frameworks = std::env::var_os("SPARKLE_FRAMEWORK_DIR")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| manifest.join("../build/sparkle/2.9.6"));
    if !frameworks.join("Sparkle.framework").is_dir() {
        panic!(
            "the sparkle feature needs Sparkle.framework in {}: run tools/sparkle/fetch-sparkle.sh",
            frameworks.display()
        );
    }
    let frameworks = frameworks.canonicalize().unwrap();
    cc::Build::new()
        .file("sparkle/MSSparkleBridge.m")
        .flag("-fobjc-arc")
        .flag(format!("-F{}", frameworks.display()))
        .flag("-mmacosx-version-min=13.0")
        .warnings(true)
        .compile("ms_sparkle_bridge");
    println!("cargo:rustc-link-search=framework={}", frameworks.display());
    println!("cargo:rustc-link-lib=framework=Sparkle");
    println!("cargo:rustc-link-lib=framework=Foundation");
    println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path/../Frameworks");
}

/// Liquid Glass (glass/MSGlass.swift): AppKit's glass views under GPUI's, compiled with swiftc into a static library.
/// The Swift runtime is the system's (/usr/lib/swift, macOS 13 has it); the compatibility shims come from the
/// toolchain.
fn build_glass() {
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let arch = match std::env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
        Ok("aarch64") => "arm64",
        Ok("x86_64") => "x86_64",
        other => panic!("Liquid Glass: no Swift target for {other:?}"),
    };
    let library = out.join("libmsglass.a");
    let status = std::process::Command::new("xcrun")
        .args([
            "swiftc",
            "-parse-as-library",
            "-emit-library",
            "-static",
            "-module-name",
            "MSGlass",
            "-O",
        ])
        .args(["-target", &format!("{arch}-apple-macos13.0")])
        .arg("glass/MSGlass.swift")
        .arg("-o")
        .arg(&library)
        .status()
        .expect("Liquid Glass: run swiftc (Xcode or its command line tools)");
    assert!(status.success(), "Liquid Glass: swiftc failed");
    let swiftc = std::process::Command::new("xcrun")
        .args(["--find", "swiftc"])
        .output()
        .expect("Liquid Glass: find swiftc");
    let swiftc = std::path::PathBuf::from(String::from_utf8(swiftc.stdout).unwrap().trim());
    let toolchain_lib = swiftc
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("lib/swift/macosx");
    let sdk = std::process::Command::new("xcrun")
        .args(["--sdk", "macosx", "--show-sdk-path"])
        .output()
        .expect("Liquid Glass: find the macOS SDK");
    let sdk = String::from_utf8(sdk.stdout).unwrap().trim().to_owned();
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=msglass");
    println!("cargo:rustc-link-search=native={}", toolchain_lib.display());
    println!("cargo:rustc-link-search=native={sdk}/usr/lib/swift");
    println!("cargo:rustc-link-lib=framework=AppKit");
    println!("cargo:rustc-link-lib=framework=QuartzCore");
    println!("cargo:rustc-link-arg=-Wl,-rpath,/usr/lib/swift");
}
