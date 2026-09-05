#!/usr/bin/env python3
"""Verify DXMT runtime artifacts against a manifest of expected hashes and sizes."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[3]
DEFAULT_MANIFEST = (
    ROOT_DIR / "tools" / "d3d12-metal-sdk" / "contracts" / "dxmt-runtime-manifest.json"
)

# Match the staged PE/Unix provider and its required loader sidecars.
REQUIRED_ARTIFACTS = (
    "d3d10core.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "dxgi_dxmt.dll",
    "nvapi64.dll", "nvngx.dll", "winemetal.dll", "winemetal.so", "ntdll.so",
    "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib",
)

DEFAULT_RUNTIME_SEARCH_PATHS = [
    Path(os.path.expanduser("~/.metalsharp/runtime/wine/lib/dxmt")),
    Path(os.path.expanduser("~/.metalsharp/runtime/wine")),
]


def sha256(path: Path) -> str | None:
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_runtime_dir(runtime_dir: Path | None) -> Path | None:
    if runtime_dir is not None:
        return runtime_dir
    for candidate in DEFAULT_RUNTIME_SEARCH_PATHS:
        if candidate.exists():
            return candidate
    return None


def locate_artifact(runtime_dir: Path, name: str) -> Path:
    direct = runtime_dir / name
    if direct.exists():
        return direct
    if name.endswith(".dll"):
        windows_sub = runtime_dir / "x86_64-windows" / name
        if windows_sub.exists():
            return windows_sub
    if name.endswith((".so", ".dylib")):
        unix_sub = runtime_dir / "x86_64-unix" / name
        if unix_sub.exists():
            return unix_sub
    return direct


def verify_manifest(manifest_path: Path, runtime_dir: Path, verbose: bool) -> int:
    if not manifest_path.exists():
        print(f"[FAIL] manifest not found: {manifest_path}", file=sys.stderr)
        return 1

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"[FAIL] manifest invalid JSON: {exc}", file=sys.stderr)
        return 1

    version = manifest.get("version")
    if version != 1:
        print(f"[FAIL] unsupported manifest version: {version}", file=sys.stderr)
        return 1

    artifacts = manifest.get("artifacts", {})
    if not isinstance(artifacts, dict) or not artifacts:
        print("[FAIL] manifest has no artifact map", file=sys.stderr)
        return 1
    missing = sorted(set(REQUIRED_ARTIFACTS) - artifacts.keys())
    if missing:
        print(f"[FAIL] manifest omits required artifacts: {', '.join(missing)}", file=sys.stderr)
        return 1

    if verbose:
        print(f"manifest: {manifest_path}")
        print(f"build_commit: {manifest.get('build_commit', 'unknown')}")
        print(f"build_timestamp: {manifest.get('build_timestamp', 'unknown')}")
        print(f"runtime_dir: {runtime_dir}")
        print(f"artifacts: {len(artifacts)}")
        print()

    failures = 0
    for name, expected in artifacts.items():
        expected_sha = expected.get("sha256", "")
        expected_size = expected.get("size", 0)
        path = locate_artifact(runtime_dir, name)

        errors: list[str] = []

        if not path.exists():
            errors.append("missing")
        else:
            actual_size = path.stat().st_size
            actual_sha = sha256(path)

            if actual_size != expected_size:
                errors.append(f"size mismatch: expected {expected_size}, got {actual_size}")
            if actual_sha != expected_sha:
                errors.append(f"hash mismatch: expected {expected_sha}, got {actual_sha}")

            if verbose:
                print(f"  {name}: size={actual_size} sha256={actual_sha}")

        if errors:
            failures += 1
            status = "FAIL"
            detail = "; ".join(errors)
        else:
            status = "PASS"
            detail = "ok"

        print(f"[{status}] {name}: {detail}")

    print()
    if failures:
        print(f"[FAIL] {failures}/{len(artifacts)} artifact(s) failed verification")
        return 1
    print(f"[PASS] all {len(artifacts)} artifacts verified")
    return 0


def generate_manifest(runtime_dir: Path, output_path: Path | None, verbose: bool) -> int:
    if not runtime_dir.exists():
        print(f"[FAIL] runtime directory not found: {runtime_dir}", file=sys.stderr)
        return 1

    # Keep the manifest aligned with stage-phase6-sandbox.py.  d3d10core,
    # dxgi_dxmt and nvngx are staged runtime artifacts; d3d10.dll is not.
    # The Unix C++ runtime sidecars are required by winemetal.so and must be
    # hashed as part of the same provenance record.
    artifacts: dict[str, dict] = {}
    found = 0
    missing: list[str] = []
    for name in REQUIRED_ARTIFACTS:
        path = locate_artifact(runtime_dir, name)
        if not path.is_file() or path.stat().st_size == 0:
            missing.append(name)
            continue
        file_hash = sha256(path)
        file_size = path.stat().st_size
        artifacts[name] = {
            "sha256": file_hash or "",
            "size": file_size,
        }
        found += 1
        if verbose:
            print(f"  {name}: size={file_size} sha256={file_hash}")

    if missing:
        print(f"[FAIL] missing or empty required artifacts: {', '.join(missing)}", file=sys.stderr)
        # Never replace an existing manifest with a partial inventory.
        return 1

    manifest = {
        "version": 1,
        "build_commit": os.environ.get("DXMT_BUILD_COMMIT", ""),
        "build_timestamp": datetime.now(timezone.utc).isoformat(),
        "_note": "Generated by verify-runtime-manifest.py --generate. Regenerate during build.",
        "artifacts": artifacts,
    }

    manifest_text = json.dumps(manifest, indent=2) + "\n"

    if output_path is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(manifest_text, encoding="utf-8")
        print(f"[PASS] wrote manifest with {found} artifacts to {output_path}")
    else:
        print(manifest_text, end="")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify DXMT runtime artifacts against a manifest."
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST,
        help="Path to dxmt-runtime-manifest.json",
    )
    parser.add_argument(
        "--runtime-dir",
        type=Path,
        default=None,
        help="DXMT runtime directory (default: auto-detect from standard MetalSharp paths)",
    )
    parser.add_argument(
        "--generate",
        action="store_true",
        help="Generate a manifest from the runtime directory instead of verifying",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output path for --generate (default: stdout)",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print detailed per-artifact information",
    )
    args = parser.parse_args()

    runtime_dir = resolve_runtime_dir(args.runtime_dir)
    if runtime_dir is None:
        print("[FAIL] could not locate runtime directory; pass --runtime-dir explicitly", file=sys.stderr)
        return 1

    if args.generate:
        return generate_manifest(runtime_dir, args.output, args.verbose)

    return verify_manifest(args.manifest, runtime_dir, args.verbose)


if __name__ == "__main__":
    raise SystemExit(main())
