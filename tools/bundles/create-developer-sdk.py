#!/usr/bin/env python3
"""Build the self-contained D3D12 developer SDK bundle from split runtime assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SDK_SOURCE = PROJECT_ROOT / "tools" / "d3d12-metal-sdk"
DEFAULT_BUNDLE_DIR = PROJECT_ROOT / "app" / "bundles"
DEFAULT_OUT_DIR = PROJECT_ROOT / "dist" / "developer-sdk"
SDK_ASSET = "metalsharp-d3d12-developer-sdk.tar.zst"
SDK_ROOT = "developer-sdk/d3d12"

RUNTIME_ASSET = "metalsharp-runtime.tar.zst"
GRAPHICS_ASSET = "metalsharp-graphics-dll.tar.zst"

CRITICAL_FILES = [
    "runtime/wine/bin/wine",
    "runtime/dxmt/x86_64-windows/d3d12.dll",
    "runtime/dxmt/x86_64-windows/dxgi.dll",
    "runtime/dxmt/x86_64-windows/dxgi_dxmt.dll",
    "runtime/dxmt/x86_64-windows/winemetal.dll",
    "runtime/dxmt/x86_64-unix/winemetal.so",
    "runtime/dxmt_vkd3d/x86_64-windows/d3d12.dll",
    "runtime/dxmt_vkd3d/x86_64-windows/dxgi.dll",
    "runtime/dxmt_vkd3d/x86_64-windows/dxgi_dxmt.dll",
    "runtime/dxmt_vkd3d/x86_64-windows/winemetal.dll",
    "runtime/dxmt_vkd3d/x86_64-unix/winemetal.so",
    "runtime/dxmt_vkd3d/x86_64-unix/libc++.1.dylib",
    "runtime/dxmt_vkd3d/x86_64-unix/libc++abi.1.dylib",
    "runtime/dxmt_vkd3d/x86_64-unix/libunwind.1.dylib",
    "scripts/run-probes.sh",
    "scripts/preflight-runtime-layout.py",
    "scripts/stage-dxmt-runtime.py",
]


def copy_tree(src: Path, dst: Path, ignore=None) -> None:
    if not src.exists():
        return
    for root, dirs, files in os.walk(src):
        root_path = Path(root)
        rel = root_path.relative_to(src)
        if ignore and ignore(rel, True):
            dirs[:] = []
            continue
        dirs[:] = [name for name in dirs if not ignore or not ignore(rel / name, True)]
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
    if src.is_file():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def extract_zst(archive: Path, dest: Path) -> None:
    if not archive.is_file() or archive.stat().st_size == 0:
        raise FileNotFoundError(f"missing bundle asset: {archive}")
    dest.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["tar", "--use-compress-program=unzstd", "-xf", str(archive), "-C", str(dest)],
        check=True,
    )


def add_tree_to_tar(tar: tarfile.TarFile, root: Path, arc_root: str) -> None:
    files = sorted(path for path in root.rglob("*") if path.is_file() or path.is_symlink())
    dirs = sorted(path for path in root.rglob("*") if path.is_dir())
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
            with path.open("rb") as handle:
                tar.addfile(info, handle)
        else:
            info.mode = 0o755
            tar.addfile(info)


def write_tar_zst(source_root: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tmp:
        tar_path = Path(tmp.name)
    try:
        with tarfile.open(tar_path, "w") as tar:
            add_tree_to_tar(tar, source_root, SDK_ROOT)
        subprocess.run(["zstd", "-q", "-19", "-T0", "-f", str(tar_path), "-o", str(output)], check=True)
        output.chmod(0o644)
    finally:
        tar_path.unlink(missing_ok=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sdk_ignore(rel: Path, is_dir: bool) -> bool:
    ignored_roots = {"cache", "external", "out"}
    if rel.parts[:1] and rel.parts[0] in ignored_roots:
        return True
    return False


def file_record(root: Path, rel: str) -> dict:
    path = root / rel
    return {
        "path": rel,
        "exists": path.exists(),
        "size": path.stat().st_size if path.exists() else 0,
        "sha256": sha256(path) if path.is_file() else None,
    }


def write_runtime_manifest(sdk_root: Path, runtime_asset: Path, graphics_asset: Path) -> None:
    records = [file_record(sdk_root, rel) for rel in CRITICAL_FILES]
    missing = [record["path"] for record in records if not record["exists"]]
    manifest = {
        "schema": "metalsharp.d3d12-developer-sdk.runtime.v1",
        "root": SDK_ROOT,
        "runtimeAsset": {
            "name": runtime_asset.name,
            "sha256": sha256(runtime_asset),
            "size": runtime_asset.stat().st_size,
        },
        "graphicsAsset": {
            "name": graphics_asset.name,
            "sha256": sha256(graphics_asset),
            "size": graphics_asset.stat().st_size,
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


def rewrite_release_manifest(source: Path, dest: Path, sdk_archive: Path) -> None:
    lines = source.read_text(encoding="utf-8").splitlines()
    rewritten: list[str] = []
    replaced = False
    note = "generated self-contained D3D12 developer SDK bundle"
    for line in lines:
        if line.startswith(f"{SDK_ASSET}\t"):
            rewritten.append(f"{SDK_ASSET}\t{SDK_ROOT}\t{sha256(sdk_archive)}\t{sdk_archive.stat().st_size}\t{note}")
            replaced = True
        else:
            rewritten.append(line)
    if not replaced:
        rewritten.append(
            f"{SDK_ASSET}\t{SDK_ROOT}\t{sha256(sdk_archive)}\t{sdk_archive.stat().st_size}\t"
            "generated self-contained D3D12 developer SDK bundle"
        )
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text("\n".join(rewritten) + "\n", encoding="utf-8")


def build_sdk(bundle_dir: Path, out_dir: Path, release_manifest: Path | None) -> Path:
    runtime_asset = bundle_dir / RUNTIME_ASSET
    graphics_asset = bundle_dir / GRAPHICS_ASSET
    out_dir.mkdir(parents=True, exist_ok=True)
    output = out_dir / SDK_ASSET

    with tempfile.TemporaryDirectory(prefix="metalsharp-developer-sdk-") as tmp_name:
        tmp = Path(tmp_name)
        runtime_src = tmp / "runtime-asset"
        graphics_src = tmp / "graphics-asset"
        sdk_root = tmp / "sdk-root"
        extract_zst(runtime_asset, runtime_src)
        extract_zst(graphics_asset, graphics_src)

        copy_tree(SDK_SOURCE, sdk_root, ignore=sdk_ignore)
        copy_tree(runtime_src / "runtime" / "wine", sdk_root / "runtime" / "wine")
        copy_tree(runtime_src / "runtime" / "host", sdk_root / "runtime" / "host")
        copy_file(runtime_src / "runtime" / "metalsharp-backend", sdk_root / "runtime" / "metalsharp-backend")
        copy_tree(graphics_src / "Graphics" / "dll" / "dxmt", sdk_root / "runtime" / "dxmt")
        copy_tree(graphics_src / "Graphics" / "dll" / "dxmt-vkd3d", sdk_root / "runtime" / "dxmt_vkd3d")
        write_runtime_manifest(sdk_root, runtime_asset, graphics_asset)
        write_tar_zst(sdk_root, output)

    if release_manifest and release_manifest.is_file():
        rewrite_release_manifest(release_manifest, out_dir / "metalsharp-bundle-manifest.tsv", output)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description="Create metalsharp-d3d12-developer-sdk.tar.zst.")
    parser.add_argument("--bundle-dir", type=Path, default=DEFAULT_BUNDLE_DIR)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument(
        "--manifest",
        type=Path,
        help="Optional release manifest to copy and rewrite with the new SDK archive hash.",
    )
    args = parser.parse_args()

    output = build_sdk(args.bundle_dir, args.out_dir, args.manifest)
    print(f"{output}\t{sha256(output)}\t{output.stat().st_size}")
    manifest = args.out_dir / "metalsharp-bundle-manifest.tsv"
    if manifest.is_file():
        print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
