#!/usr/bin/env python3
"""Surgical update of the published metalsharp-graphics-dll bundle.

Adds the vkd3d-proton VKD3D stack lanes — vkd3d-proton (d3d12/d3d12core,
x86_64 + i386), DXVK (dxgi/d3d11/d3d10core/d3d9, x86_64 + i386), and the
VKMT MoltenVK lane (libMoltenVK.dylib + MoltenVK_icd.json) — to an
existing graphics bundle, preserving every other entry byte-for-byte.
Uses the same tar normalization as update-runtime-bundle.py (uid/gid=0,
mtime=0, deterministic order) so the output is reproducible.

Can also replace the DXMT lane (--dxmt-root, dir with x86_64-windows/,
x86_64-unix/, i386-windows/, i386-unix/) when rebuilding DXMT for a new
macOS deployment target.

Usage:
  update-graphics-bundle.py --archive in.tar.zst --out out.tar.zst \\
      --dxmt-root PATH (dir with x86_64-windows/, x86_64-unix/, i386-windows/, i386-unix/) \\
      --vkd3d-root PATH (dir with x86_64-windows/ and i386-windows/) \\
      --dxvk-root PATH   (dir with x86_64-windows/ and i386-windows/) \\
      --moltenvk-root PATH (dir with libMoltenVK.dylib + MoltenVK_icd.json)
"""

import argparse
import os
import shutil
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path

LANES = {
    "dxmt": ("dxmt_root", ["x86_64-windows", "x86_64-unix", "i386-windows", "i386-unix"]),
    "vkd3d-proton": ("vkd3d_root", ["x86_64-windows", "i386-windows"]),
    "dxvk": ("dxvk_root", ["x86_64-windows", "i386-windows"]),
    "moltenvk-vkmt": ("moltenvk_root", []),
}


def extract_archive(archive: Path, dest: Path) -> None:
    subprocess.run(["tar", "--use-compress-program=unzstd", "-xf", str(archive), "-C", str(dest)], check=True)


def add_tree_to_tar(tar: tarfile.TarFile, root: Path, prefix: str) -> None:
    root_info = tarfile.TarInfo(prefix)
    root_info.type = tarfile.DIRTYPE
    root_info.mode = 0o755
    root_info.mtime = 0
    tar.addfile(root_info)
    paths = sorted([p for p in root.rglob("*") if p.is_dir()]) + sorted(
        [p for p in root.rglob("*") if p.is_file() or p.is_symlink()]
    )
    for path in paths:
        arcname = f"{prefix}/{path.relative_to(root)}"
        info = tar.gettarinfo(str(path), arcname=arcname)
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
            tar.addfile(info)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--dxmt-root", type=Path)
    parser.add_argument("--vkd3d-root", type=Path)
    parser.add_argument("--dxvk-root", type=Path)
    parser.add_argument("--moltenvk-root", type=Path)
    args = parser.parse_args()

    if not any([args.dxmt_root, args.vkd3d_root, args.dxvk_root, args.moltenvk_root]):
        parser.error("at least one of --dxmt-root/--vkd3d-root/--dxvk-root/--moltenvk-root is required")

    with tempfile.TemporaryDirectory(prefix="ms-graphics-update-") as tmp:
        tmp_path = Path(tmp)
        extracted = tmp_path / "extracted"
        extracted.mkdir()
        extract_archive(args.archive, extracted)

        staging = tmp_path / "staging"
        staging.mkdir()
        for lane, (root_arg, arches) in LANES.items():
            root = getattr(args, root_arg)
            if not root:
                continue
            lane_dst = staging / lane
            lane_dst.mkdir(parents=True)
            if arches:
                for arch in arches:
                    src = root / arch
                    if src.is_dir():
                        shutil.copytree(src, lane_dst / arch, dirs_exist_ok=True)
                    else:
                        print(f"warning: {root_arg} missing {arch} (skipped)")
            else:
                shutil.copytree(root, lane_dst, dirs_exist_ok=True)

        # Rebuild all unchanged entries, replacing (not appending) the lanes supplied
        # by the caller. Appending the same path makes BSD tar's effective result
        # depend on member order and bloats every subsequent republish.
        replaced_prefixes = tuple(
            f"Graphics/dll/{lane}" for lane, (root_arg, _) in LANES.items() if getattr(args, root_arg)
        )
        out_tar = tmp_path / "graphics.tar"
        with tarfile.open(out_tar, "w") as tar:
            for member in sorted(extracted.rglob("*")):
                arcname = str(member.relative_to(extracted))
                if any(arcname == prefix or arcname.startswith(f"{prefix}/") for prefix in replaced_prefixes):
                    continue
                info = tar.gettarinfo(str(member), arcname=arcname)
                info.uid = 0
                info.gid = 0
                info.uname = ""
                info.gname = ""
                info.mtime = 0
                if member.is_dir() and not member.is_symlink():
                    info.mode = 0o755
                    tar.addfile(info)
                elif member.is_symlink():
                    info.type = tarfile.SYMTYPE
                    info.linkname = os.readlink(member)
                    info.mode = 0o777
                    tar.addfile(info)
                elif member.is_file():
                    mode = stat.S_IMODE(member.stat().st_mode)
                    info.mode = 0o755 if mode & stat.S_IXUSR else 0o644
                    with member.open("rb") as fh:
                        tar.addfile(info, fh)
            for lane in sorted(staging.iterdir()):
                add_tree_to_tar(tar, lane, f"Graphics/dll/{lane.name}")

        subprocess.run(
            ["zstd", "-q", "-19", "-T0", "-f", str(out_tar), "-o", str(args.out)],
            check=True,
        )
        print(f"wrote {args.out} ({args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
