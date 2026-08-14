#!/usr/bin/env python3
"""Summarize Phase 17 Subnautica 2 runtime proof evidence.

This script does not launch the game. It turns a bounded run's logs, shader
cache, and Phase 16 PSO manifests into a milestone report that separates hard
runtime evidence from visual checkpoints that still need a human/screenshot.
"""

from __future__ import annotations

import argparse
import json
import re
import time
from collections import Counter
from pathlib import Path


DEFAULT_APPID = "1962700"
DEFAULT_CACHE_ROOTS = [
    Path.home() / ".metalsharp" / "shader-cache" / "vkd3d" / DEFAULT_APPID,
    Path("/Volumes/AverySSD/SteamLibrary/steamapps/common/Subnautica2/.metalsharp-cache/shader-cache/vkd3d")
    / DEFAULT_APPID,
    Path("/tmp/dxmt_shader_cache"),
]
DEFAULT_LOG_DIRS = [
    Path.home() / ".metalsharp" / "compatdata" / DEFAULT_APPID / "logs",
]
DEFAULT_TRACE_LOGS = [
    Path("/tmp/dxmt_d3d12_trace.log"),
    Path("/tmp/dxmt_ps_args_debug.log"),
]

EXPECTED_VISUAL_SEQUENCE = [
    (
        "fullscreen_after_splash",
        "Fullscreen appears immediately after splash.",
    ),
    (
        "bottom_right_loading_animation",
        "A small bottom-right loading animation renders before the main 2D loading art.",
    ),
    (
        "blue_stingray_loading_screen",
        "Blue 2D loading screen with stingray art is visible.",
    ),
    (
        "eula_agreements",
        "EULA and agreements are visible and can be accepted.",
    ),
    (
        "shader_compile_screen",
        "Subnautica 2 shader compiling screen appears with bottom white glow and title background.",
    ),
]

PATTERNS = {
    "present": re.compile(r"\bPresent\b|\bpresent\b|presentDrawable|present blit", re.IGNORECASE),
    "draw_encoded": re.compile(
        r"swapchain (?:GeometryDraw|DrawInstanced|DrawIndexedInstanced).*encoded",
        re.IGNORECASE,
    ),
    "draw_skipped": re.compile(r"Draw(?:Indexed)?Instanced SKIPPED|swapchain .* skipped", re.IGNORECASE),
    "dispatch": re.compile(r"\bDispatch\b|compute_dispatch|ReplayComputeDispatch", re.IGNORECASE),
    "readback_zero": re.compile(r"nonzero_pixels=0"),
    "readback_nonzero": re.compile(r"nonzero_pixels=([1-9][0-9]*)"),
    "pso_failure": re.compile(r"PSO COMPILE FAILURE|pso/|PipelineState.*failed|pipeline.*failed", re.IGNORECASE),
    "msl_failure": re.compile(r"DXIL MSL compilation failed|MSL compile failed|metal_library", re.IGNORECASE),
    "root_signature": re.compile(r"root_signature:", re.IGNORECASE),
    "fullscreen": re.compile(r"fullscreen|ConfigureLayer drawable", re.IGNORECASE),
    "eula": re.compile(r"\bEULA\b|agreement", re.IGNORECASE),
    "shader_compile": re.compile(r"shader comp|compil.*shader|PipelineState|DXIL shader compiled", re.IGNORECASE),
}


def file_mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def tail(path: Path, limit: int = 256_000) -> str:
    try:
        with path.open("rb") as handle:
            handle.seek(0, 2)
            size = handle.tell()
            handle.seek(max(0, size - limit))
            return handle.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def discover_files(roots: list[Path], pattern: str) -> list[Path]:
    files: list[Path] = []
    for root in roots:
        if root.exists():
            files.extend(path for path in root.rglob(pattern) if path.is_file())
    return sorted(set(files), key=lambda path: str(path))


def collect_runtime_files(cache_roots: list[Path]) -> dict:
    dxbc = discover_files(cache_roots, "*.dxbc")
    reports = discover_files(cache_roots, "*.dxil_report.txt")
    render_pso = discover_files(cache_roots, "pso-render-*.json")
    compute_pso = discover_files(cache_roots, "pso-compute-*.json")
    metallibs = discover_files(cache_roots, "*.metallib")
    msl_errors = discover_files(cache_roots, "*.msl.err.txt")
    metallib_errors = discover_files(cache_roots, "*.metallib.err.txt")

    root_signature_reports = 0
    unsupported_intrinsic_reports = 0
    unsupported_opcode_reports = 0
    for report in reports:
        text = tail(report, 48_000)
        if "root_signature:" in text:
            root_signature_reports += 1
        if re.search(r"unsupported_intrinsics=[1-9]", text):
            unsupported_intrinsic_reports += 1
        if re.search(r"unsupported_opcodes=[1-9]", text):
            unsupported_opcode_reports += 1

    return {
        "cache_roots": [str(root) for root in cache_roots],
        "dxbc_count": len(dxbc),
        "dxil_report_count": len(reports),
        "root_signature_report_count": root_signature_reports,
        "render_pso_count": len(render_pso),
        "compute_pso_count": len(compute_pso),
        "metallib_count": len(metallibs),
        "msl_error_count": len(msl_errors),
        "metallib_error_count": len(metallib_errors),
        "unsupported_intrinsic_report_count": unsupported_intrinsic_reports,
        "unsupported_opcode_report_count": unsupported_opcode_reports,
        "latest_files": [
            str(path)
            for path in sorted(
                dxbc + reports + render_pso + compute_pso + metallibs,
                key=file_mtime,
                reverse=True,
            )[:40]
        ],
    }


def collect_log_paths(log_dirs: list[Path], trace_logs: list[Path], limit: int) -> list[Path]:
    paths: list[Path] = []
    for log_dir in log_dirs:
        if log_dir.exists():
            paths.extend(path for path in log_dir.glob("launch-*.log") if path.is_file())
            paths.extend(path for path in log_dir.glob("*.log") if path.is_file())
    paths.extend(path for path in trace_logs if path.exists())
    unique = sorted(set(paths), key=file_mtime, reverse=True)
    return unique[:limit]


def collect_log_evidence(log_paths: list[Path]) -> dict:
    totals: Counter[str] = Counter()
    max_nonzero_pixels = 0
    snippets: dict[str, list[str]] = {key: [] for key in PATTERNS}
    per_log = []

    for path in log_paths:
        text = tail(path)
        counts = {}
        for key, pattern in PATTERNS.items():
            matches = pattern.findall(text)
            count = len(matches)
            counts[key] = count
            totals[key] += count
        for match in PATTERNS["readback_nonzero"].finditer(text):
            max_nonzero_pixels = max(max_nonzero_pixels, int(match.group(1)))
        for line in text.splitlines():
            for key, pattern in PATTERNS.items():
                if pattern.search(line):
                    snippets[key].append(line[-500:])
        per_log.append(
            {
                "path": str(path),
                "mtime": file_mtime(path),
                "size": path.stat().st_size if path.exists() else 0,
                "counts": {key: value for key, value in counts.items() if value},
            }
        )

    return {
        "log_paths": [str(path) for path in log_paths],
        "counts": dict(sorted(totals.items())),
        "max_nonzero_pixels": max_nonzero_pixels,
        "per_log": per_log,
        "snippets": {key: value[-12:] for key, value in snippets.items() if value},
    }


def milestone(name: str, passed: bool, detail: str, evidence: str = "") -> dict:
    return {
        "name": name,
        "passed": passed,
        "detail": detail,
        "evidence": evidence,
    }


def build_milestones(runtime: dict, logs: dict, observed_stages: set[str]) -> list[dict]:
    counts = logs["counts"]
    milestones = [
        milestone(
            "runtime_logs_present",
            bool(logs["log_paths"]),
            "At least one launch or D3D12 trace log was found.",
            f"{len(logs['log_paths'])} logs",
        ),
        milestone(
            "shader_cache_present",
            runtime["dxbc_count"] > 0 or runtime["dxil_report_count"] > 0,
            "Shader cache contains DXBC blobs or DXIL reports.",
            f"{runtime['dxbc_count']} dxbc, {runtime['dxil_report_count']} reports",
        ),
        milestone(
            "phase16_pso_manifests_present",
            runtime["render_pso_count"] > 0 or runtime["compute_pso_count"] > 0,
            "Fresh Phase 16 PSO manifest files are present.",
            f"{runtime['render_pso_count']} render, {runtime['compute_pso_count']} compute",
        ),
        milestone(
            "root_signature_reports_present",
            runtime["root_signature_report_count"] > 0,
            "DXIL reports include root-signature sections.",
            f"{runtime['root_signature_report_count']} reports",
        ),
        milestone(
            "present_path_active",
            counts.get("present", 0) > 0,
            "Swapchain present path appears active.",
            f"{counts.get('present', 0)} present traces",
        ),
        milestone(
            "draw_path_active",
            counts.get("draw_encoded", 0) > 0,
            "At least one swapchain draw was encoded.",
            f"{counts.get('draw_encoded', 0)} encoded draw traces",
        ),
        milestone(
            "compute_path_active",
            counts.get("dispatch", 0) > 0 or runtime["compute_pso_count"] > 0,
            "Compute dispatch or compute PSO evidence exists.",
            f"{counts.get('dispatch', 0)} dispatch traces, {runtime['compute_pso_count']} compute PSOs",
        ),
        milestone(
            "nonzero_pixel_readback",
            logs["max_nonzero_pixels"] > 0,
            "Readback observed nonzero pixels.",
            f"max_nonzero_pixels={logs['max_nonzero_pixels']}",
        ),
        milestone(
            "no_compile_or_pso_failures_in_tail",
            counts.get("pso_failure", 0) == 0 and counts.get("msl_failure", 0) == 0,
            "No PSO/MSL failure signatures were seen in the analyzed log tails.",
            f"pso_failures={counts.get('pso_failure', 0)} msl_failures={counts.get('msl_failure', 0)}",
        ),
        milestone(
            "shader_compile_stage_candidate",
            runtime["dxil_report_count"] >= 1000 or counts.get("shader_compile", 0) >= 1000,
            "Large shader compilation activity is visible. Linux baseline was about 9,400 shaders.",
            f"{runtime['dxil_report_count']} reports, {counts.get('shader_compile', 0)} compile traces",
        ),
    ]

    for stage, description in EXPECTED_VISUAL_SEQUENCE:
        milestones.append(
            milestone(
                f"visual_{stage}",
                stage in observed_stages,
                description,
                "observed by user/screenshot" if stage in observed_stages else "visual confirmation required",
            )
        )
    return milestones


def write_markdown(path: Path, result: dict) -> None:
    lines = [
        "# Phase 17 Runtime Proof Summary",
        "",
        "Generated from launch logs, D3D12 trace logs, shader-cache sidecars, and Phase 16 PSO manifests.",
        "",
        "## Known-Good Visual Sequence",
        "",
        "Linux Steam/Proton baseline from the user:",
        "",
        "1. Fullscreen appears immediately after splash.",
        "2. A small bottom-right loading animation renders.",
        "3. Blue 2D loading screen with stingray art appears.",
        "4. EULA and agreements load and can be accepted.",
        "5. Subnautica 2 shader compiling screen appears; Linux baseline compiled about 9,400 shaders in roughly 20 minutes on RTX 3060 / 8 GB DDR4 / 8-core CPU.",
        "",
        "## Milestones",
        "",
        "| Milestone | Status | Evidence |",
        "|---|---|---|",
    ]
    for item in result["milestones"]:
        status = "PASS" if item["passed"] else "PENDING"
        detail = item["detail"].replace("|", "\\|")
        evidence = item["evidence"].replace("|", "\\|")
        lines.append(f"| `{item['name']}` | {status} | {detail} `{evidence}` |")

    runtime = result["runtime"]
    logs = result["logs"]
    lines.extend(
        [
            "",
            "## Runtime Counts",
            "",
            f"- DXBC blobs: `{runtime['dxbc_count']}`",
            f"- DXIL reports: `{runtime['dxil_report_count']}`",
            f"- Root-signature reports: `{runtime['root_signature_report_count']}`",
            f"- Render PSO manifests: `{runtime['render_pso_count']}`",
            f"- Compute PSO manifests: `{runtime['compute_pso_count']}`",
            f"- Metallibs: `{runtime['metallib_count']}`",
            f"- MSL error sidecars: `{runtime['msl_error_count']}`",
            f"- Metallib error sidecars: `{runtime['metallib_error_count']}`",
            "",
            "## Log Counts",
            "",
        ]
    )
    for key, count in logs["counts"].items():
        lines.append(f"- `{key}`: `{count}`")
    lines.append(f"- `max_nonzero_pixels`: `{logs['max_nonzero_pixels']}`")

    lines.extend(["", "## Evidence Snippets", ""])
    for key, snippets in logs["snippets"].items():
        lines.append(f"### `{key}`")
        if snippets:
            for snippet in snippets[-6:]:
                lines.append(f"- `{snippet.replace('|', '\\|')}`")
        else:
            lines.append("None.")
        lines.append("")

    lines.extend(["## Latest Runtime Files", ""])
    for latest in runtime["latest_files"][:20]:
        lines.append(f"- `{latest}`")
    lines.append("")
    path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-root", action="append", type=Path, default=[])
    parser.add_argument("--log-dir", action="append", type=Path, default=[])
    parser.add_argument("--trace-log", action="append", type=Path, default=[])
    parser.add_argument("--observed-stage", action="append", default=[])
    parser.add_argument("--profile", default="subnautica2")
    parser.add_argument("--results-dir", type=Path, default=Path(__file__).resolve().parents[1] / "results")
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--log-limit", type=int, default=16)
    parser.add_argument("--require-nonzero", action="store_true")
    args = parser.parse_args()

    cache_roots = args.cache_root or DEFAULT_CACHE_ROOTS
    log_dirs = args.log_dir or DEFAULT_LOG_DIRS
    trace_logs = args.trace_log or DEFAULT_TRACE_LOGS
    args.results_dir.mkdir(parents=True, exist_ok=True)

    runtime = collect_runtime_files(cache_roots)
    log_paths = collect_log_paths(log_dirs, trace_logs, args.log_limit)
    logs = collect_log_evidence(log_paths)
    observed_stages = set(args.observed_stage)
    result = {
        "schema": "metalsharp.d3d12-metal.phase17-runtime-proof-summary.v1",
        "profile": args.profile,
        "generated_at": int(time.time()),
        "runtime": runtime,
        "logs": logs,
        "observed_stages": sorted(observed_stages),
        "milestones": build_milestones(runtime, logs, observed_stages),
    }

    json_path = args.json or args.results_dir / f"phase17-runtime-proof-summary-{args.profile}.json"
    md_path = args.markdown or args.results_dir / f"phase17-runtime-proof-summary-{args.profile}.md"
    json_path.write_text(json.dumps(result, indent=2) + "\n")
    write_markdown(md_path, result)
    print(md_path)
    print(json_path)

    if args.require_nonzero and logs["max_nonzero_pixels"] <= 0:
      return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
