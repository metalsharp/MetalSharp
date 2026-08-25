#!/usr/bin/env python3
"""Surgical update of the published metalsharp-runtime bundle.

Adds the controller input shim DLLs (lib/metalsharp/<arch>/*.dll) and
refreshes the runtime backend, preserving every other entry in the archive
byte-for-byte. Uses the same tar normalization as repair-runtime-bundle.py
(uid/gid=0, mtime=0, deterministic order) so the output is reproducible.

Usage:
  update-runtime-bundle.py --archive in.tar.zst --out out.tar.zst \\
      --metalsharp-lib lib/metalsharp --backend metalsharp-backend \\
      --battlenet-wine /path/to/staged/wine-staging-11.4
"""

import argparse
import os
import shutil
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path

INPUT_SHIM_DLLS = [
    "dinput.dll",
    "dinput8.dll",
    "xinput1_1.dll",
    "xinput1_2.dll",
    "xinput1_3.dll",
    "xinput1_4.dll",
    "xinput9_1_0.dll",
]


def extract_archive(archive: Path, dest: Path) -> None:
    subprocess.run(["tar", "--use-compress-program=unzstd", "-xf", str(archive), "-C", str(dest)], check=True)


def add_tree_to_tar(tar: tarfile.TarFile, root: Path) -> None:
    paths = sorted([p for p in root.rglob("*") if p.is_dir()]) + sorted(
        [p for p in root.rglob("*") if p.is_file() or p.is_symlink()]
    )
    for path in paths:
        arcname = path.relative_to(root)
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


def write_archive(source_root: Path, output: Path) -> None:
    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tmp:
        tar_path = Path(tmp.name)
    try:
        with tarfile.open(tar_path, "w") as tar:
            add_tree_to_tar(tar, source_root)
        subprocess.run(
            ["zstd", "-q", "-19", "-T0", "-f", str(tar_path), "-o", str(output)],
            check=True,
        )
        output.chmod(0o644)
    finally:
        tar_path.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--metalsharp-lib", type=Path, required=True)
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--battlenet-wine", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="runtime-bundle-update-") as tmp_name:
        tmp = Path(tmp_name)
        extracted = tmp / "extracted"
        extracted.mkdir()
        extract_archive(args.archive, extracted)

        runtime_root = extracted / "runtime"
        if not runtime_root.is_dir():
            raise FileNotFoundError(f"runtime archive does not contain runtime/: {args.archive}")

        lib_target = runtime_root / "wine" / "lib" / "metalsharp"
        added = []
        for arch in ("x86_64-windows", "i386-windows"):
            for dll in INPUT_SHIM_DLLS:
                src = args.metalsharp_lib / arch / dll
                if not src.is_file():
                    raise FileNotFoundError(f"missing shim source {src}")
                dst = lib_target / arch / dll
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
                added.append(f"{arch}/{dll}")

        backend_dst = runtime_root / "metalsharp-backend"
        if not args.backend.is_file():
            raise FileNotFoundError(f"missing backend {args.backend}")
        shutil.copy2(args.backend, backend_dst)
        backend_dst.chmod(0o755)

        battlenet_required = [
            args.battlenet_wine / "bin" / "wine",
            args.battlenet_wine / "bin" / "wineserver",
            args.battlenet_wine / "metalsharp-battlenet-runtime.json",
        ]
        for required in battlenet_required:
            if not required.is_file() or required.stat().st_size == 0:
                raise FileNotFoundError(f"missing Battle.net runtime file {required}")
        battlenet_dst = runtime_root / "launchers" / "battlenet" / "wine-staging-11.4"
        if battlenet_dst.exists():
            shutil.rmtree(battlenet_dst)
        shutil.copytree(args.battlenet_wine, battlenet_dst, symlinks=True)

        write_archive(extracted, args.out)
        print(f"updated runtime bundle: {args.out}")
        print(f"  added {len(added)} shim DLLs + refreshed backend + isolated Battle.net Wine Staging 11.4")
        for entry in added:
            print(f"    + {entry}")


if __name__ == "__main__":
    main()
