#!/usr/bin/env python3
"""Regression coverage for tools/package/prepare-native-placeholders.sh.

Issue #452: the must-build gate only rejected zero-byte files; missing
artifacts produced warnings with exit 0, so `npm run dist` could sail through
without the CMake-produced native shims and electron-builder would emit an app
missing its native surface. These tests pin the failure behavior and the
legacy placeholder/fallback behavior.

Run directly:  python3 tools/package/test_prepare_native.py
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("prepare-native-placeholders.sh")

if sys.platform == "darwin":
    PLATFORM_SHLIB_EXT = "dylib"
elif sys.platform.startswith("linux"):
    PLATFORM_SHLIB_EXT = "so"
elif sys.platform in ("win32", "cygwin", "msys"):
    PLATFORM_SHLIB_EXT = "dll"
else:
    PLATFORM_SHLIB_EXT = "dylib"

MUST_BUILD_SHLIBS = [
    f"{base}.{PLATFORM_SHLIB_EXT}"
    for base in ("d3d11", "d3d12", "dxgi", "xaudio2_9", "xinput1_4", "opengl32")
]
MUST_BUILD_BINARIES = ["metalsharp", "metalsharp_launcher"]
HOST_RUNTIME = f"libmetalsharp_host_runtime.{PLATFORM_SHLIB_EXT}"
EAC_ARTIFACTS = ["metalsharp_eac_substrate.dylib", "metalsharp_eac_libc.so.6"]
EXTERNAL_FILES = ["metalsharp-process-manager-helper", "metalsharp-activate-pid"]


class PrepareNativePlaceholdersTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="prepare-native-test-")
        self.root = Path(self.temp.name)
        self.native = self.root / "app" / "native"
        self.host = self.native / "host"
        (self.root / "include" / "metalsharp").mkdir(parents=True)
        (self.root / "include" / "metalsharp" / "HostRuntimeABI.h").write_text(
            "// metalSharp host runtime ABI\n"
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_script(self) -> subprocess.CompletedProcess:
        env = dict(os.environ, METALSHARP_PROJECT_ROOT=str(self.root))
        return subprocess.run(
            [str(SCRIPT)], env=env, capture_output=True, text=True
        )

    def write_artifact(self, rel: str, payload: bytes = b"real-binary") -> None:
        path = self.native / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)

    def stage_complete_tree(self) -> None:
        for name in MUST_BUILD_SHLIBS + MUST_BUILD_BINARIES:
            self.write_artifact(name)
        self.host.mkdir(parents=True, exist_ok=True)
        (self.host / HOST_RUNTIME).write_bytes(b"host-runtime")
        if PLATFORM_SHLIB_EXT == "dylib":
            for name in EAC_ARTIFACTS:
                self.write_artifact(name)

    def test_missing_must_build_shim_fails(self) -> None:
        # Regression for #452: a fresh app/native must fail loudly instead of
        # warning, so packaging can never ship without the CMake artifacts.
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must-build shim missing", result.stderr)
        self.assertIn(f"{self.native}/d3d11.{PLATFORM_SHLIB_EXT}", result.stderr)

    def test_complete_tree_passes(self) -> None:
        self.stage_complete_tree()
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_zero_byte_must_build_shim_fails(self) -> None:
        self.stage_complete_tree()
        self.write_artifact(f"dxgi.{PLATFORM_SHLIB_EXT}", b"")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("zero bytes", result.stderr)
        self.assertIn(f"dxgi.{PLATFORM_SHLIB_EXT}", result.stderr)

    def test_missing_host_runtime_fails(self) -> None:
        self.stage_complete_tree()
        (self.host / HOST_RUNTIME).unlink()
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("host runtime library missing", result.stderr)

    def test_missing_eac_artifacts_fail_on_macos(self) -> None:
        if PLATFORM_SHLIB_EXT != "dylib":
            self.skipTest("EAC substrate check is macOS-only")
        self.stage_complete_tree()
        (self.native / EAC_ARTIFACTS[0]).unlink()
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("EAC substrate artifact missing", result.stderr)

    def test_external_files_still_stubbed(self) -> None:
        self.stage_complete_tree()
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in EXTERNAL_FILES:
            path = self.native / name
            self.assertTrue(path.exists(), f"{name} should be stubbed")
            self.assertEqual(path.stat().st_size, 0, f"{name} should be empty")

    def test_host_abi_and_manifest_fallback_preserved(self) -> None:
        self.stage_complete_tree()
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"metalSharp host runtime ABI", (self.host / "HostRuntimeABI.h").read_bytes())
        manifest = (self.host / "manifest.json").read_text()
        self.assertIn("metalsharp-host-runtime", manifest)


if __name__ == "__main__":
    unittest.main()
