//! With the `sparkle` feature: compiles sparkle/MSSparkleBridge.m and links Sparkle.framework, which MetalSharp.app
//! carries in Contents/Frameworks. SPARKLE_FRAMEWORK_DIR names the folder holding Sparkle.framework; by default it is
//! the one tools/sparkle/fetch-sparkle.sh unpacks.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=sparkle/MSSparkleBridge.m");
    println!("cargo:rerun-if-env-changed=SPARKLE_FRAMEWORK_DIR");
    let macos = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos");
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
