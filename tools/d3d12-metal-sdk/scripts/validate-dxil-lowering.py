#!/usr/bin/env python3
"""Audit generated DXIL reports for unsupported or placeholder lowering."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def report_value(text: str, key: str) -> int:
    match = re.search(rf"^{re.escape(key)}=(\d+)$", text, re.MULTILINE)
    return int(match.group(1)) if match else -1


def audit(corpus: Path) -> dict[str, Any]:
    reports = sorted(corpus.glob("*.dxil_report.txt")) if corpus.is_dir() else []
    rows: list[dict[str, Any]] = []
    failures: list[str] = []
    for report_path in reports:
        try:
            text = report_path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
        except OSError as exc:
            failures.append(f"{report_path}: {exc}")
            continue
        intrinsic_count = report_value(text, "unsupported_intrinsics")
        opcode_count = report_value(text, "unsupported_opcodes")
        msl_match = re.search(r"^msl=(.+)$", text, re.MULTILINE)
        msl_path = Path(msl_match.group(1).strip()) if msl_match else report_path.with_suffix(".msl")
        try:
            msl = msl_path.read_text(encoding="utf-8", errors="replace") if msl_path.is_file() else ""
        except OSError:
            msl = ""
        placeholder_patterns = (
            "// unhandled opcode",
            "unknown dx intrinsic",
            "// skipped store",
        )
        placeholders = [pattern for pattern in placeholder_patterns if pattern in msl]
        row_pass = intrinsic_count == 0 and opcode_count == 0 and not placeholders
        if not row_pass:
            failures.append(
                f"{report_path.name}: intrinsics={intrinsic_count} opcodes={opcode_count} "
                f"placeholders={','.join(placeholders) or 'none'}"
            )
        rows.append(
            {
                "report": str(report_path),
                "msl": str(msl_path),
                "unsupported_intrinsics": intrinsic_count,
                "unsupported_opcodes": opcode_count,
                "placeholder_patterns": placeholders,
                "pass": row_pass,
            }
        )
    return {
        "schema": "metalsharp.d3d12.dxil-lowering-audit.v1",
        "corpus": str(corpus),
        "report_count": len(rows),
        "pass": bool(rows) and not failures,
        "failures": failures,
        "reports": rows,
    }


def main() -> int:
    args = parse_args()
    result = audit(args.corpus)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(
        f"[{'PASS' if result['pass'] else 'FAIL'}] DXIL lowering audit: "
        f"{result['report_count']} reports, {len(result['failures'])} failures"
    )
    for failure in result["failures"]:
        print(f"  - {failure}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
