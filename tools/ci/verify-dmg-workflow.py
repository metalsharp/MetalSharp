#!/usr/bin/env python3
"""Validate the DMG build/publish contract without building a DMG."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

PACKAGE_ASSETS = [
    "install-metalsharp-wine-runtime.sh",
    "metalsharp-bundle-manifest.tsv",
    "MetalSharp-Wine-Public-Source-2026-07-31.tar.zst",
    "MetalSharp-Wine-Public-Source-2026-07-31.tar.zst.sha256",
    "PARTS-SHA256SUMS.txt",
    "REASSEMBLE.txt",
]
RUNTIME_PARTS = [
    f"MetalSharp-Wine-Runtime-COMPLETE-all-arch-2026-07-31.tar.zst.part{part:02d}"
    for part in range(1, 5)
]
RELEASE_ASSETS = PACKAGE_ASSETS + RUNTIME_PARTS
RETIRED_BUNDLES = [
    "metalsharp-electron.tar.zst",
    "metalsharp-graphics-dll.tar.zst",
    "metalsharp-runtime.tar.zst",
    "metalsharp-assets.tar.zst",
    "fnalibs.tar.zst",
    "metalsharp-scripts-tools.tar.zst",
    "metalsharp-steam.tar.zst",
    "metalsharp-d3d12-developer-sdk.tar.zst",
]


def fail(message: str) -> None:
    print(f"DMG workflow check failed: {message}", file=sys.stderr)
    sys.exit(1)


def read(path: str) -> str:
    full = ROOT / path
    if not full.exists():
        fail(f"missing required file: {path}")
    return full.read_text()


def check_package_resources() -> None:
    package = json.loads(read("app/package.json"))
    build = package.get("build", {})
    resources = build.get("extraResources", [])
    if not isinstance(resources, list):
        fail("app/package.json build.extraResources must be a list")

    pairs = {
        (entry.get("from"), entry.get("to"))
        for entry in resources
        if isinstance(entry, dict)
    }
    required_pairs = {
        ("src-rust/target/release/metalsharp-backend", "runtime/metalsharp-backend"),
        ("native/host", "runtime/host"),
        ("updater", "scripts/tools/updater"),
    }
    required_pairs.add(("release-runtime", "runtime-bundle"))

    missing = sorted(required_pairs - pairs)
    if missing:
        fail(f"app/package.json missing extraResources entries: {missing}")

    runtime_resource = next(
        (entry for entry in resources if isinstance(entry, dict) and entry.get("from") == "release-runtime"),
        None,
    )
    if runtime_resource is None:
        fail("app/package.json has no complete-runtime resource directory")
    filters = runtime_resource.get("filter", [])
    missing_assets = sorted(set(PACKAGE_ASSETS) - set(filters))
    if missing_assets:
        fail(f"complete-runtime resource filter is missing: {missing_assets}")
    for entry in resources:
        source = entry.get("from", "") if isinstance(entry, dict) else ""
        if source.startswith("bundles/") and source.endswith(".tar.zst"):
            fail(f"retired split bundle is still packaged: {source}")

    if build.get("afterPack") != "build/adhoc-deep-sign.cjs":
        fail("app/package.json must keep afterPack=build/adhoc-deep-sign.cjs")
    if build.get("afterSign") != "build/notarize.cjs":
        fail("app/package.json must keep afterSign=build/notarize.cjs")


def check_dmg_verifier() -> None:
    verifier = read("tools/dmg/verify-dmg-runtime-assets.sh")
    for needle in [
        "Contents/Resources",
        "runtime/metalsharp-backend",
        "runtime/host",
        "scripts/tools/updater/update.py",
        "scripts/tools/updater/update.sh",
        "prepare-complete-runtime-assets.sh",
        "--verify-package",
        "DMG still contains retired split runtime bundles",
    ]:
        if needle not in verifier:
            fail(f"DMG verifier no longer checks {needle}")

    for asset in PACKAGE_ASSETS:
        if asset not in verifier:
            fail(f"DMG verifier no longer checks complete-runtime asset {asset}")


def check_updater_handoff() -> None:
    python_updater = read("app/updater/update.py")
    shell_updater = read("app/updater/update.sh")
    for path, updater in [("app/updater/update.py", python_updater), ("app/updater/update.sh", shell_updater)]:
        for needle in ["hdiutil", "attach", "-mountpoint", "metalsharp-update-mount", "detach_mount(mount_point" if path.endswith(".py") else "detach_mount"]:
            if needle not in updater:
                fail(f"{path} no longer mounts the downloaded DMG on a private update mount point before install")


def check_bundle_scripts() -> None:
    prepare_runtime = read("tools/dmg/prepare-complete-runtime-assets.sh")
    for needle in [
        "v0.60.0-dependency-bundles",
        "93a456a40a7bf0ad2fecace5c01c58a366f85cc2901f6f8780c056c9e3b256ee",
        "metalsharp-bundle-manifest.tsv",
        "--verify-only",
        "--verify-package",
        "PARTS-SHA256SUMS.txt",
        "shasum -a 256",
        "bash -n",
    ]:
        if needle not in prepare_runtime:
            fail(f"complete-runtime asset preparation no longer performs {needle}")
    for asset in PACKAGE_ASSETS:
        if asset not in prepare_runtime:
            fail(f"complete-runtime preparation omits release asset {asset}")
    for part in range(1, 5):
        if f'"$ARCHIVE.part{part:02d}"' not in prepare_runtime:
            fail(f"complete-runtime preparation omits runtime part {part:02d}")


def check_workflows() -> None:
    pr = read(".github/workflows/pr-ci.yml")
    main = read(".github/workflows/ci.yml")
    release = read(".github/workflows/release.yml")

    if "DMG Workflow CI" not in pr:
        fail("PR CI must keep a lightweight DMG Workflow CI job")
    for forbidden in ["electron-builder --mac dmg", "Verify mounted DMG runtime assets"]:
        if forbidden in pr:
            fail(f"PR CI should not run the full DMG build path: {forbidden}")

    for required in ["Shell CI", "Metal CI", "Vue CI", "Rust CI", "Electron CI", "C/C++/Obj-C CI", "DMG Workflow CI"]:
        if required not in main:
            fail(f"main CI missing validation job: {required}")
    for forbidden in [
        "Verify Developer SDK Bundle",
        "Build DMG",
        "Package DMG",
        "Verify DMG runtime assets",
        "metalsharp-build-artifacts",
    ]:
        if forbidden in main:
            fail(f"main CI should not run the full DMG build path: {forbidden}")
    if "group: metalsharp-developer-sdk-bundles" in main:
        fail("main CI verifier must not share the release SDK publish concurrency group")

    for required in [
        "Build DMG",
        "Download complete runtime assets for DMG and release",
        "Check Apple signing credentials",
        "Verify Apple notarization",
        "Mark unsigned DMG",
        "Create GitHub Release",
    ]:
        if required not in release:
            fail(f"release workflow missing publish step: {required}")
    for required in [
        "tools/dmg/prepare-complete-runtime-assets.sh app/release-runtime",
        "app/release-runtime/*",
        "--verify-only release-flat",
    ]:
        if required not in release:
            fail(f"release workflow missing complete-runtime contract: {required}")
    for forbidden in [
        "Publish Developer SDK Bundle",
        "tools/dmg/create-bundles.sh",
        "tools/dmg/stage-release-bundles.sh",
    ] + RETIRED_BUNDLES:
        if forbidden in release:
            fail(f"release workflow still references retired bundle path: {forbidden}")
    for asset in RELEASE_ASSETS:
        if asset not in release:
            fail(f"release workflow does not publish {asset}")
    for required in [
        "tools/dmg/check-apple-signing-readiness.sh",
        "steps.apple-signing.outputs.ready == 'true'",
        "DMG-SIGNING.txt",
    ]:
        if required not in release:
            fail(f"release workflow missing signing fallback contract: {required}")
    if "CSC_IDENTITY_AUTO_DISCOVERY=false" not in read("tools/dmg/check-apple-signing-readiness.sh"):
        fail("unsigned DMG fallback must disable Electron Builder certificate discovery")
    adhoc_sign = read("app/build/adhoc-deep-sign.cjs")
    for required in [
        "METALSHARP_UNSIGNED_DMG",
        "codesign",
        "--deep",
        "--timestamp=none",
        "--verify",
    ]:
        if required not in adhoc_sign:
            fail(f"ad-hoc deep-sign hook missing hardening contract: {required}")
    notarization = read("tools/dmg/verify-notarization.sh")
    for required in [
        "Authority=Developer ID Application",
        "xcrun stapler validate",
        "hdiutil verify",
        "spctl -a -vvv --type open",
    ]:
        if required not in notarization:
            fail(f"notarization verifier missing hardening check: {required}")


def main() -> int:
    check_package_resources()
    check_dmg_verifier()
    check_updater_handoff()
    check_bundle_scripts()
    check_workflows()
    print(f"DMG workflow contract verified ({len(PACKAGE_ASSETS)} packaged, {len(RELEASE_ASSETS)} release assets).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
