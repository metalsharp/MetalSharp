#!/usr/bin/env python3
"""Regression coverage for updater backend-version status reporting."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


UPDATE_SCRIPT = Path(__file__).resolve().parents[2] / "app" / "updater" / "update.py"


def load_update_module():
    spec = importlib.util.spec_from_file_location("metalsharp_update", UPDATE_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load updater module from {UPDATE_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


update = load_update_module()


class UpdateStatusTests(unittest.TestCase):
    def read_status(self, status_file: Path) -> dict:
        return json.loads(status_file.read_text())

    def test_backend_version_mismatch_is_reported_as_error(self):
        with tempfile.TemporaryDirectory(prefix="metalsharp-update-status-") as temp:
            status_file = Path(temp) / "status.json"

            update.report_update_result(str(status_file), "0.59.1", "0.60.0")

            status = self.read_status(status_file)
            self.assertEqual(status["phase"], "error")
            self.assertEqual(status["percent"], 95)
            self.assertEqual(status["error"], "backend_version_mismatch")
            self.assertEqual(
                status["message"],
                "Update installed, but backend reported v0.59.1 instead of v0.60.0",
            )
            self.assertEqual(status["new_version"], "0.60.0")

    def test_matching_backend_version_is_reported_as_complete(self):
        with tempfile.TemporaryDirectory(prefix="metalsharp-update-status-") as temp:
            status_file = Path(temp) / "status.json"

            update.report_update_result(str(status_file), "0.60.0", "0.60.0")

            status = self.read_status(status_file)
            self.assertEqual(status["phase"], "complete")
            self.assertEqual(status["percent"], 100)
            self.assertIsNone(status["error"])
            self.assertEqual(status["message"], "Update installed. Opening migration wizard...")


if __name__ == "__main__":
    unittest.main(verbosity=2)
