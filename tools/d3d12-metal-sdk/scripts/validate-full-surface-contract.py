#!/usr/bin/env python3
"""Validate the phase-0 full-surface contracts and their synchronized scan."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
CONTRACTS = ROOT_DIR / "tools/d3d12-metal-sdk/contracts"
DEFAULT_FULL = CONTRACTS / "d3d12-full-surface-contract.json"
DEFAULT_MATRIX = CONTRACTS / "d3d12-full-surface-matrix.json"
DEFAULT_PROVIDER = CONTRACTS / "d3d12-provider-contract.json"
DEFAULT_POLICY = CONTRACTS / "d3d12-no-op-policy.json"
DEFAULT_CENSUS = CONTRACTS / "d3d12-interface-census.json"
DEFAULT_STABLE = CONTRACTS / "agility-1.619.5-contract.json"
SCANNER = ROOT_DIR / "tools/d3d12-metal-sdk/scripts/check-noop-runtime-paths.py"
CENSUS_VALIDATOR = ROOT_DIR / "tools/d3d12-metal-sdk/scripts/validate-interface-census.py"


def load(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def required_object(path: Path, errors: list[str]) -> dict[str, Any] | None:
    if not path.exists():
        errors.append(f"missing contract: {path}")
        return None
    try:
        value = load(path)
    except json.JSONDecodeError as exc:
        errors.append(f"{path}: invalid JSON: {exc}")
        return None
    if not isinstance(value, dict):
        errors.append(f"{path}: top-level value must be an object")
        return None
    if not value.get("schema"):
        errors.append(f"{path}: schema is required")
    return value


def run_json(command: list[str]) -> dict[str, Any]:
    output = subprocess.check_output(command, cwd=ROOT_DIR, text=True)
    value = json.loads(output)
    if not isinstance(value, dict):
        raise ValueError("scanner output is not an object")
    return value


def validate(args: argparse.Namespace) -> list[str]:
    errors: list[str] = []
    full = required_object(args.full, errors)
    matrix = required_object(args.matrix, errors)
    provider = required_object(args.provider, errors)
    policy = required_object(args.policy, errors)
    census = required_object(args.census, errors)
    stable = required_object(args.stable, errors)
    if not all((full, matrix, provider, policy, census, stable)):
        return errors

    assert full is not None
    assert matrix is not None
    assert provider is not None
    assert policy is not None
    assert census is not None
    assert stable is not None

    if stable.get("agility_sdk_version") != "1.619.5" or stable.get("d3d12_sdk_version") != 619:
        errors.append("stable contract is not Agility 1.619.5 / D3D12SDKVersion 619")
    if len(stable.get("data", {})) != 145:
        errors.append("stable contract must enumerate 145 interfaces")
    if sum(len(item.get("methods", {})) for item in stable.get("data", {}).values()) != 537:
        errors.append("stable contract must enumerate 537 methods")

    stable_baseline = full.get("stable_baseline")
    if not isinstance(stable_baseline, dict):
        errors.append("full-surface stable_baseline is required")
    else:
        if stable_baseline.get("version") != "1.619.5":
            errors.append("full-surface baseline version is not 1.619.5")
        if stable_baseline.get("d3d12_sdk_version") != 619:
            errors.append("full-surface baseline SDK version is not 619")
        if stable_baseline.get("preview_lane") != "1.721.3-preview":
            errors.append("full-surface preview lane must be 1.721.3-preview")

    for contract, name in ((full, "full-surface"), (matrix, "matrix"), (provider, "provider"), (policy, "no-op policy")):
        if contract.get("state") != "phase0_inventory":
            errors.append(f"{name} contract must be phase0_inventory")
    if full.get("summary", {}).get("promotion_ready") is not False:
        errors.append("phase-0 full-surface contract cannot be promotion-ready")
    if provider.get("no_silent_fallback") is not True:
        errors.append("provider contract must forbid silent fallback")

    categories = matrix.get("required_categories")
    if not isinstance(categories, list) or len(categories) < 15:
        errors.append("matrix must contain the full required category inventory")
    else:
        for index, category in enumerate(categories):
            if not isinstance(category, dict):
                errors.append(f"matrix category {index} must be an object")
                continue
            if not category.get("id"):
                errors.append(f"matrix category {index} has no id")
            if category.get("behavior_status") != "unverified":
                errors.append(f"matrix category {index} must be unverified in phase 0")

    forbidden = policy.get("legal_request_forbidden_outcomes")
    if not isinstance(forbidden, list) or "E_NOTIMPL" not in forbidden or "S_OK with no state change or observable side effect" not in forbidden:
        errors.append("no-op policy is missing legal fail-closed outcomes")

    # Ensure the source-backed census is synchronized.  Its own validator
    # computes the current source tree digest.
    census_result = subprocess.run(
        ["python3", str(CENSUS_VALIDATOR), "--contract", str(args.stable), "--census", str(args.census)],
        cwd=ROOT_DIR,
        text=True,
        capture_output=True,
        check=False,
    )
    if census_result.returncode != 0:
        errors.extend(line for line in census_result.stdout.splitlines() if line.startswith("[FAIL]"))
        if not census_result.stdout.strip():
            errors.append(f"interface census validator failed: {census_result.stderr.strip()}")

    try:
        scan = run_json(["python3", str(SCANNER), "--source-root", str(ROOT_DIR), "--format", "json"])
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, ValueError) as exc:
        errors.append(f"no-op scanner failed: {exc}")
    else:
        scan_count = scan.get("summary", {}).get("finding_count")
        if scan_count != full.get("summary", {}).get("static_finding_count"):
            errors.append(
                "full-surface static_finding_count is stale "
                f"(contract={full.get('summary', {}).get('static_finding_count')}, scan={scan_count})"
            )

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--full", type=Path, default=DEFAULT_FULL)
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX)
    parser.add_argument("--provider", type=Path, default=DEFAULT_PROVIDER)
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--census", type=Path, default=DEFAULT_CENSUS)
    parser.add_argument("--stable", type=Path, default=DEFAULT_STABLE)
    args = parser.parse_args()
    errors = validate(args)
    if errors:
        for error in errors:
            print(f"[FAIL] {error}")
        return 1
    full = load(args.full)
    summary = full["summary"]
    print(
        f"[PASS] phase-0 full-surface contracts: {summary['interface_count']} interfaces, "
        f"{summary['method_count']} methods, {summary['static_finding_count']} static findings; "
        "promotion remains disabled"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
