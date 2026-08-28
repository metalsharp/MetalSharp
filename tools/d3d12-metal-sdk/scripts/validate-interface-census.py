#!/usr/bin/env python3
"""Validate the phase-0 interface census against the pinned contract and source tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
DEFAULT_CONTRACT = ROOT_DIR / "tools/d3d12-metal-sdk/contracts/agility-1.619.5-contract.json"
DEFAULT_CENSUS = ROOT_DIR / "tools/d3d12-metal-sdk/contracts/d3d12-interface-census.json"
RUNTIME_ROOTS = (
    ROOT_DIR / "vendor/dxmt/src/d3d12",
    ROOT_DIR / "vendor/dxmt/src/dxgi",
    ROOT_DIR / "vendor/dxmt/src/dxmt",
    ROOT_DIR / "vendor/dxmt/src/winemetal",
)
EXTENSIONS = {".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm"}


def load(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def sha256(path: Path) -> bytes:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return bytes.fromhex(digest.hexdigest())


def runtime_tree_digest() -> str:
    files: list[Path] = []
    for root in RUNTIME_ROOTS:
        if root.exists():
            files.extend(path for path in root.rglob("*") if path.is_file() and path.suffix in EXTENSIONS)
    digest = hashlib.sha256()
    for path in sorted(set(files)):
        digest.update(path.relative_to(ROOT_DIR).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(sha256(path))
        digest.update(b"\n")
    return digest.hexdigest()


def git_head() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT_DIR, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def validate(contract: dict[str, Any], census: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    contract_data = contract.get("data")
    interfaces = census.get("interfaces")
    if not isinstance(contract_data, dict):
        return ["stable contract data must be an object"]
    if not isinstance(interfaces, dict):
        return ["census interfaces must be an object"]

    if contract.get("agility_sdk_version") != "1.619.5":
        errors.append("census source contract must be Agility 1.619.5")
    if contract.get("d3d12_sdk_version") != 619:
        errors.append("census source contract must use D3D12SDKVersion 619")
    if len(contract_data) != 145:
        errors.append(f"stable contract has {len(contract_data)} interfaces; expected 145")
    contract_method_count = sum(len(item.get("methods", {})) for item in contract_data.values())
    if contract_method_count != 537:
        errors.append(f"stable contract has {contract_method_count} methods; expected 537")
    if len(interfaces) != len(contract_data):
        errors.append("census interface count does not match stable contract")

    seen_guids: set[str] = set()
    census_method_count = 0
    for interface, details in sorted(contract_data.items()):
        census_item = interfaces.get(interface)
        if not isinstance(census_item, dict):
            errors.append(f"missing census interface: {interface}")
            continue
        if census_item.get("guid") != details.get("guid"):
            errors.append(f"{interface}: GUID mismatch")
        if census_item.get("parent") != details.get("parent"):
            errors.append(f"{interface}: parent mismatch")
        guid = details.get("guid", "")
        if not isinstance(guid, str) or len(guid.split("-")) != 5:
            errors.append(f"{interface}: malformed GUID")
        if guid in seen_guids:
            errors.append(f"{interface}: duplicate GUID")
        seen_guids.add(guid)

        contract_methods = details.get("methods", {})
        census_methods = census_item.get("methods")
        if not isinstance(census_methods, dict):
            errors.append(f"{interface}: census methods must be an object")
            continue
        if set(contract_methods) != set(census_methods):
            missing = sorted(set(contract_methods) - set(census_methods))
            extra = sorted(set(census_methods) - set(contract_methods))
            if missing:
                errors.append(f"{interface}: missing methods: {', '.join(missing)}")
            if extra:
                errors.append(f"{interface}: unknown methods: {', '.join(extra)}")
        census_method_count += len(census_methods)
        if census_item.get("method_count") != len(contract_methods):
            errors.append(f"{interface}: method_count is not synchronized")
        for method, record in census_methods.items():
            if not isinstance(record, dict):
                errors.append(f"{interface}.{method}: census record must be an object")
                continue
            owner = record.get("owner")
            if not isinstance(owner, dict):
                errors.append(f"{interface}.{method}: owner is required")
                continue
            if not owner.get("primary_source") or not owner.get("candidate_sources"):
                errors.append(f"{interface}.{method}: legal method has no source owner")
            primary = owner.get("primary_source")
            if isinstance(primary, str) and not (ROOT_DIR / primary).exists():
                errors.append(f"{interface}.{method}: primary owner does not exist: {primary}")
            if record.get("behavior_status") != "unverified":
                errors.append(f"{interface}.{method}: phase-0 behavior status must remain unverified")

    if census_method_count != contract_method_count:
        errors.append(f"census has {census_method_count} methods; contract has {contract_method_count}")
    source_identity = census.get("source_identity")
    if not isinstance(source_identity, dict):
        errors.append("census source_identity is required")
    else:
        current_digest = runtime_tree_digest()
        if source_identity.get("runtime_tree_sha256") != current_digest:
            errors.append("census runtime source tree digest is stale; regenerate phase-0 inventory")
        if not source_identity.get("git_head"):
            errors.append("census git_head provenance is required")
    summary = census.get("summary")
    if not isinstance(summary, dict):
        errors.append("census summary is required")
    else:
        if summary.get("interface_count") != len(contract_data):
            errors.append("census summary interface_count is stale")
        if summary.get("method_count") != contract_method_count:
            errors.append("census summary method_count is stale")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    parser.add_argument("--census", type=Path, default=DEFAULT_CENSUS)
    args = parser.parse_args()
    errors = validate(load(args.contract), load(args.census))
    if errors:
        for error in errors:
            print(f"[FAIL] {error}")
        return 1
    census = load(args.census)
    print(
        f"[PASS] interface census: {census['summary']['interface_count']} interfaces, "
        f"{census['summary']['method_count']} methods; source tree is current"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
