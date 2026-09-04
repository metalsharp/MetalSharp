#!/usr/bin/env python3
"""Validate the authoritative Phase 6 exhaustive-feasible classification.

The manifest is intentionally stricter than the historical bounded matrix: it
must classify every declared legal/invalid equivalence value, and a closed row
must carry both exact-positive and exact-negative evidence.  Runtime result
files are checked separately with --results-dir so a contract-only validation
never mistakes an absent local cache for evidence.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
DEFAULT_MANIFEST = ROOT_DIR / "tools/d3d12-metal-sdk/contracts/phase6-exhaustive-coverage.json"
ALLOWED_STATUS = {"open", "closed", "proven_no_go"}
ALLOWED_LEGALITY = {"legal", "invalid", "invalid_or_provider_boundary"}
REQUIRED_TARGET = {
    "agility_sdk": "1.619.5",
    "wine": "11.5",
    "metal_device": "Apple M4",
    "metal_language": "Metal 4",
    "xcode": "27 beta 6",
    "llvm": "15",
    "metal_shader_converter": "/nonexistent",
}


def load(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def is_string_list(value: Any) -> bool:
    return isinstance(value, list) and all(isinstance(item, str) and item for item in value)


def fail(errors: list[str], message: str) -> None:
    errors.append(message)


def validate(manifest: dict[str, Any], require_complete: bool = False) -> list[str]:
    errors: list[str] = []
    if manifest.get("schema") != "metalsharp.d3d12.phase6-exhaustive-feasible-coverage.v1":
        fail(errors, "manifest schema is not phase6 exhaustive-feasible v1")
    if manifest.get("phase") != 6:
        fail(errors, "manifest phase must be 6")
    if manifest.get("scope") != "ordinary_graphics":
        fail(errors, "manifest scope must be ordinary_graphics")
    target = manifest.get("target")
    if not isinstance(target, dict):
        fail(errors, "target identity is required")
    else:
        for key, expected in REQUIRED_TARGET.items():
            if target.get(key) != expected:
                fail(errors, f"target.{key} must be {expected!r}")

    dimensions = manifest.get("dimensions")
    dimension_map: dict[str, dict[str, list[str]]] = {}
    if not isinstance(dimensions, list) or not dimensions:
        fail(errors, "dimensions must be a non-empty list")
        dimensions = []
    for index, dimension in enumerate(dimensions):
        if not isinstance(dimension, dict):
            fail(errors, f"dimension {index} must be an object")
            continue
        identifier = dimension.get("id")
        if not isinstance(identifier, str) or not identifier:
            fail(errors, f"dimension {index} has no id")
            continue
        if identifier in dimension_map:
            fail(errors, f"duplicate dimension id: {identifier}")
            continue
        legal = dimension.get("legal_values")
        invalid = dimension.get("invalid_values")
        if not is_string_list(legal) or len(set(legal)) != len(legal):
            fail(errors, f"dimension {identifier} legal_values must be unique strings")
            legal = []
        if not is_string_list(invalid) or len(set(invalid)) != len(invalid):
            fail(errors, f"dimension {identifier} invalid_values must be unique strings")
            invalid = []
        overlap = set(legal) & set(invalid)
        if overlap:
            fail(errors, f"dimension {identifier} legal/invalid values overlap: {sorted(overlap)}")
        observed = dimension.get("observed_host_feasible", [])
        infeasible = dimension.get("observed_host_infeasible", [])
        if "host_capability_probe" in dimension:
            if not isinstance(dimension.get("host_capability_probe"), str) or not dimension.get("host_capability_probe"):
                fail(errors, f"dimension {identifier} host_capability_probe must be a non-empty string")
            if not is_string_list(observed) or not is_string_list(infeasible):
                fail(errors, f"dimension {identifier} host feasibility lists must be string lists")
            else:
                if set(observed) - set(legal) or set(infeasible) - set(legal):
                    fail(errors, f"dimension {identifier} host feasibility values must be declared legal values")
                if set(observed) & set(infeasible):
                    fail(errors, f"dimension {identifier} host feasible/infeasible values overlap")
        dimension_map[identifier] = {"legal": legal, "invalid": invalid}

    classes = manifest.get("equivalence_classes")
    if not isinstance(classes, list) or not classes:
        fail(errors, "equivalence_classes must be a non-empty list")
        classes = []
    class_ids: set[str] = set()
    legal_classes: list[dict[str, Any]] = []
    invalid_classes: list[dict[str, Any]] = []
    no_go_requirements = manifest.get("no_go_requirements", [])
    if not is_string_list(no_go_requirements):
        fail(errors, "no_go_requirements must be a string list")
        no_go_requirements = []

    for index, row in enumerate(classes):
        prefix = f"class {index}"
        if not isinstance(row, dict):
            fail(errors, f"{prefix} must be an object")
            continue
        identifier = row.get("id")
        if not isinstance(identifier, str) or not identifier:
            fail(errors, f"{prefix} has no id")
        elif identifier in class_ids:
            fail(errors, f"duplicate class id: {identifier}")
        else:
            class_ids.add(identifier)
        legality = row.get("legality")
        status = row.get("status")
        if legality not in ALLOWED_LEGALITY:
            fail(errors, f"{prefix} {identifier}: invalid legality {legality!r}")
        if status not in ALLOWED_STATUS:
            fail(errors, f"{prefix} {identifier}: invalid status {status!r}")
        if not isinstance(row.get("provider"), str) or not row.get("provider"):
            fail(errors, f"{prefix} {identifier}: provider is required")
        for field in ("positive_probe", "negative_probe"):
            if not isinstance(row.get(field), str) or not row.get(field):
                fail(errors, f"{prefix} {identifier}: {field} is required")
        expected = row.get("expected")
        if not isinstance(expected, dict):
            fail(errors, f"{prefix} {identifier}: expected object is required")
        elif expected.get("exact_required") is not True:
            fail(errors, f"{prefix} {identifier}: expected.exact_required must be true")
        provenance = row.get("provenance")
        if not isinstance(provenance, dict):
            fail(errors, f"{prefix} {identifier}: provenance object is required")
        else:
            for key in ("source_required", "runtime_required", "abi_required"):
                if provenance.get(key) is not True:
                    fail(errors, f"{prefix} {identifier}: provenance.{key} must be true")

        covers = row.get("covers")
        if not isinstance(covers, dict):
            fail(errors, f"{prefix} {identifier}: covers object is required")
        else:
            for dimension, values in covers.items():
                if dimension in ("all_dimensions", "all_invalid_values"):
                    if values not in ("all_declared_legal_values", "all_declared_invalid_values"):
                        fail(errors, f"{prefix} {identifier}: invalid {dimension} wildcard")
                    continue
                if dimension not in dimension_map:
                    fail(errors, f"{prefix} {identifier}: unknown covered dimension {dimension}")
                    continue
                if not is_string_list(values):
                    fail(errors, f"{prefix} {identifier}: {dimension} coverage must be a string list")
                    continue
                known = set(dimension_map[dimension]["legal"] + dimension_map[dimension]["invalid"])
                unknown = set(values) - known
                if unknown:
                    fail(errors, f"{prefix} {identifier}: unknown values for {dimension}: {sorted(unknown)}")

        evidence = row.get("evidence")
        if status == "closed":
            if not isinstance(evidence, dict):
                fail(errors, f"{prefix} {identifier}: closed row has no evidence object")
            else:
                if evidence.get("positive_exact") is not True:
                    fail(errors, f"{prefix} {identifier}: closed row lacks exact positive evidence")
                if evidence.get("negative_exact") is not True:
                    fail(errors, f"{prefix} {identifier}: closed row lacks exact negative evidence")
                if not isinstance(evidence.get("result_artifact"), str) or not evidence.get("result_artifact"):
                    fail(errors, f"{prefix} {identifier}: closed row lacks result_artifact")
        if status == "proven_no_go":
            no_go = row.get("no_go")
            if not isinstance(no_go, dict):
                fail(errors, f"{prefix} {identifier}: proven_no_go row lacks no_go object")
            else:
                missing = [key for key in no_go_requirements if not no_go.get(key)]
                if missing:
                    fail(errors, f"{prefix} {identifier}: no_go missing {', '.join(missing)}")
        if legality == "legal":
            legal_classes.append(row)
        else:
            invalid_classes.append(row)

    def covered(classes_to_check: list[dict[str, Any]], dimension: str, value: str, invalid: bool) -> bool:
        wildcard = "all_invalid_values" if invalid else "all_dimensions"
        wildcard_value = "all_declared_invalid_values" if invalid else "all_declared_legal_values"
        for row in classes_to_check:
            covers = row.get("covers", {})
            if covers.get(wildcard) == wildcard_value:
                return True
            values = covers.get(dimension, [])
            if isinstance(values, list) and value in values:
                return True
        return False

    # The catch-all class is an intentional classification, not a provider
    # claim.  It ensures every value is classified while work is open.
    for identifier, values in dimension_map.items():
        for value in values["legal"]:
            if not covered(legal_classes, identifier, value, False):
                fail(errors, f"unclassified legal value: {identifier}={value}")
        for value in values["invalid"]:
            if not covered(invalid_classes, identifier, value, True):
                fail(errors, f"unclassified invalid value: {identifier}={value}")

    top_status = manifest.get("status")
    if top_status not in ALLOWED_STATUS:
        fail(errors, f"manifest status is invalid: {top_status!r}")
    open_legal = [row.get("id") for row in legal_classes if row.get("status") == "open"]
    unclassified = manifest.get("completion_predicate", {}).get("unclassified_legal_combinations")
    if require_complete:
        if top_status != "closed":
            fail(errors, "--require-complete requires top-level status=closed")
        if open_legal:
            fail(errors, "open legal rows remain: " + ", ".join(str(item) for item in open_legal))
        if unclassified != 0:
            fail(errors, "completion_predicate.unclassified_legal_combinations must be zero")
    return errors


def validate_result(result: dict[str, Any], require_complete: bool = False) -> list[str]:
    errors: list[str] = []
    if require_complete:
        required = {
            "interpolation", "invalid_descriptors", "rasterization", "rov_dimensions",
            "rov_msaa_2", "rov_msaa", "rov_msaa_8", "sample_positions",
            "view_instancing", "view_instancing_msaa", "view_id_instancing", "vrs",
            "fixed_function", "inner_coverage", "conservative_msaa",
            "conservative_msaa_2", "independent_logic", "independent_logic_2",
            "independent_logic_4", "msaa", "graphics_msaa", "graphics_msaa_depth",
            "host_inventory", "device_caps",
        }
        missing = sorted(name for name in required if result.get(name) is None)
        if missing:
            errors.append("complete result is missing probes: " + ", ".join(missing))
    if result.get("schema") != "metalsharp.d3d12.phase6-exhaustive-result.v1":
        errors.append("result schema is not phase6 exhaustive result v1")
    target = result.get("target")
    if not isinstance(target, dict):
        errors.append("result target identity is required")
    else:
        for key, expected in REQUIRED_TARGET.items():
            if target.get(key) != expected:
                errors.append(f"result target.{key} must be {expected!r}")
    stage = result.get("stage")
    if not isinstance(stage, dict):
        errors.append("result stage manifest is required")
    else:
        if stage.get("ok") is not True or stage.get("failure_count") != 0:
            errors.append("result stage is not an exact successful stage")
        if stage.get("writes_outside_sandbox") is not False:
            errors.append("result stage writes_outside_sandbox must be false")
        if stage.get("unique_runtime_required") is not True:
            errors.append("result stage must require a unique runtime")
        for key in ("source_commit", "source_tree_sha256", "runtime_dir", "sandbox_root"):
            if not isinstance(stage.get(key), str) or not stage.get(key):
                errors.append(f"result stage.{key} is required")
    abi = result.get("abi")
    if not isinstance(abi, dict):
        errors.append("Winemetal ABI result is required")
    elif abi.get("ok") is not True or abi.get("failure_count") != 0:
        errors.append("Winemetal ABI result is not ok=true/failure_count=0")
    timeout = result.get("bounded_timeout_seconds")
    if not isinstance(timeout, int) or timeout <= 0 or timeout > 120:
        errors.append("bounded_timeout_seconds must be an integer in [1, 120]")

    interpolation = result.get("interpolation")
    if not isinstance(interpolation, dict):
        errors.append("interpolation result is required")
    else:
        if interpolation.get("process_status") != 0:
            errors.append("interpolation probe process did not exit zero")
        value = interpolation.get("result")
        if not isinstance(value, dict):
            errors.append("interpolation probe JSON is required")
        else:
            if value.get("exact") is not True:
                errors.append("interpolation probe is not exact=true")
            if value.get("provider") != "native_msl_stage_in_qualifiers":
                errors.append("interpolation provider identity is wrong")
            expected_names = {
                "linear": [96, 0, 0, 255],
                "noperspective": [128, 0, 0, 255],
                "centroid": [96, 0, 0, 255],
                "sample": [96, 0, 0, 255],
                "nointerpolation": [0, 0, 0, 255],
                "evaluation": [96, 96, 96, 255],
            }
            cases = value.get("cases")
            if not isinstance(cases, list) or len(cases) != len(expected_names):
                errors.append("interpolation cases must contain exactly six qualifier/evaluation rows")
            else:
                seen: set[str] = set()
                for case in cases:
                    if not isinstance(case, dict):
                        errors.append("interpolation case must be an object")
                        continue
                    name = case.get("name")
                    if name not in expected_names:
                        errors.append(f"unknown interpolation case: {name!r}")
                        continue
                    seen.add(name)
                    if case.get("pso") != "0x00000000" or case.get("execute") != "0x00000000":
                        errors.append(f"interpolation {name}: PSO/execute HRESULT is not S_OK")
                    if case.get("readback") is not True or case.get("exact") is not True:
                        errors.append(f"interpolation {name}: readback/exact invariant failed")
                    if case.get("rgba") != expected_names[name]:
                        errors.append(f"interpolation {name}: exact RGBA does not match analytic expectation")
                if seen != set(expected_names):
                    errors.append("interpolation qualifier rows are incomplete")
            invalid = value.get("invalid_shader_bytecode")
            if not isinstance(invalid, dict):
                errors.append("interpolation invalid_shader_bytecode result is required")
            elif (invalid.get("pso") != "0x80004005" or
                  invalid.get("object_null") is not True or
                  invalid.get("exact") is not True):
                errors.append("interpolation malformed DXIL must be E_FAIL with a null PSO")
    invalid_descriptors = result.get("invalid_descriptors")
    if not isinstance(invalid_descriptors, dict):
        errors.append("invalid_descriptors result is required")
    elif invalid_descriptors.get("process_status") != 0:
        errors.append("invalid descriptor probe process did not exit zero")
    else:
        value = invalid_descriptors.get("result")
        if not isinstance(value, dict) or value.get("pass") is not True:
            errors.append("invalid descriptor probe is not pass=true")
        else:
            cases = value.get("cases")
            expected_names = {
                "rasterizer2_line_mode_4",
                "view_instance_count_5",
                "view_instance_flags_2",
                "view_instance_locations_missing",
                "rasterizer_fill_mode_0",
                "rasterizer_cull_mode_0",
                "conservative_mode_2",
                "forced_sample_count_3",
                "sample_count_0",
                "sample_count_3",
                "sample_count_64",
                "primitive_topology_unknown",
                "render_target_count_9",
                "missing_vertex_shader",
                "depth_func_0",
                "depth_format_r8_unorm",
                "blend_op_0",
                "unknown_subobject_type",
            }
            if (not isinstance(cases, list) or
                {case.get("name") for case in cases if isinstance(case, dict)} != expected_names):
                errors.append("invalid descriptor cases are incomplete")
            else:
                for case in cases:
                    if (case.get("object_null") is not True or
                        case.get("exact") is not True or
                        not str(case.get("hr", "")).startswith("0x8")):
                        errors.append(f"invalid descriptor {case.get('name')}: HRESULT/null invariant failed")
    rasterization = result.get("rasterization")
    if rasterization is not None:
        if not isinstance(rasterization, dict):
            errors.append("rasterization result must be an object or null")
        elif rasterization.get("process_status") != 0 or rasterization.get("result", {}).get("pass") is not True:
            errors.append("rasterization result is not exact pass=true")
        else:
            value = rasterization["result"]
            if (value.get("point", {}).get("red_pixels") != 1 or
                value.get("line", {}).get("red_pixels") != 14 or
                value.get("line", {}).get("red_rows") != 1 or
                value.get("point", {}).get("exact_shape") is not True or
                value.get("line", {}).get("exact_shape") is not True or
                value.get("rasterizer2_shape_evidence") is not True or
                value.get("rasterizer2_invalid", {}).get("pso_hr") != "0x80070057" or
                value.get("rasterizer2_invalid", {}).get("object_null") is not True or
                value.get("rasterizer2_invalid", {}).get("exact") is not True):
                errors.append("rasterization point/line/Rasterizer2 exact matrix is incomplete")
            modes = value.get("rasterizer2")
            if (not isinstance(modes, list) or len(modes) != 4 or
                any(not isinstance(mode, dict) for mode in modes) or
                any(mode.get("pso_hr") != "0x00000000" or
                    mode.get("execute_hr") != "0x00000000" or
                    mode.get("map_hr") != "0x00000000" or
                    mode.get("red_pixels") != (28 if mode.get("mode") == 2 else 14) or
                    mode.get("red_rows") != (2 if mode.get("mode") == 2 else 1) or
                    mode.get("expected_pixels") != (28 if mode.get("mode") == 2 else 14) or
                    mode.get("expected_rows") != (2 if mode.get("mode") == 2 else 1) or
                    mode.get("exact_shape") is not True
                    for mode in modes if isinstance(mode, dict))):
                errors.append("rasterizer2 four-mode exact rows are incomplete")
            forced_samples = value.get("forced_sample_count")
            if (not isinstance(forced_samples, list) or len(forced_samples) != 2 or
                value.get("forced_sample_count_exact") is not True or
                {(case.get("forced_count"), case.get("target_count"))
                 for case in forced_samples if isinstance(case, dict)} !=
                {(1, 1), (4, 1)} or
                any(not isinstance(case, dict) or case.get("red_pixels") != 14 or
                    case.get("red_rows") != 1 or
                    case.get("pso_hr") != "0x00000000" or
                    case.get("execute_hr") != "0x00000000" or
                    case.get("map_hr") != "0x00000000" or
                    case.get("exact_shape") is not True
                    for case in forced_samples)):
                errors.append("forced sample-count exact matrix is incomplete")
            quadrilateral_strip = value.get("quadrilateral_line_strip")
            if (not isinstance(quadrilateral_strip, list) or
                len(quadrilateral_strip) != 2 or
                value.get("quadrilateral_line_strip_exact") is not True or
                any(not isinstance(case, dict) or case.get("mode") not in (2, 3) or
                    case.get("red_pixels") != (28 if case.get("mode") == 2 else 14) or
                    case.get("red_rows") != (2 if case.get("mode") == 2 else 1) or
                    case.get("pso_hr") != "0x00000000" or
                    case.get("execute_hr") != "0x00000000" or
                    case.get("map_hr") != "0x00000000" or
                    case.get("exact_shape") is not True
                    for case in quadrilateral_strip)):
                errors.append("quadrilateral line-strip matrix is incomplete")
            quadrilateral_msaa = value.get("quadrilateral_msaa")
            expected_msaa = {(2, 2): (28, 28), (2, 4): (32, 88),
                             (3, 2): (28, 28), (3, 4): (30, 58)}
            if (not isinstance(quadrilateral_msaa, list) or
                len(quadrilateral_msaa) != 4 or
                value.get("quadrilateral_msaa_exact") is not True or
                any(not isinstance(case, dict) or
                    (case.get("mode"), case.get("sample_count")) not in expected_msaa or
                    case.get("red_pixels") != 0 or case.get("red_rows") != 2 or
                    case.get("covered_pixels") != expected_msaa.get(
                        (case.get("mode"), case.get("sample_count")), (-1, -1))[0] or
                    case.get("coverage_units") != expected_msaa.get(
                        (case.get("mode"), case.get("sample_count")), (-1, -1))[1] or
                    case.get("expected_covered_pixels") != case.get("covered_pixels") or
                    case.get("expected_coverage_units") != case.get("coverage_units") or
                    case.get("pso_hr") != "0x00000000" or
                    case.get("execute_hr") != "0x00000000" or
                    case.get("map_hr") != "0x00000000" or
                    case.get("exact_shape") is not True
                    for case in quadrilateral_msaa)):
                errors.append("quadrilateral line 2x/4x matrix is incomplete")
    rov_dimensions = result.get("rov_dimensions")
    if rov_dimensions is not None:
        if not isinstance(rov_dimensions, dict):
            errors.append("rov_dimensions result must be an object or null")
        elif (rov_dimensions.get("compile_status") != 0 or
              rov_dimensions.get("process_status") != 0 or
              rov_dimensions.get("result", {}).get("exact") is not True):
            errors.append("rov_dimensions result is not exact=true")
        else:
            cases = rov_dimensions["result"].get("cases")
            expected = {"texture1d", "texture1d_array", "texture3d"}
            if not isinstance(cases, list) or {case.get("name") for case in cases if isinstance(case, dict)} != expected:
                errors.append("rov_dimensions cases are incomplete")
            else:
                for case in cases:
                    if (case.get("pso") != "0x00000000" or
                        case.get("execute") != "0x00000000" or
                        case.get("map") != "0x00000000" or
                        case.get("readback") is not True or
                        case.get("value") != 3 or
                        case.get("exact") is not True):
                        errors.append(f"rov_dimensions {case.get('name')}: exact ordered value failed")
    host_inventory = result.get("host_inventory")
    if host_inventory is not None:
        if not isinstance(host_inventory, dict):
            errors.append("host_inventory result must be an object or null")
        elif host_inventory.get("process_status") != 0:
            errors.append("native Metal interpolation inventory did not exit zero")
        else:
            value = host_inventory.get("result")
            if (not isinstance(value, dict) or
                value.get("schema") != "metalsharp.metal-interpolation.v1" or
                value.get("exact") is not True or
                value.get("library_compiled") is not True or
                value.get("qualified_stage_in") is not True or
                value.get("explicit_center_centroid_sample_offset") is not True or
                value.get("raster_order_groups_supported") is not True or
                value.get("pull_model_interpolation_supported") is not True or
                value.get("shader_barycentrics_supported") is not True or
                value.get("programmable_sample_positions_supported") is not True):
                errors.append("native Metal interpolation inventory is incomplete")
            counts = value.get("sample_counts", []) if isinstance(value, dict) else []
            if (not isinstance(counts, list) or
                any(not isinstance(item, dict) for item in counts) or
                not all(any(item.get("count") == count and item.get("supported") is True for item in counts)
                        for count in (1, 2, 4))):
                errors.append("native Metal sample-count inventory lacks required 1/2/4 support evidence")
    device_caps = result.get("device_caps")
    if device_caps is not None:
        if not isinstance(device_caps, dict):
            errors.append("device_caps result must be an object or null")
        elif device_caps.get("process_status") != 0:
            errors.append("device caps probe process did not exit zero")
        else:
            value = device_caps.get("result")
            if not isinstance(value, dict) or value.get("pass") is not True:
                errors.append("device caps probe is not pass=true")
            levels = value.get("multisample_quality_levels", []) if isinstance(value, dict) else []
            expected_counts = {1: 1, 2: 1, 4: 1, 8: 0, 16: 0, 32: 0}
            if (not isinstance(levels, list) or
                {item.get("sample_count"): item.get("num_quality_levels")
                 for item in levels if isinstance(item, dict)} != expected_counts or
                any(item.get("exact") is not True for item in levels if isinstance(item, dict))):
                errors.append("device caps sample-count quality matrix is incomplete")
            options19 = value.get("options19", {}) if isinstance(value, dict) else {}
            if (not isinstance(options19, dict) or
                options19.get("check_hr") != "0x00000000" or
                options19.get("rasterizer_desc2_supported") is not True or
                options19.get("narrow_quadrilateral_lines_supported") is not True or
                options19.get("reported") is not True):
                errors.append("device caps RasterizerDesc2 reporting is incomplete")
            unsupported = value.get("unsupported_policy", {}) if isinstance(value, dict) else {}
            if (not isinstance(unsupported, dict) or
                unsupported.get("native_msaa8_resource_hr") != "0x80070057" or
                unsupported.get("native_msaa16_resource_hr") != "0x80070057" or
                unsupported.get("native_msaa32_resource_hr") != "0x80070057" or
                unsupported.get("writable_msaa8_resource_hr") != "0x00000000" or
                unsupported.get("msaa_resource_boundary_exact") is not True):
                errors.append("device caps native/writable 8x resource boundary is incomplete")
    conservative_msaa_2 = result.get("conservative_msaa_2")
    if conservative_msaa_2 is not None:
        if not isinstance(conservative_msaa_2, dict):
            errors.append("conservative_msaa_2 result must be an object or null")
        elif conservative_msaa_2.get("process_status") != 0:
            errors.append("conservative 2x MSAA probe process did not exit zero")
        else:
            value = conservative_msaa_2.get("result")
            if (not isinstance(value, dict) or value.get("pass") is not True or
                value.get("pixels_exact") is not True or
                value.get("red_pixels") != value.get("expected_red_pixels") or
                value.get("sample_count") != 2):
                errors.append("conservative 2x MSAA exact coverage matrix is incomplete")
    sample_positions = result.get("sample_positions")
    if sample_positions is not None:
        if not isinstance(sample_positions, dict):
            errors.append("sample_positions result must be an object or null")
        elif sample_positions.get("process_status") != 0:
            errors.append("sample-position probe process did not exit zero")
        else:
            value = sample_positions.get("result")
            pattern = value.get("pattern", {}) if isinstance(value, dict) else {}
            reset = value.get("reset", {}) if isinstance(value, dict) else {}
            if (not isinstance(value, dict) or value.get("pass") is not True or
                pattern.get("sample_count") != 4 or
                pattern.get("pixel_count") != 4 or
                pattern.get("recorded") is not True or
                pattern.get("exact") is not True or
                reset.get("recorded") is not True or
                reset.get("default_quad_exact") is not True):
                errors.append("programmable sample-position pattern/reset matrix is incomplete")
    for field, samples in (("rov_msaa_2", 2), ("rov_msaa", 4),
                           ("rov_msaa_8", 8)):
        rov_msaa = result.get(field)
        if rov_msaa is None:
            continue
        if not isinstance(rov_msaa, dict):
            errors.append(f"{field} result must be an object or null")
        elif (rov_msaa.get("compile_status") != 0 or
              rov_msaa.get("process_status") != 0):
            errors.append(f"{field} shader/probe process did not complete")
        else:
            value = rov_msaa.get("result")
            expected_values = [3] * (samples * 2)
            if (not isinstance(value, dict) or value.get("pass") is not True or
                value.get("values") != expected_values or
                value.get("values_expected") != expected_values or
                value.get("values_exact") is not True or
                value.get("sample_count") != samples or
                value.get("draw_count") != 3):
                errors.append(f"{field} ordered sample matrix is incomplete")
    view_instancing_msaa = result.get("view_instancing_msaa")
    if view_instancing_msaa is not None:
        if not isinstance(view_instancing_msaa, dict):
            errors.append("view_instancing_msaa result must be an object or null")
        elif view_instancing_msaa.get("process_status") != 0:
            errors.append("view-instancing MSAA probe process did not exit zero")
        else:
            value = view_instancing_msaa.get("result")
            if (not isinstance(value, dict) or value.get("pass") is not True or
                value.get("view_count") != 4 or value.get("sample_count") != 4 or
                value.get("depth_enabled") is not True or
                value.get("zero_mask_preserved") is not True or
                not isinstance(value.get("slices"), list) or
                len(value.get("slices")) != 4 or
                any(item.get("exact") is not True for item in value.get("slices", [])
                    if isinstance(item, dict))):
                errors.append("view-instancing 4x MSAA/depth matrix is incomplete")
    view_instancing = result.get("view_instancing")
    if view_instancing is not None:
        if not isinstance(view_instancing, dict):
            errors.append("view_instancing result must be an object or null")
        elif view_instancing.get("process_status") != 0:
            errors.append("view instancing probe process did not exit zero")
        else:
            value = view_instancing.get("result")
            if not isinstance(value, dict) or value.get("pass") is not True:
                errors.append("view instancing probe is not pass=true")
            elif (value.get("view_count") != 4 or
                  value.get("sample_count") != 1 or
                  value.get("depth_enabled") is not False or
                  value.get("zero_mask_preserved") is not True or
                  value.get("shader_exact") is not True or
                  value.get("pso_exact") is not True or
                  not isinstance(value.get("slices"), list) or
                  len(value.get("slices")) != 4 or
                  any(item.get("exact") is not True for item in value.get("slices", [])
                      if isinstance(item, dict))):
                errors.append("view instancing four-view/mask-zero matrix is incomplete")
    vrs = result.get("vrs")
    if vrs is not None:
        if not isinstance(vrs, dict):
            errors.append("vrs result must be an object or null")
        elif vrs.get("process_status") != 0:
            errors.append("VRS probe process did not exit zero")
        else:
            value = vrs.get("result")
            summary = value.get("summary", {}) if isinstance(value, dict) else {}
            if (not isinstance(value, dict) or value.get("pass") is not True or
                value.get("viewport_array_index_verified") is not True or
                not isinstance(summary, dict) or
                summary.get("tier2_matrix_complete") is not True or
                summary.get("per_primitive_viewport_indexing_complete") is not True):
                errors.append("VRS Tier-2/viewport-array matrix is incomplete")
    view_id = result.get("view_id_instancing")
    if view_id is not None:
        if not isinstance(view_id, dict):
            errors.append("view_id_instancing result must be an object or null")
        elif (view_id.get("compile_status") != 0 or
              view_id.get("process_status") != 0):
            errors.append("SV_ViewID shader/probe process did not complete")
        else:
            value = view_id.get("result")
            if (not isinstance(value, dict) or value.get("ok") is not True or
                value.get("slice0_red") is not True or
                value.get("slice1_green") is not True):
                errors.append("SV_ViewID per-view exact readback is incomplete")
    for field, expected_samples in (("independent_logic", 1),
                                    ("independent_logic_2", 2),
                                    ("independent_logic_4", 4)):
        independent_logic = result.get(field)
        if independent_logic is None:
            continue
        if not isinstance(independent_logic, dict):
            errors.append(f"{field} result must be an object or null")
        elif independent_logic.get("process_status") != 0:
            errors.append(f"{field} probe process did not exit zero")
        else:
            value = independent_logic.get("result")
            targets = value.get("targets", []) if isinstance(value, dict) else []
            if (not isinstance(value, dict) or value.get("pass") is not True or
                value.get("values_exact") is not True or
                value.get("target_count") != 8 or
                value.get("sample_count") != expected_samples or
                len(targets) != 8 or
                any(target.get("exact") is not True for target in targets
                    if isinstance(target, dict))):
                errors.append(f"{field} eight-target logic-op matrix is incomplete")
    fixed_function = result.get("fixed_function")
    if fixed_function is not None:
        if not isinstance(fixed_function, dict):
            errors.append("fixed_function result must be an object or null")
        elif fixed_function.get("process_status") != 0:
            errors.append("fixed-function probe process did not exit zero")
        else:
            value = fixed_function.get("result")
            if not isinstance(value, dict) or value.get("pass") is not True:
                errors.append("fixed-function graphics matrix is not pass=true")
            coverage = value.get("coverage", {}) if isinstance(value, dict) else {}
            required = (
                "vertex_pixel", "depth_only", "color_only", "color_depth", "msaa",
                "blend", "logic_op_xor", "logic_op_independent_readback",
                "logic_op_uav_side_effect_supported", "logic_op_uav_side_effect_readback", "front_back_stencil_reference",
                "conservative_rasterization_negative_matrix_verified",
                "triangle_fan_exact", "dynamic_depth_bias_exact",
            )
            if (not isinstance(coverage, dict) or
                any(coverage.get(field) is not True for field in required) or
                coverage.get("conservative_rasterization_case_count") != 8 or
                coverage.get("conservative_rasterization_rendered_pixels") != 304 or
                coverage.get("logic_op_uav_side_effect_value") != 0x12345678 or
                coverage.get("logic_op_uav_side_effect_hr") != "0x00000000"):
                errors.append("fixed-function coverage evidence is incomplete")
    graphics_msaa_depth = result.get("graphics_msaa_depth")
    if graphics_msaa_depth is not None:
        if not isinstance(graphics_msaa_depth, dict):
            errors.append("graphics_msaa_depth result must be an object or null")
        elif graphics_msaa_depth.get("process_status") != 0:
            errors.append("graphics MSAA depth/stencil probe process did not exit zero")
        else:
            value = graphics_msaa_depth.get("result")
            cases = value.get("cases", []) if isinstance(value, dict) else []
            expected = {"2x_depth_pass", "4x_depth_fail", "4x_stencil_pass"}
            if (not isinstance(value, dict) or value.get("pass") is not True or
                {case.get("name") for case in cases if isinstance(case, dict)} != expected or
                any(case.get("exact") is not True for case in cases
                    if isinstance(case, dict))):
                errors.append("graphics MSAA depth/stencil exact matrix is incomplete")
    graphics_msaa = result.get("graphics_msaa")
    if graphics_msaa is not None:
        if not isinstance(graphics_msaa, dict):
            errors.append("graphics_msaa result must be an object or null")
        elif graphics_msaa.get("process_status") != 0:
            errors.append("graphics MSAA probe process did not exit zero")
        else:
            value = graphics_msaa.get("result")
            if not isinstance(value, dict) or value.get("pass") is not True:
                errors.append("graphics MSAA probe is not pass=true")
            samples = value.get("samples", []) if isinstance(value, dict) else []
            expected = {(2, 4294967295, False), (4, 4294967295, False),
                        (4, 5, False), (4, 4294967295, True)}
            actual = {(item.get("count"), item.get("sample_mask"),
                       item.get("alpha_to_coverage"))
                      for item in samples if isinstance(item, dict)}
            if (actual != expected or len(samples) != 4 or
                any(item.get("exact") is not True for item in samples
                    if isinstance(item, dict)) or
                value.get("quality_exact") is not True):
                errors.append("graphics MSAA sample-frequency/mask matrix is incomplete")
    inner_coverage = result.get("inner_coverage")
    if inner_coverage is not None:
        if not isinstance(inner_coverage, dict):
            errors.append("inner_coverage result must be an object or null")
        elif inner_coverage.get("process_status") != 0:
            errors.append("inner coverage probe process did not exit zero")
        else:
            value = inner_coverage.get("result")
            if (not isinstance(value, dict) or value.get("ok") is not True or
                value.get("inner_pixels") != value.get("expected_inner_pixels") or
                value.get("outer_pixels") != value.get("expected_outer_pixels") or
                value.get("unexpected_pixels") != 0):
                errors.append("SV_InnerCoverage exact conservative matrix is incomplete")
    conservative_msaa = result.get("conservative_msaa")
    if conservative_msaa is not None:
        if not isinstance(conservative_msaa, dict):
            errors.append("conservative_msaa result must be an object or null")
        elif conservative_msaa.get("process_status") != 0:
            errors.append("conservative MSAA probe process did not exit zero")
        else:
            value = conservative_msaa.get("result")
            if (not isinstance(value, dict) or value.get("pass") is not True or
                value.get("pixels_exact") is not True or
                value.get("red_pixels") != value.get("expected_red_pixels") or
                value.get("sample_count") != 4):
                errors.append("conservative MSAA exact coverage matrix is incomplete")
    msaa = result.get("msaa")
    if msaa is not None:
        if not isinstance(msaa, dict):
            errors.append("msaa result must be an object or null")
        elif msaa.get("process_status") != 0:
            errors.append("MSAA probe process did not exit zero")
        else:
            value = msaa.get("result")
            if not isinstance(value, dict) or value.get("pass") is not True:
                errors.append("MSAA probe is not pass=true")
            elif any(value.get(field) is not True for field in (
                "values_verified",
                "graphics_color_verified",
                "resolve_value_verified",
                "resolve_array_value_verified",
                "resolve_2_value_verified",
                "resolve_8_value_verified",
                "resolve_r8_value_verified",
                "programmable_sample_positions_verified",
                "sample_positions_command_recorded",
            )):
                errors.append("MSAA/sample-position exact verification fields are incomplete")
    if result.get("exact") is not True:
        errors.append("top-level result exact must be true")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--result", type=Path)
    parser.add_argument("--results-dir", type=Path)
    parser.add_argument("--profile", default="phase6-exhaustive")
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()
    if not args.manifest.exists():
        print(f"[FAIL] missing manifest: {args.manifest}")
        return 1
    try:
        manifest = load(args.manifest)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[FAIL] cannot load manifest: {exc}")
        return 1
    if not isinstance(manifest, dict):
        print("[FAIL] manifest top-level value must be an object")
        return 1
    errors = validate(manifest, args.require_complete)
    result_path = args.result
    if result_path is None and args.results_dir is not None:
        result_path = args.results_dir / f"phase6-exhaustive-{args.profile}.json"
    if result_path is not None:
        if not result_path.exists():
            errors.append(f"missing result: {result_path}")
        else:
            try:
                result = load(result_path)
            except (OSError, json.JSONDecodeError) as exc:
                errors.append(f"cannot load result: {exc}")
            else:
                if not isinstance(result, dict):
                    errors.append("result top-level value must be an object")
                else:
                    errors.extend(validate_result(result, args.require_complete))
    if errors:
        for error in errors:
            print(f"[FAIL] {error}")
        return 1
    dimensions = len(manifest["dimensions"])
    classes = len(manifest["equivalence_classes"])
    open_legal = sum(
        1 for row in manifest["equivalence_classes"]
        if row.get("legality") == "legal" and row.get("status") == "open"
    )
    suffix = " with result evidence" if result_path is not None else ""
    print(f"[PASS] phase6 exhaustive manifest: {dimensions} dimensions, {classes} equivalence classes, {open_legal} open legal classes{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
