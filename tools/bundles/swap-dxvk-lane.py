#!/usr/bin/env python3
"""Replace the dxvk lane inside the graphics-dll and runtime bundles with the
locally built DXVK-macOS DLLs, leaving every other entry byte-identical."""
import os
import shutil
import subprocess
import tarfile
import tempfile
import hashlib
from pathlib import Path

ROOT = Path("/Volumes/AverySSD/metalsharp-dxmt-conf-clean")
NEW64 = Path("/Volumes/AverySSD/dxvk-macos/out64/bin")
NEW32 = Path("/Volumes/AverySSD/dxvk-macos/out32/bin")
GRAPHICS_TAR = ROOT / "app/bundles/metalsharp-graphics-dll.tar.zst"
RUNTIME_TAR = ROOT / "app/bundles/metalsharp-runtime.tar.zst"


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def replace_in_tar(zst_path: Path, replacements: dict, out_path: Path) -> None:
    tmp = Path(tempfile.mkdtemp(prefix="ms-bundle-surgery-"))
    try:
        subprocess.run(
            ["tar", "--use-compress-program=unzstd", "-xf", str(zst_path), "-C", str(tmp)],
            check=True,
        )
        before = {}
        for p in tmp.rglob("*"):
            if p.is_file():
                before[str(p.relative_to(tmp))] = sha256(p)
        for rel, newfile in replacements.items():
            dst = tmp / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(newfile, dst)
        after = {}
        for p in tmp.rglob("*"):
            if p.is_file():
                after[str(p.relative_to(tmp))] = sha256(p)
        changed = sorted(
            [k for k in before if before.get(k) != after.get(k)]
            + [k for k in after if k not in before]
        )
        removed = [k for k in before if k not in after]
        print(f"  changed: {changed}")
        print(f"  removed: {removed}")
        with tarfile.open(out_path, "w", format=tarfile.GNU_FORMAT) as tar:
            for p in sorted(tmp.rglob("*")):
                tar.add(p, arcname=p.relative_to(tmp), recursive=False)
        subprocess.run(["zstd", "-10", "-f", str(out_path)], check=True, stdout=subprocess.DEVNULL)
        os.replace(str(out_path) + ".zst", out_path)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


NAMES = ["d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"]

g_replace = {}
for name in NAMES:
    g_replace[f"Graphics/dll/dxvk/x86_64-windows/{name}"] = NEW64 / name
    g_replace[f"Graphics/dll/dxvk/i386-windows/{name}"] = NEW32 / name
print("== graphics-dll tar ==")
replace_in_tar(GRAPHICS_TAR, g_replace, GRAPHICS_TAR)

r_replace = {}
for name in NAMES:
    r_replace[f"runtime/wine/lib/dxvk/x86_64-windows/{name}"] = NEW64 / name
    r_replace[f"runtime/wine/lib/dxvk/i386-windows/{name}"] = NEW32 / name
print("== runtime tar ==")
replace_in_tar(RUNTIME_TAR, r_replace, RUNTIME_TAR)

print("== new hashes ==")
for t in (GRAPHICS_TAR, RUNTIME_TAR):
    print(t.name, sha256(t), t.stat().st_size)
