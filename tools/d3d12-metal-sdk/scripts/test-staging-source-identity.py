#!/usr/bin/env python3
"""Staging content identity tests without copying/loading runtime binaries."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "stage", Path(__file__).with_name("stage-phase6-sandbox.py"))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class StagingIdentityTests(unittest.TestCase):
    def test_content_snapshot_tracks_dirty_source_and_not_build_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for directory in ("src", "include", "libs"):
                path = root / "vendor/dxmt" / directory
                path.mkdir(parents=True)
                (path / "input.hpp").write_text(directory)
            before = stage.staging_source_identity(root)
            digest = before["source_tree_sha256"]
            self.assertEqual(len(digest), 64)
            self.assertEqual(before["source_identity_file_count"], 3)
            self.assertIn("not_build_attestation", before["source_identity_kind"])
            self.assertEqual(before, stage.staging_source_identity(root))
            source = root / "vendor/dxmt/src/input.hpp"
            source.write_text("uncommitted GPU scheduler change")
            changed = stage.staging_source_identity(root)
            self.assertNotEqual(digest, changed["source_tree_sha256"])
            build = root / "vendor/dxmt/build-test"
            build.mkdir()
            (build / "runtime.dll").write_bytes(b"generated")
            self.assertEqual(changed, stage.staging_source_identity(root))
            source.rename(source.with_name("renamed.hpp"))
            self.assertNotEqual(changed["source_tree_sha256"],
                                stage.staging_source_identity(root)["source_tree_sha256"])

    def test_missing_scope_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                stage.staging_source_identity(Path(temporary))


if __name__ == "__main__":
    unittest.main()
