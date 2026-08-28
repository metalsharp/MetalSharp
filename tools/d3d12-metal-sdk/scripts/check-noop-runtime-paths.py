#!/usr/bin/env python3
"""Inventory suspicious no-op and fail-closed paths in the DXMT runtime.

This is a gap scanner, not a promotion scanner.  It intentionally reports
many review candidates (for example legitimate boolean helpers and validation
returns).  Later behavior gates must classify and clear each finding.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
DEFAULT_ROOTS = (
    Path("vendor/dxmt/src/d3d12"),
    Path("vendor/dxmt/src/dxgi"),
    Path("vendor/dxmt/src/dxmt"),
    Path("vendor/dxmt/src/winemetal"),
    Path("vendor/dxmt/src/util"),
)
EXTENSIONS = {".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm"}


def source_files(source_root: Path) -> list[Path]:
    paths: list[Path] = []
    for relative in DEFAULT_ROOTS:
        root = source_root / relative
        if root.exists():
            paths.extend(path for path in root.rglob("*") if path.is_file() and path.suffix in EXTENSIONS)
    return sorted(set(paths))


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def snippet(lines: list[str], line: int) -> str:
    return lines[line - 1].strip() if 0 < line <= len(lines) else ""


def add_finding(
    findings: list[dict[str, Any]],
    path: Path,
    lines: list[str],
    line: int,
    kind: str,
    pattern: str,
    classification: str,
) -> None:
    findings.append(
        {
            "kind": kind,
            "classification": classification,
            "path": path.relative_to(ROOT_DIR).as_posix(),
            "line": line,
            "pattern": pattern,
            "snippet": snippet(lines, line),
        }
    )


def scan_file(path: Path, findings: list[dict[str, Any]]) -> None:
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    unsupported = re.compile(r"\b(?:E_NOTIMPL|DXGI_ERROR_UNSUPPORTED|D3D12_ERROR_UNSUPPORTED)\b")
    for match in unsupported.finditer(text):
        line = line_number(text, match.start())
        add_finding(
            findings,
            path,
            lines,
            line,
            "unsupported_return",
            match.group(0),
            "requires_behavior_or_documented_validation_classification",
        )

    success = re.compile(r"\breturn\s+(?:S_OK|TRUE|true|0)\s*;")
    for match in success.finditer(text):
        line = line_number(text, match.start())
        value = re.search(r"return\s+([^;]+)", match.group(0)).group(1).strip()
        add_finding(
            findings,
            path,
            lines,
            line,
            "success_or_zero_return",
            value,
            "requires_side_effect_or_output_evidence",
        )

    placeholders = re.compile(r"\breturn\s+(?:false|FALSE|nullptr|NULL|\{\})\s*;")
    for match in placeholders.finditer(text):
        line = line_number(text, match.start())
        value = re.search(r"return\s+([^;]+)", match.group(0)).group(1).strip()
        add_finding(
            findings,
            path,
            lines,
            line,
            "placeholder_return",
            value,
            "requires_behavior_or_explicit_predicate_classification",
        )

    # Empty function bodies are unambiguous static candidates.  Keep the
    # signature in the result so a reviewer can connect it to an interface.
    empty_body = re.compile(r"(?P<signature>[^{};\n]+\([^{};\n]*\))\s*\{\s*\}", re.MULTILINE)
    for match in empty_body.finditer(text):
        line = line_number(text, match.start())
        signature = " ".join(match.group("signature").split())
        add_finding(findings, path, lines, line, "empty_function_body", signature, "forbidden_for_legal_operation")

    # An empty body containing only comments is equally suspicious.  This
    # pattern is deliberately limited to short bodies to avoid crossing
    # unrelated declarations.
    comment_body = re.compile(
        r"(?P<signature>[^{};\n]+\([^{};\n]*\))\s*\{(?P<body>\s*(?://[^\n]*\n?|/\*.*?\*/\s*)*)\}",
        re.MULTILINE | re.DOTALL,
    )
    for match in comment_body.finditer(text):
        body = re.sub(r"//[^\n]*|/\*.*?\*/", "", match.group("body"), flags=re.DOTALL).strip()
        if body:
            continue
        line = line_number(text, match.start())
        signature = " ".join(match.group("signature").split())
        if not any(
            finding["kind"] == "empty_function_body"
            and finding["path"] == path.relative_to(ROOT_DIR).as_posix()
            and finding["line"] == line
            for finding in findings
        ):
            add_finding(findings, path, lines, line, "comment_only_function_body", signature, "forbidden_for_legal_operation")

    # Capability literals are a smaller, high-value subset of the broad
    # placeholder scan.  They identify fields that need behavior-backed
    # derivation, while leaving ordinary helper booleans as generic findings.
    capability = re.compile(
        r"(?i)(?:supported|tier|shader_model|work.?graph|raytracing|mesh|video|protected|dsr|rov|sample.?position|view.?instanc).{0,100}"
        r"(?:=|\()\s*(?:false|FALSE|0|D3D12_[A-Z0-9_]*_NOT_SUPPORTED|DXGI_[A-Z0-9_]*_UNSUPPORTED)"
    )
    for match in capability.finditer(text):
        line = line_number(text, match.start())
        add_finding(
            findings,
            path,
            lines,
            line,
            "capability_literal",
            " ".join(match.group(0).split()),
            "must_be_derived_from_behavior_ledger",
        )


def scan(source_root: Path) -> dict[str, Any]:
    findings: list[dict[str, Any]] = []
    files = source_files(source_root)
    for path in files:
        scan_file(path, findings)
    findings.sort(key=lambda item: (item["path"], item["line"], item["kind"], item["pattern"]))
    by_kind: dict[str, int] = {}
    for finding in findings:
        by_kind[finding["kind"]] = by_kind.get(finding["kind"], 0) + 1
    return {
        "schema": "metalsharp.d3d12-metal.no-op-scan.v1",
        "state": "phase0_inventory",
        "source_roots": [path.as_posix() for path in DEFAULT_ROOTS],
        "summary": {
            "file_count": len(files),
            "finding_count": len(findings),
            "findings_by_kind": dict(sorted(by_kind.items())),
        },
        "findings": findings,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT_DIR)
    parser.add_argument("--format", choices=("json", "markdown"), default="json")
    args = parser.parse_args()
    result = scan(args.source_root.resolve())
    if args.format == "json":
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print("# DXMT no-op runtime scan\n")
        print(f"- Files: **{result['summary']['file_count']}**")
        print(f"- Findings: **{result['summary']['finding_count']}**")
        for kind, count in result["summary"]["findings_by_kind"].items():
            print(f"- `{kind}`: **{count}**")
        print("\n## Findings\n")
        for finding in result["findings"]:
            print(
                f"- `{finding['kind']}` `{finding['path']}:{finding['line']}` "
                f"— `{finding['snippet']}`"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
