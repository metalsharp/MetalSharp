#!/usr/bin/env python3
"""Validate the DMG build/publish contract without building a DMG."""

from __future__ import annotations

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
    """The GPUI release package must carry the same runtime payload the
    Electron extraResources did."""
    packaging = read("app-gpui/package-local-preview.sh")
    for needle in [
        "app/src-c/build/metalsharp-backend\" \"$CONTENTS/Resources/runtime/metalsharp-backend",
        "app/native/host\" \"$CONTENTS/Resources/runtime/host",
        "app/updater\" \"$CONTENTS/Resources/scripts/tools/",
        'cp "$source" "$CONTENTS/Resources/bundles/"',
        "METALSHARP_GPUI_REQUIRE_BUNDLES",
    ]:
        if needle not in packaging:
            fail(f"GPUI packaging no longer stages {needle}")
    bundle_loop = next((line for line in packaging.splitlines() if line.strip().startswith("for archive in")), "")
    for asset in assets:
        if asset == "metalsharp-d3d12-developer-sdk.tar.zst":
            continue
        if asset.removesuffix(".tar.zst") not in {word.strip(";") for word in bundle_loop.split()}:
            fail(f"GPUI packaging no longer bundles {asset}")

    release_package = read("app-gpui/package-app.sh")
    for needle in [
        'METALSHARP_GPUI_APP_BUNDLE_NAME="MetalSharp.app"',
        'METALSHARP_GPUI_APP_BUNDLE_ID="com.metalsharp.app"',
        'METALSHARP_GPUI_APP_EXECUTABLE="MetalSharp"',
        "METALSHARP_GPUI_CARGO_PROFILE=release",
        "METALSHARP_GPUI_PACKAGE_BACKEND=1",
        'METALSHARP_GPUI_REQUIRE_BUNDLES="${METALSHARP_GPUI_REQUIRE_BUNDLES:-1}"',
        "CMakeLists.txt",
    ]:
        if needle not in release_package:
            fail(f"app-gpui/package-app.sh lost release identity/contract: {needle}")


def check_dmg_verifier(assets: list[str]) -> None:
    verifier = read("tools/dmg/verify-dmg-runtime-assets.sh")
    if "verify-developer-sdk.sh" in verifier:
        fail("DMG verifier must not run D3D12 developer SDK validation")
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
        if asset == "metalsharp-d3d12-developer-sdk.tar.zst":
            continue
        if asset not in verifier:
            fail(f"DMG verifier no longer checks bundle asset {asset}")


def check_updater_handoff() -> None:
    python_updater = read("app/updater/update.py")
    shell_updater = read("app/updater/update.sh")
    for path, updater in [("app/updater/update.py", python_updater), ("app/updater/update.sh", shell_updater)]:
        for needle in ["hdiutil", "attach", "-mountpoint", "metalsharp-update-mount", "detach_mount(mount_point" if path.endswith(".py") else "detach_mount"]:
            if needle not in updater:
                fail(f"{path} no longer mounts the downloaded DMG on a private update mount point before install")

    bridge = read("app-gpui/src/updater_bridge.rs")
    controller = read("app-gpui/src/ui_live.rs")
    if 'variant == "regular"' not in bridge or 'command.arg("--recover")' not in bridge:
        fail("stable in-app updates must hand off to update.sh --recover")
    if '.arg("--app-pid")' not in bridge:
        fail("FEX in-app updates must pass the MetalSharp PID to the updater")
    if "updater_bridge::spawn_install(" not in controller or "&path, pid, &target, variant," not in controller:
        fail("the selected update variant must reach the updater handoff")


def check_bundle_scripts() -> None:
    create_bundles = read("tools/dmg/create-bundles.sh")
    for needle in [
        "tools/dmg/repair-runtime-bundle.py",
        "repair_assets_fnalibs_bundle",
        "tools/bundles/verify-bundles.sh",
        "--bundle-dir \"$BUNDLE_DIR\" \"$asset\"",
        "Refreshing stale bundle",
        "metalsharp-bundle-manifest.tsv",
        "REMOTE_MANIFEST",
        "asset_matches_release_manifest",
        "Downloaded bundle does not match release manifest",
        "metalsharp/MetalSharp",
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

    if "DMG Workflow CI" not in pr:
        fail("PR CI must keep a lightweight DMG Workflow CI job")
    for forbidden in ["electron-builder --mac dmg", "Verify mounted DMG runtime assets"]:
        if forbidden in pr:
            fail(f"PR CI should not run the full DMG build path: {forbidden}")

    for required in ["Shell CI", "Metal CI", "C/C++/Obj-C CI", "DMG Workflow CI"]:
        if required not in main:
            fail(f"main CI missing validation job: {required}")
    for workflow_name, workflow in [("PR CI", pr), ("main CI", main)]:
        if "python3 -m unittest tools.dmg.tests.test_sign_notarize_dmg -v" not in workflow:
            fail(f"{workflow_name} must run the offline DMG signing pipeline tests")
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
        "Sign and notarize distributable DMG",
        "Check Apple signing credentials",
        "Verify Apple notarization",
        "Mark unsigned DMG",
        "Create GitHub Release",
        "Record release identity",
        "RELEASE-TAG.txt",
        "METALSHARP_BUNDLE_REPO: ${{ github.repository }}",
        "Package GPUI app",
        "app-gpui/package-app.sh",
        'METALSHARP_GPUI_SIGN_IDENTITY="${APPLE_SIGNING_IDENTITY:--}"',
        "tools/dmg/package-gpui-dmg.sh app-gpui/target/MetalSharp.app dist/gpui",
        "tools/dmg/verify-dmg-runtime-assets.sh dist/gpui/MetalSharp-*-arm64.dmg",
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

    dmg_job = release.split("\n  build:\n", 1)[1].split("\n  release:\n", 1)[0]
    for excluded in [
        "metalsharp-electron.tar.zst",
        "metalsharp-d3d12-developer-sdk.tar.zst",
    ]:
        if excluded in dmg_job:
            fail(f"DMG release job must not package or check excluded bundle: {excluded}")
    if "METALSHARP_UNSIGNED_DMG=1" not in read("tools/dmg/check-apple-signing-readiness.sh"):
        fail("unsigned DMG fallback must mark the build as unsigned")
    for forbidden in ["electron-builder", "npm ", "setup-node", "app/package.json", "dist/electron"]:
        if forbidden in release:
            fail(f"release workflow must not use the retired Electron toolchain: {forbidden}")
    signing_preparation = read("tools/dmg/prepare-apple-signing.sh")
    if "APPLE_SIGNING_IDENTITY=$APPLE_SIGNING_IDENTITY" not in signing_preparation:
        fail("Apple signing preparation must export the Developer ID identity for DMG signing")
    # The app is signed during packaging; only the finished DMG is notarized.
    if not (release.index("Package GPUI app") < release.index("Package DMG") < release.index("Sign and notarize distributable DMG")):
        fail("release must package and sign the app, build the DMG, then notarize the DMG")
    signing_script = read("tools/dmg/sign-notarize-dmg.sh")
    for required in [
        "codesign --force --sign",
        "-i com.metalsharp.app.dmg",
        "notarytool submit",
        "--wait",
        "--output-format json",
        'notary_status" != "Accepted"',
        "xcrun notarytool log",
        "xcrun stapler staple",
        "xcrun stapler validate",
    ]:
        if required not in signing_script:
            fail(f"DMG signing pipeline missing required operation: {required}")
    if release.index("Sign and notarize distributable DMG") > release.index("Verify Apple notarization"):
        fail("the completed DMG must be signed and notarized before notarization verification")
    packaging = read("app-gpui/package-local-preview.sh")
    for required in [
        'SIGN_IDENTITY="${METALSHARP_GPUI_SIGN_IDENTITY:--}"',
        "codesign --force --deep --sign -",
        "--options runtime",
        "--timestamp",
        "--entitlements",
        "tools/dmg/entitlements.mac.plist",
        "codesign --verify --deep --strict",
    ]:
        if required not in packaging:
            fail(f"GPUI packaging missing signing/hardening contract: {required}")
    entitlements = read("tools/dmg/entitlements.mac.plist")
    for key in [
        "com.apple.security.cs.allow-jit",
        "com.apple.security.cs.allow-unsigned-executable-memory",
        "com.apple.security.cs.disable-library-validation",
        "com.apple.security.cs.allow-dyld-environment-variables",
    ]:
        if key not in entitlements:
            fail(f"release entitlements missing {key}")
    dmg_builder = read("tools/dmg/package-gpui-dmg.sh")
    for required in ["com.metalsharp.app", "MetalSharp-$VERSION-arm64.dmg", "/Applications", "hdiutil create", "hdiutil verify"]:
        if required not in dmg_builder:
            fail(f"GPUI DMG builder missing {required}")
    if (ROOT / ".github/workflows/virustotal-release.yml").exists():
        fail("VirusTotal release workflow must be removed")
    if (ROOT / "tools/ci/virustotal-release.py").exists():
        fail("VirusTotal release scanner must be removed")
    for workflow_name, workflow in [("PR CI", pr), ("main CI", main)]:
        if "virustotal" in workflow.lower():
            fail(f"{workflow_name} must not run VirusTotal checks")

    notarization = read("tools/dmg/verify-notarization.sh")
    for required in [
        "Authority=Developer ID Application",
        'xcrun stapler validate "$dmg"',
        'codesign --verify --verbose=4 "$dmg"',
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
