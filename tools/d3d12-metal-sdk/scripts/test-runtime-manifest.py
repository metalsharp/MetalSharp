#!/usr/bin/env python3
"""Isolated manifest completeness/mutation regressions; no runtime loading."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("verify-runtime-manifest.py")
# Independent expected list: omitting a required artifact in the implementation
# must not silently shrink these tests as well.
NAMES = (
    "d3d10core.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "dxgi_dxmt.dll",
    "nvapi64.dll", "nvngx.dll", "winemetal.dll", "winemetal.so", "ntdll.so",
    "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib",
)


class RuntimeManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root / "runtime"
        self.paths = {}
        for name in NAMES:
            folder = "x86_64-windows" if name.endswith(".dll") else "x86_64-unix"
            path = self.runtime / folder / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
            self.paths[name] = path
        self.manifest = self.root / "manifest.json"

    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--runtime-dir", str(self.runtime), *args],
            capture_output=True, text=True,
        )

    def generate(self):
        return self.run_cli("--generate", "--output", str(self.manifest))

    def test_complete_roundtrip_and_mutation(self):
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(self.manifest.read_text())
        self.assertEqual(set(data["artifacts"]), set(NAMES))
        self.assertEqual(self.run_cli("--manifest", str(self.manifest)).returncode, 0)
        self.paths["winemetal.so"].write_bytes(b"mismatched provider")
        self.assertNotEqual(self.run_cli("--manifest", str(self.manifest)).returncode, 0)

    def test_every_missing_artifact_preserves_existing_manifest(self):
        for name, path in self.paths.items():
            with self.subTest(name=name):
                original = path.read_bytes()
                path.unlink()
                self.manifest.write_text("preserve existing manifest\n")
                result = self.generate()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stderr)
                self.assertEqual(self.manifest.read_text(), "preserve existing manifest\n")
                path.write_bytes(original)

    def test_empty_and_directory_artifacts_reject(self):
        path = self.paths["libunwind.1.dylib"]
        path.write_bytes(b"")
        self.assertNotEqual(self.generate().returncode, 0)
        self.assertFalse(self.manifest.exists())
        path.unlink()
        path.mkdir()
        self.assertNotEqual(self.generate().returncode, 0)
        self.assertFalse(self.manifest.exists())

    def test_partial_manifest_cannot_verify(self):
        self.assertEqual(self.generate().returncode, 0)
        original = json.loads(self.manifest.read_text())
        for name in NAMES:
            with self.subTest(name=name):
                partial = dict(original, artifacts=dict(original["artifacts"]))
                del partial["artifacts"][name]
                self.manifest.write_text(json.dumps(partial))
                result = self.run_cli("--manifest", str(self.manifest))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stderr)

    def test_loader_artifact_can_be_resolved_from_selected_wine_root(self):
        ntdll = self.paths["ntdll.so"]
        ntdll.unlink()
        wine_root = self.root / "wine"
        wine_ntdll = wine_root / "lib" / "wine" / "x86_64-unix" / "ntdll.so"
        wine_ntdll.parent.mkdir(parents=True, exist_ok=True)
        wine_ntdll.write_bytes(b"selected wine ntdll")

        result = self.run_cli(
            "--wine-dir", str(wine_root), "--generate", "--output", str(self.manifest)
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(self.manifest.read_text())
        self.assertEqual(data["loader"]["wine_dir"], str(wine_root))
        self.assertEqual(
            self.run_cli("--wine-dir", str(wine_root), "--manifest", str(self.manifest)).returncode,
            0,
        )


if __name__ == "__main__":
    unittest.main()
