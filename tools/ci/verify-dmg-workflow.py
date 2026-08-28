#!/usr/bin/env python3
"""Validate the DMG build/publish contract without building a DMG."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def fail(message: str) -> None:
    print(f"DMG workflow check failed: {message}", file=sys.stderr)
    sys.exit(1)


def read(path: str) -> str:
    full = ROOT / path
    if not full.exists():
        fail(f"missing required file: {path}")
    return full.read_text()


def manifest_assets() -> list[str]:
    assets: list[str] = []
    for line in read("tools/bundles/asset-manifest.tsv").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 3:
            fail(f"invalid manifest row: {line}")
        asset, _root, platforms = fields[:3]
        if "mac" in platforms.split(","):
            assets.append(asset)
    if not assets:
        fail("bundle manifest has no mac assets")
    return assets


def check_package_resources(assets: list[str]) -> None:
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
        ("src-c/build/metalsharp-backend", "runtime/metalsharp-backend"),
        ("native/host", "runtime/host"),
        ("updater", "scripts/tools/updater"),
    }
    required_pairs.update((f"bundles/{asset}", f"bundles/{asset}") for asset in assets)

    missing = sorted(required_pairs - pairs)
    if missing:
        fail(f"app/package.json missing extraResources entries: {missing}")

    if build.get("afterPack") != "build/adhoc-deep-sign.cjs":
        fail("app/package.json must keep afterPack=build/adhoc-deep-sign.cjs")
    if build.get("afterSign") != "build/notarize.cjs":
        fail("app/package.json must keep afterSign=build/notarize.cjs")


def check_dmg_verifier(assets: list[str]) -> None:
    verifier = read("tools/dmg/verify-dmg-runtime-assets.sh")
    for needle in [
        "Contents/Resources",
        "runtime/metalsharp-backend",
        "runtime/host",
        "scripts/tools/updater/update.py",
        "scripts/tools/updater/update.sh",
        "tools/bundles/verify-bundles.sh",
    ]:
        if needle not in verifier:
            fail(f"DMG verifier no longer checks {needle}")

    for asset in assets:
        if asset not in verifier:
            fail(f"DMG verifier no longer checks bundle asset {asset}")


def check_updater_handoff() -> None:
    python_updater = read("app/updater/update.py")
    shell_updater = read("app/updater/update.sh")
    for path, updater in [("app/updater/update.py", python_updater), ("app/updater/update.sh", shell_updater)]:
        for needle in ["hdiutil", "attach", "-mountpoint", "metalsharp-update-mount", "detach_mount(mount_point" if path.endswith(".py") else "detach_mount"]:
            if needle not in updater:
                fail(f"{path} no longer mounts the downloaded DMG on a private update mount point before install")


def check_bundle_scripts() -> None:
    create_bundles = read("tools/dmg/create-bundles.sh")
    for needle in [
        "tools/dmg/repair-runtime-bundle.py",
        "repair_assets_fnalibs_bundle",
        "tools/bundles/verify-bundles.sh",
        "--bundle-dir \"$BUNDLE_DIR\" \"$asset\"",
        "Refreshing stale bundle",
        "metalsharp-bundle-manifest.tsv",
    ]:
        if needle not in create_bundles:
            fail(f"create-bundles.sh no longer performs {needle}")

    stage_bundles = read("tools/dmg/stage-release-bundles.sh")
    if "asset-manifest.tsv" not in stage_bundles or "tar --use-compress-program=unzstd" not in stage_bundles:
        fail("stage-release-bundles.sh no longer stages bundle manifest archives")


def check_workflows() -> None:
    pr = read(".github/workflows/pr-ci.yml")
    main = read(".github/workflows/ci.yml")
    release = read(".github/workflows/release.yml")
    virustotal = read(".github/workflows/virustotal-release.yml")

    if "DMG Workflow CI" not in pr:
        fail("PR CI must keep a lightweight DMG Workflow CI job")
    for forbidden in ["electron-builder --mac dmg", "Verify mounted DMG runtime assets"]:
        if forbidden in pr:
            fail(f"PR CI should not run the full DMG build path: {forbidden}")

    for required in ["Shell CI", "Metal CI", "Vue CI", "Electron CI", "C/C++/Obj-C CI", "DMG Workflow CI"]:
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
        "Publish Developer SDK Bundle",
        "Publish developer SDK package",
        "Publish developer SDK bundle",
        "Build DMG",
        "Check Apple signing credentials",
        "Verify Apple notarization",
        "Mark unsigned DMG",
        "Create GitHub Release",
        "Record release identity",
        "RELEASE-TAG.txt",
    ]:
        if required not in release:
            fail(f"release workflow missing publish step: {required}")
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
    for required in [
        "workflow_run:",
        'workflows: ["Release CI"]',
        "github.event.workflow_run.conclusion == 'success'",
        "github.event.workflow_run.event == 'push'",
        "github.event.workflow_run.id",
        "github.event.workflow_run.head_sha",
        "gh run download",
        "RELEASE-TAG.txt",
        "VIRUSTOTAL_API_KEY",
        "virustotal-release.py validate-assets",
        "virustotal-release.py scan",
        "virustotal-release.py merge-body",
        "gh release edit",
    ]:
        if required not in virustotal:
            fail(f"VirusTotal release workflow missing exact-release contract: {required}")
    for forbidden in ["gh release download", "/releases/latest", "latest/download"]:
        if forbidden in virustotal:
            fail(f"VirusTotal release workflow may scan a stale release: {forbidden}")

    scanner = read("tools/ci/virustotal-release.py")
    for required in [
        "650 * 1024 * 1024",
        "part-{index}-of-{PART_COUNT}",
        "PART_COUNT = 3",
        "files/upload_url",
        "/api/v3/analyses/",
        "partial-file scans",
        "not a complete mountable DMG",
    ]:
        if required not in scanner:
            fail(f"VirusTotal scanner missing required behavior: {required}")

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
    assets = manifest_assets()
    check_package_resources(assets)
    check_dmg_verifier(assets)
    check_updater_handoff()
    check_bundle_scripts()
    check_workflows()
    print(f"DMG workflow contract verified ({len(assets)} mac bundle assets).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
