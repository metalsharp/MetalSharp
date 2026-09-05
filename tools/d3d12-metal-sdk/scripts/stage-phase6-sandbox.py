#!/usr/bin/env python3
"""Stage a Phase 6 runtime into a disposable, provenance-checked sandbox.

Unlike the historical runtime staging helper, this script never writes to the
MetalSharp installation, Wine's global builtin directory, or a persistent
prefix.  Every PE and Unix artifact used by a Phase 6 run is copied below the
requested sandbox root and the bridge exports are checked before a probe can
start.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
SDK_DIR = ROOT_DIR / "tools/d3d12-metal-sdk"
CONTRACT = SDK_DIR / "contracts/winemetal-bridge-contract.json"
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
SIDE_CARS = ("libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib")


def sha256(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def record(path: Path) -> dict[str, Any]:
    return {
        "path": str(path),
        "exists": path.is_file(),
        "size": path.stat().st_size if path.is_file() else 0,
        "sha256": sha256(path),
    }


def exports(path: Path) -> set[str]:
    try:
        output = subprocess.check_output(
            ["x86_64-w64-mingw32-objdump", "-p", str(path)],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return set()
    names: set[str] = set()
    for line in output.splitlines():
        parts = line.strip().split()
        if len(parts) >= 4 and re.match(r"^\[\s*\d+\]$", " ".join(parts[:2])):
            names.add(parts[-1])
    return names


def unix_exports(path: Path) -> set[str]:
    try:
        output = subprocess.check_output(["nm", "-g", str(path)], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return set()
    names: set[str] = set()
    for line in output.splitlines():
        parts = line.strip().split()
        if not parts:
            continue
        name = parts[-1]
        names.add(name)
        if name.startswith("__"):
            names.update((name[1:], name[2:]))
        elif name.startswith("_"):
            names.add(name[1:])
    return names


def git_value(*args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], cwd=ROOT_DIR, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sandbox-root", type=Path, required=True)
    parser.add_argument("--wine-root", type=Path, required=True)
    parser.add_argument("--profile", default="phase6-exhaustive")
    parser.add_argument("--results-dir", type=Path, default=SDK_DIR / "results")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    sandbox = args.sandbox_root.resolve()
    runtime = sandbox / "runtime"
    sandbox_wine = sandbox / "wine"
    runtime_windows = runtime / "x86_64-windows"
    runtime_unix = runtime / "x86_64-unix"
    wine_windows = sandbox_wine / "lib/wine/x86_64-windows"
    wine_unix = sandbox_wine / "lib/wine/x86_64-unix"
    for directory in (runtime_windows, runtime_unix, wine_windows, wine_unix):
        directory.mkdir(parents=True, exist_ok=True)

    failures: list[str] = []
    staged: list[dict[str, Any]] = []
    for source_rel, destination_rel in ARTIFACTS:
        source = build_dir / source_rel
        destination = runtime / destination_rel
        source_record = record(source)
        postprocessed = False
        if not source_record["exists"]:
            failures.append(f"missing build artifact: {source}")
        else:
            shutil.copy2(source, destination)
            if destination_rel == "x86_64-windows/winemetal.dll":
                # Meson's symbolextractor/postprocess target can leave the
                # build-tree DLL in a reduced-export form.  Run the pinned
                # Wine builtin postprocessor on the sandbox copy only.  This
                # is the crucial PE-side step: doing it in the source build
                # tree or the global Wine tree caused stale/mismatched halves.
                winebuild = args.wine_root / "bin/winebuild"
                if not winebuild.is_file():
                    failures.append(f"missing pinned winebuild: {winebuild}")
                else:
                    try:
                        subprocess.check_call(
                            [str(winebuild), "--builtin", str(destination)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                        postprocessed = True
                    except (OSError, subprocess.CalledProcessError) as exc:
                        failures.append(f"Winemetal builtin postprocess failed: {exc}")
        staged.append(
            {
                "source": source_record,
                "destination": record(destination),
                "source_hash_before_postprocess": source_record["sha256"],
                "postprocessed": postprocessed,
                "match": postprocessed
                or (source_record["sha256"] is not None
                    and source_record["sha256"] == sha256(destination)),
            }
        )

    # Cross-built DXMT PE targets are Wine builtin modules. Mirror the
    # selected build into the sandbox builtin directory as well as the route
    # directory; otherwise WINEDLLPATH can fall through to an older global
    # d3d12/dxgi module even though the route copy has the new source hash.
    for source_name, builtin_name in (
        ("d3d10core.dll", "d3d10core.dll"),
        ("d3d11.dll", "d3d11.dll"),
        ("d3d12.dll", "d3d12.dll"),
        ("dxgi_dxmt.dll", "dxgi.dll"),
    ):
        source = runtime_windows / source_name
        destination = wine_windows / builtin_name
        source_record = record(source)
        if not source_record["exists"]:
            failures.append(f"missing builtin source artifact: {source}")
            continue
        shutil.copy2(source, destination)
        staged.append(
            {
                "source": source_record,
                "destination": record(destination),
                "source_hash_before_postprocess": source_record["sha256"],
                "postprocessed": False,
                "match": source_record["sha256"] == sha256(destination),
                "builtin_duplicate": True,
            }
        )

    # The Unix loader resolves its module and Wine dependencies from a Wine
    # builtin tree.  Keep those copies inside the disposable sandbox.
    wmt_pe = runtime_windows / "winemetal.dll"
    wmt_so = runtime_unix / "winemetal.so"
    if wmt_pe.is_file():
        shutil.copy2(wmt_pe, wine_windows / "winemetal.dll")
    if wmt_so.is_file():
        shutil.copy2(wmt_so, wine_unix / "winemetal.so")
    for name in ("winemac.so", "ntdll.so"):
        source = args.wine_root / "lib/wine/x86_64-unix" / name
        destination = wine_unix / name
        if not source.is_file():
            failures.append(f"missing Wine Unix dependency: {source}")
        else:
            # Wine's loader searches the selected DXMT route first.  Keep the
            # loader dependencies in that route as well as in the sandbox
            # builtin tree; otherwise WINEDLLPATH masks ntdll.so and Wine
            # reports the misleading "could not exec the wine loader" error.
            shutil.copy2(source, destination)
            shutil.copy2(source, runtime_unix / name)
    for name in SIDE_CARS:
        source = runtime_unix / name
        if not source.is_file():
            # Existing runtimes may already contain sidecars.  Fall back to
            # the pinned toolchain only; never search an arbitrary host path.
            source = Path.home() / ".cache/metalsharp/toolchains/clang+llvm-15.0.7-x86_64-apple-darwin21.0/lib" / name
        destination = runtime_unix / name
        if not source.is_file():
            failures.append(f"missing pinned LLVM sidecar: {name}")
        elif source.resolve() != destination.resolve():
            # Re-staging an existing sandbox already has its pinned sidecars
            # at the destination; copying a file onto itself raises SameFileError.
            shutil.copy2(source, destination)

    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    required_pe = set(contract.get("required_pe_exports", []))
    actual_pe = exports(wmt_pe)
    missing_pe = sorted(required_pe - actual_pe)
    if missing_pe:
        failures.append(
            "staged Winemetal PE is incomplete; rebuild the raw builtin before staging; "
            f"missing exports={missing_pe}"
        )
    actual_unix = unix_exports(wmt_so)
    for name in ("__wine_unix_call_funcs", "__wine_unix_call_wow64_funcs"):
        if name not in actual_unix:
            failures.append(f"staged Winemetal Unix library is missing {name}")

    # A staged runtime is only valid when its duplicate builtin copies are
    # byte-identical.  This catches exactly the stale-half failure that used
    # to contaminate Phase 5 evidence.
    for source, duplicate in (
        (wmt_pe, wine_windows / "winemetal.dll"),
        (wmt_so, wine_unix / "winemetal.so"),
    ):
        if source.is_file() and duplicate.is_file() and sha256(source) != sha256(duplicate):
            failures.append(f"PE/Unix builtin duplicate differs from runtime: {source} -> {duplicate}")

    args.results_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": "metalsharp.d3d12-metal.phase6-sandbox-stage.v1",
        "profile": args.profile,
        "ok": not failures and all(item["match"] for item in staged),
        "failure_count": len(failures) + sum(not item["match"] for item in staged),
        "failures": failures,
        "build_dir": str(build_dir),
        "sandbox_root": str(sandbox),
        "runtime_dir": str(runtime),
        "wine_root": str(sandbox_wine),
        "source_commit": git_value("rev-parse", "HEAD"),
        "source_tree_sha256": git_value("rev-parse", "HEAD^{tree}"),
        "source_dirty": bool(git_value("status", "--porcelain")),
        "artifacts": staged,
        "winemetal_pe_exports": sorted(actual_pe),
        "winemetal_unix_exports": sorted(actual_unix),
        "unique_runtime_required": True,
        "writes_outside_sandbox": False,
    }
    output = args.results_dir / f"stage-phase6-sandbox-{args.profile}.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(output)
    if not manifest["ok"]:
        for failure in failures:
            print(f"[FAIL] {failure}")
        for item in staged:
            if not item["match"]:
                print(f"[FAIL] staged artifact hash mismatch: {item['destination']['path']}")
        return 1
    print(f"[PASS] phase6 sandbox staged: {runtime}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
