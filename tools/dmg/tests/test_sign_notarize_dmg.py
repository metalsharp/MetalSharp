#!/usr/bin/env python3
"""Offline contract tests for signing and notarizing the final DMG."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "tools/dmg/sign-notarize-dmg.sh"
IDENTITY = "Developer ID Application: Fixture (ABCDE12345)"


class SignNotarizeDmgTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "calls.jsonl"
        self.dmg = self.root / "MetalSharp test.dmg"
        self.dmg.write_bytes(b"fixture disk image")
        self._write_stub(
            "codesign",
            """#!/usr/bin/env python3
import json, os, sys
with open(os.environ['CALL_LOG'], 'a') as out:
    out.write(json.dumps(['codesign', *sys.argv[1:]]) + '\\n')
""",
        )
        self._write_stub(
            "xcrun",
            """#!/usr/bin/env python3
import json, os, sys
with open(os.environ['CALL_LOG'], 'a') as out:
    out.write(json.dumps(['xcrun', *sys.argv[1:]]) + '\\n')
if sys.argv[1:3] == ['notarytool', 'submit']:
    print(json.dumps({'id': 'fixture-id', 'status': os.environ.get('FAKE_NOTARY_STATUS', 'Accepted')}))
    if os.environ.get('FAIL_NOTARY') == '1':
        raise SystemExit(1)
if sys.argv[1:3] == ['notarytool', 'log']:
    print(json.dumps({'status': 'fixture rejection details'}))
""",
        )

    def _write_stub(self, name: str, body: str) -> None:
        executable = self.bin / name
        executable.write_text(body)
        executable.chmod(0o755)

    def _run(self, **extra_env: str) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.update(
            {
                "PATH": f"{self.bin}{os.pathsep}{env['PATH']}",
                "CALL_LOG": str(self.log),
                "APPLE_SIGNING_IDENTITY": IDENTITY,
            }
        )
        env.update(extra_env)
        return subprocess.run(
            ["bash", str(SCRIPT), str(self.dmg)],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def _calls(self) -> list[list[str]]:
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_signs_submits_staples_and_validates_with_apple_id_credentials(self) -> None:
        result = self._run(
            APPLE_ID="developer@example.test",
            APPLE_APP_SPECIFIC_PASSWORD="fixture-password",
            APPLE_TEAM_ID="ABCDE12345",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self._calls(),
            [
                ["codesign", "--force", "--sign", IDENTITY, "--timestamp", "-i", "com.metalsharp.app.dmg", str(self.dmg)],
                ["codesign", "--verify", "--verbose=4", str(self.dmg)],
                [
                    "xcrun",
                    "notarytool",
                    "submit",
                    str(self.dmg),
                    "--wait",
                    "--timeout",
                    "2h",
                    "--output-format",
                    "json",
                    "--apple-id",
                    "developer@example.test",
                    "--password",
                    "fixture-password",
                    "--team-id",
                    "ABCDE12345",
                ],
                ["xcrun", "stapler", "staple", str(self.dmg)],
                ["xcrun", "stapler", "validate", str(self.dmg)],
            ],
        )

    def test_supports_app_store_connect_api_key_credentials(self) -> None:
        api_key = self.root / "AuthKey_KEY123.p8"
        api_key.write_text("fixture API private key")
        result = self._run(
            APPLE_API_KEY=str(api_key),
            APPLE_API_KEY_ID="KEY123",
            APPLE_API_ISSUER="issuer-uuid",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self._calls()[2],
            [
                "xcrun",
                "notarytool",
                "submit",
                str(self.dmg),
                "--wait",
                "--timeout",
                "2h",
                "--output-format",
                "json",
                "--key",
                str(api_key),
                "--key-id",
                "KEY123",
                "--issuer",
                "issuer-uuid",
            ],
        )
        self.assertEqual(self._calls()[-2:], [["xcrun", "stapler", "staple", str(self.dmg)], ["xcrun", "stapler", "validate", str(self.dmg)]])

    def test_notary_failure_does_not_staple_or_validate(self) -> None:
        result = self._run(
            APPLE_ID="developer@example.test",
            APPLE_APP_SPECIFIC_PASSWORD="fixture-password",
            APPLE_TEAM_ID="ABCDE12345",
            FAIL_NOTARY="1",
        )
        self.assertNotEqual(result.returncode, 0)
        calls = self._calls()
        self.assertEqual(calls[-2][1:3], ["notarytool", "submit"])
        self.assertEqual(calls[-1][1:3], ["notarytool", "log"])
        self.assertFalse(any(call[1:3] == ["stapler", "staple"] for call in calls))

    def test_rejects_non_accepted_notary_status(self) -> None:
        result = self._run(
            APPLE_ID="developer@example.test",
            APPLE_APP_SPECIFIC_PASSWORD="fixture-password",
            APPLE_TEAM_ID="ABCDE12345",
            FAKE_NOTARY_STATUS="Invalid",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("status: Invalid", result.stderr)
        calls = self._calls()
        self.assertEqual(calls[-1][1:3], ["notarytool", "log"])
        self.assertFalse(any(call[1:3] == ["stapler", "staple"] for call in calls))

    def test_requires_complete_signing_and_notarization_credentials(self) -> None:
        result = self._run()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("notarization credentials", result.stderr.lower())
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
