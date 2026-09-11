#!/usr/bin/env python3
"""Audit Mach-O minimum macOS deployment targets in runtime artifacts."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

VERSION_RE = re.compile(r"^(\d+)(?:\.(\d+))?(?:\.(\d+))?$")
BUILD_MINOS_RE = re.compile(r"LC_BUILD_VERSION[\s\S]{0,180}?\n\s*minos\s+([^\s]+)")
LEGACY_MINOS_RE = re.compile(r"LC_VERSION_MIN_MACOSX[\s\S]{0,180}?\n\s*version\s+([^\s]+)")


def version_tuple(value: str) -> tuple[int, int, int]:
    match = VERSION_RE.fullmatch(value)
    if not match:
        raise ValueError(f"invalid macOS version: {value}")
    return tuple(int(part or 0) for part in match.groups())


def mach_o_minimum(path: Path) -> list[str]:
    file_result = subprocess.run(
        ["file", "-b", str(path)], capture_output=True, text=True, check=True
    )
    if "Mach-O" not in file_result.stdout:
        return []
    load_commands = subprocess.run(["otool", "-l", str(path)], capture_output=True, text=True, check=True)
    values = BUILD_MINOS_RE.findall(load_commands.stdout)
    values.extend(LEGACY_MINOS_RE.findall(load_commands.stdout))
    return sorted(set(values), key=version_tuple)


def undefined_symbols(path: Path, symbols: list[str]) -> list[str]:
    if not symbols:
        return []
    result = subprocess.run(["nm", "-u", str(path)], capture_output=True, text=True, check=False)
    found: list[str] = []
    for symbol in symbols:
        pattern = re.compile(rf"(?:^|\\s)_?{re.escape(symbol)}(?:$|\\s)", re.MULTILINE)
        if pattern.search(result.stdout):
            found.append(symbol)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="+", type=Path, help="Files or directories to audit")
    parser.add_argument("--max", dest="maximum", default="14.0", help="maximum permitted macOS version")
    parser.add_argument("--json", dest="json_path", type=Path, help="write the complete audit report")
    parser.add_argument(
        "--forbid-undefined-symbol",
        dest="forbidden_symbols",
        action="append",
        default=[],
        help="fail if a Mach-O has this undefined symbol (repeatable; accepts the name without leading underscore)",
    )
    args = parser.parse_args()
    maximum = version_tuple(args.maximum)

    files: list[Path] = []
    for root in args.roots:
        if root.is_file():
            files.append(root)
        elif root.is_dir():
            files.extend(path for path in root.rglob("*") if path.is_file())
        else:
            parser.error(f"path does not exist: {root}")

    records: list[dict[str, object]] = []
    failures: list[dict[str, object]] = []
    undefined_failures: list[dict[str, object]] = []
    for path in sorted(set(files)):
        try:
            minimums = mach_o_minimum(path)
        except subprocess.CalledProcessError as error:
            parser.error(f"failed to inspect {path}: {error}")
        if not minimums:
            continue
        record = {"path": str(path), "minimums": minimums}
        records.append(record)
        if any(version_tuple(value) > maximum for value in minimums):
            failures.append(record)
        forbidden = undefined_symbols(path, args.forbidden_symbols)
        if forbidden:
            undefined_failures.append({"path": str(path), "symbols": forbidden})

    report = {
        "maximum": args.maximum,
        "mach_o_files": len(records),
        "violations": failures,
        "undefined_symbol_violations": undefined_failures,
        "files": records,
    }
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(json.dumps(report, indent=2) + "\n")

    print(f"Audited {len(records)} Mach-O files; maximum allowed macOS {args.maximum}.")
    if failures:
        print(f"FAIL: {len(failures)} Mach-O files exceed the deployment target:")
        for failure in failures:
            print(f"  {', '.join(failure['minimums'])}: {failure['path']}")
    if undefined_failures:
        print(f"FAIL: {len(undefined_failures)} Mach-O files reference forbidden undefined symbols:")
        for failure in undefined_failures:
            print(f"  {', '.join(failure['symbols'])}: {failure['path']}")
    if failures or undefined_failures:
        return 1
    print("PASS: every Mach-O minimum deployment target is within the allowed range.")
    if args.forbidden_symbols:
        print("PASS: no forbidden undefined symbols were found.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
