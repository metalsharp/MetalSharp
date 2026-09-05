#!/usr/bin/env python3
"""Shell-helper regression only; mock output is not shader execution evidence."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class VariantBuildTests(unittest.TestCase):
    def run_helper(self, compiler_body, success, expected=None):
        source = Path(__file__).with_name("run-probes.sh").read_text()
        matches = re.findall(r"^compile_work_graph_chain_variant\(\) \{\n.*?^\}\n",
                             source, re.MULTILINE | re.DOTALL)
        self.assertEqual(len(matches), 1)
        with tempfile.TemporaryDirectory(prefix="wg variant ") as directory:
            root = Path(directory)
            output = root / "out/bin/probe_workgraph_chain_test.cso"
            output.parent.mkdir(parents=True)
            output.write_bytes(b"stale")
            compiler = root / "mock compiler"
            compiler.write_text("#!/bin/sh\n" + compiler_body)
            compiler.chmod(0o755)
            env = os.environ.copy()
            env.update(SDK_DIR=str(root), WINE_PREFIX=str(root / "prefix"),
                       WINE_BIN=str(compiler))
            # Calling in a conditional deliberately disables implicit errexit:
            # the helper must propagate failures explicitly.
            script = matches[0] + '\nif compile_work_graph_chain_variant _test -DTEST=1; then exit 0; else exit 1; fi\n'
            result = subprocess.run(["bash", "-c", script], env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0 if success else 1, result.stderr)
            if expected is None:
                self.assertFalse(output.exists(), "stale/partial bytecode survived failure")
            else:
                self.assertEqual(output.read_bytes(), expected)

    def test_failed_compiler_removes_stale_output(self):
        self.run_helper("exit 1\n", False)

    def test_success_without_output_rejects_stale_file(self):
        self.run_helper("exit 0\n", False)

    def test_partial_output_is_removed(self):
        self.run_helper('while [ "$#" -gt 0 ]; do\n'
                        '  if [ "$1" = "-Fo" ]; then printf partial > "$2"; exit 1; fi\n'
                        '  shift\ndone\nexit 2\n', False)

    def test_success_keeps_new_output_with_spaced_paths(self):
        self.run_helper('while [ "$#" -gt 0 ]; do\n'
                        '  if [ "$1" = "-Fo" ]; then printf new > "$2"; exit 0; fi\n'
                        '  shift\ndone\nexit 2\n', True, b"new")


if __name__ == "__main__":
    unittest.main()
