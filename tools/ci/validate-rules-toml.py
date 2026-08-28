#!/usr/bin/env python3
"""Validate `configs/mtsp-rules.toml` is well-formed.

This is a lightweight validator that runs anywhere Python + `toml` is available,
without compiling the backend. It checks:

1. The TOML parses.
2. No duplicate `[overrides.APPID]` top-level sections.
3. Every override has a non-empty `name` string and a `pipeline` string.
4. Every `pipeline` value is one of the known pipeline ids.
5. Sub-tables (e.g. `[overrides.APPID.diagnostics]`) only use the keys we expect
   (`dependencies`, `env`, `diagnostics`) — anything else is a typo.

Exit code 0 on success, 1 on any validation error.
"""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

try:
    import toml  # type: ignore
except ImportError:
    print(
        "error: Python module `toml` is not installed. Install it with:\n"
        "  pip3 install --user toml",
        file=sys.stderr,
    )
    sys.exit(2)

ROOT = Path(__file__).resolve().parents[2]
RULES_PATH = ROOT / "configs" / "mtsp-rules.toml"

VALID_PIPELINES = {
    "m9",
    "m10",
    "m10_32",
    "m11",
    "m11_32",
    "m12",
    "dxmt",
    "vkd3d",
    "d3dmetal",
    "m13",
    "fna_arm64",
    "fna_x86",
    "wine_bare",
    "steam",
    "mac_steam",
    "mac_native",
    "wine_managed",
    "managed",
}

VALID_SUB_KEYS = {"dependencies", "env", "diagnostics"}


def fail(errors: list[str]) -> None:
    print(
        f"mtsp-rules TOML validation FAILED ({len(errors)} error(s)):",
        file=sys.stderr,
    )
    for err in errors[:50]:
        print(f"  - {err}", file=sys.stderr)
    if len(errors) > 50:
        print(f"  ... ({len(errors) - 50} more)", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    if not RULES_PATH.exists():
        print(f"error: rules file not found: {RULES_PATH}", file=sys.stderr)
        return 1

    raw = RULES_PATH.read_text()
    errors: list[str] = []

    # 1. Parse the TOML.
    try:
        data = toml.loads(raw)
    except Exception as exc:
        fail([f"TOML parse error: {exc}"])
        return 1

    overrides = data.get("overrides", {})

    # 2. No duplicate `[overrides.APPID]` top-level sections.
    #    The `toml` parser already raises on duplicates, but we also do a
    #    line-scan so the error message is clear if a future parser silently
    #    keeps the last definition.
    section_re = re.compile(r"^\[overrides\.(\d+)\]\s*$")
    section_counts: Counter[int] = Counter()
    for line in raw.splitlines():
        m = section_re.match(line)
        if m:
            section_counts[int(m.group(1))] += 1
    duplicates = sorted(
        appid for appid, count in section_counts.items() if count > 1
    )
    for appid in duplicates:
        errors.append(
            f"[overrides.{appid}] defined {section_counts[appid]} times"
        )
    if duplicates:
        # Skip the per-entry checks — the parsed map only has one of the
        # duplicates, so iterating it would give a misleading "OK" report.
        fail(errors)
        return 1

    # 3 & 4. Every override has name + pipeline; pipeline is valid.
    for appid, rule in overrides.items():
        if not isinstance(rule, dict):
            errors.append(f"[overrides.{appid}] is not a table")
            continue
        name = rule.get("name", "")
        pipeline = rule.get("pipeline", "")
        if not isinstance(name, str) or not name.strip():
            errors.append(f"[overrides.{appid}] missing or empty `name`")
        if not isinstance(pipeline, str) or not pipeline:
            errors.append(f"[overrides.{appid}] missing `pipeline`")
        elif pipeline not in VALID_PIPELINES:
            errors.append(
                f"[overrides.{appid}] unknown pipeline {pipeline!r}"
            )

    # 5. Sub-table header check — only `[overrides.NNNN.{section}]` where
    #    `section` is in VALID_SUB_KEYS is allowed. Anything else is a typo
    #    (e.g. `[overrides.123.deps]` instead of `.dependencies`).
    sub_re = re.compile(r"^\[overrides\.(\d+)\.([a-z_]+)\]\s*$")
    seen_sub_keys: dict[int, set[str]] = {}
    for line in raw.splitlines():
        m = sub_re.match(line)
        if m:
            appid = int(m.group(1))
            key = m.group(2)
            if key not in VALID_SUB_KEYS:
                errors.append(
                    f"[overrides.{appid}.{key}] is not a recognized sub-table"
                )
            seen_sub_keys.setdefault(appid, set()).add(key)

    if errors:
        fail(errors)

    print(
        f"mtsp-rules TOML OK: {len(overrides)} override entries, "
        f"{len(section_counts)} top-level sections, "
        f"{sum(len(v) for v in seen_sub_keys.values())} sub-tables."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
