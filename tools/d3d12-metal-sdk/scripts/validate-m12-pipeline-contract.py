#!/usr/bin/env python3
"""Validate the M12 D3D12 launch/logging contract against the local tree."""

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
    / "m12-pipeline-contract.json"
)

REQUIRED_ENV_KEYS = {
    "WINEDLLOVERRIDES",
    "WINEDLLPATH",
    "DXMT_WINEMETAL_UNIXLIB",
    "METALSHARP_SHADER_CACHE_PATH",
    "DXMT_PIPELINE_CACHE_PATH",
}

REQUIRED_STAGE_IDS = {
    "recipe-and-route",
    "launch-env",
    "runtime-logging",
    "dxgi-entry",
    "d3d12-entry",
    "shader-engine",
    "developer-stress-exe",
}

REQUIRED_GATE_IDS = {
    "pipeline-contract",
    "rust-launch-env",
    "runtime-layout",
    "shader-engine",
    "m12-check",
    "stress-game",
}

def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: top-level JSON must be an object")
    return data


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def read_rel(path: str, errors: list[str]) -> str:
    target = ROOT_DIR / path
    if not target.exists():
        errors.append(f"missing source path: {path}")
        return ""
    if not target.is_file():
        errors.append(f"source path is not a file: {path}")
        return ""
    return target.read_text(encoding="utf-8", errors="replace")


def validate_contract_shape(data: dict[str, Any], errors: list[str]) -> None:
    require(data.get("schema") == "metalsharp.d3d12.m12-pipeline.v1", "unexpected or missing schema", errors)

    required_env = data.get("required_env")
    require(isinstance(required_env, list) and bool(required_env), "required_env must be a non-empty list", errors)
    if isinstance(required_env, list):
        keys = {entry.get("key") for entry in required_env if isinstance(entry, dict)}
        missing = sorted(REQUIRED_ENV_KEYS - keys)
        require(not missing, f"required_env missing keys: {', '.join(missing)}", errors)
        for entry in required_env:
            if not isinstance(entry, dict):
                errors.append("required_env entries must be objects")
                continue
            require(bool(entry.get("key")), "required_env entry missing key", errors)
            require(bool(entry.get("value_contains")), f"{entry.get('key')}: missing value_contains", errors)
            require(bool(entry.get("proves")), f"{entry.get('key')}: missing proves", errors)

    stages = data.get("pipeline_stages")
    require(isinstance(stages, list) and bool(stages), "pipeline_stages must be a non-empty list", errors)
    if isinstance(stages, list):
        ids = {entry.get("id") for entry in stages if isinstance(entry, dict)}
        missing = sorted(REQUIRED_STAGE_IDS - ids)
        require(not missing, f"pipeline_stages missing ids: {', '.join(missing)}", errors)
        for stage in stages:
            if not isinstance(stage, dict):
                errors.append("pipeline_stages entries must be objects")
                continue
            require(bool(stage.get("id")), "pipeline stage missing id", errors)
            require(bool(stage.get("source")), f"{stage.get('id')}: missing source", errors)
            require(bool(stage.get("flow")), f"{stage.get('id')}: missing flow", errors)
            source = stage.get("source")
            if isinstance(source, str) and source:
                require((ROOT_DIR / source).exists(), f"{stage.get('id')}: source path does not exist: {source}", errors)

    gates = data.get("dev_gates")
    require(isinstance(gates, list) and bool(gates), "dev_gates must be a non-empty list", errors)
    if isinstance(gates, list):
        ids = {entry.get("id") for entry in gates if isinstance(entry, dict)}
        missing = sorted(REQUIRED_GATE_IDS - ids)
        require(not missing, f"dev_gates missing ids: {', '.join(missing)}", errors)
        for gate in gates:
            if not isinstance(gate, dict):
                errors.append("dev_gates entries must be objects")
                continue
            require(bool(gate.get("id")), "dev gate missing id", errors)
            require(bool(gate.get("command")), f"{gate.get('id')}: missing command", errors)
            require(bool(gate.get("pinpoints")), f"{gate.get('id')}: missing pinpoints", errors)


def validate_source_contract(data: dict[str, Any], errors: list[str]) -> None:
    launcher = read_rel("app/src-rust/src/mtsp/launcher.rs", errors)
    engine = read_rel("app/src-rust/src/mtsp/engine.rs", errors)
    log_cpp = read_rel("vendor/dxmt/src/util/log/log.cpp", errors)

    for pattern in [
        "steam_pipeline_env_pairs",
        "build_cache_paths",
        "m12-pipeline",
        "DXMT_WINEMETAL_UNIXLIB",
        "DXMT_PIPELINE_CACHE_PATH",
        "METALSHARP_SHADER_CACHE_PATH",
        "DXMT_LOG_PATH",
        "dxmt_m12",
    ]:
        require(pattern in launcher, f"launcher missing `{pattern}`", errors)

    for pattern in [
        "id: PipelineId::M12",
        "lib/dxmt_m12/x86_64-windows",
        "lib/dxmt_m12/x86_64-unix",
        "winemetal,d3d12,dxgi,dxgi_dxmt,d3d11,d3d10core=n,b",
        'shader_cache_subdir: Some("m12")',
    ]:
        require(pattern in engine, f"engine missing `{pattern}`", errors)

    require("DXMT_LOG_PATH" in log_cpp, "DXMT logger missing `DXMT_LOG_PATH`", errors)
    require(
        'path += env::getExeBaseName() + "_" + base' in log_cpp,
        "DXMT logger missing per-component log file naming",
        errors,
    )

    for comment in data.get("source_comments", []):
        if not isinstance(comment, dict):
            errors.append("source_comments entries must be objects")
            continue
        path = str(comment.get("path") or "")
        pattern = str(comment.get("must_contain") or "")
        require(bool(path), "source_comments entry missing path", errors)
        require(bool(pattern), f"{path}: source_comments entry missing must_contain", errors)
        if path and pattern:
            require(pattern in read_rel(path, errors), f"{path}: missing source comment `{pattern}`", errors)


def validate_m12_route_guards(data: dict[str, Any], errors: list[str]) -> None:
    engine = read_rel("app/src-rust/src/mtsp/engine.rs", errors)
    launcher = read_rel("app/src-rust/src/mtsp/launcher.rs", errors)

    def m12_engine_block() -> str:
        start = engine.find("id: PipelineId::M12")
        end = engine.find("id: PipelineId::M11", start)
        if start == -1 or end == -1:
            return ""
        return engine[start:end]

    block = m12_engine_block()
    require(bool(block), "could not isolate M12 engine block", errors)

    forbidden = [str(item) for item in data.get("forbidden_m12_launcher_patterns", [])]
    require(bool(forbidden), "forbidden_m12_launcher_patterns must be non-empty", errors)
    for pattern in forbidden:
        require(pattern not in block, f"M12 engine block must not contain `{pattern}`", errors)

    for pattern in [
        'wine_overrides: Some("',
        '!m12.deploy_dlls.iter().any(|dll| dll.filename == "metalsharp_ntdll_hook.dll")',
        'winedllpath.contains("dxmt_m12/x86_64-windows")',
        'path.contains("dxmt_m12")',
    ]:
        require(pattern in engine + launcher, f"M12 route guard missing `{pattern}`", errors)


def validate_evidence(data: dict[str, Any], errors: list[str]) -> None:
    evidence = data.get("evidence")
    require(isinstance(evidence, list) and bool(evidence), "evidence must be a non-empty list", errors)
    if not isinstance(evidence, list):
        return
    for entry in evidence:
        if not isinstance(entry, dict):
            errors.append("evidence entries must be objects")
            continue
        path = entry.get("path")
        require(bool(entry.get("kind")), "evidence entry missing kind", errors)
        require(bool(path), "evidence entry missing path", errors)
        require(bool(entry.get("note")), "evidence entry missing note", errors)
        if isinstance(path, str) and path:
            require((ROOT_DIR / path).exists(), f"evidence path does not exist: {path}", errors)


def validate(data: dict[str, Any]) -> dict[str, Any]:
    errors: list[str] = []
    validate_contract_shape(data, errors)
    validate_source_contract(data, errors)
    validate_m12_route_guards(data, errors)
    validate_evidence(data, errors)
    return {
        "schema": "metalsharp.d3d12.m12-pipeline.audit.v1",
        "ok": not errors,
        "summary": {
            "required_env_count": len(data.get("required_env", [])) if isinstance(data.get("required_env"), list) else 0,
            "stage_count": len(data.get("pipeline_stages", [])) if isinstance(data.get("pipeline_stages"), list) else 0,
            "gate_count": len(data.get("dev_gates", [])) if isinstance(data.get("dev_gates"), list) else 0,
            "error_count": len(errors),
        },
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    audit = validate(load_json(args.contract))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")

    if audit["ok"]:
        print(
            "[PASS] M12 pipeline contract: "
            f"{audit['summary']['required_env_count']} env keys, "
            f"{audit['summary']['stage_count']} stages, "
            f"{audit['summary']['gate_count']} gates"
        )
        return 0

    for error in audit["errors"]:
        print(f"[FAIL] {error}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
