#!/usr/bin/env python3
"""Validate D3D12 Metal SDK contract files."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
DEFAULT_CONTRACT_ROOT = ROOT_DIR / "tools" / "d3d12-metal-sdk" / "contracts"


REQUIRED_CONTRACTS = [
    "d3d12-metal-contract.json",
    "agility-1.619.3-contract.json",
    "feature-support-contract.json",
    "fl12-2-gate-contract.json",
    "dxgi-contract.json",
    "unsupported-api-ledger.json",
    "risky-stub-ledger.json",
    "contract-waivers.json",
    "winemetal-bridge-contract.json",
]

DECLARED_TIERS = {"required", "emulated", "stubbed-safe", "unsupported"}

REQUIRED_UNSUPPORTED_APIS = {
    "D3D12 ray tracing tiers",
    "D3D12 state objects",
    "D3D12 mesh shader tiers",
    "D3D12 amplification shader tiers",
    "D3D12 work graphs",
    "D3D12 node shaders",
    "D3D12 video encode/decode/process APIs",
    "D3D12 protected resource sessions",
    "D3D12 DSR",
    "D3D12 stream output",
    "D3D12 sparse and reserved resources",
    "D3D12 geometry shaders outside proven emulation",
    "D3D12 hull/domain tessellation shaders",
}


def load(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def validate_evidence(path: Path, data: dict[str, Any], errors: list[str]) -> None:
    evidence = data.get("evidence")
    require(isinstance(evidence, list) and len(evidence) > 0, f"{path}: missing evidence list", errors)
    if isinstance(evidence, list):
        for i, entry in enumerate(evidence):
            require(isinstance(entry, dict), f"{path}: evidence[{i}] must be object", errors)
            if isinstance(entry, dict):
                require(bool(entry.get("kind")), f"{path}: evidence[{i}] missing kind", errors)
                require(bool(entry.get("path")), f"{path}: evidence[{i}] missing path", errors)
                require(bool(entry.get("note")), f"{path}: evidence[{i}] missing note", errors)


def validate_ledgers(path: Path, data: dict[str, Any], errors: list[str]) -> None:
    entries = data.get("entries")
    require(isinstance(entries, list) and len(entries) > 0, f"{path}: ledger entries must be non-empty", errors)
    if isinstance(entries, list):
        for i, entry in enumerate(entries):
            require(isinstance(entry, dict), f"{path}: entries[{i}] must be object", errors)
            if isinstance(entry, dict):
                require(bool(entry.get("api")), f"{path}: entries[{i}] missing api", errors)
                require(bool(entry.get("state")), f"{path}: entries[{i}] missing state", errors)
                tier = entry.get("tier")
                require(tier in DECLARED_TIERS, f"{path}: entries[{i}] invalid tier `{tier}`", errors)
                require(bool(entry.get("reason") or entry.get("risk")), f"{path}: entries[{i}] missing reason/risk", errors)


def active_waiver_ids(root: Path, errors: list[str]) -> set[str]:
    path = root / "contract-waivers.json"
    if not path.exists():
        return set()
    try:
        data = load(path)
    except json.JSONDecodeError as exc:
        errors.append(f"{path}: invalid JSON while collecting waivers: {exc}")
        return set()
    return {
        waiver["id"]
        for waiver in data.get("waivers", [])
        if isinstance(waiver, dict) and waiver.get("status") == "active" and waiver.get("id")
    }


def validate_feature_support(path: Path, data: dict[str, Any], waiver_ids: set[str], errors: list[str]) -> None:
    validate_evidence(path, data, errors)
    features = data.get("features")
    require(isinstance(features, dict) and len(features) > 0, f"{path}: features must be non-empty object", errors)
    if not isinstance(features, dict):
        return
    for feature, entry in features.items():
        require(isinstance(entry, dict), f"{path}: feature `{feature}` must be object", errors)
        if not isinstance(entry, dict):
            continue
        tier = entry.get("tier")
        require(tier in DECLARED_TIERS, f"{path}: feature `{feature}` invalid tier `{tier}`", errors)
        reported = entry.get("reported")
        require(reported in {"supported", "unsupported", "partial", "n/a"}, f"{path}: feature `{feature}` invalid reported `{reported}`", errors)
        if reported in {"supported", "partial"}:
            require(bool(entry.get("probe") or entry.get("waiver")), f"{path}: feature `{feature}` reported {reported} without probe/waiver", errors)
        if entry.get("state") == "stub_risky" and reported in {"supported", "partial"}:
            waiver = entry.get("waiver")
            probe_status = entry.get("probe_status")
            require(
                waiver in waiver_ids or probe_status == "passed",
                f"{path}: risky supported feature `{feature}` lacks active waiver or passing probe",
                errors,
            )


def validate_waivers(path: Path, data: dict[str, Any], errors: list[str]) -> None:
    waivers = data.get("waivers")
    require(isinstance(waivers, list), f"{path}: waivers must be a list", errors)
    if isinstance(waivers, list):
        for i, waiver in enumerate(waivers):
            require(isinstance(waiver, dict), f"{path}: waivers[{i}] must be object", errors)
            if isinstance(waiver, dict):
                require(bool(waiver.get("id")), f"{path}: waivers[{i}] missing id", errors)
                require(bool(waiver.get("kind")), f"{path}: waivers[{i}] missing kind", errors)
                require(bool(waiver.get("target")), f"{path}: waivers[{i}] missing target", errors)
                require(bool(waiver.get("status")), f"{path}: waivers[{i}] missing status", errors)
                require(
                    bool(waiver.get("justification")),
                    f"{path}: waivers[{i}] missing justification",
                    errors,
                )
                require(bool(waiver.get("expires_when")), f"{path}: waivers[{i}] missing expires_when", errors)
                evidence = waiver.get("evidence")
                require(
                    isinstance(evidence, list) and len(evidence) > 0,
                    f"{path}: waivers[{i}] evidence must be non-empty",
                    errors,
                )


def validate_fl12_2_gate(path: Path, data: dict[str, Any], errors: list[str]) -> None:
    validate_evidence(path, data, errors)
    target = data.get("target")
    require(isinstance(target, dict), f"{path}: target must be an object", errors)
    if isinstance(target, dict):
        require(target.get("feature_level") == "12_2", f"{path}: target.feature_level must be `12_2`", errors)
        require(target.get("shader_model") == "6_7", f"{path}: target.shader_model must be `6_7`", errors)
        levels = target.get("expected_levels")
        require(
            levels == ["11_0", "11_1", "12_0", "12_1", "12_2"],
            f"{path}: target.expected_levels must enumerate 11_0 through 12_2",
            errors,
        )
        require(target.get("promotion_is_atomic") is True, f"{path}: promotion_is_atomic must be true", errors)

    identity = data.get("identity")
    require(isinstance(identity, dict), f"{path}: identity must be an object", errors)
    if isinstance(identity, dict):
        for key in ("environment_result", "host_runtime_result", "wine_version", "required_artifacts"):
            require(bool(identity.get(key)), f"{path}: identity.{key} is required", errors)
        require(identity.get("wine_version") == "wine-11.5", f"{path}: identity.wine_version must be wine-11.5", errors)
        require(
            isinstance(identity.get("required_artifacts"), list) and len(identity["required_artifacts"]) > 0,
            f"{path}: identity.required_artifacts must be a non-empty list",
            errors,
        )

    valid_operators = {"equals", "at_least", "bitmask_all", "hr_success"}
    for section in ("query_requirements", "behavior_requirements"):
        entries = data.get(section)
        require(isinstance(entries, list) and len(entries) > 0, f"{path}: {section} must be non-empty", errors)
        if not isinstance(entries, list):
            continue
        ids: set[str] = set()
        for index, entry in enumerate(entries):
            prefix = f"{path}: {section}[{index}]"
            require(isinstance(entry, dict), f"{prefix} must be an object", errors)
            if not isinstance(entry, dict):
                continue
            check_id = entry.get("id")
            require(isinstance(check_id, str) and bool(check_id), f"{prefix} missing id", errors)
            if isinstance(check_id, str) and check_id:
                require(check_id not in ids, f"{prefix} duplicates id `{check_id}`", errors)
                ids.add(check_id)
            for key in ("api", "probe", "path"):
                require(isinstance(entry.get(key), str) and bool(entry.get(key)), f"{prefix} missing {key}", errors)
            require(entry.get("operator") in valid_operators, f"{prefix} has invalid operator `{entry.get('operator')}`", errors)
            require("expected" in entry, f"{prefix} missing expected value", errors)


def validate_reference_contract(path: Path, data: dict[str, Any], errors: list[str]) -> None:
    validate_evidence(path, data, errors)
    require(isinstance(data.get("summary"), dict), f"{path}: missing summary", errors)
    require(isinstance(data.get("data"), dict), f"{path}: missing imported data object", errors)
    summary = data.get("summary", {})
    if isinstance(summary, dict):
        require(summary.get("interface_count", 0) > 0, f"{path}: interface_count must be > 0", errors)
        require(summary.get("method_count", 0) > 0, f"{path}: method_count must be > 0", errors)


def validate_winemetal_bridge(path: Path, data: dict[str, Any], errors: list[str]) -> None:
    source_audit = data.get("source_audit")
    require(isinstance(source_audit, dict), f"{path}: missing source_audit object", errors)
    if isinstance(source_audit, dict):
        for key in ("pe_header", "cxx_facade", "pe_thunks", "unix_bridge", "unix_struct_header"):
            value = source_audit.get(key)
            require(isinstance(value, str) and bool(value), f"{path}: source_audit.{key} missing", errors)
            if isinstance(value, str) and value:
                require((ROOT_DIR / value).exists(), f"{path}: source_audit.{key} path does not exist: {value}", errors)
    for key in (
        "required_pe_exports",
        "required_unix_call_entries",
        "required_render_command_stream_entries",
        "probe_coverage",
    ):
        value = data.get(key)
        require(isinstance(value, list) and len(value) > 0, f"{path}: {key} must be non-empty list", errors)
    sizes = data.get("critical_unixcall_struct_sizes")
    require(isinstance(sizes, dict) and len(sizes) > 0, f"{path}: critical_unixcall_struct_sizes must be non-empty object", errors)
    if isinstance(sizes, dict):
        for struct_name, size in sizes.items():
            require(struct_name.startswith("unixcall_"), f"{path}: invalid unixcall struct name `{struct_name}`", errors)
            require(isinstance(size, int) and size > 0 and size % 8 == 0, f"{path}: invalid size for `{struct_name}`: {size}", errors)
    deferred = data.get("deferred_until_claimed")
    require(isinstance(deferred, list), f"{path}: deferred_until_claimed must be list", errors)
    if isinstance(deferred, list):
        for i, entry in enumerate(deferred):
            require(isinstance(entry, dict), f"{path}: deferred_until_claimed[{i}] must be object", errors)
            if isinstance(entry, dict):
                require(bool(entry.get("surface")), f"{path}: deferred_until_claimed[{i}] missing surface", errors)
                require(bool(entry.get("reason")), f"{path}: deferred_until_claimed[{i}] missing reason", errors)


def validate_contracts(root: Path) -> list[str]:
    errors: list[str] = []
    waiver_ids = active_waiver_ids(root, errors)
    for name in REQUIRED_CONTRACTS:
        path = root / name
        require(path.exists(), f"missing required contract: {path}", errors)
        if not path.exists():
            continue
        try:
            data = load(path)
        except json.JSONDecodeError as exc:
            errors.append(f"{path}: invalid JSON: {exc}")
            continue
        require(isinstance(data, dict), f"{path}: top-level JSON must be object", errors)
        if not isinstance(data, dict):
            continue
        require(bool(data.get("schema")), f"{path}: missing schema", errors)
        if name in {"d3d12-metal-contract.json", "agility-1.619.3-contract.json"}:
            validate_reference_contract(path, data, errors)
        elif name in {"unsupported-api-ledger.json", "risky-stub-ledger.json"}:
            validate_ledgers(path, data, errors)
            if name == "unsupported-api-ledger.json":
                apis = {entry.get("api") for entry in data.get("entries", []) if isinstance(entry, dict)}
                missing = sorted(REQUIRED_UNSUPPORTED_APIS - apis)
                require(not missing, f"{path}: missing required unsupported APIs: {', '.join(missing)}", errors)
        elif name == "contract-waivers.json":
            validate_waivers(path, data, errors)
        elif name == "feature-support-contract.json":
            validate_feature_support(path, data, waiver_ids, errors)
        elif name == "fl12-2-gate-contract.json":
            validate_fl12_2_gate(path, data, errors)
        elif name == "winemetal-bridge-contract.json":
            validate_winemetal_bridge(path, data, errors)
        else:
            validate_evidence(path, data, errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=DEFAULT_CONTRACT_ROOT)
    args = parser.parse_args()

    errors = validate_contracts(args.root)
    if errors:
        for error in errors:
            print(f"[FAIL] {error}")
        return 1
    print(f"[PASS] validated {len(REQUIRED_CONTRACTS)} D3D12 Metal SDK contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
