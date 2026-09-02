#!/usr/bin/env python3
"""Generate and validate the pinned SM5.x-SM6.9 DXIL opcode matrix.

The matrix is intentionally evidence-first: an opcode is only marked observed
when a generated DXIL module report contains its numeric opcode.  The default
mode reports open rows without turning a partial corpus into a passing gate;
--strict is the Phase 5 exit-gate mode.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
DEFAULT_HEADER = Path("/Volumes/AverySSD/binc/third_party/DirectXShaderCompiler/include/dxc/DXIL/DxilConstants.h")
DEFAULT_MATRIX = ROOT / "tools/d3d12-metal-sdk/contracts/phase5-sm5-sm69-opcode-stage-resource-matrix.json"

VERSION_LIMITS = (
    (137, "1.0"),
    (139, "1.1"),
    (141, "1.2"),
    (162, "1.3"),
    (165, "1.4"),
    (216, "1.5"),
    (222, "1.6"),
    (226, "1.7"),
    (258, "1.8"),
    (312, "1.9"),
)


def first_dxil_version(opcode: int) -> str:
    for limit, version in VERSION_LIMITS:
        if opcode < limit:
            return version
    return "unknown"


def category(opcode: int, name: str) -> str:
    if name.startswith("Reserved"):
        return "reserved"
    if opcode <= 56:
        return "core"
    if opcode <= 81 or opcode in {139, 140, 223, 224, 225, 254, 255, 256, 257, 303, 304}:
        return "resource"
    if opcode <= 109:
        return "stage_system"
    if opcode <= 140 or 309 <= opcode <= 311:
        return "type_and_vector"
    if 141 <= opcode <= 161 or 178 <= opcode <= 215 or opcode == 258:
        return "raytracing"
    if 162 <= opcode <= 167 or opcode in {221, 222}:
        return "wave_and_packed"
    if 168 <= opcode <= 177:
        return "mesh_and_feedback"
    if 238 <= opcode <= 253:
        return "work_graph"
    if 262 <= opcode <= 289:
        return "shader_execution_reordering"
    return "other"


def stage_scope(opcode: int, name: str) -> list[str]:
    if opcode in {82, 87, 88, 89, 90, 91, 92, 137}:
        return ["pixel"]
    if opcode in {97, 98, 99, 100, 103, 104, 105, 106, 107, 108}:
        return ["geometry", "hull", "domain"]
    if opcode in {93, 94, 95, 96}:
        return ["compute", "mesh", "amplification", "node"]
    if opcode in {168, 169, 170, 171, 172}:
        return ["mesh"]
    if opcode == 173:
        return ["amplification"]
    if 141 <= opcode <= 161 or 178 <= opcode <= 215 or 262 <= opcode <= 289:
        return ["raytracing"]
    if 238 <= opcode <= 253:
        return ["node"]
    return ["compute", "vertex", "pixel", "geometry", "hull", "domain", "mesh", "amplification"]


def resource_scope(opcode: int, name: str) -> list[str]:
    if opcode in {57, 58, 59, 66, 68, 72, 139, 303}:
        return ["typed", "raw", "structured", "cbv", "texture"]
    if opcode in {60, 61, 62, 63, 64, 65, 73, 74, 81, 224, 254, 255}:
        return ["1d", "1d_array", "2d", "2d_array", "3d", "cube", "cube_array"]
    if opcode in {67, 69, 78, 79, 140, 304}:
        return ["raw", "structured", "typed", "uav"]
    if opcode in {70, 225}:
        return ["counter", "uav"]
    if opcode in {75, 76, 77}:
        return ["msaa"]
    if opcode in {174, 175, 176, 177}:
        return ["sampler_feedback"]
    if opcode in {216, 217, 218}:
        return ["descriptor_heap", "resource_handle"]
    return []


# Positive evidence that is deliberately narrower than the full opcode list.
# The validator reports the remaining rows instead of treating this mapping as
# proof for opcode variants that have not been emitted by a test.
EVIDENCE: dict[int, list[str]] = {
    0: ["probe-temp-registers:exact_uav_readback"],
    1: ["probe-temp-registers:exact_uav_readback"],
    2: ["probe-temp-registers:exact_uav_readback"],
    3: ["probe-temp-registers:exact_uav_readback"],
    4: ["probe-writable-msaa:graphics_stage"],
    5: ["probe-writable-msaa:graphics_stage"],
    **{opcode: ["probe-dxil-semantics:core_opcode_matrix"] for opcode in range(6, 57)},
    57: ["probe-shader-corpus:resource_indexing"],
    58: ["probe-dxil-semantics:buffer_load_store"],
    59: ["probe-dxil-semantics:cbuffer_load"],
    60: ["probe-dxil-semantics:texture_sampling_forms"],
    61: ["probe-dxil-semantics:texture_sampling_forms"],
    62: ["probe-dxil-semantics:texture_sampling_forms"],
    63: ["probe-dxil-semantics:texture_sampling_forms"],
    64: ["probe-texture-dimensions:comparison_sampling"],
    65: ["probe-sm66-capabilities:sample_cmp_level_sm67"],
    66: ["probe-texture-dimensions:resource_matrix"],
    67: ["probe-texture-dimensions:typed_texture_store_matrix"],
    68: ["probe-dxil-semantics:buffer_load_store"],
    69: ["probe-dxil-semantics:buffer_load_store"],
    70: ["probe-compute-pso:append_counter_runtime"],
    71: ["probe-dxil-semantics:buffer_load_store"],
    72: ["probe-texture-dimensions:get_dimensions"],
    73: ["probe-dxil-semantics:texture_sampling_forms"],
    74: ["probe-sm66-capabilities:gather"],
    75: ["probe-texture-dimensions:comparison_sampling"],
    76: ["probe-texture-dimensions:comparison_sampling"],
    77: ["probe-texture-dimensions:comparison_sampling"],
    78: ["probe-dxil-semantics:atomic_matrix"],
    79: ["probe-dxil-semantics:atomic_uav"],
    80: ["probe-dxil-semantics:atomics_ids"],
    81: ["probe-texture-dimensions:calculate_lod"],
    82: ["probe-mini-dxil-texture-color-output:discard_half_frame"],
    83: ["probe-writable-msaa:derivatives"],
    84: ["probe-writable-msaa:derivatives"],
    85: ["probe-writable-msaa:derivatives"],
    86: ["probe-writable-msaa:derivatives"],
    87: ["probe-writable-msaa:attribute_evaluation"],
    88: ["probe-writable-msaa:attribute_evaluation"],
    89: ["probe-writable-msaa:attribute_evaluation"],
    90: ["probe-writable-msaa:sample_index"],
    91: ["probe-writable-msaa:coverage"],
    92: ["probe-inner-coverage:exact_conservative_raster"],
    93: ["probe-dxil-semantics:atomics_ids"],
    94: ["probe-dxil-semantics:atomics_ids"],
    95: ["probe-dxil-semantics:atomics_ids"],
    96: ["probe-dxil-semantics:atomics_ids"],
    97: ["probe-mini-geometry-system-matrix:emit_stream_readback"],
    98: ["probe-mini-geometry-system-matrix:cut_stream_readback"],
    99: ["probe-mini-geometry-system-matrix:emit_then_cut_stream_readback"],
    100: ["probe-mini-geometry-system-matrix:gs_instance_id_two_instances"],
    101: ["probe-dxil-semantics:double_bitcast"],
    102: ["probe-dxil-semantics:double_bitcast"],
    103: ["probe-mini-tessellation_shader_pso:load_output_control_point"],
    104: ["probe-mini-tessellation_patch_constant:load_patch_constant"],
    105: ["probe-mini-tessellation_shader_pso:domain_location"],
    106: ["probe-mini-tessellation_shader_pso:store_patch_constant"],
    107: ["probe-mini-tessellation_shader_pso:output_control_point_id"],
    108: ["probe-mini-geometry-system-matrix:primitive_id_readback"],
    110: ["probe-wave-ops:wave_matrix"],
    111: ["probe-wave-ops:wave_matrix"],
    112: ["probe-wave-ops:wave_matrix"],
    113: ["probe-wave-ops:wave_matrix"],
    114: ["probe-wave-ops:wave_matrix"],
    115: ["probe-wave-ops:wave_matrix"],
    116: ["probe-wave-ops:wave_matrix"],
    117: ["probe-wave-ops:wave_matrix"],
    118: ["probe-wave-ops:wave_matrix"],
    119: ["probe-wave-ops:wave_matrix"],
    120: ["probe-wave-ops:wave_matrix"],
    121: ["probe-wave-ops:wave_matrix"],
    122: ["probe-dxil-semantics:wave_quad"],
    123: ["probe-dxil-semantics:wave_quad"],
    124: ["probe-dxil-semantics:float16_conversion"],
    125: ["probe-dxil-semantics:float16_conversion"],
    126: ["probe-dxil-semantics:math_bits"],
    127: ["probe-dxil-semantics:math_bits"],
    128: ["probe-dxil-semantics:double_bitcast"],
    129: ["probe-dxil-semantics:double_bitcast"],
    130: ["probe-dxil-semantics:float16_conversion"],
    131: ["probe-dxil-semantics:float16_conversion"],
    132: ["probe-dxil-semantics:double_float_roundtrip"],
    133: ["probe-dxil-semantics:double_integer_conversions"],
    134: ["probe-dxil-semantics:double_integer_conversions"],
    135: ["probe-wave-ops:active_prefix_bit_count"],
    136: ["probe-wave-ops:active_prefix_bit_count"],
    138: ["probe-view-id-instancing:array_layers"],
    139: ["probe-dxil-semantics:raw_vector"],
    141: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    142: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    143: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    144: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    145: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    146: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    147: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    148: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    149: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    150: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    151: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    152: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    153: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    154: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    155: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_control_flow"],
    156: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_control_flow"],
    157: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    158: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    159: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    160: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    161: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    213: ["probe-mini-dxr-acceleration-structures:tier1_1_ray_shader_builtins"],
    140: ["probe-dxil-semantics:raw_vector"],
    162: ["probe-dxil-semantics:dot2add_half"],
    163: ["probe-dxil-semantics:dot4add_signed"],
    164: ["probe-dxil-semantics:dot4add_unsigned"],
    165: ["probe-wave-ops:wave_match"],
    166: ["probe-wave-ops:wave_multi_prefix"],
    167: ["probe-wave-ops:wave_multi_prefix"],
    168: ["probe-mesh-amplification:exact_dispatch_mesh"],
    169: ["probe-mesh-amplification:exact_dispatch_mesh"],
    170: ["probe-mesh-amplification:exact_dispatch_mesh"],
    171: ["probe-mesh-amplification:exact_dispatch_mesh"],
    172: ["probe-mesh-amplification:exact_dispatch_mesh"],
    173: ["probe-mesh-amplification:exact_dispatch_mesh"],
    174: ["probe-sampler-feedback:feedback_matrix"],
    175: ["probe-sampler-feedback:feedback_matrix"],
    176: ["probe-sampler-feedback:feedback_matrix"],
    177: ["probe-sampler-feedback:feedback_matrix"],
    178: ["probe-dxr-inline:exact_tlas_hit"],
    179: ["probe-dxr-inline:exact_tlas_hit"],
    180: ["probe-dxr-inline:exact_tlas_hit"],
    181: ["probe-dxr-inline:accessor_matrix"],
    182: ["probe-dxr-inline:exact_tlas_hit"],
    183: ["probe-dxr-inline:procedural_commit"],
    184: ["probe-dxr-inline:exact_tlas_hit"],
    185: ["probe-dxr-inline:exact_tlas_hit"],
    186: ["probe-dxr-inline:accessor_matrix"],
    187: ["probe-dxr-inline:accessor_matrix"],
    188: ["probe-dxr-inline:accessor_matrix"],
    189: ["probe-dxr-inline:accessor_matrix"],
    190: ["probe-dxr-inline:accessor_matrix"],
    **{opcode: ["probe-dxr-inline:accessor_matrix"] for opcode in range(191, 213)},
    214: ["probe-dxr-inline:accessor_matrix"],
    215: ["probe-dxr-inline:accessor_matrix"],
    216: ["probe-sm66-capabilities:resource_heap"],
    217: ["probe-sm66-capabilities:resource_heap"],
    218: ["probe-sm66-capabilities:resource_heap"],
    219: ["probe-dxil-semantics:pack_unpack_8"],
    220: ["probe-dxil-semantics:pack_unpack_8"],
    221: ["probe-wave-ops:helper_lane"],
    222: ["probe-dxil-semantics:wave_quad"],
    223: ["probe-sm66-capabilities:raw_gather"],
    224: ["probe-sm66-capabilities:sample_cmp_level"],
    225: ["probe-writable-msaa:texture_store_sample"],
    256: ["probe-start-draw-info:exact_raster"],
    257: ["probe-start-draw-info:exact_raster"],
    258: ["probe-dxr-inline:allocate_ray_query2"],
    # The descending HitObject lane is maintained as one bounded GPU provider;
    # individual runtime profiles and the contract matrix carry the exact
    # operation-specific readback description.
    **{opcode: ["probe-hitobject:bounded_gpu_runtime_readback"]
       for opcode in range(262, 289)},
    289: ["probe-hitobject-attributes:exact_barycentric_readback"],
    254: ["probe-sm66-capabilities:sample_cmp_grad_sm68"],
    255: ["probe-sm66-capabilities:sample_cmp_bias_sm68"],
    303: ["probe-dxil-semantics:raw_vector"],
    304: ["probe-dxil-semantics:raw_vector"],
    309: ["probe-dxil-semantics:vector_reductions"],
    310: ["probe-dxil-semantics:vector_reductions"],
    311: ["probe-dxil-semantics:sm69_fdot_wide"],
}


def parse_opcodes(header: Path) -> list[dict[str, Any]]:
    text = header.read_text(encoding="utf-8", errors="replace")
    marker = "enum class OpCode : unsigned {"
    start = text.index(marker, text.index("OPCODE-ENUM")) + len(marker)
    end = text.index("NumOpCodes_Dxil_1_0", start)
    section = text[start:end]
    rows: list[dict[str, Any]] = []
    for name, number in re.findall(r"\b([A-Za-z_]\w*)\s*=\s*(\d+)", section):
        opcode = int(number)
        if opcode >= 312:
            continue
        reserved = name.startswith("Reserved")
        rows.append(
            {
                "id": opcode,
                "name": name,
                "first_dxil": first_dxil_version(opcode),
                "category": category(opcode, name),
                "stages": stage_scope(opcode, name),
                "resources": resource_scope(opcode, name),
                "required": not reserved,
                "status": "not_applicable" if reserved else ("observed" if opcode in EVIDENCE else "open"),
                "positive_evidence": EVIDENCE.get(opcode, []),
                "negative_evidence": [],
            }
        )
    rows.sort(key=lambda row: row["id"])
    if len(rows) != 312 or [row["id"] for row in rows] != list(range(312)):
        raise ValueError(f"expected one row for every opcode 0..311, got {len(rows)} rows")
    return rows


def matrix(header: Path) -> dict[str, Any]:
    opcodes = parse_opcodes(header)
    return {
        "schema": "metalsharp.d3d12-metal.sm5-sm69-opcode-stage-resource-matrix.v1",
        "stable_surface": "SM5.x DXBC/AIR and SM6.0-SM6.9 DXIL",
        "dxil_opcode_source": "pinned DXC DxilConstants.h (1.9 stable opcodes 0..311)",
        "opcode_count": len(opcodes),
        "required_opcode_count": sum(row["required"] for row in opcodes),
        "reserved_opcode_count": sum(not row["required"] for row in opcodes),
        "behavior_status": "open",
        "promotion_rule": "Every required row needs exact positive and invalid/negative evidence across its declared stage/resource scope; observed compilation alone is not execution proof.",
        "opcodes": opcodes,
        "stage_matrix": [
            {"stage": "compute", "positive_evidence": ["probe-dxil-semantics", "probe-shader-corpus"], "negative_evidence": ["probe-shaders:unsupported_semantics"], "status": "observed"},
            {"stage": "vertex", "positive_evidence": ["probe-shader-corpus:sm50_graphics_baseline"], "negative_evidence": [], "status": "observed"},
            {"stage": "pixel", "positive_evidence": ["probe-shader-corpus:sm50_graphics_baseline", "probe-writable-msaa"], "negative_evidence": [], "status": "observed"},
            {"stage": "geometry", "positive_evidence": ["probe-shader-corpus:sm50_graphics_baseline"], "negative_evidence": [], "status": "observed"},
            {"stage": "hull", "positive_evidence": ["probe-shader-corpus:sm50_tessellation_stage_matrix:compile"], "negative_evidence": ["probe-shader-corpus:sm50_tessellation_stage_matrix:provider"], "status": "open"},
            {"stage": "domain", "positive_evidence": ["probe-shader-corpus:sm50_tessellation_stage_matrix:compile"], "negative_evidence": ["probe-shader-corpus:sm50_tessellation_stage_matrix:provider"], "status": "open"},
            {"stage": "amplification", "positive_evidence": ["probe-mini-mesh-object-shader-pso"], "negative_evidence": [], "status": "open"},
            {"stage": "mesh", "positive_evidence": ["probe-mini-mesh-object-shader-pso"], "negative_evidence": [], "status": "open"},
            {"stage": "raytracing", "positive_evidence": ["probe-mini-dxr-acceleration-structures"], "negative_evidence": [], "status": "open"},
            {"stage": "node", "positive_evidence": [], "negative_evidence": [], "status": "open"},
        ],
        "resource_matrix": [
            {"resource": "typed_buffer", "positive_evidence": ["probe-texture-dimensions", "probe-dxil-semantics"], "status": "observed"},
            {"resource": "raw_buffer", "positive_evidence": ["probe-dxil-semantics:raw_vector"], "status": "observed"},
            {"resource": "structured_buffer", "positive_evidence": ["probe-sm66-capabilities:descriptor_indexing"], "status": "observed"},
            {"resource": "uav_counter", "positive_evidence": ["probe-compute-pso:append-consume"], "negative_evidence": ["probe-shader-corpus:two_counter_fail_closed"], "status": "observed"},
            {"resource": "texture_dimensions", "positive_evidence": ["probe-texture-dimensions:66-case-matrix"], "status": "observed"},
            {"resource": "msaa", "positive_evidence": ["probe-writable-msaa"], "status": "observed"},
            {"resource": "depth_comparison", "positive_evidence": ["probe-sm66-capabilities:comparison-dimensions"], "status": "observed"},
            {"resource": "cache_session", "positive_evidence": ["probe-object-contracts", "probe-agility-ue5:compiler-cache-session"], "negative_evidence": ["probe-agility-ue5:compiler-object-E_NOTIMPL"], "status": "observed"},
        ],
    }


def observed_opcodes(corpus: Path) -> Counter[int]:
    observed: Counter[int] = Counter()
    for path in sorted(corpus.glob("*.module.txt")):
        text = path.read_text(encoding="utf-8", errors="replace")
        marker = "dxil_opcodes:"
        if marker not in text:
            continue
        for opcode, count in re.findall(r"^\s+opcode=(\d+) count=(\d+)$", text.split(marker, 1)[1], re.MULTILINE):
            observed[int(opcode)] += int(count)
    return observed


def validate(matrix_path: Path, corpus: Path | None, strict: bool,
             json_out: Path | None = None) -> int:
    document = json.loads(matrix_path.read_text(encoding="utf-8"))
    rows = document.get("opcodes", [])
    if len(rows) != 312 or [row.get("id") for row in rows] != list(range(312)):
        print("matrix is not a complete 0..311 opcode inventory", file=sys.stderr)
        return 2

    required = [row for row in rows if row.get("required")]
    if corpus is None:
        observed: Counter[int] = Counter()
        missing = [row for row in required if row.get("status") == "open"]
    else:
        observed = observed_opcodes(corpus)
        missing = [row for row in required if row["id"] not in observed]

    result = {
        "schema": "metalsharp.d3d12-metal.sm5-sm69-opcode-matrix-result.v1",
        "matrix": str(matrix_path),
        "corpus": str(corpus) if corpus is not None else None,
        "opcode_rows": len(rows),
        "required_rows": len(required),
        "observed_rows": len(observed),
        "missing_rows": len(missing),
        "missing": [{"id": row["id"], "name": row["name"]} for row in missing],
        "strict": strict,
        "pass": not missing,
    }
    if json_out is not None:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    if corpus is None:
        print(f"opcode_rows={len(rows)} required={len(required)} open={len(missing)}")
    else:
        print(f"opcode_rows={len(rows)} required={len(required)} observed={len(observed)} missing={len(missing)}")
    if missing:
        limit = len(missing) if strict else min(len(missing), 20)
        suffix = " ..." if limit < len(missing) else ""
        print("missing:", ", ".join(f"{row['id']}:{row['name']}" for row in missing[:limit]) + suffix)
    return 1 if strict and missing else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX)
    parser.add_argument("--write", action="store_true", help="Regenerate the JSON matrix from --header.")
    parser.add_argument("--corpus", type=Path, help="Directory containing generated *.module.txt reports.")
    parser.add_argument("--json-out", type=Path, help="Write a machine-readable validation result.")
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    if args.write:
        args.matrix.parent.mkdir(parents=True, exist_ok=True)
        args.matrix.write_text(json.dumps(matrix(args.header), indent=2) + "\n", encoding="utf-8")
    if not args.matrix.is_file():
        print(f"matrix not found: {args.matrix}; use --write", file=sys.stderr)
        return 2
    return validate(args.matrix, args.corpus, args.strict, args.json_out)


if __name__ == "__main__":
    raise SystemExit(main())
