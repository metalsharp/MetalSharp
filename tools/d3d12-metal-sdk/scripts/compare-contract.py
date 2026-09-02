#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[3]
SDK_DIR = ROOT_DIR / "tools" / "d3d12-metal-sdk"
DEFAULT_RESULTS_DIR = SDK_DIR / "results"
DEFAULT_CONTRACTS_DIR = SDK_DIR / "contracts"

REQUIRED_PROBES = [
    "winemetal-abi",
    "probe-loader",
    "probe-agility-ue5",
    "probe-device-caps",
    "probe-legacy-regression",
    "probe-dxgi-factory",
    "probe-resources",
    "probe-resource-views-formats",
    "probe-descriptors",
    "probe-shaders",
    "probe-dxil-semantics",
    "probe-shader-corpus",
    "probe-sm66-capabilities",
    "probe-writable-msaa",
    "probe-sampler-feedback",
    "probe-sampler-feedback-pixel",
    "probe-wave-ops",
    "probe-reflection-abi",
    "probe-queues",
    "probe-graphics-pso",
    "probe-compute-pso",
    "probe-command-replay",
    "probe-barriers-render-pass",
    "probe-render-headless",
]

OPTIONAL_PROBES = [
    "probe-present-windowed",
]


@dataclass
class Issue:
    severity: str
    category: str
    title: str
    detail: str


@dataclass
class RiskStatus:
    status: str
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare D3D12 Metal probe results against repo-owned contracts.")
    parser.add_argument("--profile", default="metalsharp", help="Probe result profile suffix to read.")
    parser.add_argument("--results-dir", type=Path, default=DEFAULT_RESULTS_DIR, help="Directory containing probe JSON outputs.")
    parser.add_argument("--contracts-dir", type=Path, default=DEFAULT_CONTRACTS_DIR, help="Directory containing contract JSON files.")
    parser.add_argument("--markdown-out", type=Path, help="Write a Markdown summary here.")
    parser.add_argument("--json-out", type=Path, help="Write a machine-readable summary here.")
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def write_text(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data, encoding="utf-8")


def get_nested(obj: dict[str, Any], *keys: str) -> Any:
    cur: Any = obj
    for key in keys:
        if not isinstance(cur, dict) or key not in cur:
            return None
        cur = cur[key]
    return cur


def parse_shader_model(value: Any) -> tuple[int, int] | None:
    if not isinstance(value, str):
        return None
    parts = value.split("_", 1)
    if len(parts) != 2:
        return None
    try:
        return int(parts[0]), int(parts[1])
    except ValueError:
        return None


def shader_model_at_least(observed: Any, target: Any) -> bool:
    observed_model = parse_shader_model(observed)
    target_model = parse_shader_model(target)
    if observed_model is None or target_model is None:
        return False
    return observed_model >= target_model


def collect_states(obj: Any, prefix: str = "") -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    if isinstance(obj, dict):
        if isinstance(obj.get("state"), str):
            found.append((prefix or "<root>", obj["state"]))
        for key, value in obj.items():
            child = f"{prefix}.{key}" if prefix else key
            found.extend(collect_states(value, child))
    elif isinstance(obj, list):
        for idx, value in enumerate(obj):
            child = f"{prefix}[{idx}]"
            found.extend(collect_states(value, child))
    return found


def load_probe_results(results_dir: Path, profile: str) -> dict[str, dict[str, Any]]:
    results: dict[str, dict[str, Any]] = {}
    for path in sorted(results_dir.glob(f"probe-*-{profile}.json")):
        probe_name = path.name[: -(len(profile) + len(".json") + 1)]
        results[probe_name] = load_json(path)
    abi_path = results_dir / f"winemetal-abi-{profile}.json"
    if abi_path.exists():
        results["winemetal-abi"] = load_json(abi_path)
    return results


def load_waivers(contracts_dir: Path, profile: str) -> dict[str, dict[str, Any]]:
    waivers_path = contracts_dir / "contract-waivers.json"
    if not waivers_path.exists():
        return {}
    waivers_doc = load_json(waivers_path)
    active: dict[str, dict[str, Any]] = {}
    for waiver in waivers_doc.get("waivers", []):
        if waiver.get("status") != "active":
            continue
        profiles = waiver.get("applies_to_profiles", [])
        if profiles and profile not in profiles:
            continue
        target = waiver.get("target")
        if isinstance(target, str):
            active[target] = waiver
    return active


def check_required_probes(results: dict[str, dict[str, Any]]) -> tuple[list[Issue], dict[str, Any]]:
    issues: list[Issue] = []
    summary: dict[str, Any] = {"required": {}, "optional": {}}

    for probe in REQUIRED_PROBES:
        data = results.get(probe)
        if data is None:
            issues.append(Issue("error", "probe", f"Missing required probe `{probe}`", "Expected JSON result was not found."))
            summary["required"][probe] = {"present": False, "pass": False}
            continue
        passed = bool(data.get("pass", data.get("passed", data.get("ok"))))
        summary["required"][probe] = {"present": True, "pass": passed}
        if not passed:
            issues.append(Issue("error", "probe", f"Required probe `{probe}` did not pass", "Comparator only trusts passing proof targets."))

    for probe in OPTIONAL_PROBES:
        data = results.get(probe)
        summary["optional"][probe] = {"present": data is not None, "pass": bool(data.get("pass")) if data else False}

    return issues, summary


def check_unsupported(results: dict[str, dict[str, Any]], ledger: dict[str, Any]) -> tuple[list[Issue], list[dict[str, Any]]]:
    device_caps = results.get("probe-device-caps", {})
    advanced = get_nested(device_caps, "advanced_features") or {}
    unsupported_checks = {
        "D3D12 ray tracing tiers": advanced.get("raytracing_tier"),
        "D3D12 mesh shader tiers": advanced.get("mesh_shader_tier"),
        "D3D12 sampler feedback tier": advanced.get("sampler_feedback_tier"),
        "D3D12 WaveOps feature report": 1 if get_nested(device_caps, "options1", "wave_ops") else 0,
    }

    issues: list[Issue] = []
    summary: list[dict[str, Any]] = []

    for entry in ledger.get("entries", []):
        api = entry["api"]
        # The ledger also records APIs whose broad surface is intentionally
        # limited to a behavior-proven subset.  Only entries explicitly marked
        # unsupported must remain at a conservative zero query value.
        if entry.get("state") != "unsupported":
            summary.append(
                {
                    "api": api,
                    "state": entry.get("state"),
                    "observed": unsupported_checks.get(api, "not_checked_by_probe"),
                    "compliant": True,
                    "detail": "Behavior-proven subset is advertised; only unsupported breadth remains ledgered.",
                }
            )
            continue
        if api not in unsupported_checks:
            summary.append(
                {
                    "api": api,
                    "state": entry.get("state"),
                    "observed": "not_checked_by_probe",
                    "compliant": True,
                    "detail": "Unsupported ledger entry is recorded, but the current device-caps probe has no comparable field yet.",
                }
            )
            continue
        observed = unsupported_checks.get(api)
        compliant = observed == 0
        summary.append({"api": api, "state": entry.get("state"), "observed": observed, "compliant": compliant})
        if not compliant:
            issues.append(
                Issue(
                    "error",
                    "unsupported",
                    f"Unsupported feature `{api}` is advertised as available",
                    f"Expected conservative zero/unsupported value, observed `{observed}`.",
                )
            )

    return issues, summary


def check_feature_contract(results: dict[str, dict[str, Any]], contract: dict[str, Any]) -> tuple[list[Issue], list[dict[str, Any]]]:
    device_caps = results.get("probe-device-caps", {})
    shader_probe = results.get("probe-shaders", {})
    dxil_semantics_probe = results.get("probe-dxil-semantics", {})
    shader_corpus_probe = results.get("probe-shader-corpus", {})
    sm66_probe = results.get("probe-sm66-capabilities", {})
    wave_probe = results.get("probe-wave-ops", {})
    reflection_probe = results.get("probe-reflection-abi", {})

    issues: list[Issue] = []
    summary: list[dict[str, Any]] = []

    features = contract.get("features", {})

    feature_level_contract = features.get("D3D12_FEATURE_FEATURE_LEVELS", {})
    observed_level = get_nested(device_caps, "feature_levels", "max_supported")
    expected_min = feature_level_contract.get("expected_minimum")
    target = feature_level_contract.get("current_target")
    compliant = bool(get_nested(device_caps, "feature_levels", "meets_12_0"))
    summary.append(
        {
            "feature": "D3D12_FEATURE_FEATURE_LEVELS",
            "state": feature_level_contract.get("state"),
            "observed": observed_level,
            "target": target,
            "compliant": compliant,
        }
    )
    if not compliant:
        issues.append(
            Issue(
                "error",
                "feature_support",
                "Feature level report fell below contract minimum",
                f"Expected at least `{expected_min}`, observed `{observed_level}`.",
            )
        )

    shader_model_contract = features.get("D3D12_FEATURE_SHADER_MODEL", {})
    observed_shader_model = get_nested(device_caps, "shader_model", "highest")
    shader_model_target = shader_model_contract.get("current_target")
    shader_model_ok = shader_model_at_least(observed_shader_model, shader_model_target)
    sm66_reported = shader_model_at_least(observed_shader_model, "6_6")
    sm67_reported = shader_model_at_least(observed_shader_model, "6_7")
    sm66_reportable = bool(get_nested(sm66_probe, "summary", "sm66_reportable"))
    sm67_reportable = bool(get_nested(sm66_probe, "summary", "sm67_reportable"))
    dxil_to_msl_ok = bool(get_nested(shader_probe, "dxc", "dxil_to_msl"))
    dxil_semantics_ok = bool(dxil_semantics_probe.get("pass", dxil_semantics_probe.get("ok")))
    synthetic_corpus_ok = bool(get_nested(shader_corpus_probe, "summary", "synthetic_shader_corpus_proven"))
    dxil_path_proven = dxil_to_msl_ok
    shader_summary = {
        "feature": "D3D12_FEATURE_SHADER_MODEL",
        "state": shader_model_contract.get("state"),
        "observed": observed_shader_model,
        "target": shader_model_target,
        "sm66_reported": sm66_reported,
        "sm66_reportable": sm66_reportable,
        "sm67_reported": sm67_reported,
        "sm67_reportable": sm67_reportable,
        "dxil_to_msl_proven": dxil_to_msl_ok,
        "dxil_semantics_proven": dxil_semantics_ok,
        "synthetic_shader_corpus_proven": synthetic_corpus_ok,
        "dxil_path_proven": dxil_path_proven,
    }
    shader_summary["compliant"] = (
        shader_model_ok and dxil_path_proven and synthetic_corpus_ok and
        (not sm66_reported or sm66_reportable) and
        (not sm67_reported or sm67_reportable)
    )
    summary.append(shader_summary)
    if not shader_summary["compliant"]:
        issues.append(
            Issue(
                "error",
                "feature_support",
                "Shader model report advertises an unproven SM6 path",
                f"Observed `{observed_shader_model}` against target `{shader_model_target}` with DXIL-to-MSL proof `{dxil_to_msl_ok}`, synthetic corpus `{synthetic_corpus_ok}`, SM 6.6 reportable `{sm66_reportable}`, and SM 6.7 reportable `{sm67_reportable}`.",
            )
        )
    if dxil_semantics_ok and not dxil_path_proven:
        issues.append(
            Issue(
                "error",
                "feature_support",
                "DXIL semantic coverage cannot substitute for primary DXIL-to-MSL proof",
                "The opcode semantic probe passed, but shader compliance requires the primary DXIL-to-MSL path.",
            )
        )

    options_contract = features.get("D3D12_FEATURE_D3D12_OPTIONS", {})
    options = get_nested(device_caps, "options") or {}
    missing = [
        field for field in options_contract.get("required_fields", [])
        if field_to_options_value(options, field) is None
    ]
    compliant = not missing
    summary.append(
        {
            "feature": "D3D12_FEATURE_D3D12_OPTIONS",
            "state": options_contract.get("state"),
            "missing_fields": missing,
            "compliant": compliant,
        }
    )
    if not compliant:
        issues.append(
            Issue(
                "error",
                "feature_support",
                "D3D12 options contract is missing required fields",
                f"Missing fields: {', '.join(missing)}.",
            )
        )

    options1_contract = features.get("D3D12_FEATURE_D3D12_OPTIONS1", {})
    options1 = get_nested(device_caps, "options1") or {}
    missing = [field for field in options1_contract.get("required_fields", []) if not field_to_options1_value(options1, field)]
    waveops_reported = bool(options1.get("wave_ops"))
    waveops_reportable = bool(get_nested(wave_probe, "summary", "waveops_reportable"))
    compliant = not missing and (not waveops_reported or waveops_reportable)
    summary.append(
        {
            "feature": "D3D12_FEATURE_D3D12_OPTIONS1",
            "state": options1_contract.get("state"),
            "missing_fields": missing,
            "waveops_reported": waveops_reported,
            "waveops_reportable": waveops_reportable,
            "compliant": compliant,
        }
    )
    if not compliant:
        issues.append(
            Issue(
                "error",
                "feature_support",
                "D3D12 options1 contract is missing required fields or advertises unproven WaveOps",
                f"Missing fields: {', '.join(missing)}; WaveOps reported `{waveops_reported}` reportable `{waveops_reportable}`.",
            )
        )

    reflection_ok = bool(get_nested(reflection_probe, "summary", "reflected_bindings_match_root_signature")) and bool(
        get_nested(reflection_probe, "summary", "negative_mismatch_failures_deterministic")
    )
    summary.append(
        {
            "feature": "D3D12_SHADER_REFLECTION_DESCRIPTOR_ABI",
            "state": "required",
            "reflected_bindings_match_root_signature": bool(
                get_nested(reflection_probe, "summary", "reflected_bindings_match_root_signature")
            ),
            "descriptor_table_indexing_validated": bool(
                get_nested(reflection_probe, "summary", "descriptor_table_indexing_validated")
            ),
            "graphics_compute_same_descriptor_abi": bool(
                get_nested(reflection_probe, "summary", "graphics_compute_same_descriptor_abi")
            ),
            "negative_mismatch_failures_deterministic": bool(
                get_nested(reflection_probe, "summary", "negative_mismatch_failures_deterministic")
            ),
            "compliant": reflection_ok,
        }
    )
    if not reflection_ok:
        issues.append(
            Issue(
                "error",
                "feature_support",
                "Shader reflection descriptor ABI is not proven",
                "Expected primary compiler reflection to match root signature bindings with deterministic mismatch diagnostics.",
            )
        )

    corpus_summary = get_nested(shader_corpus_probe, "summary") or {}
    corpus_required_fields = [
        "sm50_baseline",
        "sm60_to_sm66_progression",
        "resource_indexing",
        "uav_writes",
        "typed_and_structured_buffers",
        "texture_sampling",
        "root_constants",
        "waveops_compile_link",
        "unsupported_feature_rejection",
    ]
    corpus_missing = [field for field in corpus_required_fields if not bool(corpus_summary.get(field))]
    corpus_compliant = bool(corpus_summary.get("synthetic_shader_corpus_proven")) and not corpus_missing
    summary.append(
        {
            "feature": "D3D12_SYNTHETIC_SHADER_CORPUS",
            "state": "required",
            "missing_fields": corpus_missing,
            "title_captures_gating": bool(corpus_summary.get("title_captures_gating")),
            "compliant": corpus_compliant and not bool(corpus_summary.get("title_captures_gating")),
        }
    )
    if not corpus_compliant or bool(corpus_summary.get("title_captures_gating")):
        issues.append(
            Issue(
                "error",
                "feature_support",
                "Synthetic shader corpus is incomplete",
                f"Missing required corpus fields: {', '.join(corpus_missing)}.",
            )
        )

    advanced = get_nested(device_caps, "advanced_features") or {}
    for feature_name, key_map in (
        ("D3D12_FEATURE_D3D12_OPTIONS9", ["atomic64_typed_resource", "atomic64_group_shared"]),
        ("D3D12_FEATURE_D3D12_OPTIONS11", ["atomic64_descriptor_heap_resource"]),
    ):
        contract_entry = features.get(feature_name, {})
        advertised = {key: advanced.get(key) for key in key_map}
        expects_support = contract_entry.get("reported") == "supported"
        atomic_proof = bool(
            get_nested(sm66_probe, "summary", "runtime_correctness_complete")
        )
        compliant = (
            all(bool(value) for value in advertised.values()) and atomic_proof
            if expects_support
            else not any(bool(value) for value in advertised.values())
        )
        summary.append(
            {
                "feature": feature_name,
                "state": contract_entry.get("state"),
                "advertised": advertised,
                "expected_supported": expects_support,
                "runtime_proof": atomic_proof,
                "compliant": compliant,
            }
        )
        if not compliant:
            issues.append(
                Issue(
                    "error",
                    "feature_support",
                    f"{feature_name} atomic64 report does not match its contract",
                    f"Observed values: {advertised}, expected_supported={expects_support}, runtime_proof={atomic_proof}.",
                )
            )

    return issues, summary


def field_to_options_value(options: dict[str, Any], field: str) -> Any:
    mapping = {
        "ResourceBindingTier": options.get("resource_binding_tier"),
        "DoublePrecisionFloatShaderOps": options.get("double_precision_float_shader_ops"),
        "ROVsSupported": options.get("rovs_supported"),
        "ConservativeRasterizationTier": options.get("conservative_rasterization_tier"),
        "OutputMergerLogicOp": options.get("output_merger_logic_op"),
    }
    return mapping.get(field)


def field_to_options1_value(options1: dict[str, Any], field: str) -> Any:
    mapping = {
        "WaveOps": options1.get("wave_ops"),
        "WaveLaneCountMin": options1.get("wave_lane_count_min"),
        "WaveLaneCountMax": options1.get("wave_lane_count_max"),
        "Int64ShaderOps": options1.get("int64_shader_ops"),
    }
    value = mapping.get(field)
    if isinstance(value, bool):
        return value
    return value not in (None, 0, "")


def risky_status(target: str, results: dict[str, dict[str, Any]]) -> RiskStatus:
    device_caps = results.get("probe-device-caps", {})
    shader_probe = results.get("probe-shaders", {})
    dxil_semantics_probe = results.get("probe-dxil-semantics", {})
    dxgi_probe = results.get("probe-dxgi-factory", {})

    if target == "D3D12_FEATURE_SHADER_MODEL reports SM 6.6":
        used = bool(get_nested(device_caps, "requirements", "shader_model_6_6_or_better"))
        if not used:
            return RiskStatus("not_used", "Profile does not advertise SM 6.6.")
        sm66_probe = results.get("probe-sm66-capabilities", {})
        if not bool(get_nested(sm66_probe, "summary", "sm66_reportable")):
            return RiskStatus("failed", "SM 6.6 is advertised but the SM 6.6 capability audit is not reportable.")
        dxil_ok = bool(get_nested(shader_probe, "dxc", "dxil_to_msl"))
        bindless_note = str(get_nested(shader_probe, "dxmt_shader_paths", "bindless_descriptor_indexing") or "")
        if not dxil_ok:
            return RiskStatus("failed", "SM 6.6 is advertised but the primary DXIL-to-MSL path is not proven.")
        if "ready_for_next_shader_case" in bindless_note:
            return RiskStatus("waiver_required", "SM6 compute is proven, but broader descriptor-indexing or graphics SM6 coverage is still pending.")
        return RiskStatus("covered", "SM6 shader path is probe-covered without known gaps.")

    if target == "D3D12_FEATURE_D3D12_OPTIONS1 WaveOps reports supported":
        used = bool(get_nested(device_caps, "options1", "wave_ops"))
        if not used:
            return RiskStatus("not_used", "Profile does not advertise WaveOps.")
        wave_probe = results.get("probe-wave-ops", {})
        if bool(get_nested(wave_probe, "summary", "waveops_reportable")):
            return RiskStatus("covered", "WaveOps is probe-covered without known runtime gaps.")
        return RiskStatus("failed", "WaveOps is advertised, but the WaveOps audit is not reportable.")

    if target == "IDXGIFactory7 RegisterAdaptersChangedEvent":
        observed = str(get_nested(dxgi_probe, "edge_cases", "RegisterAdaptersChangedEvent") or "")
        decision = str(get_nested(dxgi_probe, "edge_cases", "register_adapters_changed_decision") or "")
        if observed == "0x00000000" and decision == "safe_success_observed":
            return RiskStatus("covered", "Probe verified event registration, a nonzero cookie, initially unsignaled state, and unregister cleanup.")
        if observed == "0x80004001" and decision == "safe_rejection_observed":
            return RiskStatus("covered", "Probe verified the explicit safe rejection path.")
        return RiskStatus("failed", f"Observed `{observed}` with decision `{decision}`.")

    return RiskStatus("unknown", "No comparator rule exists for this risky stub.")


def check_risky_stubs(
    results: dict[str, dict[str, Any]], ledger: dict[str, Any], waivers: dict[str, dict[str, Any]]
) -> tuple[list[Issue], list[dict[str, Any]]]:
    issues: list[Issue] = []
    summary: list[dict[str, Any]] = []

    for entry in ledger.get("entries", []):
        target = entry["api"]
        status = risky_status(target, results)
        waiver = waivers.get(target)
        effective_status = status.status

        if status.status == "waiver_required":
            if waiver:
                effective_status = "waived"
            else:
                issues.append(
                    Issue(
                        "error",
                        "risky_stub",
                        f"Risky stub `{target}` lacks an explicit waiver",
                        status.detail,
                    )
                )
        elif status.status in {"failed", "unknown"}:
            issues.append(Issue("error", "risky_stub", f"Risky stub `{target}` is not safely covered", status.detail))

        summary.append(
            {
                "api": target,
                "state": entry.get("state"),
                "evaluation": effective_status,
                "detail": status.detail,
                "waiver": waiver,
            }
        )

    return issues, summary


def check_native_metal_coverage(contracts: list[tuple[str, dict[str, Any]]]) -> tuple[list[Issue], list[dict[str, Any]]]:
    native_entries: list[dict[str, Any]] = []
    issues: list[Issue] = []

    for contract_name, doc in contracts:
        for path, state in collect_states(doc):
            if state == "native_metal":
                native_entries.append({"contract": contract_name, "path": path, "covered": False})
                issues.append(
                    Issue(
                        "error",
                        "native_metal",
                        f"`{contract_name}:{path}` claims `native_metal` without probe coverage mapping",
                        "Add a probe coverage rule before treating this as a trusted contract claim.",
                    )
                )

    return issues, native_entries


def build_markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# D3D12 Metal Contract Summary",
        "",
        f"- Profile: `{summary['profile']}`",
        f"- Overall: `{'PASS' if summary['pass'] else 'FAIL'}`",
        f"- Required probes passing: `{summary['required_probe_passes']}/{summary['required_probe_total']}`",
        f"- Optional windowed present: `{summary['probe_summary']['optional'].get('probe-present-windowed', {}).get('pass', False)}`",
        f"- Issues: `{len(summary['issues'])}`",
        "",
        "## Required Probes",
    ]

    for probe, data in summary["probe_summary"]["required"].items():
        lines.append(f"- `{probe}`: present=`{data['present']}` pass=`{data['pass']}`")

    lines.extend(["", "## Unsupported Features"])
    for item in summary["unsupported_checks"]:
        lines.append(f"- `{item['api']}`: observed=`{item['observed']}` compliant=`{item['compliant']}`")

    lines.extend(["", "## Feature Contract Checks"])
    for item in summary["feature_checks"]:
        lines.append(f"- `{item['feature']}`: compliant=`{item['compliant']}`")

    lines.extend(["", "## Risky Stubs"])
    for item in summary["risky_stub_checks"]:
        status = item["evaluation"]
        if item.get("waiver"):
            lines.append(f"- `{item['api']}`: `{status}`")
            lines.append(f"  waiver: `{item['waiver']['id']}`")
        else:
            lines.append(f"- `{item['api']}`: `{status}`")

    if summary["native_metal_entries"]:
        lines.extend(["", "## Native Metal Coverage Gaps"])
        for item in summary["native_metal_entries"]:
            lines.append(f"- `{item['contract']}:{item['path']}`: covered=`{item['covered']}`")

    if summary["issues"]:
        lines.extend(["", "## Issues"])
        for issue in summary["issues"]:
            lines.append(f"- [{issue['severity']}] {issue['title']}: {issue['detail']}")

    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()

    results = load_probe_results(args.results_dir, args.profile)
    contracts_dir = args.contracts_dir

    feature_support = load_json(contracts_dir / "feature-support-contract.json")
    d3d12_contract = load_json(contracts_dir / "d3d12-metal-contract.json")
    unsupported = load_json(contracts_dir / "unsupported-api-ledger.json")
    risky_stub = load_json(contracts_dir / "risky-stub-ledger.json")

    waivers = load_waivers(contracts_dir, args.profile)

    issues: list[Issue] = []
    for target, waiver in waivers.items():
        issues.append(
            Issue(
                "error",
                "waiver",
                f"Active waiver `{waiver.get('id', target)}` is not allowed in the strict Phase 15 gate",
                "Remove the waiver or replace it with probe-backed contract proof.",
            )
        )

    probe_issues, probe_summary = check_required_probes(results)
    issues.extend(probe_issues)

    unsupported_issues, unsupported_summary = check_unsupported(results, unsupported)
    issues.extend(unsupported_issues)

    feature_issues, feature_summary = check_feature_contract(results, feature_support)
    issues.extend(feature_issues)

    risky_issues, risky_summary = check_risky_stubs(results, risky_stub, waivers)
    issues.extend(risky_issues)

    native_issues, native_entries = check_native_metal_coverage(
            [
                ("d3d12-metal-contract.json", d3d12_contract),
                ("feature-support-contract.json", feature_support),
            ]
    )
    issues.extend(native_issues)

    manifest_path = contracts_dir / "dxmt-runtime-manifest.json"
    manifest_issue = None
    if manifest_path.exists():
        manifest = load_json(manifest_path)
        artifacts = manifest.get("artifacts", {})
        zero_hash = "0" * 64
        placeholder = all(
            entry.get("sha256", "") == zero_hash for entry in artifacts.values()
        )
        if placeholder:
            manifest_issue = Issue(
                "error",
                "runtime_manifest",
                "DXMT runtime manifest contains placeholder hashes",
                "Regenerate manifest from a coherent DXMT build: verify-runtime-manifest.py --generate",
            )
            issues.append(manifest_issue)
    manifest_summary = {"checked": manifest_path.exists(), "placeholder": manifest_issue is not None}

    summary = {
        "schema": "metalsharp.d3d12-metal.contract-comparison.v1",
        "profile": args.profile,
        "pass": not issues,
        "required_probe_total": len(REQUIRED_PROBES),
        "required_probe_passes": sum(
            1 for probe in REQUIRED_PROBES if probe_summary["required"].get(probe, {}).get("pass")
        ),
        "probe_summary": probe_summary,
        "unsupported_checks": unsupported_summary,
        "feature_checks": feature_summary,
        "risky_stub_checks": risky_summary,
        "native_metal_entries": native_entries,
        "runtime_manifest": manifest_summary,
        "issues": [issue.__dict__ for issue in issues],
    }

    markdown = build_markdown(summary)

    markdown_out = args.markdown_out or (args.results_dir / f"contract-summary-{args.profile}.md")
    json_out = args.json_out or (args.results_dir / f"contract-summary-{args.profile}.json")
    write_text(markdown_out, markdown)
    write_text(json_out, json.dumps(summary, indent=2, sort_keys=True) + "\n")

    print(markdown_out)
    print(json_out)
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
