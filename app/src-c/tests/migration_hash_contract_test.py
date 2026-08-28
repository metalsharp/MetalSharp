#!/usr/bin/env python3
"""Keep migration runtime verification hashes synchronized with bundle contracts."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MIGRATION_SOURCE = ROOT / "app/src-c/runtime/migration.c"

CONTRACTS = {
    "migration_m12_hashes": ROOT / "tools/bundles/m12-dxmt-runtime-hashes.tsv",
    "migration_vkd3d_hashes": ROOT / "tools/bundles/vkd3d-proton-runtime-hashes.tsv",
    "migration_dxvk_hashes": ROOT / "tools/bundles/dxvk-runtime-hashes.tsv",
}


def read_manifest(path: Path) -> dict[str, str]:
    rows: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#") or line == "path\tsha256":
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not re.fullmatch(r"[0-9a-f]{64}", fields[1]):
            raise AssertionError(f"invalid hash contract row in {path}: {line}")
        rows[fields[0]] = fields[1]
    if not rows:
        raise AssertionError(f"hash contract is empty: {path}")
    return rows


def read_c_table(source: str, name: str) -> dict[str, str]:
    match = re.search(
        rf"static const char\* const {re.escape(name)}\[\]\[2\] = \{{(.*?)\}};",
        source,
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"missing migration hash table: {name}")
    rows = dict(re.findall(r'\{"([^"]+)",\s*"([0-9a-f]{64})"\}', match.group(1)))
    if not rows:
        raise AssertionError(f"migration hash table is empty: {name}")
    return rows


def main() -> int:
    source = MIGRATION_SOURCE.read_text()
    for table, manifest_path in CONTRACTS.items():
        expected = read_manifest(manifest_path)
        actual = read_c_table(source, table)
        if actual != expected:
            missing = sorted(expected.keys() - actual.keys())
            extra = sorted(actual.keys() - expected.keys())
            changed = sorted(
                path for path in expected.keys() & actual.keys() if expected[path] != actual[path]
            )
            print(
                f"migration hash contract mismatch for {table}: "
                f"missing={missing} extra={extra} changed={changed}",
                file=sys.stderr,
            )
            return 1
    print("migration hash contracts match published bundle contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
