#!/usr/bin/env python3
"""Stage MetalSharp's isolated Battle.net Wine runtime from its pinned archive."""

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

VERSION = "11.4"
ARCHIVE_SHA256 = "087fd5cc907f792ba01e4a25af1ee817cb354adb331a2d8571c53ed0792fad5c"
ARCHIVE_URL = (
    "https://github.com/dawn-winery/dawn-signed/releases/download/"
    "wine-gcenx-11.4-osx64/wine-staging-11.4-osx64-signed.tar.xz"
)
ARCHIVE_ROOT = "wine-staging-11.4-osx64-signed"
WINE_SUBTREE = f"{ARCHIVE_ROOT}/Contents/Resources/wine"
MANIFEST_NAME = "metalsharp-battlenet-runtime.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_member(member: tarfile.TarInfo) -> None:
    path = PurePosixPath(member.name)
    if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != ARCHIVE_ROOT:
        raise ValueError(f"unsafe archive member: {member.name}")
    if member.ischr() or member.isblk() or member.isfifo():
        raise ValueError(f"unsupported archive member type: {member.name}")
    if member.issym() or member.islnk():
        target = PurePosixPath(member.linkname)
        if target.is_absolute() or ".." in target.parts:
            raise ValueError(f"unsafe archive link: {member.name} -> {member.linkname}")


def validate_archive(archive: Path) -> None:
    actual = sha256(archive)
    if actual != ARCHIVE_SHA256:
        raise ValueError(f"Battle.net Wine archive hash mismatch: expected {ARCHIVE_SHA256}, got {actual}")
    saw_wine = False
    saw_wineserver = False
    with tarfile.open(archive, "r:xz") as bundle:
        for member in bundle:
            validate_member(member)
            saw_wine |= member.name == f"{WINE_SUBTREE}/bin/wine" and member.isfile()
            saw_wineserver |= member.name == f"{WINE_SUBTREE}/bin/wineserver" and member.isfile()
    if not saw_wine or not saw_wineserver:
        raise ValueError("Battle.net Wine archive is missing its Wine loader or Wineserver")


def verify_macho(path: Path) -> None:
    result = subprocess.run(["/usr/bin/file", "-b", str(path)], check=True, capture_output=True, text=True)
    if "Mach-O 64-bit executable x86_64" not in result.stdout:
        raise ValueError(f"unexpected executable identity for {path}: {result.stdout.strip()}")


def verify_staged(root: Path) -> None:
    wine = root / "bin/wine"
    wineserver = root / "bin/wineserver"
    for executable in (wine, wineserver):
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise ValueError(f"missing executable: {executable}")
        verify_macho(executable)
    result = subprocess.run([str(wine), "--version"], check=True, capture_output=True, text=True)
    if result.stdout.strip() != "wine-11.4 (Staging)":
        raise ValueError(f"unexpected Wine version: {result.stdout.strip()}")
    for path in root.rglob("*"):
        if path.is_symlink():
            continue
        if path.stat().st_mode & (stat.S_ISUID | stat.S_ISGID):
            raise ValueError(f"setuid/setgid payload is forbidden: {path}")


def stage(archive: Path, output: Path) -> None:
    validate_archive(archive)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="metalsharp-battlenet-wine-") as temporary_name:
        temporary = Path(temporary_name)
        subprocess.run(["/usr/bin/tar", "-xJf", str(archive), "-C", str(temporary)], check=True)
        source = temporary / WINE_SUBTREE
        verify_staged(source)
        staged = output.with_name(f"{output.name}.tmp.{os.getpid()}")
        if staged.exists():
            shutil.rmtree(staged)
        shutil.copytree(source, staged, symlinks=True)
        manifest = {
            "schema": "metalsharp.battlenet-wine-runtime.v1",
            "version": VERSION,
            "channel": "staging",
            "wineVersion": "wine-11.4 (Staging)",
            "archive": {
                "url": ARCHIVE_URL,
                "sha256": ARCHIVE_SHA256,
                "upstream": "Gcenx/macOS_Wine_builds 11.4",
                "redistributor": "dawn-winery/dawn-signed",
            },
            "isolation": "runtime/launchers/battlenet/wine-staging-11.4",
            "battleNetCompatibility": {
                "codeWeaversHack": 19610,
                "graphicsArgs": [
                    "--in-process-gpu",
                    "--use-gl=swiftshader",
                    "--disable-gpu-compositing",
                ],
            },
        }
        (staged / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        verify_staged(staged)
        if output.exists():
            shutil.rmtree(output)
        staged.rename(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    stage(args.archive.resolve(), args.out.resolve())
    print(args.out.resolve())


if __name__ == "__main__":
    main()
