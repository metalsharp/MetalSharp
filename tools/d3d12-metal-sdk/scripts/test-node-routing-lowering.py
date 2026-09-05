#!/usr/bin/env python3
"""Check actual routing calls survive DXIL-to-MSL expression coercion."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--shader", required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="node-routing-lowering-") as directory:
    for entry in ("array_entry", "sparse_entry"):
        output = Path(directory) / (entry + ".metal")
        command = [args.compiler, args.shader, entry, str(output)]
        disabled = subprocess.run(command, capture_output=True, text=True)
        assert disabled.returncode == 7, (entry, "legacy lowering must reject dynamic array indexing", disabled.stderr)
        enabled = subprocess.run(command + ["--node-routing"], capture_output=True, text=True)
        assert enabled.returncode == 0, (entry, enabled.stderr)
        source = output.read_text()
        for operation in ("resolve", "index") + (("valid",) if entry == "sparse_entry" else ()):
            expression = f"m12_node_{operation}_output(buf28,"
            assert expression in source, (entry, operation, "routing expression disappeared")
        assert "c->version == 4u" in source and "c->version == 5u" in source
print("PASS: opt-in array routing calls survive lowering; legacy dynamic indexing rejects")
