#!/usr/bin/env python3
"""Unit tests for the VirusTotal release scanner's offline behavior."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("virustotal-release.py")
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("virustotal_release", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class VirusTotalReleaseTests(unittest.TestCase):
    def test_split_dmg_creates_three_verified_parts(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "MetalSharp-test.dmg"
            source.write_bytes(b"abcdefghijk")
            parts = MODULE.split_dmg(source, root / "parts", max_bytes=4)

            self.assertEqual([part["size"] for part in parts], [4, 4, 3])
            rebuilt = b"".join(Path(part["path"]).read_bytes() for part in parts)
            self.assertEqual(rebuilt, source.read_bytes())
            self.assertEqual([part["part"] for part in parts], [1, 2, 3])
            self.assertEqual([part["part_count"] for part in parts], [3, 3, 3])

    def test_split_dmg_rejects_oversized_thirds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "too-large.dmg"
            source.write_bytes(b"a" * 13)
            with self.assertRaisesRegex(MODULE.ScanError, "equal third exceeds"):
                MODULE.split_dmg(source, Path(directory) / "parts", max_bytes=4)

    def test_replace_marked_section_is_idempotent(self) -> None:
        first_section = (
            f"{MODULE.START_MARKER}\nscan one\n{MODULE.END_MARKER}\n"
        )
        first = MODULE.replace_marked_section("Release notes\n", first_section)
        section = (
            f"{MODULE.START_MARKER}\nscan two\n{MODULE.END_MARKER}\n"
        )
        with_markers = MODULE.replace_marked_section(first, section)
        replaced = MODULE.replace_marked_section(with_markers, section)
        self.assertEqual(replaced, with_markers)
        self.assertIn("Release notes", replaced)
        self.assertIn("scan two", replaced)
        self.assertNotIn("scan one", replaced)

    def test_validate_assets_requires_exact_published_digest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dmg = root / "MetalSharp-test.dmg"
            dmg.write_bytes(b"release payload")
            assets = root / "assets.json"
            assets.write_text(
                json.dumps(
                    {
                        "assets": [
                            {
                                "name": dmg.name,
                                "size": dmg.stat().st_size,
                                "digest": f"sha256:{MODULE.sha256_file(dmg)}",
                            }
                        ]
                    }
                )
            )
            args = argparse.Namespace(artifact_dir=str(root), assets_json=str(assets))
            self.assertEqual(MODULE.validate_assets_command(args), 0)

            payload = json.loads(assets.read_text())
            payload["assets"][0]["digest"] = "sha256:" + "0" * 64
            assets.write_text(json.dumps(payload))
            with self.assertRaisesRegex(MODULE.ScanError, "does not match exact"):
                MODULE.validate_assets_command(args)

    def test_markdown_discloses_partial_scan_limit(self) -> None:
        results = [
            {
                "dmg": "MetalSharp.dmg",
                "part": part,
                "part_count": 3,
                "size": 1024 * 1024,
                "sha256": character * 64,
                "stats": {"malicious": 0, "suspicious": 1, "undetected": 9},
            }
            for part, character in enumerate(("a", "b", "c"), start=1)
        ]
        report = MODULE.markdown_report("v1.2.3", results)
        self.assertIn("one raw third", report)
        self.assertIn("not a complete mountable DMG", report)
        self.assertEqual(report.count("1/10"), 3)
        for part, character in enumerate(("a", "b", "c"), start=1):
            self.assertIn(
                f"[Part {part}](https://www.virustotal.com/gui/file/{character * 64})",
                report,
            )
        self.assertNotIn("[Report]", report)
        self.assertNotIn("| 1/3 |", report)


if __name__ == "__main__":
    unittest.main()
