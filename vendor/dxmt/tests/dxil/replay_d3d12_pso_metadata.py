#!/usr/bin/env python3
"""Replay D3D12 PSO vertex metadata against cached DXIL/MSL sidecars.

This is intentionally file-only: it reads captured pso-render-*.json files,
shader sidecars, and optionally a generated MSL dump directory. It does not
launch Wine, Steam, MetalSharp, or a game process.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


ATTR_CALL_RE = re.compile(r"vkd3d_load_vertex_attr\(([^)]*)\)")
HEX_MASK_RE = re.compile(r"^0x[0-9a-fA-F]+$")


def load_json(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text())
    except Exception:
        return None
    return data if isinstance(data, dict) else None


def parse_slot_mask(value: Any) -> int | None:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        try:
            if HEX_MASK_RE.match(text):
                return int(text, 16)
            return int(text, 10)
        except ValueError:
            return None
    return None


def int_value(value: Any, fallback: int = 0) -> int:
    if value is None:
        return fallback
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def compact_table_index(slot_mask: int, slot: int) -> int | None:
    if slot < 0 or slot >= 32 or not (slot_mask & (1 << slot)):
        return None
    lower_slots = slot_mask & ((1 << slot) - 1)
    return lower_slots.bit_count()


def shader_sidecar(cache: Path, msl_dir: Path | None, shader_hash: str, suffixes: list[str]) -> Path | None:
    roots = []
    if msl_dir is not None:
        roots.append(msl_dir)
    roots.append(cache)
    for root in roots:
        for suffix in suffixes:
            path = root / f"{shader_hash}{suffix}"
            if path.exists():
                return path
    return None


def read_text(path: Path | None) -> str:
    if path is None:
        return ""
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def parse_attr_calls(msl: str) -> list[list[str]]:
    calls: list[list[str]] = []
    for line in msl.splitlines():
        if "static inline" in line and "vkd3d_load_vertex_attr" in line:
            continue
        for match in ATTR_CALL_RE.finditer(line):
            calls.append([part.strip() for part in match.group(1).split(",")])
    return calls


def normalize_input_elements(pipeline: dict[str, Any]) -> tuple[list[dict[str, Any]], str]:
    input_layout = pipeline.get("input_layout")
    if isinstance(input_layout, dict) and isinstance(input_layout.get("elements"), list):
        return [element for element in input_layout["elements"] if isinstance(element, dict)], "captured"
    return [], "missing"


def expected_call_prefix(element: dict[str, Any]) -> str:
    per_instance = 1 if str(element.get("class", "")) == "per_instance" or int_value(element.get("input_slot_class"), 0) == 1 else 0
    table_index = int_value(element.get("table_index"), 0)
    return (
        f"vkd3d_load_vertex_attr({table_index}, "
        f"{int_value(element.get('offset'), 0)}, "
        f"{int_value(element.get('dxgi_format'), 0)}, "
        f"{per_instance}, "
        f"{int_value(element.get('step_rate'), 1)}, vid, iid, buf16, "
        f"buf{table_index}, buf29, buf30)"
    )


def validate_metadata_order(
    manifest: Path,
    pipeline_name: str,
    elements: list[dict[str, Any]],
    input_layout: dict[str, Any],
) -> list[dict[str, str]]:
    failures: list[dict[str, str]] = []
    slot_mask = parse_slot_mask(input_layout.get("slot_mask"))
    if slot_mask is None:
        slot_mask = 0
        for element in elements:
            if element.get("system_value"):
                continue
            slot = int_value(element.get("slot"), -1)
            if 0 <= slot < 32:
                slot_mask |= 1 << slot

    for element in elements:
        if element.get("system_value"):
            continue
        mode = str(element.get("table_indexing_mode", "compact_by_slot_mask"))
        slot = int_value(element.get("slot"), -1)
        table_index = int_value(element.get("table_index"), -1)
        if mode != "compact_by_slot_mask":
            failures.append(
                {
                    "manifest": str(manifest),
                    "pipeline": pipeline_name,
                    "kind": "unsupported-table-indexing-mode",
                    "detail": f"slot={slot} mode={mode}",
                }
            )
            continue
        expected = compact_table_index(slot_mask, slot)
        if expected is None or table_index != expected:
            failures.append(
                {
                    "manifest": str(manifest),
                    "pipeline": pipeline_name,
                    "kind": "table-index-mismatch",
                    "detail": f"slot={slot} expected={expected} actual={table_index} slot_mask=0x{slot_mask:08x}",
                }
            )
    return failures


def audit_pipeline(
    manifest: Path,
    pipeline: dict[str, Any],
    cache: Path,
    msl_dir: Path | None,
    require_captured_input_layout: bool,
) -> tuple[dict[str, Any], list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    failures: list[dict[str, str]] = []
    warnings: list[dict[str, str]] = []
    access_issues: list[dict[str, str]] = []

    name = str(pipeline.get("name") or manifest.stem)
    d3d12 = pipeline.get("d3d12") if isinstance(pipeline.get("d3d12"), dict) else {}
    vs_hash = str(d3d12.get("vs_hash") or pipeline.get("vertex", {}).get("hash") or "")
    input_count = int(d3d12.get("input_elements") or 0)

    def add(target: list[dict[str, str]], kind: str, detail: str) -> None:
        target.append({"manifest": str(manifest), "pipeline": name, "kind": kind, "detail": detail})

    dxbc_path = shader_sidecar(cache, None, vs_hash, [".dxbc"]) if vs_hash else None
    module_path = shader_sidecar(cache, None, vs_hash, [".module.txt"]) if vs_hash else None
    msl_path = shader_sidecar(cache, msl_dir, vs_hash, [".metal", ".msl"]) if vs_hash else None

    if not vs_hash:
        add(failures, "missing-vs-hash", "render PSO has no vertex shader hash")
    if vs_hash and dxbc_path is None:
        add(failures, "missing-dxbc", f"vs_hash={vs_hash}")
    if vs_hash and module_path is None:
        add(failures, "missing-module", f"vs_hash={vs_hash}")
    if vs_hash and msl_path is None:
        add(failures, "missing-msl", f"vs_hash={vs_hash}")

    module_text = read_text(module_path)
    msl_text = read_text(msl_path)
    attr_calls = parse_attr_calls(msl_text)
    has_vertex_id = "[[vertex_id]]" in msl_text
    has_vid_assignment = re.search(r"=\s*vid\b", msl_text) is not None
    has_load_input = "dx.op.loadInput" in module_text
    has_float_load_input = "dx.op.loadInput.f32" in module_text
    has_raw_buffer_load = "dx.op.rawBufferLoad" in module_text

    input_layout = pipeline.get("input_layout") if isinstance(pipeline.get("input_layout"), dict) else {}
    elements, metadata_source = normalize_input_elements(pipeline)
    if input_count > 0 and metadata_source == "missing":
        add(
            access_issues if not require_captured_input_layout else failures,
            "missing-captured-input-layout",
            "captured PSO predates input_layout.elements; rerun only offline capture/dump path before requiring exact PSO metadata",
        )

    if elements:
        failures.extend(validate_metadata_order(manifest, name, elements, input_layout))
        call_text = "\n".join(f"vkd3d_load_vertex_attr({', '.join(call)})" for call in attr_calls)
        for element in elements:
            if element.get("system_value"):
                continue
            expected = expected_call_prefix(element)
            if expected not in call_text:
                add(
                    failures,
                    "missing-metadata-aware-msl-call",
                    f"expected `{expected}`",
                )

    if input_count == 0 and has_load_input and not has_float_load_input and not attr_calls:
        if not has_vertex_id or not has_vid_assignment:
            if has_raw_buffer_load:
                add(warnings, "argumentless-loadinput-with-raw-buffer-shader", f"vs_hash={vs_hash}")
            else:
                add(failures, "fullscreen-loadinput-not-vertex-id", f"vs_hash={vs_hash}")

    if input_count > 0 and not attr_calls:
        add(warnings, "input-layout-without-msl-pull", f"input_elements={input_count}")
    if attr_calls and input_count == 0:
        add(warnings, "msl-pull-without-input-layout-count", f"calls={len(attr_calls)}")

    row = {
        "manifest": str(manifest),
        "pipeline": name,
        "vs_hash": vs_hash,
        "input_elements": input_count,
        "metadata_source": metadata_source,
        "metadata_elements": len(elements),
        "dxbc": str(dxbc_path) if dxbc_path else "",
        "module": str(module_path) if module_path else "",
        "msl": str(msl_path) if msl_path else "",
        "load_input": has_load_input,
        "float_load_input": has_float_load_input,
        "raw_buffer_load": has_raw_buffer_load,
        "vertex_id": has_vertex_id,
        "vid_assignment": has_vid_assignment,
        "vertex_pull_calls": len(attr_calls),
    }
    return row, failures, warnings, access_issues


def count_by_kind(items: list[dict[str, str]]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for item in items:
        kind = item.get("kind", "unknown")
        counts[kind] = counts.get(kind, 0) + 1
    return dict(sorted(counts.items()))


def build_replayability(
    rows: list[dict[str, Any]], access_issues: list[dict[str, str]]
) -> dict[str, Any]:
    input_layout_pso_count = sum(1 for row in rows if row["input_elements"] > 0)
    captured_input_layout_pso_count = sum(
        1 for row in rows if row["input_elements"] > 0 and row["metadata_source"] == "captured"
    )
    missing_input_layout_pso_count = sum(
        1 for row in rows if row["input_elements"] > 0 and row["metadata_source"] == "missing"
    )
    legacy_capture_count = sum(
        1 for issue in access_issues if issue.get("kind") == "missing-captured-input-layout"
    )

    if input_layout_pso_count == 0:
        vertex_status = "not-applicable"
        recommendation = "Cache has no render PSOs with D3D12 input-layout elements."
    elif missing_input_layout_pso_count == 0:
        vertex_status = "complete"
        recommendation = "Cache can prove D3D12 vertex/PSO metadata ordering offline."
    elif captured_input_layout_pso_count == 0:
        vertex_status = "legacy-cache"
        recommendation = (
            "Cache predates captured input_layout.elements for all input-layout PSOs; "
            "use this for shader sidecar checks only, or recapture PSO metadata before "
            "requiring exact vertex replay proof."
        )
    else:
        vertex_status = "partial"
        recommendation = (
            "Cache has mixed PSO metadata coverage; captured PSOs can prove vertex "
            "metadata ordering, older PSOs need an offline recapture before strict replay."
        )

    return {
        "vertex_metadata_status": vertex_status,
        "input_layout_pso_count": input_layout_pso_count,
        "captured_input_layout_pso_count": captured_input_layout_pso_count,
        "missing_input_layout_pso_count": missing_input_layout_pso_count,
        "legacy_capture_count": legacy_capture_count,
        "can_strictly_replay_vertex_metadata": input_layout_pso_count > 0
        and missing_input_layout_pso_count == 0,
        "needs_offline_recapture_for_strict_vertex_replay": missing_input_layout_pso_count > 0,
        "recommendation": recommendation,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, help="Directory containing pso-render JSON and shader sidecars.")
    parser.add_argument("--msl-dir", help="Optional generated MSL dump directory to prefer over cached .msl files.")
    parser.add_argument("--output", help="Write JSON report to this path.")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--require-captured-input-layout", action="store_true")
    args = parser.parse_args()

    cache = Path(args.cache)
    msl_dir = Path(args.msl_dir) if args.msl_dir else None
    manifests = sorted(cache.glob("pso-render-*.json"))
    if args.limit > 0:
        manifests = manifests[: args.limit]

    rows: list[dict[str, Any]] = []
    failures: list[dict[str, str]] = []
    warnings: list[dict[str, str]] = []
    access_issues: list[dict[str, str]] = []

    for manifest in manifests:
        payload = load_json(manifest)
        if not payload or not isinstance(payload.get("pipelines"), list):
            failures.append({"manifest": str(manifest), "pipeline": "", "kind": "invalid-manifest", "detail": "not a PSO manifest"})
            continue
        for pipeline in payload["pipelines"]:
            if not isinstance(pipeline, dict) or pipeline.get("type") != "render":
                continue
            row, row_failures, row_warnings, row_access = audit_pipeline(
                manifest,
                pipeline,
                cache,
                msl_dir,
                args.require_captured_input_layout,
            )
            rows.append(row)
            failures.extend(row_failures)
            warnings.extend(row_warnings)
            access_issues.extend(row_access)

    replayability = build_replayability(rows, access_issues)
    report = {
        "schema": "metalsharp.d3d12-metal.offline-pso-shader-replay.v1",
        "cache": str(cache),
        "msl_dir": str(msl_dir) if msl_dir else "",
        "status": "complete" if not access_issues else "partial",
        "ok": not failures,
        "render_pso_count": len(rows),
        "failure_count": len(failures),
        "warning_count": len(warnings),
        "access_issue_count": len(access_issues),
        "metadata_captured_count": sum(1 for row in rows if row["metadata_source"] == "captured"),
        "metadata_missing_count": sum(1 for row in rows if row["metadata_source"] == "missing" and row["input_elements"] > 0),
        "vertex_pull_pso_count": sum(1 for row in rows if row["vertex_pull_calls"] > 0),
        "fullscreen_vertex_id_count": sum(1 for row in rows if row["input_elements"] == 0 and row["vid_assignment"]),
        "failure_kinds": count_by_kind(failures),
        "warning_kinds": count_by_kind(warnings),
        "access_issue_kinds": count_by_kind(access_issues),
        "replayability": replayability,
        "failures": failures,
        "warnings": warnings,
        "access_issues": access_issues,
        "pipelines": rows,
    }

    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(encoded)
    else:
        sys.stdout.write(encoded)

    if failures:
        for failure in failures[:20]:
            print(f"{failure['kind']}: {failure['manifest']} {failure['detail']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
