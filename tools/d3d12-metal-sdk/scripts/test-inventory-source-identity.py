#!/usr/bin/env python3
"""Prove generator/validator identity parity and compiler mutation detection."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True


def load(name):
    path = Path(__file__).with_name(name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SourceIdentityTests(unittest.TestCase):
    def test_compiler_sources_participate_in_both_digests(self):
        generator = load("generate-full-surface-inventory")
        validator = load("validate-interface-census")
        relative_roots = [p.relative_to(generator.ROOT_DIR) for p in generator.RUNTIME_ROOTS]
        self.assertEqual(set(relative_roots),
                         {p.relative_to(validator.ROOT_DIR) for p in validator.RUNTIME_ROOTS})
        self.assertIn(Path("vendor/dxmt/src/airconv"), relative_roots)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for module in (generator, validator):
                module.ROOT_DIR = root
                module.RUNTIME_ROOTS = tuple(root / p for p in relative_roots)
            compiler = root / "vendor/dxmt/src/airconv"
            compiler.mkdir(parents=True)
            paths = [compiler / "node_gpu_entry_msl.hpp", compiler / "shader.metal"]
            for path in paths:
                path.write_text("original\n")
            def digest():
                value = generator.source_tree_digest(generator.runtime_files())
                self.assertEqual(value, validator.runtime_tree_digest())
                return value
            previous = digest()
            for path in paths:
                with self.subTest(path=path.name):
                    path.write_text("changed\n")
                    current = digest()
                    self.assertNotEqual(previous, current)
                    previous = current
            (compiler / "ignored.log").write_text("not source")
            self.assertEqual(previous, digest())


if __name__ == "__main__":
    unittest.main()
