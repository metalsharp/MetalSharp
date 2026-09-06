#!/usr/bin/env python3
"""Generate the phase-0 full-surface inventory and gap contracts.

The inventory is intentionally conservative.  It catalogs every method from
our pinned stable Agility contract and every suspicious runtime path, but it
never promotes a method merely because its name appears in a source file.
Behavioral promotion happens in later phase gates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
SDK_DIR = ROOT_DIR / "tools" / "d3d12-metal-sdk"
CONTRACT_DIR = SDK_DIR / "contracts"
RUNTIME_ROOTS = (
    ROOT_DIR / "vendor" / "dxmt" / "src" / "airconv",
    ROOT_DIR / "vendor" / "dxmt" / "src" / "d3d12",
    ROOT_DIR / "vendor" / "dxmt" / "src" / "dxgi",
    ROOT_DIR / "vendor" / "dxmt" / "src" / "dxmt",
    ROOT_DIR / "vendor" / "dxmt" / "src" / "winemetal",
    ROOT_DIR / "vendor" / "dxmt" / "src" / "util",
)
STABLE_CONTRACT = CONTRACT_DIR / "agility-1.619.5-contract.json"


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(*args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], cwd=ROOT_DIR, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def runtime_files() -> list[Path]:
    files: list[Path] = []
    extensions = {".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm", ".metal"}
    for root in RUNTIME_ROOTS:
        if root.exists():
            files.extend(path for path in root.rglob("*") if path.is_file() and path.suffix in extensions)
    return sorted(set(files))


def source_tree_digest(files: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in files:
        relative = path.relative_to(ROOT_DIR).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(bytes.fromhex(sha256(path)))
        digest.update(b"\n")
    return digest.hexdigest()


def line_hits(text: str, method: str) -> list[int]:
    pattern = re.compile(rf"\b{re.escape(method)}\b")
    return [index for index, line in enumerate(text.splitlines(), start=1) if pattern.search(line)]


def owner_candidates(interface: str) -> list[str]:
    """Return source owners in priority order, including planned files."""
    if interface.startswith("IDXGI"):
        if "SwapChain" in interface:
            return [
                "vendor/dxmt/src/d3d12/d3d12_swapchain.cpp",
                "vendor/dxmt/src/dxgi/dxgi_swapchain.cpp",
                "vendor/dxmt/src/dxgi/dxgi_factory.cpp",
            ]
        if "Output" in interface:
            return ["vendor/dxmt/src/dxgi/dxgi_output.cpp"]
        if "Resource" in interface or "Surface" in interface:
            return ["vendor/dxmt/src/dxgi/dxgi_resource.hpp", "vendor/dxmt/src/dxgi/dxgi_resource.cpp"]
        return ["vendor/dxmt/src/dxgi/dxgi_factory.cpp", "vendor/dxmt/src/dxgi/dxgi_adapter.cpp"]
    if interface.startswith("ID3D10") or interface.startswith("ID3D11"):
        return [
            "vendor/dxmt/src/d3d11/d3d11_device.cpp",
            "vendor/dxmt/src/d3d11/d3d11_context_impl.cpp",
            "vendor/dxmt/src/d3d11/d3d11_resource_helper.cpp",
        ]
    if "GraphicsCommandList" in interface or interface == "ID3D12CommandList":
        return [
            "vendor/dxmt/src/d3d12/d3d12_command_list.cpp",
            "vendor/dxmt/src/d3d12/d3d12_command_defs.hpp",
        ]
    if "CommandQueue" in interface:
        return [
            "vendor/dxmt/src/d3d12/d3d12_command_queue.cpp",
            "vendor/dxmt/src/dxmt/dxmt_command_queue.cpp",
        ]
    if "CommandAllocator" in interface:
        return ["vendor/dxmt/src/d3d12/d3d12_command_allocator.cpp"]
    if "DescriptorHeap" in interface:
        return ["vendor/dxmt/src/d3d12/d3d12_descriptor_heap.cpp"]
    if "RootSignature" in interface:
        return ["vendor/dxmt/src/d3d12/d3d12_root_signature.cpp"]
    if "QueryHeap" in interface:
        return ["vendor/dxmt/src/d3d12/d3d12_query_heap.cpp"]
    if "Fence" in interface:
        return ["vendor/dxmt/src/d3d12/d3d12_fence.cpp"]
    if "Heap" in interface and interface.startswith("ID3D12"):
        return ["vendor/dxmt/src/d3d12/d3d12_heap.cpp"]
    if "Resource" in interface and interface.startswith("ID3D12"):
        return ["vendor/dxmt/src/d3d12/d3d12_resource.cpp"]
    if "Pipeline" in interface or "StateObject" in interface or "MetaCommand" in interface:
        return [
            "vendor/dxmt/src/d3d12/d3d12_pipeline_state.cpp",
            "vendor/dxmt/src/d3d12/d3d12_device.cpp",
        ]
    if "Shader" in interface or "Compiler" in interface or "Cache" in interface:
        return [
            "vendor/dxmt/src/d3d12/d3d12_shader_compiler.cpp",
            "vendor/dxmt/src/d3d12/d3d12_device.cpp",
        ]
    if "Video" in interface:
        return [
            "vendor/dxmt/src/d3d12/d3d12_video.cpp",
            "vendor/dxmt/src/d3d12/d3d12_device.cpp",
        ]
    if "Protected" in interface:
        return [
            "vendor/dxmt/src/d3d12/d3d12_protected.cpp",
            "vendor/dxmt/src/d3d12/d3d12_device.cpp",
        ]
    if "WorkGraph" in interface or "Node" in interface:
        return [
            "vendor/dxmt/src/d3d12/d3d12_work_graph.cpp",
            "vendor/dxmt/src/d3d12/d3d12_device.cpp",
        ]
    if "DSR" in interface:
        return [
            "vendor/dxmt/src/d3d12/d3d12_dsr.cpp",
            "vendor/dxmt/src/d3d12/d3d12_device.cpp",
        ]
    if "Debug" in interface or "Tools" in interface or "DRED" in interface:
        return ["vendor/dxmt/src/d3d12/d3d12_device.cpp"]
    if interface in {"ID3D10Blob", "ID3D11Blob"}:
        return ["vendor/dxmt/src/d3d12/d3d12_shader_compiler.cpp"]
    return ["vendor/dxmt/src/d3d12/d3d12.cpp", "vendor/dxmt/src/d3d12/d3d12_device.cpp"]


def build_source_index(files: list[Path]) -> dict[Path, str]:
    return {path: path.read_text(encoding="utf-8", errors="replace") for path in files}


def method_record(
    interface: str,
    method: str,
    details: dict[str, Any],
    source_index: dict[Path, str],
) -> dict[str, Any]:
    candidates = owner_candidates(interface)
    existing_candidates = [path for path in candidates if (ROOT_DIR / path).exists()]
    hits: list[dict[str, Any]] = []
    for path, text in source_index.items():
        lines = line_hits(text, method)
        if lines:
            hits.append(
                {
                    "path": path.relative_to(ROOT_DIR).as_posix(),
                    "lines": lines,
                }
            )
    hits.sort(key=lambda item: item["path"])
    return {
        "return_type": details.get("return_type", "unknown"),
        "difficulty": details.get("difficulty", "unknown"),
        "declared_translation": details.get("translation", ""),
        "owner": {
            "phase": "phase-0-inventory",
            "primary_source": existing_candidates[0] if existing_candidates else candidates[0],
            "candidate_sources": candidates,
            "owner_file_exists": bool(existing_candidates),
        },
        "source_references": hits,
        "inventory_status": "source_reference_found" if hits else "no_source_reference_found",
        "behavior_status": "unverified",
    }


def build_census(stable: dict[str, Any], source_index: dict[Path, str], source_files: list[Path]) -> dict[str, Any]:
    interfaces: dict[str, Any] = {}
    for interface, details in sorted(stable["data"].items()):
        methods = details.get("methods", {})
        interfaces[interface] = {
            "header": details.get("file", "unknown"),
            "guid": details.get("guid", ""),
            "parent": details.get("parent", ""),
            "method_count": len(methods),
            "methods": {
                method: method_record(interface, method, method_details, source_index)
                for method, method_details in sorted(methods.items())
            },
        }
    method_count = sum(item["method_count"] for item in interfaces.values())
    return {
        "schema": "metalsharp.d3d12-metal.interface-census.v1",
        "state": "phase0_inventory",
        "sdk": {
            "package": "Microsoft.Direct3D.D3D12",
            "version": stable.get("agility_sdk_version"),
            "d3d12_sdk_version": stable.get("d3d12_sdk_version"),
            "contract": "tools/d3d12-metal-sdk/contracts/agility-1.619.5-contract.json",
        },
        "source_identity": {
            "git_head": git_output("rev-parse", "HEAD"),
            "runtime_roots": [path.relative_to(ROOT_DIR).as_posix() for path in RUNTIME_ROOTS],
            "runtime_file_count": len(source_files),
            "runtime_tree_sha256": source_tree_digest(source_files),
        },
        "summary": {
            "interface_count": len(interfaces),
            "method_count": method_count,
            "interfaces_with_missing_owner_file": sum(
                1
                for item in interfaces.values()
                if not any(method["owner"]["owner_file_exists"] for method in item["methods"].values())
            ),
            "methods_with_no_source_reference": sum(
                1
                for item in interfaces.values()
                for method in item["methods"].values()
                if method["inventory_status"] == "no_source_reference_found"
            ),
        },
        "interfaces": interfaces,
    }


def no_op_policy() -> dict[str, Any]:
    return {
        "schema": "metalsharp.d3d12-metal.no-op-policy.v1",
        "state": "phase0_inventory",
        "scope": [
            "vendor/dxmt/src/d3d12",
            "vendor/dxmt/src/dxgi",
            "vendor/dxmt/src/dxmt",
            "vendor/dxmt/src/winemetal",
            "vendor/dxmt/src/util",
        ],
        "legal_request_forbidden_outcomes": [
            "E_NOTIMPL",
            "DXGI_ERROR_UNSUPPORTED",
            "D3D12_ERROR_UNSUPPORTED",
            "S_OK with no state change or observable side effect",
            "empty command body",
            "uninitialized output",
            "false or zero capability report that is not behavior-derived",
            "dropped command record",
        ],
        "allowed_validation_outcomes": [
            "E_INVALIDARG for malformed or out-of-domain input",
            "E_NOINTERFACE for an interface outside the declared contract",
            "documented device-removed or out-of-memory errors",
            "documented invalid feature/query result for an unknown feature ID",
        ],
        "scanner_patterns": {
            "unsupported_returns": ["E_NOTIMPL", "DXGI_ERROR_UNSUPPORTED", "D3D12_ERROR_UNSUPPORTED"],
            "success_returns": ["return S_OK", "return TRUE", "return 0"],
            "placeholder_returns": ["return false", "return nullptr", "return {}"],
            "empty_bodies": "function body containing only whitespace/comments",
        },
        "promotion_rule": "Static inventory is evidence of a gap, never evidence of support; behavior probes must clear each finding.",
    }


def provider_contract() -> dict[str, Any]:
    providers = [
        ("metal-native", "Metal 3/4 native operation", "phase1_integrated"),
        ("metal-emulation", "Metal compute/replay semantic provider", "phase1_selection_integrated"),
        ("cpu-reference", "Deterministic CPU/WARP-compatible provider", "phase1_selection_integrated"),
        ("videotoolbox-corevideo", "D3D12 video and surface provider", "selection_reserved_phase9"),
        ("display-coreanimation", "DXGI window/display/duplication provider", "bounded_phase12"),
        ("shared-mach-iosurface", "Cross-process resources/heaps/fences", "selection_reserved_phase3"),
        ("protected-platform", "Protected-memory/security provider", "selection_reserved_phase10"),
    ]
    provider_evidence = {
        "videotoolbox-corevideo": [
            "tools/d3d12-metal-sdk/probes/probe_video.cpp",
            "tools/d3d12-metal-sdk/probes/probe_video_process.cpp",
            "tools/d3d12-metal-sdk/contracts/phase9-video-coverage.json",
        ],
        "display-coreanimation": [
            "tools/d3d12-metal-sdk/probes/probe_dxgi_factory/probe_dxgi_factory.cpp",
            "vendor/dxmt/src/dxgi/dxgi_output.cpp",
            "tools/d3d12-metal-sdk/contracts/phase12-display-coverage.json",
        ],
    }
    return {
        "schema": "metalsharp.d3d12-metal.provider-contract.v1",
        "state": "phase1_architecture",
        "selection_rule": "Select by caller SDK family, operation semantics, host capability, and passing behavior evidence.",
        "no_silent_fallback": True,
        "host_capabilities": {
            "schema_version": 1,
            "implementation": "vendor/dxmt/src/dxmt/dxmt_capabilities.cpp",
            "interface": "vendor/dxmt/src/dxmt/dxmt_capabilities.hpp",
            "probe": "tools/d3d12-metal-sdk/scripts/run-source-probes.sh --caps-only",
        },
        "timeline": {
            "schema_version": 1,
            "implementation": "vendor/dxmt/src/dxmt/dxmt_timeline.hpp",
            "queue_integration": "vendor/dxmt/src/dxmt/dxmt_command_queue.cpp",
            "probe": "tools/d3d12-metal-sdk/scripts/run-provider-architecture-probe.sh",
            "status": "phase1_proven",
        },
        "resource_state": {
            "schema_version": 1,
            "implementation": "vendor/dxmt/src/d3d12/d3d12_resource_state.hpp",
            "resource_integration": "vendor/dxmt/src/d3d12/d3d12_resource.hpp",
            "replay_integration": "vendor/dxmt/src/d3d12/d3d12_command_queue.cpp",
            "probe": "tools/d3d12-metal-sdk/scripts/run-source-probes.sh --barriers-render-pass-only",
            "status": "phase1_integrated",
        },
        "phase1_evidence": [
            "tools/d3d12-metal-sdk/scripts/run-provider-architecture-probe.sh",
            "docs/roadmaps/d3d12-full-surface-phase1-provider-proof.md",
        ],
        "providers": [
            {
                "id": provider_id,
                "purpose": purpose,
                "implementation_status": status,
                "positive_evidence": provider_evidence.get(provider_id, [
                    "tools/d3d12-metal-sdk/scripts/run-provider-architecture-probe.sh",
                    "docs/roadmaps/d3d12-full-surface-phase1-provider-proof.md",
                ]),
                "negative_evidence": [],
            }
            for provider_id, purpose, status in providers
        ],
        "feature_provider_requirements": {
            "ordinary_graphics": ["metal-native", "metal-emulation", "cpu-reference"],
            "work_graphs": ["metal-emulation", "cpu-reference"],
            "video": ["videotoolbox-corevideo", "cpu-reference"],
            "display": ["display-coreanimation", "cpu-reference"],
            "sharing": ["shared-mach-iosurface"],
            "protected_resources": ["protected-platform"],
        },
    }


def full_surface_contract(census: dict[str, Any], scan: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": "metalsharp.d3d12-metal.full-surface-contract.v1",
        "state": "phase0_inventory",
        "stable_baseline": {
            "package": "Microsoft.Direct3D.D3D12",
            "version": "1.619.5",
            "d3d12_sdk_version": 619,
            "interface_contract": "tools/d3d12-metal-sdk/contracts/agility-1.619.5-contract.json",
            "preview_lane": "1.721.3-preview",
        },
        "inventory": {
            "interface_census": "tools/d3d12-metal-sdk/contracts/d3d12-interface-census.json",
            "static_scan": "docs/roadmaps/d3d12-full-surface-phase0-inventory.md",
            "no_op_policy": "tools/d3d12-metal-sdk/contracts/d3d12-no-op-policy.json",
            "phase_checkpoint_manifests": [
                "tools/d3d12-metal-sdk/contracts/phase6-graphics-coverage.json",
                "tools/d3d12-metal-sdk/contracts/phase7-mesh-workgraph-coverage.json",
                "tools/d3d12-metal-sdk/contracts/phase8-dxr-coverage.json",
                "tools/d3d12-metal-sdk/contracts/phase9-video-coverage.json",
                "tools/d3d12-metal-sdk/contracts/phase12-display-coverage.json",
                "tools/d3d12-metal-sdk/contracts/phase13-diagnostics-coverage.json",
            ],
        },
        "summary": {
            "interface_count": census["summary"]["interface_count"],
            "method_count": census["summary"]["method_count"],
            "static_finding_count": scan["summary"]["finding_count"],
            "legal_methods_require_behavior_proof": True,
            "promotion_ready": False,
        },
        "required_surfaces": [
            "core_d3d12_d3dgi_interfaces",
            "sm5x_to_sm69_dxil_and_dxbc",
            "graphics_compute_copy_and_video_queues",
            "resources_heaps_residency_and_tiled_resources_tier4",
            "enhanced_and_legacy_barriers",
            "vrs_msaa_rov_conservative_raster_and_view_instancing",
            "mesh_amplification_work_graphs_and_node_shaders",
            "dxr_1_1_and_stable_dxr_1_2_ser_omm",
            "video_toolbox_corevideo",
            "protected_resources",
            "dsr",
            "dxgi_1_6_display_duplication_and_composition",
            "agility_debug_cache_tools_and_sharing",
        ],
        "forbidden_until_behavior_proven": [
            "query_only_capability_promotion",
            "legal_e_notimpl_or_unsupported_return",
            "legal_s_ok_noop",
            "dropped_command",
            "uninitialized_output",
        ],
    }


def matrix_contract(census: dict[str, Any], scan: dict[str, Any]) -> dict[str, Any]:
    categories = [
        "inbox_no_agility_loader",
        "agility_4_legacy",
        "agility_600_legacy",
        "agility_602_through_611",
        "agility_613_through_616",
        "agility_618",
        "agility_619_stable",
        "agility_721_preview_opt_in",
        "com_all_145_interfaces_and_537_methods",
        "resources_heaps_residency_and_sharing",
        "queues_commands_barriers_and_indirect",
        "shader_models_sm5x_through_sm69",
        "graphics_raster_rov_vrs_msaa_and_formats",
        "mesh_amplification_work_graphs_and_nodes",
        "dxr_11_and_dxr_12_ser_omm",
        "video_providers",
        "protected_resources",
        "dsr",
        "dxgi_display_and_duplication",
        "cache_debug_tools_and_configuration",
        "legacy_d3d10_d3d11_regression",
    ]
    return {
        "schema": "metalsharp.d3d12-metal.full-surface-matrix.v1",
        "state": "phase0_inventory",
        "stable_sdk_version": "1.619.5",
        "preview_sdk_version": "1.721.3-preview",
        "required_categories": [
            {
                "id": category,
                "positive_probe": None,
                "negative_probe": None,
                "behavior_status": "unverified",
            }
            for category in categories
        ],
        "method_inventory": {
            "interface_count": census["summary"]["interface_count"],
            "method_count": census["summary"]["method_count"],
            "static_findings": scan["summary"]["finding_count"],
        },
        "promotion_rule": "Every row needs executable behavior, exact readback/side-effect evidence, invalid-input evidence, and source/runtime provenance.",
    }


def markdown_report(census: dict[str, Any], scan: dict[str, Any], source_files: list[Path]) -> str:
    findings_by_kind: dict[str, int] = {}
    for finding in scan["findings"]:
        kind = finding["kind"]
        findings_by_kind[kind] = findings_by_kind.get(kind, 0) + 1
    lines = [
        "# Full-Surface Phase 0 Inventory",
        "",
        "**State:** Phase 0 inventory complete; implementation and behavior gates remain open.",
        "**Stable baseline:** Microsoft DirectX Agility SDK 1.619.5 (`D3D12SDKVersion=619`)",
        "**Preview lane:** Agility SDK 1.721.3-preview (`D3D12SDKVersion=721`), opt-in only",
        f"**Generated:** {datetime.now(timezone.utc).isoformat()}",
        f"**Git HEAD:** `{census['source_identity']['git_head']}`",
        f"**Runtime source files scanned:** {len(source_files)}",
        f"**Runtime source tree SHA-256:** `{census['source_identity']['runtime_tree_sha256']}`",
        "",
        "## Interface census",
        "",
        f"- Interfaces: **{census['summary']['interface_count']}**",
        f"- Methods: **{census['summary']['method_count']}**",
        f"- Methods with no textual source reference: **{census['summary']['methods_with_no_source_reference']}**",
        "- Textual source references are inventory clues only; they do not promote behavior.",
        "",
        "## Static runtime findings",
        "",
        f"- Total findings: **{scan['summary']['finding_count']}**",
    ]
    for kind, count in sorted(findings_by_kind.items()):
        lines.append(f"- `{kind}`: **{count}**")
    lines.extend(
        [
            "",
            "These findings are intentionally not suppressed. Later phases must attach a behavior probe and clear each legal-operation finding; expected invalid-input returns remain explicitly classified.",
            "",
            "## Top findings",
            "",
        ]
    )
    for finding in scan["findings"][:120]:
        location = f"{finding['path']}:{finding['line']}"
        snippet = finding.get("snippet", "").strip().replace("|", "\\|")
        lines.append(f"- `{finding['kind']}` `{location}` — `{snippet}`")
    lines.extend(
        [
            "",
            "## Phase 0 artifacts",
            "",
            "- `contracts/d3d12-full-surface-contract.json`",
            "- `contracts/d3d12-full-surface-matrix.json`",
            "- `contracts/d3d12-provider-contract.json`",
            "- `contracts/d3d12-interface-census.json`",
            "- `contracts/d3d12-no-op-policy.json`",
            "- `contracts/d3d12-sdk-compatibility-matrix.json`",
            "- `docs/roadmaps/d3d12-full-surface-phase0-inventory.md`",
            "",
            "Phase 0 does not claim any runtime feature. It establishes the complete, versioned inventory and the evidence required before promotion.",
            "",
        ]
    )
    return "\n".join(lines)


def run_static_scan(script: Path, source_root: Path) -> dict[str, Any]:
    output = subprocess.check_output(
        ["python3", str(script), "--source-root", str(source_root), "--format", "json"],
        cwd=ROOT_DIR,
        text=True,
    )
    return json.loads(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stable-contract", type=Path, default=STABLE_CONTRACT)
    parser.add_argument("--out", type=Path, default=CONTRACT_DIR)
    parser.add_argument("--report", type=Path, default=ROOT_DIR / "docs/roadmaps/d3d12-full-surface-phase0-inventory.md")
    args = parser.parse_args()

    stable = load_json(args.stable_contract)
    source_files = runtime_files()
    source_index = build_source_index(source_files)
    census = build_census(stable, source_index, source_files)
    scanner = SDK_DIR / "scripts" / "check-noop-runtime-paths.py"
    scan = run_static_scan(scanner, ROOT_DIR)

    write_json(args.out / "d3d12-interface-census.json", census)
    write_json(args.out / "d3d12-no-op-policy.json", no_op_policy())
    write_json(args.out / "d3d12-provider-contract.json", provider_contract())
    write_json(args.out / "d3d12-full-surface-contract.json", full_surface_contract(census, scan))
    write_json(args.out / "d3d12-full-surface-matrix.json", matrix_contract(census, scan))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(markdown_report(census, scan, source_files), encoding="utf-8")

    print(
        json.dumps(
            {
                "schema": "metalsharp.d3d12-metal.phase0-inventory-result.v1",
                "interface_count": census["summary"]["interface_count"],
                "method_count": census["summary"]["method_count"],
                "static_finding_count": scan["summary"]["finding_count"],
                "runtime_tree_sha256": census["source_identity"]["runtime_tree_sha256"],
                "artifacts": [
                    str(args.out / "d3d12-interface-census.json"),
                    str(args.out / "d3d12-no-op-policy.json"),
                    str(args.out / "d3d12-provider-contract.json"),
                    str(args.out / "d3d12-full-surface-contract.json"),
                    str(args.out / "d3d12-full-surface-matrix.json"),
                    str(args.report),
                ],
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
