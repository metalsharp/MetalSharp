#!/usr/bin/env python3
"""Validate that D3D12 Metal claims have explicit probe and CI coverage."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[3]
SDK_DIR = ROOT_DIR / "tools" / "d3d12-metal-sdk"
CONTRACTS_DIR = SDK_DIR / "contracts"

REQUIRED_GROUPS = {
    "loader_runtime_route": ["probe-loader"],
    "device_caps_feature_reports": ["probe-device-caps"],
    "feature_levels_11_0_through_12_2": ["probe-feature-levels"],
    "dxgi_factory_swapchain": ["probe-dxgi-factory", "probe-mini-swapchain-present"],
    "queues_fences_command_lists": ["probe-queues", "probe-command-replay", "probe-mini-command-queue"],
    "resources_heaps_views_mapping_copies": ["probe-resources", "probe-resource-views-formats", "probe-mini-texture-sample"],
    "descriptor_heaps_root_signatures": ["probe-descriptors", "probe-mini-root-signature", "probe-mini-descriptors"],
    "graphics_pso_state_matrix": ["probe-graphics-pso", "probe-mini-graphics-pso"],
    "compute_pso_dispatch": ["probe-compute-pso", "probe-mini-compute-dispatch", "probe-mini-compute-first-use-dispatch"],
    "dxil_opcode_groups": ["probe-dxil-semantics", "probe-mini-dxil-texture-color-output"],
    "synthetic_shader_corpus": ["probe-shader-corpus"],
    "sm66_capability_audit": ["probe-sm66-capabilities"],
    "waveops_capability_audit": ["probe-wave-ops"],
    "shader_reflection_argument_binding": ["probe-shaders", "probe-reflection-abi"],
    "barriers_resource_state": ["probe-barriers-render-pass"],
    "query_heaps_timestamps_counters": ["probe-queues", "probe-barriers-render-pass"],
    "indirect_commands": ["probe-command-replay"],
    "agility_sdk_compiler_cache": ["probe-agility-ue5"],
    "winemetal_bridge_abi": ["winemetal-abi"],
}

CI_TOUCH_PATHS = [
    "vendor/dxmt/src/d3d12",
    "vendor/dxmt/src/airconv",
    "vendor/dxmt/src/winemetal",
    "tools/d3d12-metal-sdk",
]


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def collect_probe_tokens() -> set[str]:
    run_probes = (SDK_DIR / "scripts" / "run-probes.sh").read_text(encoding="utf-8")
    compare_contract = (SDK_DIR / "scripts" / "compare-contract.py").read_text(encoding="utf-8")
    tokens = set(re.findall(r"probe[-_][A-Za-z0-9_-]+", run_probes + "\n" + compare_contract))
    normalized = {token.replace("_", "-").removesuffix("-exe") for token in tokens}
    if "winemetal-abi" in run_probes:
        normalized.add("winemetal-abi")
    return normalized


def collect_claimed_probe_paths() -> list[tuple[Path, str]]:
    claims: list[tuple[Path, str]] = []
    for path in sorted(CONTRACTS_DIR.glob("*.json")):
        data = load_json(path)
        stack = [data]
        while stack:
            value = stack.pop()
            if isinstance(value, dict):
                probe = value.get("probe") or value.get("next_probe")
                if isinstance(probe, str):
                    claims.append((path, probe))
                evidence = value.get("evidence")
                if isinstance(evidence, list):
                    for item in evidence:
                        if isinstance(item, str) and "probe" in item:
                            claims.append((path, item))
                stack.extend(value.values())
            elif isinstance(value, list):
                stack.extend(value)
    bridge = load_json(CONTRACTS_DIR / "winemetal-bridge-contract.json")
    for probe in bridge.get("probe_coverage", []):
        if isinstance(probe, str):
            claims.append((CONTRACTS_DIR / "winemetal-bridge-contract.json", probe))
    return claims


def path_exists_for_claim(claim: str) -> bool:
    if claim.startswith("tools/d3d12-metal-sdk/probes/probe_mini_suite"):
        return (SDK_DIR / "probes" / "probe_mini_suite" / "probe_mini_suite.cpp").exists()
    path = ROOT_DIR / claim
    if path.exists():
        return True
    if not claim.startswith("tools/d3d12-metal-sdk/probes/"):
        return True
    cpp_path = path.with_suffix(".cpp")
    nested_cpp = path / f"{path.name}.cpp"
    return cpp_path.exists() or nested_cpp.exists()


def validate_ci_policy(errors: list[str]) -> None:
    workflow = ROOT_DIR / ".github" / "workflows" / "pr-ci.yml"
    text = workflow.read_text(encoding="utf-8")
    for required in (
        "tools/d3d12-metal-sdk/scripts/validate-contracts.py",
        "tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py",
    ):
        if required not in text:
            errors.append(f"{workflow}: CI does not run {required}")
    for touch_path in CI_TOUCH_PATHS:
        if touch_path not in text:
            errors.append(f"{workflow}: CI policy does not mention touch path {touch_path}")


def main() -> int:
    errors: list[str] = []
    tokens = collect_probe_tokens()
    for group, probes in REQUIRED_GROUPS.items():
        if not any(probe in tokens for probe in probes):
            errors.append(f"probe group `{group}` has no runnable probe token from {probes}")

    for source, claim in collect_claimed_probe_paths():
        if not path_exists_for_claim(claim):
            errors.append(f"{source}: claimed probe path does not exist: {claim}")

    validate_ci_policy(errors)

    if errors:
        for error in errors:
            print(f"[FAIL] {error}")
        return 1
    print(f"[PASS] validated {len(REQUIRED_GROUPS)} D3D12 Metal SDK probe groups")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
