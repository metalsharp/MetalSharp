#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
APP_DIR = PROJECT_ROOT / "app"
SOURCE_BUNDLES = APP_DIR / "bundles"
OUT_DIR = PROJECT_ROOT / "dist" / "bundles"

SPLIT_BUNDLES = {
    "electron": "metalsharp-electron.tar.zst",
    "graphics": "metalsharp-graphics-dll.tar.zst",
    "runtime": "metalsharp-runtime.tar.zst",
    "assets": "metalsharp-assets.tar.zst",
    "scripts": "metalsharp-scripts-tools.tar.zst",
    "steam": "metalsharp-steam.tar.zst",
    "sdk": "metalsharp-d3d12-developer-sdk.tar.zst",
}


def copy_tree(src: Path, dst: Path, ignore=None) -> None:
    if not src.exists():
        return
    for root, dirs, files in os.walk(src):
        root_path = Path(root)
        rel = root_path.relative_to(src)
        if ignore and ignore(rel, True):
            dirs[:] = []
            continue
        dirs[:] = [d for d in dirs if not ignore or not ignore(rel / d, True)]
        target_root = dst / rel
        target_root.mkdir(parents=True, exist_ok=True)
        for file_name in files:
            rel_file = rel / file_name
            if ignore and ignore(rel_file, False):
                continue
            src_file = root_path / file_name
            dst_file = target_root / file_name
            if src_file.is_symlink():
                target = os.readlink(src_file)
                dst_file.unlink(missing_ok=True)
                os.symlink(target, dst_file)
            else:
                shutil.copy2(src_file, dst_file)


def copy_file(src: Path, dst: Path) -> None:
    if src.exists():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def require_file(src: Path, description: str) -> None:
    if not src.is_file() or src.stat().st_size == 0:
        raise FileNotFoundError(f"missing required {description}: {src}")


def require_host_runtime(host_dir: Path) -> None:
    require_file(host_dir / "manifest.json", "host runtime manifest")
    require_file(host_dir / "HostRuntimeABI.h", "host runtime ABI header")
    libraries = [
        host_dir / "libmetalsharp_host_runtime.dylib",
        host_dir / "libmetalsharp_host_runtime.so",
        host_dir / "metalsharp_host_runtime.dll",
    ]
    if not any(path.is_file() and path.stat().st_size > 0 for path in libraries):
        names = ", ".join(str(path) for path in libraries)
        raise FileNotFoundError(f"missing required non-empty host runtime library; checked {names}")


def extract_zst(archive: Path, dest: Path) -> None:
    if not archive.exists():
        raise FileNotFoundError(f"missing source archive: {archive}")
    dest.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["tar", "--use-compress-program=unzstd", "-xf", str(archive), "-C", str(dest)],
        check=True,
    )


def add_tree_to_tar(tar: tarfile.TarFile, root: Path, arc_root: str) -> None:
    files = sorted([p for p in root.rglob("*") if p.is_file()])
    dirs = sorted([p for p in root.rglob("*") if p.is_dir()])
    for path in [root, *dirs, *files]:
        rel = path.relative_to(root)
        arcname = Path(arc_root) / rel if str(rel) != "." else Path(arc_root)
        info = tar.gettarinfo(str(path), arcname=str(arcname))
        info.uid = 0
        info.gid = 0
        info.uname = ""
        info.gname = ""
        info.mtime = 0
        if path.is_symlink():
            info.type = tarfile.SYMTYPE
            info.linkname = os.readlink(path)
            info.mode = 0o777
            tar.addfile(info)
        elif path.is_file():
            mode = stat.S_IMODE(path.stat().st_mode)
            info.mode = 0o755 if mode & stat.S_IXUSR else 0o644
            with path.open("rb") as fh:
                tar.addfile(info, fh)
        else:
            info.mode = 0o755
            tar.addfile(info)


def write_tar_zst(source_root: Path, arc_root: str, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tmp:
        tar_path = Path(tmp.name)
    try:
        with tarfile.open(tar_path, "w") as tar:
            add_tree_to_tar(tar, source_root, arc_root)
        subprocess.run(["zstd", "-q", "-19", "-T0", "-f", str(tar_path), "-o", str(output)], check=True)
        output.chmod(0o644)
    finally:
        tar_path.unlink(missing_ok=True)


def sha256(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


SDK_CRITICAL_FILES = [
    "runtime/wine/bin/wine",
    "runtime/host/manifest.json",
    "runtime/host/HostRuntimeABI.h",
    "runtime/host/libmetalsharp_host_runtime.dylib",
    "runtime/dxmt/x86_64-windows/d3d12.dll",
    "runtime/dxmt/x86_64-windows/dxgi.dll",
    "runtime/dxmt/x86_64-windows/dxgi_dxmt.dll",
    "runtime/dxmt/x86_64-windows/winemetal.dll",
    "runtime/dxmt/x86_64-unix/winemetal.so",
    "runtime/dxmt_m12/x86_64-windows/d3d12.dll",
    "runtime/dxmt_m12/x86_64-windows/dxgi.dll",
    "runtime/dxmt_m12/x86_64-windows/dxgi_dxmt.dll",
    "runtime/dxmt_m12/x86_64-windows/winemetal.dll",
    "runtime/dxmt_m12/x86_64-unix/winemetal.so",
    "runtime/dxmt_m12/x86_64-unix/libc++.1.dylib",
    "runtime/dxmt_m12/x86_64-unix/libc++abi.1.dylib",
    "runtime/dxmt_m12/x86_64-unix/libunwind.1.dylib",
    "scripts/run-probes.sh",
    "scripts/preflight-runtime-layout.py",
    "scripts/stage-dxmt-runtime.py",
]


def sdk_file_record(root: Path, rel: str) -> dict:
    path = root / rel
    return {
        "path": rel,
        "exists": path.exists(),
        "size": path.stat().st_size if path.exists() else 0,
        "sha256": sha256(path) if path.is_file() else None,
    }


def write_sdk_runtime_manifest(sdk_root: Path, runtime_archive: Path, graphics_archive: Path) -> None:
    records = [sdk_file_record(sdk_root, rel) for rel in SDK_CRITICAL_FILES]
    missing = [record["path"] for record in records if not record["exists"]]
    manifest = {
        "schema": "metalsharp.d3d12-developer-sdk.runtime.v1",
        "root": "developer-sdk/d3d12",
        "runtimeAsset": {
            "name": runtime_archive.name,
            "sha256": sha256(runtime_archive),
            "size": runtime_archive.stat().st_size,
        },
        "graphicsAsset": {
            "name": graphics_archive.name,
            "sha256": sha256(graphics_archive),
            "size": graphics_archive.stat().st_size,
        },
        "platforms": {
            "macos": "bundled Wine runtime and DXMT Metal bridge are staged and ready to run on Apple Silicon hosts",
            "linux": "SDK probes, contracts, and scripts are portable; use a local Wine runtime with the staged DXMT files for non-Metal host investigation",
            "windows": "probe sources, contracts, and PowerShell helpers are included; native Windows does not use Wine",
        },
        "criticalFiles": records,
        "ok": not missing,
        "missing": missing,
    }
    out = sdk_root / "runtime" / "manifest.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def build_staging(tmp: Path) -> dict[str, Path]:
    source1 = tmp / "source-bundle-1"
    source2 = tmp / "source-bundle-2"
    source_dxmt = tmp / "source-dxmt"
    extract_zst(SOURCE_BUNDLES / "metalsharp_bundle.tar.zst", source1)
    extract_zst(SOURCE_BUNDLES / "metalsharp_bundle2.tar.zst", source2)
    extract_zst(SOURCE_BUNDLES / "dxmt.tar.zst", source_dxmt)

    roots = {
        "electron": tmp / "electron-root",
        "graphics": tmp / "graphics-root",
        "runtime": tmp / "runtime-root",
        "assets": tmp / "assets-root",
        "scripts": tmp / "scripts-root",
        "steam": tmp / "steam-root",
        "sdk": tmp / "sdk-root",
    }
    for root in roots.values():
        root.mkdir(parents=True, exist_ok=True)

    copy_tree(APP_DIR / "dist", roots["electron"] / "dist")
    copy_file(APP_DIR / "package.json", roots["electron"] / "package.json")

    wine_src = source1 / "wine-11.5"
    copy_tree(wine_src, roots["runtime"] / "wine")
    copy_tree(source2 / "wine" / "etc", roots["runtime"] / "wine" / "etc")
    backend = APP_DIR / "build" / "c-backend" / "metalsharp-backend"
    require_file(backend, "runtime backend")
    copy_file(backend, roots["runtime"] / "metalsharp-backend")
    require_host_runtime(APP_DIR / "native" / "host")
    copy_tree(APP_DIR / "native" / "host", roots["runtime"] / "host")
    require_host_runtime(roots["runtime"] / "host")

    # Validate native shim dylibs are real binaries, not zero-byte placeholders.
    # `require_file` already checks `is_file() and st_size == 0`, so this guards
    # against the legacy `prepare-native-placeholders.sh` stub files slipping
    # into the scripts-tools bundle when CMake post-build copy did not run.
    # We validate only the platform-specific extension set so the bundle can
    # be produced on the host that built the artifacts (macOS .dylib, Linux
    # .so, Windows .dll). Cross-platform binaries (metalsharp, metalsharp_launcher)
    # are always required.
    native_dylibs = [
        "d3d11.dylib",
        "d3d12.dylib",
        "dxgi.dylib",
        "xaudio2_9.dylib",
        "xinput1_4.dylib",
        "opengl32.dylib",
    ]
    native_so = [
        "d3d11.so",
        "d3d12.so",
        "dxgi.so",
        "xaudio2_9.so",
        "xinput1_4.so",
    ]
    native_dll = [name.replace(".so", ".dll") for name in native_so]
    native_binaries = ["metalsharp", "metalsharp_launcher"]
    if sys.platform == "darwin":
        platform_shlibs = native_dylibs
    elif sys.platform.startswith("linux"):
        platform_shlibs = native_so
    elif sys.platform in ("win32", "cygwin", "msys"):
        platform_shlibs = native_dll
    else:
        platform_shlibs = native_dylibs + native_so
    for name in platform_shlibs + native_binaries:
        require_file(APP_DIR / "native" / name, f"native shim {name}")
    require_file(
        PROJECT_ROOT / "lib" / "metalsharp" / "x86_64-windows" / "metalsharp_ntdll_hook.dll",
        "MetalSharp ntdll hook DLL",
    )
    require_file(
        PROJECT_ROOT / "lib" / "metalsharp" / "i386-windows" / "metalsharp_ntdll_hook.dll",
        "MetalSharp ntdll hook DLL (i386, MetalFX poller for 32-bit games)",
    )
    copy_tree(PROJECT_ROOT / "lib" / "metalsharp", roots["runtime"] / "wine" / "lib" / "metalsharp")

    copy_tree(source_dxmt / "x86_64-unix", roots["graphics"] / "dxmt" / "x86_64-unix")
    copy_tree(source_dxmt / "x86_64-windows", roots["graphics"] / "dxmt" / "x86_64-windows")
    m12_root_env = os.environ.get("METALSHARP_DXMT_M12_ROOT")
    m12_root = Path(m12_root_env).expanduser() if m12_root_env else Path.home() / ".metalsharp" / "runtime" / "wine" / "lib" / "dxmt_m12"
    if m12_root.exists():
        copy_tree(m12_root / "x86_64-unix", roots["graphics"] / "dxmt_m12" / "x86_64-unix")
        copy_tree(m12_root / "x86_64-windows", roots["graphics"] / "dxmt_m12" / "x86_64-windows")

    for name in ["mono-arm64", "goldberg", "shims", "shader-cache"]:
        copy_tree(source2 / name, roots["assets"] / name)
    copy_tree(source2 / "wine" / "etc", roots["assets"] / "wine" / "etc")

    optional_archives = {
        "dxvk.tar.zst": ("dxvk", "dxvk-1.10.3"),
        "mono-x86.tar.zst": ("mono-x86", "mono-x86"),
        "fnalibs.tar.zst": ("fnalibs", "fnalibs"),
        "fna-kickstart.tar.zst": ("fna-kickstart", "fna-kickstart"),
    }
    for archive_name, (extract_name, target_name) in optional_archives.items():
        archive = SOURCE_BUNDLES / archive_name
        if archive.exists():
            extracted = tmp / f"source-{extract_name}"
            extract_zst(archive, extracted)
            source = extracted / target_name
            if source.exists():
                copy_tree(source, roots["assets"] / target_name)
            else:
                copy_tree(extracted, roots["assets"] / target_name)

    fnalibs = roots["assets"] / "fnalibs"
    kickstart_osx = roots["assets"] / "fna-kickstart" / "osx"
    for name in ["libFNA3D.0.dylib", "libSDL2-2.0.0.dylib", "libFAudio.0.dylib", "libtheorafile.dylib"]:
        copy_file(fnalibs / name, kickstart_osx / name)

    copy_tree(APP_DIR / "updater", roots["scripts"] / "updater")
    copy_tree(PROJECT_ROOT / "configs", roots["scripts"] / "configs")
    copy_tree(APP_DIR / "native", roots["scripts"] / "native", ignore=lambda rel, is_dir: rel.parts[:1] == ("host",))
    for cef_asset in SOURCE_BUNDLES.glob("cef*"):
        copy_file(cef_asset, roots["scripts"] / "cef" / cef_asset.name)

    for steam_asset in ["SteamSetup.exe", "steamwebhelper.exe", "steamwebhelper-wrapper.c"]:
        require_file(SOURCE_BUNDLES / steam_asset, f"Steam asset {steam_asset}")
        copy_file(SOURCE_BUNDLES / steam_asset, roots["steam"] / steam_asset)

    def sdk_ignore(rel: Path, is_dir: bool) -> bool:
        return is_dir and rel.parts[:1] in {("cache",), ("external",), ("out",)}

    copy_tree(PROJECT_ROOT / "tools" / "d3d12-metal-sdk", roots["sdk"], ignore=sdk_ignore)
    copy_tree(roots["runtime"] / "wine", roots["sdk"] / "runtime" / "wine")
    copy_tree(roots["runtime"] / "host", roots["sdk"] / "runtime" / "host")
    copy_file(roots["runtime"] / "metalsharp-backend", roots["sdk"] / "runtime" / "metalsharp-backend")
    copy_tree(roots["graphics"] / "dxmt", roots["sdk"] / "runtime" / "dxmt")
    copy_tree(roots["graphics"] / "dxmt_m12", roots["sdk"] / "runtime" / "dxmt_m12")
    write_sdk_runtime_manifest(
        roots["sdk"],
        SOURCE_BUNDLES / "metalsharp_bundle.tar.zst",
        SOURCE_BUNDLES / "dxmt.tar.zst",
    )

    return roots


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", default=str(OUT_DIR))
    parser.add_argument("--only", help="comma-separated bundle keys")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    selected = set(args.only.split(",")) if args.only else set(SPLIT_BUNDLES)

    with tempfile.TemporaryDirectory(prefix="metalsharp-split-bundles-") as tmp_name:
        roots = build_staging(Path(tmp_name))
        manifest_lines = ["asset\troot\tsha256\tsize\tnotes"]
        arc_roots = {
            "electron": "electron",
            "graphics": "Graphics/dll",
            "runtime": "runtime",
            "assets": "assets",
            "scripts": "scripts/tools",
            "steam": "steam",
            "sdk": "developer-sdk/d3d12",
        }
        for key, filename in SPLIT_BUNDLES.items():
            if key not in selected:
                continue
            output = out_dir / filename
            write_tar_zst(roots[key], arc_roots[key], output)
            manifest_lines.append(f"{filename}\t{arc_roots[key]}\t{sha256(output)}\t{output.stat().st_size}\tgenerated split bundle")

    manifest = out_dir / "metalsharp-bundle-manifest.tsv"
    manifest.write_text("\n".join(manifest_lines) + "\n")
    print(manifest)
    for line in manifest.read_text().splitlines()[1:]:
        print(line)


if __name__ == "__main__":
    main()
