#!/usr/bin/env python3
"""Fail-closed run identity tests without GPU work or external validators."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "gate", Path(__file__).with_name("validate-full-surface-gate.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class IdentityTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.row = {"id": "test", "result": "probe-test", "checks": [["pass"]]}
        self.result = self.root / "probe-test-test.json"
        self.result.write_text('{"pass":true}')
        os.utime(self.result, (1000.5, 1000.5))
        self.identity = self.root / "host-runtime-test.json"
        audit = {}
        for name in ("d3d12.dll", "d3d11.dll", "d3d10core.dll", "dxgi.dll",
                     "dxgi_dxmt.dll", "winemetal.dll"):
            audit[name] = {"match": True,
                           "selected": {"path": "/stage/" + name, "size": 10, "sha256": "a" * 64},
                           "probe_copy": {"path": "/probe/" + name, "size": 10, "sha256": "a" * 64}}
        self.good = {"profile": "test", "run_started_at": 1000.25,
                     "loader_pe_copy_audit": audit}

    def check(self, identity):
        self.identity.write_text(json.dumps(identity))
        return gate.check_result(self.root, "test", self.row)[0]["pass"]

    def test_valid_and_same_second_stale(self):
        self.assertTrue(self.check(self.good))
        self.assertFalse(self.check(dict(self.good, run_started_at=1000.75)))

    def test_invalid_timestamps(self):
        for value in (None, True, False, "1000", 0, -1, float("nan"),
                      float("inf"), float("-inf"), 10**400):
            with self.subTest(value=value):
                self.assertFalse(self.check(dict(self.good, run_started_at=value)))

    def test_non_object_identity(self):
        for value in (None, [], True, 1, "identity"):
            with self.subTest(value=value):
                self.assertFalse(self.check(value))

    def test_wrong_profile_or_audit(self):
        self.assertFalse(self.check(dict(self.good, profile="different")))
        for audit in (None, {}, [], {"copy": {"match": False}}):
            self.assertFalse(self.check(dict(self.good, loader_pe_copy_audit=audit)))

    def test_audit_match_flag_does_not_override_metadata(self):
        for field, value in (("sha256", "b" * 64), ("size", 11),
                             ("size", True), ("size", 0), ("path", ""),
                             ("sha256", "x" * 64), ("sha256", "a")):
            with self.subTest(field=field, value=value):
                identity = copy.deepcopy(self.good)
                identity["loader_pe_copy_audit"]["d3d12.dll"]["probe_copy"][field] = value
                self.assertFalse(self.check(identity))
        identity = copy.deepcopy(self.good)
        identity["loader_pe_copy_audit"]["d3d12.dll"] = {"match": True}
        self.assertFalse(self.check(identity))

    def test_audit_requires_every_application_directory_copy(self):
        for name in self.good["loader_pe_copy_audit"]:
            with self.subTest(name=name):
                identity = copy.deepcopy(self.good)
                del identity["loader_pe_copy_audit"][name]
                self.assertFalse(self.check(identity))

    def test_missing_and_malformed_identity(self):
        self.assertFalse(gate.check_result(self.root, "test", self.row)[0]["pass"])
        self.identity.write_text("not json")
        self.assertFalse(gate.check_result(self.root, "test", self.row)[0]["pass"])

    def test_missing_result_spec(self):
        self.assertFalse(gate.check_result(self.root, "test", {"id": "empty"})[0]["pass"])


if __name__ == "__main__":
    unittest.main()
