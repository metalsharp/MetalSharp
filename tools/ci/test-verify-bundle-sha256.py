#!/usr/bin/env python3
"""Regression coverage for tools/ci/verify-bundle-sha256.sh.

The M12 gate downloads prebuilt runtime/graphics bundles from the `bundles`
GitHub release and extracts/executes them (Wine, DXMT DLLs). The verifier must
reject any archive whose SHA-256 does not match the pinned manifest
(tools/ci/m12-bundle-hashes.tsv), mirroring the installer's hash pinning.
"""

import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("verify-bundle-sha256.sh")

ASSET = "metalsharp-runtime.tar.zst"


def sha256_hex(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


class VerifyBundleSha256Tests(unittest.TestCase):
    def run_verifier(self, manifest: Path, asset: str, bundle: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(SCRIPT), str(manifest), asset, str(bundle)],
            capture_output=True,
            text=True,
        )

    def test_accepts_archive_matching_pinned_hash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="verify-bundle-test-") as temp:
            root = Path(temp)
            manifest = root / "m12-bundle-hashes.tsv"
            bundle = root / ASSET
            payload = b"fake bundle payload"
            bundle.write_bytes(payload)
            manifest.write_text(
                f"asset\tsha256\n{ASSET}\t{sha256_hex(payload)}\n"
            )
            result = self.run_verifier(manifest, ASSET, bundle)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_tampered_archive(self) -> None:
        with tempfile.TemporaryDirectory(prefix="verify-bundle-test-") as temp:
            root = Path(temp)
            manifest = root / "m12-bundle-hashes.tsv"
            bundle = root / ASSET
            manifest.write_text(f"asset\tsha256\n{ASSET}\t{sha256_hex(b'good payload')}\n")
            bundle.write_bytes(b"tampered payload")
            result = self.run_verifier(manifest, ASSET, bundle)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("mismatch", result.stderr)
            self.assertIn(sha256_hex(b"tampered payload"), result.stderr)

    def test_rejects_asset_missing_from_manifest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="verify-bundle-test-") as temp:
            root = Path(temp)
            manifest = root / "m12-bundle-hashes.tsv"
            bundle = root / ASSET
            bundle.write_bytes(b"payload")
            manifest.write_text("asset\tsha256\nother-asset.tar.zst\tabc\n")
            result = self.run_verifier(manifest, ASSET, bundle)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no pinned SHA-256", result.stderr)

    def test_rejects_missing_and_empty_archives(self) -> None:
        with tempfile.TemporaryDirectory(prefix="verify-bundle-test-") as temp:
            root = Path(temp)
            manifest = root / "m12-bundle-hashes.tsv"
            bundle = root / ASSET
            manifest.write_text(f"asset\tsha256\n{ASSET}\t{sha256_hex(b'payload')}\n")
            result = self.run_verifier(manifest, ASSET, bundle)
            self.assertNotEqual(result.returncode, 0)
            bundle.write_bytes(b"")
            result = self.run_verifier(manifest, ASSET, bundle)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing or empty", result.stderr)

    def test_rejects_missing_manifest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="verify-bundle-test-") as temp:
            root = Path(temp)
            bundle = root / ASSET
            bundle.write_bytes(b"payload")
            result = self.run_verifier(root / "no-such-manifest.tsv", ASSET, bundle)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("manifest not found", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
