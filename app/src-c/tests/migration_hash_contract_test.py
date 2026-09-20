#!/usr/bin/env python3
"""Keep migration runtime verification hashes synchronized with bundle contracts."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MIGRATION_SOURCE = ROOT / "app/src-c/runtime/migration.c"

CONTRACTS = {
    "migration_dxmt_hashes": ROOT / "tools/bundles/dxmt-runtime-hashes.tsv",
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


def read_migration_cdhashes(path: Path) -> dict[str, str]:
    """Parse '# migration-cdhash: <path> <cdhash>' markers.

    Declares the install-time code-signature identifier (CDHash) that
    migration.c must pin for paths whose installed bytes differ from the
    archive (see read_migration_skips)."""
    cdhashes: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if line.startswith("# migration-cdhash:"):
            fields = line[len("# migration-cdhash:") :].strip().split(None, 1)
            if len(fields) != 2 or not re.fullmatch(r"[0-9a-f]{40}", fields[1]):
                raise AssertionError(f"invalid migration-cdhash marker in {path}: {line}")
            cdhashes[fields[0]] = fields[1]
    return cdhashes


def read_migration_skips(path: Path) -> dict[str, str]:
    """Parse '# migration-skip: <path> <reason>' comments.

    Marks members whose installed bytes legitimately differ from the archive
    (e.g. the DXMT bridge is ad-hoc re-signed by the installer with
    non-deterministic signatures), so migration.c pins a different property
    (signature validity) instead of the archive hash. The test enforces that
    the C table really omits exactly these paths — no more, no less."""
    skips: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if line.startswith("# migration-skip:"):
            fields = line[len("# migration-skip:") :].strip().split(None, 1)
            if len(fields) != 2:
                raise AssertionError(f"invalid migration-skip marker in {path}: {line}")
            skips[fields[0]] = fields[1]
    return skips


def read_c_table(source: str, name: str, hash_length: int = 64) -> dict[str, str]:
    match = re.search(
        rf"static const char\* const {re.escape(name)}\[\]\[2\] = \{{(.*?)\}};",
        source,
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"missing migration hash table: {name}")
    rows = dict(re.findall(rf'\{{"([^"]+)",\s*"([0-9a-f]{{{hash_length}}})"\}}', match.group(1)))
    if not rows:
        raise AssertionError(f"migration hash table is empty: {name}")
    return rows


def main() -> int:
    source = MIGRATION_SOURCE.read_text()

    makefile = (ROOT / "app/src-c/Makefile").read_text()
    version_match = re.search(r"^VERSION \?= (\S+)", makefile, re.MULTILINE)
    migration_version_match = re.search(r'#define MIGRATION_VERSION "(\S+)"', source)
    if not version_match or not migration_version_match:
        print("could not read VERSION from Makefile or MIGRATION_VERSION from migration.c", file=sys.stderr)
        return 1
    if version_match.group(1) != migration_version_match.group(1):
        print(
            f"version drift: Makefile VERSION={version_match.group(1)} but "
            f"migration.c MIGRATION_VERSION={migration_version_match.group(1)} — update migration.c",
            file=sys.stderr,
        )
        return 1
    for table, manifest_path in CONTRACTS.items():
        manifest = read_manifest(manifest_path)
        skips = read_migration_skips(manifest_path)
        unknown_skips = sorted(skips.keys() - manifest.keys())
        if unknown_skips:
            print(
                f"migration-skip markers reference unknown paths in {manifest_path}: {unknown_skips}",
                file=sys.stderr,
            )
            return 1
        expected = {p: h for p, h in manifest.items() if p not in skips}
        actual = read_c_table(source, table)
        pinned_skips = sorted(set(skips.keys()) & set(actual.keys()))
        if pinned_skips:
            print(
                f"migration table {table} must not pin migration-skip paths (installer mutates them): {pinned_skips}",
                file=sys.stderr,
            )
            return 1
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
    # Enforce the alternative pin for skipped paths: CDHash table must cover
    # every migration-cdhash marker exactly, so a skipped path can never end
    # up pinned by nothing.
    cdhash_markers = read_migration_cdhashes(CONTRACTS["migration_dxmt_hashes"])
    cdhash_table = read_c_table(source, "migration_dxmt_cdhashes", hash_length=40)
    if cdhash_markers != cdhash_table:
        print(
            f"migration cdhash contract mismatch: markers={sorted(cdhash_markers)} table={sorted(cdhash_table)}",
            file=sys.stderr,
        )
        return 1

    print("migration hash contracts match published bundle contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
