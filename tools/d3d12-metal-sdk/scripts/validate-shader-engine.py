#!/usr/bin/env python3
"""Validate the VKD3D D3D12 shader-engine contract against the local tree."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[3]
DEFAULT_CONTRACT = (
    ROOT_DIR
    / "tools"
    / "d3d12-metal-sdk"
    / "contracts"
    / "d3d12-shader-engine-contract.json"
)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: top-level JSON must be an object")
    return data


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def text_for_paths(paths: list[str], errors: list[str]) -> str:
    chunks: list[str] = []
    for rel in paths:
        path = ROOT_DIR / rel
        if not path.exists():
            errors.append(f"missing source path: {rel}")
            continue
        if not path.is_file():
            errors.append(f"source path is not a file: {rel}")
            continue
        chunks.append(path.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(chunks)


def validate_contract(data: dict[str, Any]) -> dict[str, Any]:
    errors: list[str] = []
    warnings: list[str] = []

    require(
        data.get("schema") == "metalsharp.d3d12.shader-engine.v1",
        "unexpected or missing shader-engine schema",
        errors,
    )

    surfaces = data.get("source_surfaces")
    require(isinstance(surfaces, list) and bool(surfaces), "source_surfaces must be a non-empty list", errors)
    surface_results: list[dict[str, Any]] = []
    if isinstance(surfaces, list):
        seen_ids: set[str] = set()
        for surface in surfaces:
            require(isinstance(surface, dict), "source_surfaces entries must be objects", errors)
            if not isinstance(surface, dict):
                continue
            sid = str(surface.get("id") or "")
            require(bool(sid), "source surface missing id", errors)
            require(sid not in seen_ids, f"duplicate source surface id: {sid}", errors)
            seen_ids.add(sid)

            paths = surface.get("paths")
            patterns = surface.get("must_contain")
            require(isinstance(paths, list) and bool(paths), f"{sid}: paths must be non-empty", errors)
            require(isinstance(patterns, list) and bool(patterns), f"{sid}: must_contain must be non-empty", errors)
            if not isinstance(paths, list) or not isinstance(patterns, list):
                continue

            local_errors: list[str] = []
            combined = text_for_paths([str(path) for path in paths], local_errors)
            missing_patterns = [
                str(pattern)
                for pattern in patterns
                if str(pattern) not in combined
            ]
            for err in local_errors:
                errors.append(f"{sid}: {err}")
            for pattern in missing_patterns:
                errors.append(f"{sid}: missing required pattern `{pattern}`")
            surface_results.append(
                {
                    "id": sid,
                    "path_count": len(paths),
                    "required_pattern_count": len(patterns),
                    "missing_pattern_count": len(missing_patterns),
                    "ok": not local_errors and not missing_patterns,
                }
            )

    gates = data.get("offline_gates")
    require(isinstance(gates, list) and bool(gates), "offline_gates must be a non-empty list", errors)
    gate_results: list[dict[str, Any]] = []
    if isinstance(gates, list):
        seen_ids: set[str] = set()
        for gate in gates:
            require(isinstance(gate, dict), "offline_gates entries must be objects", errors)
            if not isinstance(gate, dict):
                continue
            gid = str(gate.get("id") or "")
            require(bool(gid), "offline gate missing id", errors)
            require(gid not in seen_ids, f"duplicate offline gate id: {gid}", errors)
            seen_ids.add(gid)
            require(bool(gate.get("command")), f"{gid}: missing command", errors)
            require(bool(gate.get("proves")), f"{gid}: missing proves", errors)
            gate_results.append({"id": gid, "ok": bool(gate.get("command")) and bool(gate.get("proves"))})

    artifact_names = data.get("runtime_artifacts")
    require(
        isinstance(artifact_names, list) and {"d3d12.dll", "dxgi.dll", "winemetal.dll", "winemetal.so"}.issubset(set(artifact_names)),
        "runtime_artifacts must include d3d12.dll, dxgi.dll, winemetal.dll, and winemetal.so",
        errors,
    )

    evidence = data.get("evidence")
    require(isinstance(evidence, list) and bool(evidence), "evidence must be a non-empty list", errors)
    if isinstance(evidence, list):
        for entry in evidence:
            if not isinstance(entry, dict):
                errors.append("evidence entries must be objects")
                continue
            rel = entry.get("path")
            require(bool(entry.get("kind")), "evidence entry missing kind", errors)
            require(bool(rel), "evidence entry missing path", errors)
            require(bool(entry.get("note")), "evidence entry missing note", errors)
            if rel and not (ROOT_DIR / str(rel)).exists():
                errors.append(f"evidence path does not exist: {rel}")

    return {
        "schema": "metalsharp.d3d12.shader-engine.audit.v1",
        "ok": not errors,
        "summary": {
            "surface_count": len(surface_results),
            "offline_gate_count": len(gate_results),
            "runtime_artifact_count": len(artifact_names) if isinstance(artifact_names, list) else 0,
            "error_count": len(errors),
            "warning_count": len(warnings),
        },
        "surfaces": surface_results,
        "offline_gates": gate_results,
        "errors": errors,
        "warnings": warnings,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    data = load_json(args.contract)
    audit = validate_contract(data)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")

    if audit["ok"]:
        print(
            "[PASS] shader engine contract: "
            f"{audit['summary']['surface_count']} surfaces, "
            f"{audit['summary']['offline_gate_count']} gates"
        )
        return 0

    for error in audit["errors"]:
        print(f"[FAIL] {error}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
