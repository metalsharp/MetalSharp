#!/usr/bin/env python3
"""Stage rebuilt DXMT D3D12 artifacts into a MetalSharp runtime."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[3]
DEFAULT_BUILD_DIR = ROOT_DIR / "vendor" / "dxmt" / "build-metalsharp-x64"
DEFAULT_RUNTIME_DIR = Path(os.path.expanduser("~/.metalsharp/runtime/wine/lib/dxmt"))
DEFAULT_PREFIX_DIR = Path(os.path.expanduser("~/.metalsharp/prefix-steam"))
DEFAULT_RESULTS_DIR = ROOT_DIR / "tools" / "d3d12-metal-sdk" / "results"
LLVM_TOOLCHAIN_NAME = "clang+llvm-15.0.7-x86_64-apple-darwin21.0"

ARTIFACTS = [
    ("src/d3d10/d3d10core.dll", "x86_64-windows/d3d10core.dll"),
    ("src/d3d11/d3d11.dll", "x86_64-windows/d3d11.dll"),
    ("src/d3d12/d3d12.dll", "x86_64-windows/d3d12.dll"),
    ("src/dxgi/dxgi.dll", "x86_64-windows/dxgi.dll"),
    ("src/dxgi/dxgi_dxmt.dll", "x86_64-windows/dxgi_dxmt.dll"),
    ("src/winemetal/winemetal.dll", "x86_64-windows/winemetal.dll"),
    ("src/nvapi/nvapi64.dll", "x86_64-windows/nvapi64.dll"),
    ("src/nvngx/nvngx.dll", "x86_64-windows/nvngx.dll"),
    ("src/winemetal/unix/winemetal.so", "x86_64-unix/winemetal.so"),
]

UNIX_SIDECARS = [
    "libc++.1.dylib",
    "libc++abi.1.dylib",
    "libunwind.1.dylib",
]


def sha256(path: Path) -> str | None:
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_record(path: Path) -> dict:
    return {
        "path": str(path),
        "exists": path.exists(),
        "size": path.stat().st_size if path.exists() else 0,
        "sha256": sha256(path),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Stage rebuilt DXMT artifacts into ~/.metalsharp runtime.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--runtime-dir", type=Path, default=DEFAULT_RUNTIME_DIR)
    parser.add_argument("--prefix", type=Path, default=DEFAULT_PREFIX_DIR)
    parser.add_argument("--results-dir", type=Path, default=DEFAULT_RESULTS_DIR)
    parser.add_argument("--profile", default="metalsharp")
    parser.add_argument("--dry-run", action="store_true", help="Write the manifest without copying files.")
    return parser.parse_args()


def sidecar_source(name: str) -> Path | None:
    roots = [
        Path(os.environ["METALSHARP_X86_LLVM_ROOT"]) if os.environ.get("METALSHARP_X86_LLVM_ROOT") else None,
        Path(os.path.expanduser("~/.metalsharp/toolchains")),
        Path("/Volumes/AverySSD/toolchains"),
    ]
    for root in roots:
        if root is None:
            continue
        candidate = root / LLVM_TOOLCHAIN_NAME / "lib" / name
        if candidate.exists():
            return candidate
    return None


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir
    runtime_dir = args.runtime_dir
    wine_lib_dir = runtime_dir.parent / "wine"
    prefix = args.prefix
    args.results_dir.mkdir(parents=True, exist_ok=True)

    records: list[dict] = []
    failures: list[str] = []

    artifacts = list(ARTIFACTS)
    artifacts.extend(
        [
            # A cross-built DXMT target is a Wine builtin PE. Keep the active
            # Wine module directory in lockstep with the selected runtime;
            # otherwise Wine can resolve an older builtin before the staged
            # x86_64-windows copy and silently run stale code.
            ("src/d3d10/d3d10core.dll", str(wine_lib_dir / "x86_64-windows" / "d3d10core.dll")),
            ("src/d3d11/d3d11.dll", str(wine_lib_dir / "x86_64-windows" / "d3d11.dll")),
            ("src/d3d12/d3d12.dll", str(wine_lib_dir / "x86_64-windows" / "d3d12.dll")),
            ("src/dxgi/dxgi_dxmt.dll", str(wine_lib_dir / "x86_64-windows" / "dxgi.dll")),
            ("src/winemetal/winemetal.dll", str(wine_lib_dir / "x86_64-windows" / "winemetal.dll")),
            ("src/winemetal/unix/winemetal.so", str(wine_lib_dir / "x86_64-unix" / "winemetal.so")),
            ("src/winemetal/winemetal.dll", str(prefix / "drive_c" / "windows" / "system32" / "winemetal.dll")),
        ]
    )
    for sidecar in UNIX_SIDECARS:
        source = sidecar_source(sidecar)
        if source is None:
            failures.append(f"missing LLVM sidecar: {sidecar}")
            continue
        artifacts.append((str(source), f"x86_64-unix/{sidecar}"))
        artifacts.append((str(source), str(wine_lib_dir / "x86_64-unix" / sidecar)))

    for src_rel, dst_rel in artifacts:
        src_path = Path(src_rel)
        src = src_path if src_path.is_absolute() else build_dir / src_path
        dst = Path(dst_rel)
        if not dst.is_absolute():
            dst = runtime_dir / dst
        before = file_record(dst)
        source = file_record(src)

        copied = False
        if not src.exists():
            failures.append(f"missing build artifact: {src}")
        elif not args.dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            copied = True

        after = file_record(dst)
        records.append(
            {
                "source": source,
                "destination_before": before,
                "destination_after": after,
                "copied": copied,
                "match": source["sha256"] is not None and source["sha256"] == after["sha256"],
            }
        )

    mismatches = [
        record
        for record in records
        if record["source"]["exists"] and not args.dry_run and not record["match"]
    ]
    result = {
        "schema": "metalsharp.d3d12-metal.stage-runtime.v1",
        "profile": args.profile,
        "build_dir": str(build_dir),
        "runtime_dir": str(runtime_dir),
        "dry_run": args.dry_run,
        "ok": not failures and not mismatches,
        "failure_count": len(failures) + len(mismatches),
        "failures": failures,
        "artifacts": records,
    }

    out_path = args.results_dir / f"stage-runtime-{args.profile}.json"
    out_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(out_path)

    if not result["ok"]:
        for failure in failures:
            print(f"stage failed: {failure}", file=sys.stderr)
        for mismatch in mismatches:
            print(
                "stage failed: hash mismatch "
                f"{mismatch['source']['path']} -> {mismatch['destination_after']['path']}",
                file=sys.stderr,
            )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
