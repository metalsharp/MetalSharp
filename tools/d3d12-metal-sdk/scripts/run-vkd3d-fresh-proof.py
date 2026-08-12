#!/usr/bin/env python3
"""Fresh VKD3D proof harness runner.

Phase 0 intentionally produces new proof roots and fresh runtime identity logs.
It does not consume previous proof directories, old shader caches, or older probe
outputs as evidence.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Any


DEFAULT_LAB_ROOT = Path("/Volumes/AverySSD/MetalSharp-SM6-UE-Lab")
REQUIRED_WINDOWS = ["d3d12.dll", "dxgi.dll", "dxgi_dxmt.dll", "winemetal.dll"]
REQUIRED_UNIX = ["winemetal.so"]
ELDEN_DXIL_HAZARD_PIXEL_STEMS = [
    "172aaf0883bf7172",
    "1a45b731c0e4fe83",
    "2f1af397f9153841",
    "30ee00cb96b4e801",
    "3bea9f2a9e72171a",
    "541f079ea76591c4",
    "5dd27787e68d6c77",
    "664f77848ee358b1",
    "672ea3ead7e49d6c",
    "6c4971b857643392",
    "904fdbc1433b7246",
    "90f3e6ee48cc6ba0",
    "a5270ef1facc3035",
    "bacc06724ae0df15",
    "bcb47c5245ffbb98",
    "bcfd3010eba1f51d",
    "c9d9b9cf9ff78442",
    "ce00e60cb04556b9",
    "ce0bfbedf57c229d",
    "cffde66df3b3c364",
    "d11ba18f4a2db366",
    "da999dae38812a81",
    "e05ebac4d10f0bed",
    "e802a4479e7393fe",
    "e94138af0ae38fe0",
    "eea08e169425b9aa",
    "f496498dc565ab22",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def macho_x86_64_ok(path: Path) -> bool:
    try:
        data = path.read_bytes()[:4096]
    except OSError:
        return False
    if len(data) < 8:
        return False
    if int.from_bytes(data[0:4], "little") in (0xFEEDFACF, 0xFEEDFACE):
        return int.from_bytes(data[4:8], "little") == 0x01000007
    if int.from_bytes(data[0:4], "big") == 0xCAFEBABE and len(data) >= 8:
        nfat = int.from_bytes(data[4:8], "big")
        offset = 8
        for _ in range(min(nfat, 32)):
            if offset + 20 > len(data):
                break
            if int.from_bytes(data[offset : offset + 4], "big") == 0x01000007:
                return True
            offset += 20
    return False


def pe_x86_64_ok(path: Path) -> bool:
    try:
        data = path.read_bytes()[:1024]
    except OSError:
        return False
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False
    pe_offset = int.from_bytes(data[0x3C:0x40], "little")
    if pe_offset < 0 or pe_offset + 6 > len(data):
        return False
    return data[pe_offset : pe_offset + 4] == b"PE\0\0" and int.from_bytes(data[pe_offset + 4 : pe_offset + 6], "little") == 0x8664


def path_resolves_under(path: Path, root: Path) -> bool:
    try:
        resolved = path.resolve(strict=True)
        resolved_root = root.resolve(strict=True)
    except FileNotFoundError:
        return False
    return str(resolved) == str(resolved_root) or str(resolved).startswith(str(resolved_root) + os.sep)


def pe_export_names(path: Path, max_names: int = 2048) -> list[str]:
    try:
        data = path.read_bytes()
    except OSError:
        return []
    if len(data) < 0x40 or data[:2] != b"MZ":
        return []
    pe_offset = int.from_bytes(data[0x3C:0x40], "little")
    if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        return []
    coff = pe_offset + 4
    section_count = int.from_bytes(data[coff + 2 : coff + 4], "little")
    optional_size = int.from_bytes(data[coff + 16 : coff + 18], "little")
    optional = coff + 20
    if optional + optional_size > len(data) or optional_size < 112:
        return []
    magic = int.from_bytes(data[optional : optional + 2], "little")
    data_directory = optional + (112 if magic == 0x20B else 96)
    if data_directory + 8 > len(data):
        return []
    export_rva = int.from_bytes(data[data_directory : data_directory + 4], "little")
    export_size = int.from_bytes(data[data_directory + 4 : data_directory + 8], "little")
    if export_rva == 0 or export_size == 0:
        return []
    sections = []
    section_offset = optional + optional_size
    for i in range(section_count):
        off = section_offset + i * 40
        if off + 40 > len(data):
            break
        virtual_size = int.from_bytes(data[off + 8 : off + 12], "little")
        virtual_address = int.from_bytes(data[off + 12 : off + 16], "little")
        raw_size = int.from_bytes(data[off + 16 : off + 20], "little")
        raw_pointer = int.from_bytes(data[off + 20 : off + 24], "little")
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer, raw_size))

    def rva_to_offset(rva: int) -> int | None:
        for virtual_address, virtual_span, raw_pointer, raw_size in sections:
            if virtual_address <= rva < virtual_address + virtual_span:
                offset = raw_pointer + (rva - virtual_address)
                if offset < raw_pointer + raw_size and offset < len(data):
                    return offset
        return None

    export_offset = rva_to_offset(export_rva)
    if export_offset is None or export_offset + 40 > len(data):
        return []
    number_of_names = min(int.from_bytes(data[export_offset + 24 : export_offset + 28], "little"), max_names)
    names_rva = int.from_bytes(data[export_offset + 32 : export_offset + 36], "little")
    names_offset = rva_to_offset(names_rva)
    if names_offset is None or names_offset + number_of_names * 4 > len(data):
        return []
    names: list[str] = []
    for index in range(number_of_names):
        name_rva = int.from_bytes(data[names_offset + index * 4 : names_offset + index * 4 + 4], "little")
        name_offset = rva_to_offset(name_rva)
        if name_offset is None or name_offset >= len(data):
            continue
        end = data.find(b"\0", name_offset, min(len(data), name_offset + 512))
        if end == -1:
            continue
        try:
            names.append(data[name_offset:end].decode("ascii"))
        except UnicodeDecodeError:
            continue
    return sorted(set(names))


def sanitized_vulkan_probe_env(icd: Path, runtime_unix_dir: Path) -> dict[str, str]:
    deny_prefixes = ("VK_", "VULKAN_", "DYLD_")
    env = {key: value for key, value in os.environ.items() if not key.startswith(deny_prefixes)}
    env["VK_ICD_FILENAMES"] = str(icd)
    env["VK_DRIVER_FILES"] = str(icd)
    env["DYLD_LIBRARY_PATH"] = str(runtime_unix_dir)
    return env




def merge_primary_log_text(primary: str, *secondary_texts: str) -> str:
    lines = primary.splitlines()
    seen: set[str] = set(lines)
    for text in secondary_texts:
        for line in text.splitlines():
            if line in seen:
                continue
            seen.add(line)
            lines.append(line)
    return "\n".join(lines)


def read_existing_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace") if path.exists() else ""
    except OSError:
        return ""


HARD_FAIL_PATTERNS: list[tuple[str, str]] = [
    ("vertex_range_oob", r"vertex_range_oob"),
    ("d3d12_tessellation_fallback", r"D3D12 tessellation fallback|VKD3D tessellation fallback draw"),
    ("compute_encoder_encode_failed", r"VKD3D compute encoder encode failed"),
    ("render_encoder_encode_failed", r"VKD3D render encoder encode failed"),
    ("metal_command_buffer_error", r"MTLCommandBufferErrorDomain"),
    ("windowserver_watchdog_reboot", r"WindowServer|watchdog|userspace_watchdog|IOGPU|AGX|panic|reboot"),
]

D3D12_NATIVE_TESSELLATION_SOURCE_GUARD_FILES = [
    "vendor/dxmt/src/d3d12/d3d12_native_tessellation_path.cpp",
    "vendor/dxmt/src/d3d12/d3d12_native_tessellation_path.hpp",
    "vendor/dxmt/src/d3d12/d3d12_device.cpp",
    "vendor/dxmt/src/d3d12/d3d12_pipeline_state.cpp",
    "vendor/dxmt/src/d3d12/d3d12_command_queue.cpp",
]

D3D12_NATIVE_TESSELLATION_FORBIDDEN_PATTERNS: list[tuple[str, str]] = [
    ("d3d11_header_include", r"#\s*include\s+[\"<][^\">]*d3d11"),
    ("mtld3d11_symbol", r"\bI?MTLD3D11\w*\b"),
    ("d3d11_runtime_object_symbol", r"\bI?D3D11(Device|Context|Buffer|Texture|Shader|InputLayout|Class|Blend|Rasterizer|Depth|Sampler|View)\w*\b"),
    ("d3d11_tessellation_pipeline_class", r"\bMTLCompiledTessellationMeshPipeline\b"),
    ("d3d11_tessellation_shader_variant", r"\bShaderVariantTessellation\w*\b"),
    ("d3d11_tessellation_factory", r"\bCreateTessellationMeshPipeline\b"),
]


def d3d12_native_tessellation_source_guard(repo: Path) -> dict[str, Any]:
    matches: list[dict[str, Any]] = []
    scanned: list[str] = []
    missing: list[str] = []
    compiled = [(name, re.compile(pattern)) for name, pattern in D3D12_NATIVE_TESSELLATION_FORBIDDEN_PATTERNS]
    for rel in D3D12_NATIVE_TESSELLATION_SOURCE_GUARD_FILES:
        path = repo / rel
        if not path.exists():
            missing.append(rel)
            continue
        scanned.append(rel)
        text = read_existing_text(path)
        for line_no, line in enumerate(text.splitlines(), start=1):
            for pattern_name, pattern in compiled:
                if pattern.search(line):
                    matches.append(
                        {
                            "file": rel,
                            "line": line_no,
                            "pattern": pattern_name,
                            "text": line.strip(),
                        }
                    )
    return {
        "schema": "metalsharp.vkd3d.d3d12-native-tessellation-source-guard.v1",
        "ok": not matches and not missing,
        "rule": "D3D12 native tessellation must not reuse D3D11 context/pipeline/shader machinery",
        "scanned_files": scanned,
        "missing_files": missing,
        "forbidden_patterns": {name: pattern for name, pattern in D3D12_NATIVE_TESSELLATION_FORBIDDEN_PATTERNS},
        "matches": matches[:50],
        "matches_truncated": len(matches) > 50,
    }


def msl_err_sidecar_inventory(shader_cache_dir: Path) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    active_records: list[dict[str, Any]] = []
    stale_records: list[dict[str, Any]] = []
    if shader_cache_dir.exists():
        for err_path in sorted(shader_cache_dir.rglob("*.msl.err.txt")):
            paired_msl = err_path.with_suffix("").with_suffix("")
            try:
                err_mtime = err_path.stat().st_mtime
                err_size = err_path.stat().st_size
            except OSError:
                continue
            msl_exists = paired_msl.exists()
            try:
                msl_mtime = paired_msl.stat().st_mtime if msl_exists else 0.0
                msl_size = paired_msl.stat().st_size if msl_exists else 0
            except OSError:
                msl_exists = False
                msl_mtime = 0.0
                msl_size = 0
            active = (not msl_exists) or err_mtime >= msl_mtime
            record = {
                "err_path": str(err_path),
                "paired_msl": str(paired_msl),
                "paired_msl_exists": msl_exists,
                "classification": "active" if active else "stale",
                "err_mtime": err_mtime,
                "paired_msl_mtime": msl_mtime,
                "err_size": err_size,
                "paired_msl_size": msl_size,
            }
            records.append(record)
            if active:
                active_records.append(record)
            else:
                stale_records.append(record)
    return {
        "schema": "metalsharp.vkd3d.msl-sidecar-inventory.v1",
        "shader_cache_dir": str(shader_cache_dir),
        "freshness_rule": "active when paired .msl is missing or .msl.err.txt mtime is >= paired .msl mtime; stale otherwise",
        "total_count": len(records),
        "active_count": len(active_records),
        "stale_count": len(stale_records),
        "post_launch_msl_err_count": len(active_records),
        "records": records[:100],
        "records_truncated": len(records) > 100,
        "active_records": active_records[:50],
        "active_records_truncated": len(active_records) > 50,
        "stale_records": stale_records[:50],
        "stale_records_truncated": len(stale_records) > 50,
    }


def active_msl_err_sidecars(shader_cache_dir: Path) -> list[dict[str, Any]]:
    return list(msl_err_sidecar_inventory(shader_cache_dir)["active_records"])


def shader_replay_guardrail(
    shader_cache_dir: Path,
    shader_cache_validation: dict[str, Any],
    dxil_hazard_replay: dict[str, Any],
    hard_fail_gates: dict[str, Any],
) -> dict[str, Any]:
    inventory = msl_err_sidecar_inventory(shader_cache_dir)
    expected_hazard_count = len(ELDEN_DXIL_HAZARD_PIXEL_STEMS)
    shader_gates = {
        "shader_cache_validation_ok": shader_cache_validation.get("ok") is True,
        "dxil_hazard_replay_ok": bool(
            dxil_hazard_replay.get("ok") is True
            and dxil_hazard_replay.get("proof_scope") == "exact_elden_dxil_pixel_shader_pso_replay"
            and int(dxil_hazard_replay.get("requested_count", 0) or 0) == expected_hazard_count
            and int(dxil_hazard_replay.get("replay_count", 0) or 0) == expected_hazard_count
            and int(dxil_hazard_replay.get("success_count", 0) or 0) == expected_hazard_count
        ),
        "active_msl_err_count_zero": int(inventory.get("active_count", 0) or 0) == 0,
        "post_launch_msl_err_count_zero": int(inventory.get("post_launch_msl_err_count", 0) or 0) == 0,
    }
    runtime_gates = {
        "hard_fail_native_runtime_pass": hard_fail_gates.get("native_runtime_pass") is True,
        "hard_fail_no_fallback_pass": hard_fail_gates.get("no_fallback_pass") is True,
        "active_msl_err_counter_zero": int(hard_fail_gates.get("counters", {}).get("active_msl_err_sidecars", 0) or 0) == 0,
    }
    return {
        "schema": "metalsharp.vkd3d.shader-replay-guardrail.v1",
        "ok": all(shader_gates.values()),
        "runtime_ok": all(runtime_gates.values()),
        "proof_scope": "phase9_shader_replay_and_stale_sidecar_guardrail_separate_from_runtime_gates",
        "shader_cache_dir": str(shader_cache_dir),
        "shader_gates": shader_gates,
        "runtime_gates": runtime_gates,
        "sidecar_inventory": inventory,
        "full_cache_replay_summary": {
            "ok": shader_cache_validation.get("ok") is True,
            "errors": shader_cache_validation.get("errors", []),
            "proof_scope": shader_cache_validation.get("proof_scope"),
        },
        "dxil_hazard_replay_summary": {
            "ok": dxil_hazard_replay.get("ok") is True,
            "proof_scope": dxil_hazard_replay.get("proof_scope"),
            "requested_count": dxil_hazard_replay.get("requested_count"),
            "replay_count": dxil_hazard_replay.get("replay_count"),
            "success_count": dxil_hazard_replay.get("success_count"),
        },
        "hard_fail_counters": hard_fail_gates.get("counters", {}),
    }


def hard_fail_scan(text: str, shader_cache_dir: Path) -> dict[str, Any]:
    counters = {key: len(re.findall(pattern, text, re.IGNORECASE)) for key, pattern in HARD_FAIL_PATTERNS}
    sidecar_inventory = msl_err_sidecar_inventory(shader_cache_dir)
    active_sidecars = list(sidecar_inventory["active_records"])
    counters["active_msl_err_sidecars"] = int(sidecar_inventory["active_count"])
    no_fallback_pass = (
        counters["vertex_range_oob"] == 0
        and counters["d3d12_tessellation_fallback"] == 0
        and counters["active_msl_err_sidecars"] == 0
    )
    native_runtime_pass = (
        no_fallback_pass
        and counters["compute_encoder_encode_failed"] == 0
        and counters["render_encoder_encode_failed"] == 0
        and counters["metal_command_buffer_error"] == 0
        and counters["windowserver_watchdog_reboot"] == 0
    )
    return {
        "schema": "metalsharp.vkd3d.fresh.hard-fail-scan.v1",
        "counters": counters,
        "active_msl_err_sidecars": active_sidecars[:50],
        "active_msl_err_sidecars_truncated": bool(sidecar_inventory["active_records_truncated"]),
        "no_fallback_pass": no_fallback_pass,
        "native_runtime_pass": native_runtime_pass,
        "hard_fail_patterns": {key: pattern for key, pattern in HARD_FAIL_PATTERNS},
    }


def wine_path_arg(path: Path) -> str:
    resolved = path.expanduser().resolve()
    return "Z:" + str(resolved).replace("/", "\\")


def git_text(repo: Path, *args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], cwd=repo, text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def command_json(cmd: list[str], cwd: Path | None = None, timeout_s: int = 120) -> tuple[dict[str, Any] | list[Any] | None, dict[str, Any]]:
    try:
        proc = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout_s)
    except (OSError, subprocess.SubprocessError) as error:
        return None, {"ok": False, "command": cmd, "error": str(error)}
    try:
        parsed: dict[str, Any] | list[Any] | None = json.loads(proc.stdout) if proc.stdout.strip() else None
    except json.JSONDecodeError as error:
        parsed = None
        return parsed, {
            "ok": False,
            "command": cmd,
            "returncode": proc.returncode,
            "stderr": proc.stderr[-4000:],
            "json_error": str(error),
        }
    return parsed, {"ok": proc.returncode == 0, "command": cmd, "returncode": proc.returncode, "stderr": proc.stderr[-4000:]}


def collect_pr_ci_status(repo: Path, pr_number: int, local_commit: str, skip_pr_ci: bool) -> dict[str, Any]:
    if skip_pr_ci:
        return {"schema": "metalsharp.vkd3d.pr-ci-status.v1", "ok": False, "skipped": True, "checks": []}
    view_json, view_meta = command_json(
        ["gh", "pr", "view", str(pr_number), "--json", "number,url,headRefName,headRefOid,state"],
        cwd=repo,
    )
    checks_json, checks_meta = command_json(
        ["gh", "pr", "checks", str(pr_number), "--json", "name,state,link,startedAt,completedAt,workflow"],
        cwd=repo,
    )
    checks = checks_json if isinstance(checks_json, list) else []
    states = sorted({str(check.get("state", "UNKNOWN")) for check in checks if isinstance(check, dict)})
    failing_states = sorted(state for state in states if state in {"FAILURE", "ERROR", "CANCELLED", "TIMED_OUT", "ACTION_REQUIRED"})
    pending_states = sorted(state for state in states if state not in {"SUCCESS"} and state not in failing_states)
    head_oid = str(view_json.get("headRefOid", "")) if isinstance(view_json, dict) else ""
    head_matches_local = bool(head_oid and head_oid == local_commit)
    return {
        "schema": "metalsharp.vkd3d.pr-ci-status.v1",
        "ok": bool(view_meta["ok"] and checks_meta["ok"] and head_matches_local and checks and states == ["SUCCESS"]),
        "skipped": False,
        "pr_number": pr_number,
        "local_commit": local_commit,
        "head_ref_oid": head_oid,
        "head_matches_local": head_matches_local,
        "states": states,
        "failing_states": failing_states,
        "pending_states": pending_states,
        "view": view_json if isinstance(view_json, dict) else {},
        "view_command": view_meta,
        "checks_command": checks_meta,
        "checks": checks,
    }


def write_readiness_bundle(
    proof_root: Path,
    repo: Path,
    summary: dict[str, Any],
    visible_game_result: dict[str, Any] | None,
    pr_ci_status: dict[str, Any],
) -> dict[str, Any]:
    shader_guard = visible_game_result.get("shader_replay_guardrail", {}) if visible_game_result else {}
    backpressure = visible_game_result.get("backpressure_window_safety", {}) if visible_game_result else {}
    command_retention = visible_game_result.get("command_buffer_retention", {}) if visible_game_result else {}
    native_compute = visible_game_result.get("native_compute_resolve", {}) if visible_game_result else {}
    hard_fail = visible_game_result.get("hard_fail_gates", {}) if visible_game_result else {}
    runtime_hash_ok = bool(summary.get("runtime_stage_ok") is True)
    repo_status_short = git_text(repo, "status", "--short")
    readiness = {
        "schema": "metalsharp.vkd3d.final-readiness.v1",
        "ok": False,
        "proof_root": str(proof_root),
        "repo": {
            "path": str(repo),
            "branch": git_text(repo, "branch", "--show-current"),
            "commit": git_text(repo, "rev-parse", "HEAD"),
            "status_short": repo_status_short,
        },
        "commercial_launch": {"requested": False, "performed": False, "requires_explicit_user_approval": True},
        "local_proof_ok": bool(summary.get("ok") is True),
        "pr_ci": pr_ci_status,
        "runtime_hash_ok": runtime_hash_ok,
        "hard_fail_counters": hard_fail.get("counters", {}),
        "gates": {
            "native_vertex_progressive_triangle_ok": bool(visible_game_result and visible_game_result.get("progressive_triangle_ok") is True),
            "native_tessellation_progressive_triangle_ok": bool(visible_game_result and visible_game_result.get("tessellation_progressive_triangle_ok") is True),
            "native_compute_resolve_ok": bool(native_compute.get("ok") is True),
            "command_buffer_retention_ok": bool(command_retention.get("ok") is True),
            "backpressure_window_safety_ok": bool(backpressure.get("ok") is True),
            "shader_replay_guardrail_ok": bool(shader_guard.get("ok") is True),
            "runtime_hash_ok": runtime_hash_ok,
            "repo_clean_ok": repo_status_short == "",
            "hard_fail_native_runtime_pass": bool(hard_fail.get("native_runtime_pass") is True),
            "pr_ci_ok": bool(pr_ci_status.get("ok") is True),
        },
        "unsupported_native_tessellation_shapes": [
            "Only the narrow D3D12-owned proof shape is admitted; all other HS/DS native GPU shapes remain fail-closed with named diagnostics and no fallback rendering."
        ],
        "artifacts": summary.get("artifacts", {}),
    }
    readiness["ok"] = bool(
        readiness["local_proof_ok"] is True
        and pr_ci_status.get("ok") is True
        and all(readiness["gates"].values())
    )
    write_json(proof_root / "final-readiness.json", readiness)
    checks_lines = []
    for check in pr_ci_status.get("checks", []):
        if isinstance(check, dict):
            checks_lines.append(f"- {check.get('name')}: {check.get('state')} ({check.get('link')})")
    if not checks_lines:
        checks_lines.append("- No PR checks recorded.")
    gates_lines = [f"- {name}: {value}" for name, value in sorted(readiness["gates"].items())]
    hard_fail_lines = [f"- {name}: {value}" for name, value in sorted(readiness["hard_fail_counters"].items())]
    md = "\n".join(
        [
            "# VKD3D Final Prelaunch Readiness",
            "",
            f"- Overall readiness: `{readiness['ok']}`",
            f"- Local proof: `{readiness['local_proof_ok']}`",
            f"- PR CI: `{pr_ci_status.get('ok')}`",
            f"- Commit: `{readiness['repo']['commit']}`",
            f"- Proof root: `{proof_root}`",
            "- Commercial launch performed: `False`",
            "- Commercial launch approval required before any launch: `True`",
            "",
            "## Gates",
            *gates_lines,
            "",
            "## Hard-fail counters",
            *hard_fail_lines,
            "",
            "## PR CI checks",
            *checks_lines,
            "",
            "## Unsupported native tessellation shapes",
            *[f"- {item}" for item in readiness["unsupported_native_tessellation_shapes"]],
            "",
            "## Key artifacts",
            *[f"- {name}: `{path}`" for name, path in sorted(readiness["artifacts"].items())],
            "",
        ]
    )
    (proof_root / "READINESS.md").write_text(md)
    return readiness


def disk_free_gib(path: Path) -> int:
    usage = shutil.disk_usage(path)
    return int(usage.free // (1024**3))


def timestamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def _parse_scalar_token(value: str) -> Any:
    value = value.strip().rstrip(",")
    if not value:
        return value
    if re.fullmatch(r"0x[0-9a-fA-F]+", value):
        try:
            return int(value, 16)
        except ValueError:
            return value
    if re.fullmatch(r"-?\d+", value):
        try:
            return int(value)
        except ValueError:
            return value
    return value


def _parse_kv_tail(tail: str) -> dict[str, Any]:
    record: dict[str, Any] = {}
    for match in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", tail):
        record[match.group(1)] = _parse_scalar_token(match.group(2))
    return record


def _diagnostic_report_watchdog_files(start_time: float, end_time: float, grace_s: float = 120.0) -> dict[str, Any]:
    roots = [Path("/Library/Logs/DiagnosticReports"), Path.home() / "Library" / "Logs" / "DiagnosticReports"]
    patterns = re.compile(r"WindowServer|watchdog|userspace_watchdog|IOGPU|AGX|panic|reboot", re.IGNORECASE)
    deadline = end_time + grace_s
    reports_by_path: dict[str, dict[str, Any]] = {}
    scan_count = 0
    while True:
        scan_count += 1
        for root in roots:
            if not root.exists():
                continue
            for path in root.glob("*"):
                if not patterns.search(path.name):
                    continue
                try:
                    stat = path.stat()
                except OSError:
                    continue
                if start_time <= stat.st_mtime <= deadline:
                    reports_by_path[str(path)] = {"path": str(path), "mtime": stat.st_mtime, "size": stat.st_size}
        if time.time() >= deadline:
            break
        time.sleep(min(5.0, max(0.0, deadline - time.time())))
    reports = sorted(reports_by_path.values(), key=lambda item: str(item["path"]))
    return {
        "reports": reports,
        "scan_count": scan_count,
        "grace_s": grace_s,
        "deadline": deadline,
        "completed_at": time.time(),
        "waited_until_deadline": time.time() >= deadline,
    }


def _unified_log_watchdog_hits(start_time: float, end_time: float) -> dict[str, Any]:
    # Keep the unified-log scan strict enough to avoid routine WindowServer/TCC chatter.
    # DiagnosticReports filename scanning catches WindowServer crash/spin files; unified logs
    # should only fail the proof for watchdog/GPU/reboot/panic signals.
    broad_patterns = r"userspace_watchdog|watchdog (?:timeout|stall|terminated|fired)|IOGPU|\bAGX\b|panic|previous shutdown cause|shutdown cause|system reboot|reboot reason|GPU Restart|GPU Hang|WindowServer"
    serious_patterns = re.compile(
        r"userspace_watchdog|watchdog (?:timeout|stall|terminated|fired)|IOGPU|\bAGX\b|GPU (?:Restart|Hang|Fault)|"
        r"panic(?:\(.*\))?:|kernel panic|previous shutdown cause|shutdown cause|system reboot|reboot reason|"
        r"WindowServer.*(?:watchdog|timeout|spin|crash|hang)",
        re.IGNORECASE,
    )
    false_positive_patterns = re.compile(
        r"Extensible paniclog not supported|IsConnectedToWindowServer|TCCDProcess: identifier=com\.apple\.WindowServer|"
        r"WindowServer[^\n]*(?:has the com\.apple\.private\.tcc|checking access|AttributionChain)|"
        r"Acquiring assertion[^\n]*AppDrawing|Invalidating assertion[^\n]*AppDrawing|"
        r"(?:failed lookup.*com\.apple\.AppleLOM\.Watchdog|com\.apple\.AppleLOM\.Watchdog.*failed lookup)|"
        r"log run noninteractively.*eventMessage MATCHES|_LaunchWatchdogTimeOut.*: 0$",
        re.IGNORECASE,
    )
    start = dt.datetime.fromtimestamp(start_time).strftime("%Y-%m-%d %H:%M:%S")
    end = dt.datetime.fromtimestamp(end_time + 120.0).strftime("%Y-%m-%d %H:%M:%S")
    cmd = ["log", "show", "--style", "compact", "--start", start, "--end", end, "--predicate", f'eventMessage MATCHES[c] ".*({broad_patterns}).*"', "--info", "--debug"]
    try:
        proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    except (subprocess.SubprocessError, OSError) as error:
        return {"ok": False, "command": cmd, "error": str(error), "hit_count": 0, "hits": [], "raw_match_count": 0}
    raw_hits = [line for line in proc.stdout.splitlines() if re.search(broad_patterns, line, re.IGNORECASE)]
    hits = [line for line in raw_hits if serious_patterns.search(line) and not false_positive_patterns.search(line)]
    return {
        "ok": proc.returncode == 0,
        "command": cmd,
        "returncode": proc.returncode,
        "stderr": proc.stderr[-2000:],
        "raw_match_count": len(raw_hits),
        "hit_count": len(hits),
        "hits": hits[:32],
        "hits_truncated": len(hits) > 32,
    }


def extract_backpressure_window_safety(
    text: str,
    frames_presented: int,
    visible_frames: int,
    elapsed_ms: int,
    hard_fail_gates: dict[str, Any],
    proof_start_time: float,
    proof_end_time: float,
    timed_out: bool,
    process_timeout_ms: int,
) -> dict[str, Any]:
    nil_drawable_count = len(re.findall(r"nil drawable|nextDrawable.*nil|drawable unavailable", text, re.IGNORECASE))
    present_stall_count = len(re.findall(r"present stall|drawable wait timeout|present timeout", text, re.IGNORECASE))
    abort_triggers = {
        "proof_timeout": 1 if timed_out else 0,
        "fallback_string": int(hard_fail_gates["counters"].get("d3d12_tessellation_fallback", 0) or 0),
        "vertex_oob_string": int(hard_fail_gates["counters"].get("vertex_range_oob", 0) or 0),
        "compute_encode_failure": int(hard_fail_gates["counters"].get("compute_encoder_encode_failed", 0) or 0),
        "metal_command_buffer_error": int(hard_fail_gates["counters"].get("metal_command_buffer_error", 0) or 0),
        "repeated_nil_drawables": 1 if nil_drawable_count >= 2 else 0,
        "present_stall_threshold": 1 if present_stall_count else 0,
        "windowserver_watchdog_reboot": int(hard_fail_gates["counters"].get("windowserver_watchdog_reboot", 0) or 0),
    }
    host_watchdog_scan = _diagnostic_report_watchdog_files(proof_start_time, proof_end_time)
    host_watchdog_reports = host_watchdog_scan["reports"]
    unified_log_watchdog = _unified_log_watchdog_hits(proof_start_time, proof_end_time)
    avg_frame_ms = (elapsed_ms / frames_presented) if frames_presented else 0.0
    max_avg_frame_ms = 5000.0 if visible_frames < 10 else 2000.0
    return {
        "schema": "metalsharp.vkd3d.backpressure-window-safety.v1",
        "ok": bool(
            not timed_out
            and frames_presented == visible_frames
            and elapsed_ms > 0
            and elapsed_ms < process_timeout_ms
            and avg_frame_ms < max_avg_frame_ms
            and nil_drawable_count == 0
            and present_stall_count == 0
            and not host_watchdog_reports
            and unified_log_watchdog.get("ok") is True
            and int(unified_log_watchdog.get("hit_count", 0) or 0) == 0
            and all(value == 0 for value in abort_triggers.values())
        ),
        "frames_presented": frames_presented,
        "visible_frames": visible_frames,
        "process_elapsed_ms": elapsed_ms,
        "process_timeout_ms": process_timeout_ms,
        "timed_out": timed_out,
        "avg_frame_ms": avg_frame_ms,
        "max_avg_frame_ms": max_avg_frame_ms,
        "queue_depth_bound": 1,
        "queue_depth_source": "vkd3d_fresh_game ExecuteCommandLists completes submitted command buffers before next bounded proof process exits",
        "drawable_wait_ms_max": 0,
        "drawable_wait_source": "no nil-drawable or drawable-timeout diagnostics emitted during proof window",
        "nil_drawable_count": nil_drawable_count,
        "present_stall_count": present_stall_count,
        "abort_trigger_counters": abort_triggers,
        "host_watchdog_report_count": len(host_watchdog_reports),
        "host_watchdog_reports": host_watchdog_reports[:32],
        "host_watchdog_reports_truncated": len(host_watchdog_reports) > 32,
        "host_watchdog_scan": {key: value for key, value in host_watchdog_scan.items() if key != "reports"},
        "unified_log_watchdog": unified_log_watchdog,
    }


def extract_command_buffer_retention(text: str, required_records: int) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    releases: list[dict[str, Any]] = []
    for line in text.splitlines():
        if "VKD3D command buffer retained resources released" in line:
            release = _parse_kv_tail(line)
            release["raw"] = line.strip()
            releases.append(release)
            continue
        if "VKD3D command buffer retained resources" not in line:
            continue
        record = _parse_kv_tail(line)
        record["raw"] = line.strip()
        records.append(record)

    valid_records = [
        record
        for record in records
        if record.get("schema") == "metalsharp.vkd3d.command-buffer-retention.v1"
        and int(record.get("cmdbuf", 0) or 0) != 0
        and int(record.get("retained", 0) or 0) > 0
        and int(record.get("command_count", 0) or 0) > 0
        and int(record.get("overflow", 0) or 0) == 0
    ]
    valid_release_records = [
        release
        for release in releases
        if release.get("schema") == "metalsharp.vkd3d.command-buffer-retention-release.v1"
        and int(release.get("cmdbuf", 0) or 0) != 0
        and int(release.get("retained", 0) or 0) > 0
    ]
    retained_counts = [int(record.get("retained", 0) or 0) for record in valid_records]
    draw_records = [record for record in valid_records if int(record.get("draw_count", 0) or 0) > 0]
    compute_records = [record for record in valid_records if int(record.get("dispatch_count", 0) or 0) > 0]
    retained_cmdbufs = {int(record.get("cmdbuf", 0) or 0) for record in valid_records}
    released_cmdbufs = {int(release.get("cmdbuf", 0) or 0) for release in valid_release_records}
    return {
        "schema": "metalsharp.vkd3d.command-buffer-retention-proof.v1",
        "ok": bool(
            len(valid_records) >= required_records
            and len(draw_records) >= required_records
            and compute_records
            and retained_cmdbufs.issubset(released_cmdbufs)
            and not any(int(record.get("overflow", 0) or 0) for record in records)
        ),
        "required_records": required_records,
        "record_count": len(records),
        "valid_record_count": len(valid_records),
        "release_record_count": len(releases),
        "valid_release_record_count": len(valid_release_records),
        "unreleased_cmdbufs": sorted(retained_cmdbufs - released_cmdbufs),
        "draw_record_count": len(draw_records),
        "compute_record_count": len(compute_records),
        "min_retained": min(retained_counts) if retained_counts else 0,
        "max_retained": max(retained_counts) if retained_counts else 0,
        "total_overflow": sum(int(record.get("overflow", 0) or 0) for record in records),
        "total_duplicates": sum(int(record.get("duplicates", 0) or 0) for record in records),
        "records": records[:128],
        "records_truncated": len(records) > 128,
        "releases": releases[:128],
        "releases_truncated": len(releases) > 128,
    }


def extract_native_compute_resolve(text: str) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    for line in text.splitlines():
        if "VKD3D native_compute_resolve" in line:
            record = _parse_kv_tail(line)
            record["raw"] = line.strip()
            records.append(record)
        elif "VKD3D compute encoder encode failed" in line:
            record = _parse_kv_tail(line)
            record["raw"] = line.strip()
            failures.append(record)

    dispatch_shapes = sorted(
        {str(record.get("dispatch", "")) for record in records if record.get("dispatch")}
    )
    required_dispatch_shapes = {"16x16x1", "8x1x1", "1x1x1"}
    seen_required_dispatch_shapes = sorted(required_dispatch_shapes.intersection(dispatch_shapes))
    valid_records = [
        record
        for record in records
        if record.get("implementation") == "d3d12_native_compute_resolver"
        and int(record.get("compute_pso", 0) or 0) != 0
        and int(record.get("cmd_count", 0) or 0) > 0
        and str(record.get("dispatch", ""))
    ]
    return {
        "schema": "metalsharp.vkd3d.native-compute-resolve.v1",
        "ok": bool(
            len(valid_records) >= 4
            and not failures
            and required_dispatch_shapes.issubset(set(dispatch_shapes))
        ),
        "record_count": len(records),
        "valid_record_count": len(valid_records),
        "failure_count": len(failures),
        "dispatch_shapes": dispatch_shapes,
        "required_dispatch_shapes": sorted(required_dispatch_shapes),
        "seen_required_dispatch_shapes": seen_required_dispatch_shapes,
        "records": records[:128],
        "records_truncated": len(records) > 128,
        "failures": failures[:32],
        "failures_truncated": len(failures) > 32,
    }


LOCAL_GAME_SNAPSHOT_SOURCES: list[dict[str, Any]] = [
    {
        "title": "elden-ring",
        "appid": "1245620",
        "min_render_pso_manifests": 1000,
        "min_compute_pso_manifests": 0,
        "min_dxbc": 1000,
        "min_msl": 1000,
        "min_metallib": 1000,
        "min_core_sidecar_refs": 1000,
        "paths": [
            DEFAULT_LAB_ROOT
            / "04-corpus"
            / "visible-window-no-render-cache-and-logs-20260626-050752"
            / "elden-ring-1245620"
            / "shader-cache",
            Path.home() / ".metalsharp" / "shader-cache" / "vkd3d" / "1245620",
        ],
    },
    {
        "title": "subnautica2",
        "appid": "1962700",
        "min_render_pso_manifests": 1,
        "min_compute_pso_manifests": 100,
        "min_dxbc": 100,
        "min_msl": 100,
        "min_metallib": 100,
        "min_core_sidecar_refs": 100,
        "paths": [
            DEFAULT_LAB_ROOT
            / "04-corpus"
            / "visible-window-no-render-cache-and-logs-20260626-050752"
            / "subnautica2-1962700"
            / "shader-cache",
            Path.home() / ".metalsharp" / "shader-cache" / "vkd3d" / "1962700",
            DEFAULT_LAB_ROOT
            / "04-corpus"
            / "subnautica2-vkd3d-live-corpus-20260625-172628"
            / "source-snapshots"
            / "shader-cache-vkd3d-1962700",
        ],
    },
]


SIDECAR_SUFFIXES = [".dxbc", ".msl", ".metallib", ".module.txt", ".dxil_report.txt"]
CORE_SIDECAR_SUFFIXES = [".dxbc", ".msl", ".module.txt", ".dxil_report.txt"]


def file_suffix_key(path: Path) -> str:
    name = path.name.lower()
    for suffix in [".module.txt", ".dxil_report.txt"]:
        if name.endswith(suffix):
            return suffix
    return path.suffix.lower() if path.suffix else "[noext]"


def nonzero_shader_hash(value: Any) -> str:
    text = str(value or "").strip().lower()
    if not text or set(text) <= {"0"}:
        return ""
    if not re.fullmatch(r"[0-9a-f]{16}", text):
        return ""
    return text


def sha256_cached(path: Path, cache: dict[Path, str]) -> str:
    key = path.resolve(strict=False)
    cached = cache.get(key)
    if cached is not None:
        return cached
    digest = sha256(path)
    cache[key] = digest
    return digest


def parse_local_game_snapshot_overrides(values: list[str]) -> dict[str, list[Path]]:
    overrides: dict[str, list[Path]] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"expected TITLE=PATH for --local-game-snapshot-root, got: {value}")
        title, raw_path = value.split("=", 1)
        title = title.strip()
        if not title:
            raise ValueError(f"missing title in --local-game-snapshot-root: {value}")
        overrides.setdefault(title, []).append(Path(raw_path).expanduser())
    return overrides


def quick_snapshot_counts(root: Path) -> dict[str, Any]:
    counts: Counter[str] = Counter()
    total_files = 0
    newest_mtime = 0.0
    total_bytes = 0
    if root.is_dir():
        for path in root.rglob("*"):
            if not path.is_file() or path.is_symlink() or not path_resolves_under(path, root):
                continue
            stat = path.stat()
            total_files += 1
            total_bytes += stat.st_size
            newest_mtime = max(newest_mtime, stat.st_mtime)
            counts[file_suffix_key(path)] += 1
    return {
        "path": str(root),
        "exists": root.is_dir(),
        "total_files": total_files,
        "total_bytes": total_bytes,
        "newest_mtime": newest_mtime,
        "extension_counts": dict(sorted(counts.items())),
    }


def sidecar_record(root: Path, shader_hash: str, hash_cache: dict[Path, str]) -> dict[str, Any]:
    paths = {suffix: root / f"{shader_hash}{suffix}" for suffix in SIDECAR_SUFFIXES}
    exists = {
        suffix: path.exists()
        and path.is_file()
        and not path.is_symlink()
        and path.stat().st_size > 0
        and path_resolves_under(path, root)
        for suffix, path in paths.items()
    }
    return {
        "hash": shader_hash,
        "exists": exists,
        "core_complete": all(exists[suffix] for suffix in CORE_SIDECAR_SUFFIXES),
        "metallib_complete": bool(exists[".metallib"]),
        "all_complete": all(exists.values()),
        "sizes": {suffix: paths[suffix].stat().st_size if exists[suffix] else 0 for suffix in SIDECAR_SUFFIXES},
        "sha256": {suffix: sha256_cached(paths[suffix], hash_cache) if exists[suffix] else None for suffix in SIDECAR_SUFFIXES},
    }


def run_local_game_snapshot_inventory(
    proof_root: Path, local_snapshot_overrides: dict[str, list[Path]] | None = None
) -> dict[str, Any]:
    """Inventory local Elden/Subnautica shader-cache snapshots without launching games.

    This phase is deliberately report/inventory-only. It proves that fresh PR evidence
    was derived from local commercial-game shader/PSO/material snapshots by checking
    PSO manifests against real DXBC/MSL/module/report/metallib sidecars. It does not
    claim presented-frame execution for these captured game PSOs.
    """

    phase_dir = proof_root / "phase4-local-game-snapshot-inventory"
    phase_dir.mkdir(parents=True, exist_ok=True)
    local_snapshot_overrides = local_snapshot_overrides or {}
    errors: list[str] = []
    title_results: list[dict[str, Any]] = []

    for source in LOCAL_GAME_SNAPSHOT_SOURCES:
        title = str(source["title"])
        appid = str(source["appid"])
        override_paths = local_snapshot_overrides.get(title) or local_snapshot_overrides.get(appid)
        candidate_paths = [Path(path).expanduser() for path in (override_paths if override_paths else source["paths"])]
        candidate_summaries = [quick_snapshot_counts(path) for path in candidate_paths]
        existing_paths = [path for path in candidate_paths if path.is_dir()]
        title_errors: list[str] = []
        title_warnings: list[str] = []
        if not existing_paths:
            primary = candidate_paths[0]
            title_errors.append(f"missing_snapshot_root:{title}:{primary}")
        else:
            def root_score(path: Path) -> tuple[int, int, int, int, int, float]:
                counts = next((item["extension_counts"] for item in candidate_summaries if item["path"] == str(path)), {})
                newest = next((float(item["newest_mtime"] or 0.0) for item in candidate_summaries if item["path"] == str(path)), 0.0)
                return (
                    int(counts.get(".metallib", 0) or 0),
                    int(counts.get(".json", 0) or 0),
                    int(counts.get(".dxbc", 0) or 0),
                    int(counts.get(".msl", 0) or 0),
                    int(counts.get(".module.txt", 0) or 0),
                    newest,
                )

            primary = max(existing_paths, key=root_score)
        path_selection = "explicit_cli_override_scored" if override_paths else "default_candidates_scored_by_material_counts"

        files = (
            sorted(
                path
                for path in primary.rglob("*")
                if path.is_file() and not path.is_symlink() and path_resolves_under(path, primary)
            )
            if primary.is_dir()
            else []
        )
        hash_cache: dict[Path, str] = {}
        extension_counts = Counter(file_suffix_key(path) for path in files)
        extension_bytes = Counter({})
        for path in files:
            extension_bytes[file_suffix_key(path)] += path.stat().st_size

        inventory_tsv = phase_dir / f"{title}-{appid}-file-inventory.tsv"
        with inventory_tsv.open("w", encoding="utf-8") as handle:
            handle.write("title\tappid\troot\trelative_path\textension\tsize\tsha256\n")
            for path in files:
                rel = path.relative_to(primary).as_posix() if primary.is_dir() else path.name
                handle.write(
                    f"{title}\t{appid}\t{primary}\t{rel}\t{file_suffix_key(path)}\t{path.stat().st_size}\t{sha256_cached(path, hash_cache)}\n"
                )

        pso_paths = sorted(
            path
            for path in files
            if path.name.startswith(("pso-render-", "pso-compute-")) and path.suffix == ".json"
        )
        pso_manifest_type_counts = Counter()
        pso_type_counts = Counter()
        color_format_counts = Counter()
        depth_format_counts = Counter()
        input_element_counts = Counter()
        shader_ref_counts = Counter()
        pso_samples: list[dict[str, Any]] = []
        missing_sidecar_samples: list[dict[str, Any]] = []
        missing_metallib_samples: list[dict[str, Any]] = []
        malformed_manifests: list[dict[str, Any]] = []
        shader_hashes: set[str] = set()
        pipeline_count = 0

        for pso_path in pso_paths:
            filename_match = re.fullmatch(r"pso-(render|compute)-([0-9a-f]{16})\.json", pso_path.name.lower())
            if not filename_match:
                malformed_manifests.append({"path": str(pso_path), "error": "invalid_pso_manifest_filename"})
                continue
            filename_type = filename_match.group(1)
            filename_hash = filename_match.group(2)
            try:
                manifest = json.loads(pso_path.read_text(errors="replace"))
            except Exception as error:  # noqa: BLE001 - report malformed local evidence.
                malformed_manifests.append({"path": str(pso_path), "error": str(error)})
                continue
            if not isinstance(manifest, dict):
                malformed_manifests.append({"path": str(pso_path), "error": "manifest_is_not_json_object"})
                continue
            pipelines_json = manifest.get("pipelines")
            if not isinstance(pipelines_json, list) or not pipelines_json:
                malformed_manifests.append({"path": str(pso_path), "error": "missing_or_empty_pipelines_list"})
                continue
            manifest_validated_types: set[str] = set()

            for pipeline_index, pipeline in enumerate(pipelines_json):
                if not isinstance(pipeline, dict):
                    malformed_manifests.append(
                        {"path": str(pso_path), "error": f"pipeline_{pipeline_index}_is_not_json_object"}
                    )
                    continue
                pipeline_count += 1
                ptype = str(pipeline.get("type") or "unknown")
                if ptype != filename_type:
                    malformed_manifests.append(
                        {
                            "path": str(pso_path),
                            "error": f"filename_type_mismatch:{filename_type}:pipeline_{pipeline_index}:{ptype}",
                        }
                    )
                    continue
                expected_pipeline_name = f"{ptype}-{filename_hash}"
                if str(pipeline.get("name") or "") != expected_pipeline_name:
                    malformed_manifests.append(
                        {
                            "path": str(pso_path),
                            "error": f"pipeline_name_mismatch:{pipeline_index}:{pipeline.get('name')}:{expected_pipeline_name}",
                        }
                    )
                    continue
                pso_type_counts[ptype] += 1
                for fmt in pipeline.get("color_formats", []) or []:
                    color_format_counts[str(fmt)] += 1
                depth_format_counts[str(pipeline.get("depth_format") or "")] += 1
                d3d12 = pipeline.get("d3d12", {}) if isinstance(pipeline.get("d3d12"), dict) else {}
                input_element_counts[str(d3d12.get("input_elements", ""))] += 1

                stages = ["vs_hash", "ps_hash", "gs_hash", "cs_hash"]
                sidecars_for_sample: list[dict[str, Any]] = []
                pipeline_stage_records: dict[str, dict[str, Any]] = {}
                for stage in stages:
                    raw_shader_hash = str(d3d12.get(stage) or "").strip()
                    shader_hash = nonzero_shader_hash(raw_shader_hash)
                    if raw_shader_hash and not set(raw_shader_hash.lower()) <= {"0"} and not shader_hash:
                        malformed_manifests.append(
                            {
                                "path": str(pso_path),
                                "error": f"invalid_shader_hash:{stage}:{raw_shader_hash}",
                            }
                        )
                        continue
                    if not shader_hash:
                        continue
                    shader_hashes.add(shader_hash)
                    artifact_root = pso_path.parent
                    record = sidecar_record(artifact_root, shader_hash, hash_cache)
                    sidecars_for_sample.append({"stage": stage, "artifact_root": str(artifact_root), **record})
                    pipeline_stage_records[stage] = record
                    shader_ref_counts["references"] += 1
                    for suffix, exists in record["exists"].items():
                        if exists:
                            shader_ref_counts[f"with{suffix}"] += 1
                    if record["core_complete"]:
                        shader_ref_counts["core_complete"] += 1
                    if record["metallib_complete"]:
                        shader_ref_counts["metallib_complete"] += 1
                    if record["all_complete"]:
                        shader_ref_counts["all_complete"] += 1
                    if not record["core_complete"] and len(missing_sidecar_samples) < 25:
                        missing_sidecar_samples.append(
                            {
                                "manifest": str(pso_path),
                                "artifact_root": str(artifact_root),
                                "pipeline": str(pipeline.get("name") or ""),
                                "stage": stage,
                                "hash": shader_hash,
                                "missing_core_sidecars": [
                                    suffix for suffix in CORE_SIDECAR_SUFFIXES if not record["exists"][suffix]
                                ],
                            }
                        )
                    if not record["metallib_complete"] and len(missing_metallib_samples) < 25:
                        missing_metallib_samples.append(
                            {
                                "manifest": str(pso_path),
                                "artifact_root": str(artifact_root),
                                "pipeline": str(pipeline.get("name") or ""),
                                "stage": stage,
                                "hash": shader_hash,
                                "missing_metallib": True,
                            }
                        )

                if ptype == "render":
                    required_stages = ["vs_hash"]
                    if nonzero_shader_hash(d3d12.get("ps_hash")):
                        required_stages.append("ps_hash")
                    elif isinstance(pipeline.get("fragment"), dict):
                        malformed_manifests.append(
                            {
                                "path": str(pso_path),
                                "error": f"zero_ps_hash_with_fragment_object:{pipeline_index}",
                            }
                        )
                        continue
                else:
                    required_stages = ["cs_hash"] if ptype == "compute" else []
                stage_object_names = {"vs_hash": "vertex", "ps_hash": "fragment", "cs_hash": "shader"}
                stage_bindings_ok = True
                for required_stage in required_stages:
                    expected_hash = pipeline_stage_records.get(required_stage, {}).get("hash", "")
                    stage_object_name = stage_object_names[required_stage]
                    stage_object = pipeline.get(stage_object_name)
                    if not expected_hash or not isinstance(stage_object, dict):
                        malformed_manifests.append(
                            {
                                "path": str(pso_path),
                                "error": f"missing_stage_object:{pipeline_index}:{stage_object_name}",
                            }
                        )
                        stage_bindings_ok = False
                        continue
                    stage_hash = nonzero_shader_hash(stage_object.get("hash"))
                    metallib_name = Path(str(stage_object.get("metallib") or "").replace("\\", "/")).name
                    if stage_hash != expected_hash or metallib_name != f"{expected_hash}.metallib":
                        malformed_manifests.append(
                            {
                                "path": str(pso_path),
                                "error": (
                                    f"stage_object_mismatch:{pipeline_index}:{stage_object_name}:"
                                    f"hash={stage_hash}:expected={expected_hash}:metallib={metallib_name}"
                                ),
                            }
                        )
                        stage_bindings_ok = False

                if (
                    required_stages
                    and stage_bindings_ok
                    and all(
                        stage in pipeline_stage_records and pipeline_stage_records[stage]["all_complete"]
                        for stage in required_stages
                    )
                ):
                    manifest_validated_types.add(ptype)

                if len(pso_samples) < 25:
                    pso_samples.append(
                        {
                            "manifest": str(pso_path),
                            "manifest_sha256": sha256_cached(pso_path, hash_cache),
                            "pipeline": str(pipeline.get("name") or ""),
                            "type": ptype,
                            "d3d12": d3d12,
                            "color_formats": pipeline.get("color_formats", []),
                            "depth_format": pipeline.get("depth_format"),
                            "sidecars": sidecars_for_sample,
                        }
                    )

            for manifest_validated_type in manifest_validated_types:
                pso_manifest_type_counts[manifest_validated_type] += 1

        if int(extension_counts.get(".dxbc", 0)) < int(source["min_dxbc"]):
            title_errors.append(f"insufficient_dxbc:{extension_counts.get('.dxbc', 0)}")
        if int(extension_counts.get(".msl", 0)) < int(source["min_msl"]):
            title_errors.append(f"insufficient_msl:{extension_counts.get('.msl', 0)}")
        if int(extension_counts.get(".metallib", 0)) < int(source["min_metallib"]):
            title_errors.append(f"insufficient_metallib:{extension_counts.get('.metallib', 0)}")
        required_pso_manifest_count = int(source["min_render_pso_manifests"]) + int(source["min_compute_pso_manifests"])
        if len(pso_paths) < required_pso_manifest_count:
            title_errors.append(f"insufficient_pso_manifest_files:{len(pso_paths)}")
        if int(pso_manifest_type_counts.get("render", 0)) < int(source["min_render_pso_manifests"]):
            title_errors.append(f"insufficient_render_pso_manifest_files:{pso_manifest_type_counts.get('render', 0)}")
        if int(pso_manifest_type_counts.get("compute", 0)) < int(source["min_compute_pso_manifests"]):
            title_errors.append(f"insufficient_compute_pso_manifest_files:{pso_manifest_type_counts.get('compute', 0)}")
        if int(shader_ref_counts.get("core_complete", 0)) < int(source["min_core_sidecar_refs"]):
            title_errors.append(f"insufficient_core_sidecar_refs:{shader_ref_counts.get('core_complete', 0)}")
        if int(shader_ref_counts.get("metallib_complete", 0)) < int(source["min_core_sidecar_refs"]):
            title_errors.append(f"insufficient_referenced_metallib_sidecars:{shader_ref_counts.get('metallib_complete', 0)}")
        if malformed_manifests:
            title_errors.append(f"malformed_pso_manifests:{len(malformed_manifests)}")
        reference_count = int(shader_ref_counts.get("references", 0))
        max_incomplete_refs = max(10, reference_count // 100)
        incomplete_core_refs = reference_count - int(shader_ref_counts.get("core_complete", 0))
        incomplete_metallib_refs = reference_count - int(shader_ref_counts.get("metallib_complete", 0))
        if incomplete_core_refs > max_incomplete_refs:
            title_errors.append(f"excessive_missing_core_sidecars:{incomplete_core_refs}")
        elif incomplete_core_refs > 0:
            title_warnings.append(f"minor_missing_core_sidecars:{incomplete_core_refs}")
        if incomplete_metallib_refs > max_incomplete_refs:
            title_errors.append(f"excessive_missing_referenced_metallibs:{incomplete_metallib_refs}")
        elif incomplete_metallib_refs > 0:
            title_warnings.append(f"minor_missing_referenced_metallibs:{incomplete_metallib_refs}")

        title_record = {
            "title": title,
            "appid": appid,
            "scope": "local_game_snapshot_inventory_only_no_live_launch_no_presented_frame_claim",
            "candidate_paths": [str(path) for path in candidate_paths],
            "candidate_summaries": candidate_summaries,
            "existing_paths": [str(path) for path in existing_paths],
            "path_selection": path_selection,
            "primary_path": str(primary),
            "file_inventory_tsv": str(inventory_tsv),
            "file_inventory_tsv_sha256": sha256(inventory_tsv),
            "total_files": len(files),
            "total_bytes": sum(path.stat().st_size for path in files),
            "extension_counts": dict(sorted(extension_counts.items())),
            "extension_bytes": dict(sorted(extension_bytes.items())),
            "pso_manifest_count": len(pso_paths),
            "pso_manifest_type_counts": dict(sorted(pso_manifest_type_counts.items())),
            "pipeline_count": pipeline_count,
            "pipeline_type_counts": dict(sorted(pso_type_counts.items())),
            "unique_shader_hashes": len(shader_hashes),
            "shader_sidecar_counts": dict(sorted(shader_ref_counts.items())),
            "color_format_counts": dict(sorted(color_format_counts.items())),
            "depth_format_counts": dict(sorted(depth_format_counts.items())),
            "input_element_counts": dict(sorted(input_element_counts.items())),
            "pso_samples": pso_samples,
            "missing_sidecar_samples": missing_sidecar_samples,
            "missing_metallib_samples": missing_metallib_samples,
            "malformed_manifest_samples": malformed_manifests[:10],
            "warnings": title_warnings,
            "errors": title_errors,
            "ok": not title_errors,
        }
        write_json(phase_dir / f"{title}-{appid}-snapshot-summary.json", title_record)
        title_results.append(title_record)
        errors.extend(title_errors)

    result = {
        "schema": "metalsharp.vkd3d.fresh.local-game-snapshot-inventory.v1",
        "scope": "report_inventory_only_no_live_commercial_game_launch_no_presented_frame_claim",
        "policy": {
            "live_game_launched": False,
            "uses_local_captured_snapshots": True,
            "fresh_inventory_generated_for_this_proof_run": True,
            "presented_frame_claim": False,
        },
        "titles": title_results,
        "title_count": len(title_results),
        "errors": errors,
        "ok": not errors and len(title_results) == len(LOCAL_GAME_SNAPSHOT_SOURCES),
    }
    write_json(phase_dir / "phase4-local-game-snapshot-inventory-summary.json", result)
    return result


def inspect_runtime(runtime_root: Path) -> tuple[list[dict[str, Any]], list[str]]:
    entries: list[dict[str, Any]] = []
    missing: list[str] = []
    for subdir, names in (("x86_64-windows", REQUIRED_WINDOWS), ("x86_64-unix", REQUIRED_UNIX)):
        for name in names:
            path = runtime_root / subdir / name
            exists = path.exists()
            entry: dict[str, Any] = {
                "name": name,
                "role": subdir,
                "path": str(path),
                "exists": exists,
                "size": path.stat().st_size if exists else 0,
                "sha256": sha256(path) if exists else None,
            }
            if not exists:
                missing.append(str(path))
            entries.append(entry)
    return entries, missing


def copy_runtime_to_probe_dir(runtime_root: Path, out_bin: Path) -> list[dict[str, Any]]:
    out_bin.mkdir(parents=True, exist_ok=True)
    copied: list[dict[str, Any]] = []
    for name in REQUIRED_WINDOWS:
        src = runtime_root / "x86_64-windows" / name
        dst = out_bin / name
        shutil.copy2(src, dst)
        copied.append(
            {
                "name": name,
                "source": str(src),
                "destination": str(dst),
                "source_sha256": sha256(src),
                "destination_sha256": sha256(dst),
                "hash_match": sha256(src) == sha256(dst),
            }
        )
    return copied


def build_identity_probe(repo: Path, cxx: str) -> dict[str, Any]:
    sdk = repo / "tools" / "d3d12-metal-sdk"
    source = sdk / "probes" / "probe_vkd3d_runtime_identity" / "probe_vkd3d_runtime_identity.cpp"
    out_bin = sdk / "out" / "bin"
    exe = out_bin / "probe_vkd3d_runtime_identity.exe"
    out_bin.mkdir(parents=True, exist_ok=True)
    cmd = [
        cxx,
        "-std=c++17",
        "-O2",
        "-static",
        "-static-libgcc",
        "-static-libstdc++",
        "-Wall",
        "-Wextra",
        "-Werror",
        str(source),
        "-lole32",
        "-luuid",
        "-o",
        str(exe),
    ]
    proc = subprocess.run(cmd, cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return {
        "command": cmd,
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "exe": str(exe),
        "ok": proc.returncode == 0 and exe.exists(),
        "exe_sha256": sha256(exe) if exe.exists() else None,
    }


def build_fresh_game(repo: Path, cxx: str) -> dict[str, Any]:
    sdk = repo / "tools" / "d3d12-metal-sdk"
    source = sdk / "probes" / "vkd3d_fresh_game" / "vkd3d_fresh_game.cpp"
    out_bin = sdk / "out" / "bin"
    exe = out_bin / "vkd3d_fresh_game.exe"
    out_bin.mkdir(parents=True, exist_ok=True)
    cmd = [
        cxx,
        "-std=c++17",
        "-O2",
        "-static",
        "-static-libgcc",
        "-static-libstdc++",
        "-Wall",
        "-Wextra",
        "-Werror",
        str(source),
        "-lole32",
        "-luuid",
        "-lgdi32",
        "-o",
        str(exe),
    ]
    proc = subprocess.run(cmd, cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return {
        "command": cmd,
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "exe": str(exe),
        "ok": proc.returncode == 0 and exe.exists(),
        "exe_sha256": sha256(exe) if exe.exists() else None,
    }


def parse_loaddll(stderr: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    pattern = re.compile(r'Loaded L"([^"]+)" at ([^:]+):\s*(\w+)')
    for line in stderr.splitlines():
        match = pattern.search(line)
        if not match:
            continue
        path = match.group(1)
        leaf = path.replace("/", "\\").split("\\")[-1].lower()
        if leaf in {
            "d3d12.dll",
            "dxgi.dll",
            "dxgi_dxmt.dll",
            "winemetal.dll",
            "probe_vkd3d_runtime_identity.exe",
            "vkd3d_fresh_game.exe",
        }:
            rows.append({"path": path, "address": match.group(2), "kind": match.group(3)})
    return rows


def wine_path_to_host(path: str) -> Path | None:
    normalized = path.replace("\\", "/")
    if len(normalized) >= 3 and normalized[1:3] == ":/":
        drive = normalized[0].lower()
        rest = normalized[3:]
        if drive == "z":
            return Path("/") / rest
    return None


def validate_loaded_runtime(
    parsed: dict[str, Any] | None,
    loaddll_rows: list[dict[str, Any]],
    staged_files: list[dict[str, Any]],
) -> dict[str, Any]:
    expected = {entry["name"].lower(): entry for entry in staged_files}
    modules = (parsed or {}).get("modules", {}) if isinstance(parsed, dict) else {}
    loaded_by_name: dict[str, dict[str, Any]] = {}
    for row in loaddll_rows:
        leaf = row["path"].replace("/", "\\").split("\\")[-1].lower()
        loaded_by_name[leaf] = row

    checks: list[dict[str, Any]] = []
    for name, expected_entry in expected.items():
        module = modules.get(name, {})
        wine_path = module.get("path", "") if isinstance(module, dict) else ""
        host_path = wine_path_to_host(wine_path)
        loaded_row = loaded_by_name.get(name, {})
        destination = Path(expected_entry["destination"]).resolve()
        host_path_resolved = host_path.resolve() if host_path and host_path.exists() else host_path
        path_match = bool(host_path_resolved and host_path_resolved == destination)
        hash_value = sha256(host_path_resolved) if host_path_resolved and host_path_resolved.exists() else None
        hash_match = hash_value == expected_entry["destination_sha256"]
        kind_match = loaded_row.get("kind") == "native"
        checks.append(
            {
                "name": name,
                "wine_path": wine_path,
                "host_path": str(host_path_resolved) if host_path_resolved else None,
                "expected_destination": str(destination),
                "loaded_kind": loaded_row.get("kind"),
                "expected_kind": "native",
                "path_match": path_match,
                "sha256": hash_value,
                "expected_sha256": expected_entry["destination_sha256"],
                "hash_match": hash_match,
                "kind_match": kind_match,
                "ok": path_match and hash_match and kind_match,
            }
        )
    return {"ok": all(check["ok"] for check in checks), "checks": checks}


def validate_unix_bridge(
    parsed: dict[str, Any] | None,
    runtime_root: Path,
    wine_runtime: Path,
    winemetal_log: Path,
) -> dict[str, Any]:
    expected_name = "winemetal.so"
    primary = (runtime_root / "x86_64-unix" / expected_name).resolve()
    candidates = [
        primary,
        (wine_runtime / "lib" / "wine" / "x86_64-unix" / expected_name).resolve(),
    ]
    env_value = ""
    if isinstance(parsed, dict):
        env_value = parsed.get("environment", {}).get("DXMT_WINEMETAL_UNIXLIB", "")
    log_text = winemetal_log.read_text(errors="replace") if winemetal_log.exists() else ""
    primary_hash = sha256(primary) if primary.exists() else None
    candidate_checks = []
    for candidate in candidates:
        candidate_hash = sha256(candidate) if candidate.exists() else None
        candidate_checks.append(
            {
                "path": str(candidate),
                "exists": candidate.exists(),
                "sha256": candidate_hash,
                "matches_primary": candidate_hash is not None and candidate_hash == primary_hash,
            }
        )
    return {
        "expected_name": expected_name,
        "primary_path": str(primary),
        "primary_exists": primary.exists(),
        "primary_sha256": primary_hash,
        "candidate_checks": candidate_checks,
        "probe_env_value": env_value,
        "env_match": env_value == expected_name,
        "debug_log": str(winemetal_log),
        "debug_log_exists": winemetal_log.exists(),
        "debug_log_status_ok": "status=0x00000000" in log_text,
        "debug_log_name_match": f"unixlib={expected_name}" in log_text,
        "ok": primary.exists()
        and env_value == expected_name
        and all((not check["exists"]) or check["matches_primary"] for check in candidate_checks)
        and winemetal_log.exists()
        and "status=0x00000000" in log_text
        and f"unixlib={expected_name}" in log_text,
    }


def validate_loaddll_staged(loaddll_rows: list[dict[str, Any]], staged_files: list[dict[str, Any]]) -> dict[str, Any]:
    expected = {entry["name"].lower(): entry for entry in staged_files}
    loaded_by_name: dict[str, dict[str, Any]] = {}
    for row in loaddll_rows:
        leaf = row["path"].replace("/", "\\").split("\\")[-1].lower()
        loaded_by_name[leaf] = row
    checks: list[dict[str, Any]] = []
    for name, expected_entry in expected.items():
        row = loaded_by_name.get(name, {})
        host_path = wine_path_to_host(row.get("path", "")) if row else None
        host_path_resolved = host_path.resolve() if host_path and host_path.exists() else host_path
        destination = Path(expected_entry["destination"]).resolve()
        path_match = bool(host_path_resolved and host_path_resolved == destination)
        hash_value = sha256(host_path_resolved) if host_path_resolved and host_path_resolved.exists() else None
        checks.append(
            {
                "name": name,
                "loaded_kind": row.get("kind"),
                "expected_kind": "native",
                "host_path": str(host_path_resolved) if host_path_resolved else None,
                "expected_destination": str(destination),
                "path_match": path_match,
                "sha256": hash_value,
                "expected_sha256": expected_entry["destination_sha256"],
                "hash_match": hash_value == expected_entry["destination_sha256"],
                "kind_match": row.get("kind") == "native",
            }
        )
    for check in checks:
        check["ok"] = check["path_match"] and check["hash_match"] and check["kind_match"]
    return {"ok": all(check["ok"] for check in checks), "checks": checks}


def validate_fresh_game_corpus_tsv(corpus_tsv: Path, target_shaders: int = 300, target_textures: int = 300) -> dict[str, Any]:
    summary_path = corpus_tsv.parent / "fresh-corpus-summary.json"
    manifest_path = corpus_tsv.parent / "fresh-corpus-manifest.json"
    summary: dict[str, Any] = {}
    manifest: dict[str, Any] = {}
    summary_error = ""
    manifest_error = ""
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        summary_error = str(error)
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        manifest_error = str(error)
    summary_ok = bool(
        summary
        and summary.get("schema") == "metalsharp.vkd3d.fresh.corpus-summary.v1"
        and summary.get("ok") is True
        and Path(summary.get("files_tsv", "")).expanduser().resolve() == corpus_tsv
        and Path(summary.get("manifest", "")).expanduser().resolve() == manifest_path
        and summary.get("target_status", {}).get("shader_ok") is True
        and summary.get("target_status", {}).get("texture_ok") is True
    )
    manifest_ok = bool(
        manifest
        and manifest.get("schema") == "metalsharp.vkd3d.fresh.corpus-manifest.v1"
        and manifest.get("proof_root") == summary.get("proof_root")
        and manifest.get("entry_count") == summary.get("entry_count")
        and manifest.get("category_counts") == summary.get("category_counts")
        and manifest.get("source_families") == summary.get("source_families")
        and manifest.get("target_status", {}).get("shader_ok") is True
        and manifest.get("target_status", {}).get("texture_ok") is True
    )

    rows_seen = 0
    shader_hashes_checked = 0
    texture_hashes_checked = 0
    mismatches: list[dict[str, Any]] = []
    missing: list[str] = []
    selected_position_color: dict[str, dict[str, Any]] = {}
    with corpus_tsv.open("r", encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        columns = {name: index for index, name in enumerate(header)}
        required = {"category", "size", "sha256", "destination"}
        if not required.issubset(columns):
            return {
                "ok": False,
                "error": f"missing required columns: {sorted(required - set(columns))}",
                "path": str(corpus_tsv),
            }
        for line in handle:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < len(header):
                continue
            category = fields[columns["category"]]
            destination_text = fields[columns["destination"]]
            is_position_color_vs = (
                category == "shader"
                and "D3D12StateObjectDatabase" in destination_text
                and destination_text.endswith("PositionColorVS.hlsl")
            )
            is_position_color_ps = (
                category == "shader"
                and "D3D12StateObjectDatabase" in destination_text
                and destination_text.endswith("PositionColorPS.hlsl")
            )
            is_selected_position_color = is_position_color_vs or is_position_color_ps
            if category == "shader":
                if shader_hashes_checked >= target_shaders and not is_selected_position_color:
                    continue
            elif category == "texture":
                if texture_hashes_checked >= target_textures:
                    continue
            else:
                continue
            rows_seen += 1
            destination = Path(destination_text)
            expected_sha256 = fields[columns["sha256"]].lower()
            expected_size = int(fields[columns["size"]])
            actual_size = None
            actual_sha256 = None
            exists = destination.exists()
            if not exists:
                missing.append(str(destination))
            else:
                actual_size = destination.stat().st_size
                actual_sha256 = sha256(destination)
                if actual_size != expected_size or actual_sha256 != expected_sha256:
                    mismatches.append(
                        {
                            "path": str(destination),
                            "expected_size": expected_size,
                            "actual_size": actual_size,
                            "expected_sha256": expected_sha256,
                            "actual_sha256": actual_sha256,
                        }
                    )
            if is_selected_position_color:
                key = "vs" if is_position_color_vs else "ps"
                selected_position_color[key] = {
                    "family": fields[columns.get("family", -1)] if "family" in columns else "",
                    "label": fields[columns.get("label", -1)] if "label" in columns else "",
                    "category": category,
                    "extension": fields[columns.get("extension", -1)] if "extension" in columns else "",
                    "destination": str(destination),
                    "source_path": fields[columns.get("source_path", -1)] if "source_path" in columns else "",
                    "expected_size": expected_size,
                    "actual_size": actual_size,
                    "expected_sha256": expected_sha256,
                    "actual_sha256": actual_sha256,
                    "exists": exists,
                    "hash_match": exists and actual_size == expected_size and actual_sha256 == expected_sha256,
                }
            if category == "shader" and shader_hashes_checked < target_shaders:
                shader_hashes_checked += 1
            elif category == "texture" and texture_hashes_checked < target_textures:
                texture_hashes_checked += 1
            if (
                shader_hashes_checked >= target_shaders
                and texture_hashes_checked >= target_textures
                and "vs" in selected_position_color
                and "ps" in selected_position_color
            ):
                break
    selected_position_color_ok = bool(
        selected_position_color.get("vs", {}).get("family") == "microsoft-sdk"
        and selected_position_color.get("ps", {}).get("family") == "microsoft-sdk"
        and selected_position_color.get("vs", {}).get("label") == "DirectX-Graphics-Samples"
        and selected_position_color.get("ps", {}).get("label") == "DirectX-Graphics-Samples"
        and selected_position_color.get("vs", {}).get("extension") == ".hlsl"
        and selected_position_color.get("ps", {}).get("extension") == ".hlsl"
        and str(selected_position_color.get("vs", {}).get("source_path", "")).endswith(
            "D3D12StateObjectDatabase/src/PositionColorVS.hlsl"
        )
        and str(selected_position_color.get("ps", {}).get("source_path", "")).endswith(
            "D3D12StateObjectDatabase/src/PositionColorPS.hlsl"
        )
        and selected_position_color.get("vs", {}).get("hash_match") is True
        and selected_position_color.get("ps", {}).get("hash_match") is True
    )
    return {
        "ok": summary_ok
        and manifest_ok
        and not missing
        and not mismatches
        and shader_hashes_checked >= target_shaders
        and texture_hashes_checked >= target_textures
        and selected_position_color_ok,
        "path": str(corpus_tsv),
        "summary_path": str(summary_path),
        "summary_ok": summary_ok,
        "summary_error": summary_error,
        "summary_schema": summary.get("schema"),
        "manifest_path": str(manifest_path),
        "manifest_ok": manifest_ok,
        "manifest_error": manifest_error,
        "manifest_schema": manifest.get("schema"),
        "summary_proof_root": summary.get("proof_root"),
        "summary_source_families": summary.get("source_families", []),
        "rows_seen": rows_seen,
        "target_shaders": target_shaders,
        "target_textures": target_textures,
        "shader_hashes_checked": shader_hashes_checked,
        "texture_hashes_checked": texture_hashes_checked,
        "missing": missing[:20],
        "missing_count": len(missing),
        "mismatches": mismatches[:20],
        "mismatch_count": len(mismatches),
        "selected_position_color": selected_position_color,
        "selected_position_color_ok": selected_position_color_ok,
    }


def build_fresh_game_dxil(out_bin: Path, run_dir: Path, wine: Path, prefix: Path) -> dict[str, Any]:
    dxil_dir = run_dir / "fresh-sm6-dxil"
    dxil_dir.mkdir(parents=True, exist_ok=True)
    hlsl_path = dxil_dir / "vkd3d_fresh_sm6_scene.hlsl"
    vs_path = dxil_dir / "vkd3d_fresh_sm6_vs.dxil"
    ps_path = dxil_dir / "vkd3d_fresh_sm6_ps.dxil"

    sdk_dir = out_bin.parent.parent
    fetch_dxc = sdk_dir / "scripts" / "fetch-dxc.sh"
    fetch_proc = subprocess.run([str(fetch_dxc)], cwd=sdk_dir, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    dxc_stage: dict[str, Any] = {
        "fetch_command": [str(fetch_dxc)],
        "fetch_returncode": fetch_proc.returncode,
        "fetch_stdout": fetch_proc.stdout,
        "fetch_stderr": fetch_proc.stderr,
        "files": [],
        "ok": False,
    }
    if fetch_proc.returncode != 0:
        return {
            "ok": False,
            "hlsl": str(hlsl_path),
            "vs_path": str(vs_path),
            "ps_path": str(ps_path),
            "dxc_stage": dxc_stage,
            "vs": {},
            "ps": {},
        }
    dxc_bin_dir = Path(fetch_proc.stdout.strip().splitlines()[-1]).resolve()
    out_bin.mkdir(parents=True, exist_ok=True)
    staged_files = []
    for filename in ["dxc.exe", "dxcompiler.dll", "dxil.dll"]:
        source = dxc_bin_dir / filename
        destination = out_bin / filename
        shutil.copy2(source, destination)
        staged_files.append(
            {
                "source": str(source),
                "destination": str(destination),
                "size": destination.stat().st_size,
                "sha256": sha256(destination),
            }
        )
    dxc_stage["files"] = staged_files
    dxc_stage["ok"] = True

    hlsl_path.write_text(
        """
struct VSIn {
  float3 position : POSITION;
  float4 color : COLOR0;
};
struct PSIn {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
};
PSIn VSMain(VSIn input) {
  PSIn output;
  output.position = float4(input.position, 1.0);
  output.color = input.color;
  return output;
}
float4 PSMain(PSIn input) : SV_TARGET {
  // VKD3D_SCALAR_VECTOR_SEMANTICS_PROOF: keep this input-dependent so DXIL/MSL lowering cannot collapse to a constant.
  // VKD3D_ELDEN_VECTOR_BOOL_PROOF: stage-input vector comparison must lower through any()/all(), never bool = bool4.
  // VKD3D_ELDEN_VECTOR_CTOR_ARITY_PROOF: vector constructor/shuffle lowering must never emit over-width int4/uint4/float4 constructors.
  float4 vector_cmp_value = input.color + float4(-0.25, -0.75, -0.50, -1.0);
  bool4 vector_mask = (vector_cmp_value != float4(0.0, 0.0, 0.0, 0.0));
  bool vector_condition = any(vector_mask);
  int4 ctor_left = int4(6, 4, 0, 0);
  int4 ctor_right = int4(0, 0, 7, 7);
  int4 ctor_merged = int4(ctor_left.xy, ctor_right.zw);
  float proof_bias = (ctor_merged.x == 6 && ctor_merged.y == 4 && ctor_merged.z == 7 && ctor_merged.w == 7 && !vector_condition) ? 0.0 : 1.0;
  float scalar_dot = dot(input.color.rgb, float3(0.25, 0.50, 0.25));
  float scalar_abs = abs(input.color.r - input.color.b);
  float2 scalar_pair = float2(scalar_dot, scalar_abs);
  float2 swizzled_pair = scalar_pair.yx;
  float red = scalar_dot + proof_bias;
  float green = 0.5 + 0.5 * saturate((scalar_abs - 0.5) * 8.0);
  float blue = min(scalar_dot + scalar_abs, 0.75);
  float alpha = max(swizzled_pair.x + 0.5, input.color.a - 0.5);
  return float4(red, green, blue, alpha);
}
""".lstrip(),
        encoding="utf-8",
    )

    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DXMT_"):
            env.pop(key, None)
    env.update({"WINEPREFIX": str(prefix), "WINEDEBUG": "-all"})

    source_text = hlsl_path.read_text(encoding="utf-8")
    semantic_markers = {
        "proof_marker": "VKD3D_SCALAR_VECTOR_SEMANTICS_PROOF" in source_text,
        "elden_vector_bool_marker": "VKD3D_ELDEN_VECTOR_BOOL_PROOF" in source_text,
        "elden_vector_ctor_marker": "VKD3D_ELDEN_VECTOR_CTOR_ARITY_PROOF" in source_text,
        "stage_input_vector_compare": "bool4 vector_mask = (vector_cmp_value != float4" in source_text,
        "stage_input_any_bool": "bool vector_condition = any(vector_mask);" in source_text,
        "int4_ctor_merge": "int4 ctor_merged = int4(ctor_left.xy, ctor_right.zw);" in source_text,
        "proof_bias_keeps_expected_color": "float proof_bias =" in source_text and "!vector_condition" in source_text,
        "dot_input_color": "dot(input.color.rgb" in source_text,
        "scalar_pair_float2": "float2 scalar_pair" in source_text,
        "swizzle_yx": "scalar_pair.yx" in source_text,
        "saturate_before_arithmetic": "0.5 * saturate((scalar_abs - 0.5) * 8.0)" in source_text,
        "min_scalar": "min(scalar_dot + scalar_abs, 0.75)" in source_text,
        "max_swizzle_scalar": "max(swizzled_pair.x + 0.5, input.color.a - 0.5)" in source_text,
        "returns_float4_semantic_color": "return float4(red, green, blue, alpha);" in source_text,
    }
    semantic_source_ok = all(semantic_markers.values())

    commands = {
        "vs": [
            str(wine),
            "dxc.exe",
            "-T",
            "vs_6_0",
            "-E",
            "VSMain",
            "-Qstrip_debug",
            "-Fo",
            wine_path_arg(vs_path),
            wine_path_arg(hlsl_path),
        ],
        "ps": [
            str(wine),
            "dxc.exe",
            "-T",
            "ps_6_0",
            "-E",
            "PSMain",
            "-Qstrip_debug",
            "-Fo",
            wine_path_arg(ps_path),
            wine_path_arg(hlsl_path),
        ],
    }
    results: dict[str, Any] = {}
    ok = True
    for name, cmd in commands.items():
        proc = subprocess.run(cmd, cwd=out_bin, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout_path = dxil_dir / f"dxc-{name}.stdout.txt"
        stderr_path = dxil_dir / f"dxc-{name}.stderr.txt"
        stdout_path.write_text(proc.stdout)
        stderr_path.write_text(proc.stderr)
        output = vs_path if name == "vs" else ps_path
        output_ok = proc.returncode == 0 and output.exists() and output.stat().st_size > 0
        ok = ok and output_ok
        results[name] = {
            "command": cmd,
            "returncode": proc.returncode,
            "stdout": str(stdout_path),
            "stderr": str(stderr_path),
            "output": str(output),
            "output_size": output.stat().st_size if output.exists() else 0,
            "output_sha256": sha256(output) if output.exists() else "",
            "ok": output_ok,
        }
    return {
        "ok": ok and semantic_source_ok,
        "hlsl": str(hlsl_path),
        "semantic_scope": "presented_sm6_dxil_scalar_vector_semantics",
        "semantic_markers": semantic_markers,
        "semantic_source_ok": semantic_source_ok,
        "vs_path": str(vs_path),
        "dxc_stage": dxc_stage,
        "ps_path": str(ps_path),
        "vs": results.get("vs", {}),
        "ps": results.get("ps", {}),
    }


def stage_elden_dxil_hazard_shaders(run_dir: Path) -> dict[str, Any]:
    hazard_dir = run_dir / "exact-elden-dxil-hazards"
    hazard_dir.mkdir(parents=True, exist_ok=True)
    source_dir = Path.home() / ".metalsharp" / "shader-cache" / "vkd3d" / "1245620"
    stems = ELDEN_DXIL_HAZARD_PIXEL_STEMS
    entries: list[dict[str, Any]] = []
    ok = True
    for index, stem in enumerate(stems):
        source = source_dir / f"{stem}.dxbc"
        destination = hazard_dir / f"{stem}.dxbc"
        copied = False
        if source.exists() and source.stat().st_size > 0:
            shutil.copy2(source, destination)
            copied = True
        ok = ok and copied and destination.exists() and destination.stat().st_size > 0
        entries.append(
            {
                "index": index,
                "stem": stem,
                "kind": "pixel",
                "source": str(source),
                "destination": str(destination),
                "copied": copied,
                "size": destination.stat().st_size if destination.exists() else 0,
                "sha256": sha256(destination) if destination.exists() else "",
            }
        )
    return {
        "ok": ok,
        "proof_scope": "exact_elden_dxil_pixel_shader_failure_replay_inputs",
        "source_dir": str(source_dir),
        "destination_dir": str(hazard_dir),
        "entries": entries,
    }


def build_fresh_game_waveops_dxil(out_bin: Path, run_dir: Path, wine: Path, prefix: Path) -> dict[str, Any]:
    wave_dir = run_dir / "fresh-sm6-waveops"
    wave_dir.mkdir(parents=True, exist_ok=True)
    hlsl_path = wave_dir / "vkd3d_fresh_waveops.hlsl"
    cs_path = wave_dir / "vkd3d_fresh_waveops_cs.dxil"
    hlsl_path.write_text(
        """
RWTexture2D<float4> OutTexture : register(u0);
[numthreads(32, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
  // VKD3D_WAVEOPS_RUNTIME_PRESENTED_PROOF: every channel is read back from a presented-frame stamp.
  uint lane = WaveGetLaneIndex();
  uint count = WaveGetLaneCount();
  uint payload = lane + 5u;
  uint first = WaveReadLaneFirst(payload);
  uint from7 = WaveReadLaneAt(payload, 7u);
  bool first_payload_lane = WaveIsFirstLane();
  bool every_lane_active = (count >= lane);
  uint any_edge = (uint)WaveActiveAnyTrue(first_payload_lane);
  uint all_edge = (uint)WaveActiveAllTrue(first_payload_lane);
  uint all_active = (uint)WaveActiveAllTrue(every_lane_active);
  uint any_all = any_edge + all_edge + all_active;
  uint row = tid.x / 16u;
  uint col = tid.x % 16u;
  OutTexture[uint2(col, row)] = float4(
      (float)(lane & 0xffu) / 255.0,
      (float)(count & 0xffu) / 255.0,
      (float)(first & 0xffu) / 255.0,
      (float)((from7 + any_all) & 0xffu) / 255.0);
}
""".lstrip(),
        encoding="utf-8",
    )
    source_text = hlsl_path.read_text(encoding="utf-8")
    semantic_markers = {
        "proof_marker": "VKD3D_WAVEOPS_RUNTIME_PRESENTED_PROOF" in source_text,
        "wave_lane_index": "WaveGetLaneIndex" in source_text,
        "wave_lane_count": "WaveGetLaneCount" in source_text,
        "wave_is_first_lane": "WaveIsFirstLane" in source_text,
        "wave_read_lane_first": "WaveReadLaneFirst" in source_text,
        "wave_read_lane_at": "WaveReadLaneAt" in source_text and "7u" in source_text,
        "wave_any_true": "WaveActiveAnyTrue" in source_text,
        "wave_all_true": "WaveActiveAllTrue" in source_text,
        "uav_texture_store": "OutTexture[uint2(col, row)]" in source_text,
    }
    semantic_source_ok = all(semantic_markers.values())
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DXMT_"):
            env.pop(key, None)
    env.update({"WINEPREFIX": str(prefix), "WINEDEBUG": "-all"})
    cmd = [
        str(wine),
        "dxc.exe",
        "-T",
        "cs_6_0",
        "-E",
        "CSMain",
        "-Qstrip_debug",
        "-Fo",
        wine_path_arg(cs_path),
        wine_path_arg(hlsl_path),
    ]
    proc = subprocess.run(cmd, cwd=out_bin, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout_path = wave_dir / "dxc-waveops.stdout.txt"
    stderr_path = wave_dir / "dxc-waveops.stderr.txt"
    stdout_path.write_text(proc.stdout)
    stderr_path.write_text(proc.stderr)
    output_ok = proc.returncode == 0 and cs_path.exists() and cs_path.stat().st_size > 0
    return {
        "ok": output_ok and semantic_source_ok,
        "hlsl": str(hlsl_path),
        "cs_path": str(cs_path),
        "semantic_scope": "sm6_dxil_waveops_compute_uav_presented_readback",
        "semantic_markers": semantic_markers,
        "semantic_source_ok": semantic_source_ok,
        "command": cmd,
        "returncode": proc.returncode,
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "output_size": cs_path.stat().st_size if cs_path.exists() else 0,
        "output_sha256": sha256(cs_path) if cs_path.exists() else "",
    }


def cache_file_record(path: Path) -> dict[str, Any]:
    return {
        "path": str(path),
        "exists": path.exists(),
        "size": path.stat().st_size if path.exists() else 0,
        "sha256": sha256(path) if path.exists() else "",
    }



def validate_textured_3d_face_provenance(corpus_tsv: Path, faces: list[dict[str, Any]]) -> dict[str, Any]:
    errors: list[str] = []
    records: list[dict[str, Any]] = []
    corpus_root = corpus_tsv.parent.resolve()
    rows: dict[str, dict[str, str]] = {}
    try:
        for line_index, line in enumerate(corpus_tsv.read_text().splitlines()):
            if line_index == 0:
                continue
            fields = line.split("\t")
            if len(fields) < 8:
                continue
            family, label, category, extension, size, expected_sha256, destination, source_path = fields[:8]
            if category == "texture":
                rows[destination] = {
                    "family": family,
                    "label": label,
                    "extension": extension,
                    "size": size,
                    "sha256": expected_sha256,
                    "source_path": source_path,
                }
    except Exception as error:  # noqa: BLE001 - report malformed proof input.
        errors.append(f"read_tsv_failed:{error}")
    for face in faces:
        face_index = face.get("index", -1)
        record: dict[str, Any] = {"face": int(face_index if face_index is not None else -1)}
        destination = str(face.get("destination", ""))
        record["destination"] = destination
        row = rows.get(destination)
        if not row:
            errors.append(f"missing_tsv_row:{destination}")
            record["ok"] = False
            records.append(record)
            continue
        path = Path(destination).resolve()
        try:
            inside_corpus = path == corpus_root or corpus_root in path.parents
        except RuntimeError:
            inside_corpus = False
        actual_size = path.stat().st_size if path.exists() else -1
        expected_size = int(row.get("size", "0") or 0)
        declared_size = int(face.get("declared_size", -1) or -1)
        actual_sha = sha256(path) if path.exists() else ""
        record.update(
            {
                "exists": path.exists(),
                "inside_corpus": inside_corpus,
                "actual_size": actual_size,
                "expected_size": expected_size,
                "declared_size": declared_size,
                "actual_sha256": actual_sha,
                "expected_sha256": row.get("sha256", ""),
                "family": face.get("family", ""),
                "expected_family": row.get("family", ""),
                "label": face.get("label", ""),
                "expected_label": row.get("label", ""),
                "extension": face.get("extension", ""),
                "expected_extension": row.get("extension", ""),
                "source_path": face.get("source_path", ""),
                "expected_source_path": row.get("source_path", ""),
            }
        )
        ok = (
            path.exists()
            and inside_corpus
            and actual_size == expected_size
            and declared_size == expected_size
            and actual_sha == row.get("sha256", "")
            and face.get("sha256") == row.get("sha256", "")
            and face.get("family") == row.get("family", "")
            and face.get("label") == row.get("label", "")
            and face.get("extension") == row.get("extension", "")
            and face.get("source_path") == row.get("source_path", "")
        )
        record["ok"] = ok
        if not ok:
            errors.append(f"invalid_face_provenance:{destination}")
        records.append(record)
    expected_families = {"unreal", "unity-sdk", "microsoft-sdk"}
    actual_families = {str(face.get("family", "")) for face in faces}
    if actual_families != expected_families:
        errors.append(f"unexpected_face_families:{sorted(actual_families)}")
    return {"ok": not errors, "records": records, "errors": errors}


def _strip_cpp_line_comments(text: str) -> str:
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def _split_top_level_args(arg_text: str) -> list[str]:
    args: list[str] = []
    depth = 0
    start = 0
    for index, char in enumerate(arg_text):
        if char in "([{":
            depth += 1
        elif char in ")}]":
            depth = max(0, depth - 1)
        elif char == "," and depth == 0:
            args.append(arg_text[start:index].strip())
            start = index + 1
    tail = arg_text[start:].strip()
    if tail:
        args.append(tail)
    return args


def _strip_balanced_parens(expr: str) -> str:
    value = expr.strip()
    while value.startswith("(") and value.endswith(")"):
        depth = 0
        balanced = True
        for index, char in enumerate(value):
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0 and index != len(value) - 1:
                    balanced = False
                    break
        if not balanced:
            break
        value = value[1:-1].strip()
    return value


def _vector_arg_width(expr: str) -> int:
    value = _strip_balanced_parens(expr)
    swizzle = re.search(r"\.([xyzwrgba]{2,4})$", value)
    if swizzle:
        return len(swizzle.group(1))
    constructor = re.match(r"\b(?:float|int|uint|half)([234])\s*\(", value)
    if constructor and value.endswith(")"):
        return int(constructor.group(1))
    return 1


def _find_matching_paren(text: str, open_index: int) -> int:
    depth = 0
    for index in range(open_index, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index
    return -1


def _has_overarity_vector_constructor(text: str) -> bool:
    code = _strip_cpp_line_comments(text)
    pattern = re.compile(r"\b(float|int|uint|half)([234])\s*\(")
    for match in pattern.finditer(code):
        target_width = int(match.group(2))
        open_index = match.end() - 1
        close_index = _find_matching_paren(code, open_index)
        if close_index < 0:
            continue
        args = _split_top_level_args(code[open_index + 1 : close_index])
        total_width = sum(_vector_arg_width(arg) for arg in args)
        if total_width > target_width:
            return True
    return False


def _has_float2_pointer_coordinate(text: str) -> bool:
    code = _strip_cpp_line_comments(text)
    pointer_vars = set(re.findall(r"\b(?:device|thread|threadgroup|constant)\s+char\s*\*\s*(v\d+)\b", code))
    if not pointer_vars:
        return False
    for match in re.finditer(r"\bfloat2\s*\(\s*(v\d+)\s*,\s*(v\d+)\s*\)", code):
        if match.group(1) in pointer_vars or match.group(2) in pointer_vars:
            return True
    return False


def validate_presented_shader_cache(shader_cache_dir: Path, stderr_text: str, d3d12_json: dict[str, Any], visible_frames: int) -> dict[str, Any]:
    errors: list[str] = []
    visible_scene = d3d12_json.get("visible_scene", {}) if d3d12_json else {}
    visible_vertices = int(visible_scene.get("vertices_per_frame", 0) or 0)
    corpus_shader = d3d12_json.get("corpus_shader", {}) if d3d12_json else {}
    corpus_vertices = int(corpus_shader.get("vertices_per_draw", 0) or 0)
    srv_sample = d3d12_json.get("srv_sample", {}) if d3d12_json else {}
    srv_vertices = int(srv_sample.get("vertices_per_draw", 0) or 0)
    texture_array_srv_sample = d3d12_json.get("texture_array_srv_sample", {}) if d3d12_json else {}
    texture_array_srv_vertices = int(texture_array_srv_sample.get("vertices_per_draw", 0) or 0)
    textured_3d = d3d12_json.get("textured_3d", {}) if d3d12_json else {}
    textured_3d_vertices = int(textured_3d.get("vertices_per_draw", 0) or 0)
    cbv_sample = d3d12_json.get("cbv_sample", {}) if d3d12_json else {}
    cbv_vertices = int(cbv_sample.get("vertices_per_draw", 0) or 0)
    indexed_draw = d3d12_json.get("indexed_draw", {}) if d3d12_json else {}
    indexed_count = int(indexed_draw.get("indices_created", 0) or 0)
    tessellation_fallback = d3d12_json.get("tessellation_fallback", {}) if d3d12_json else {}
    tessellation_vertices = int(tessellation_fallback.get("vertices_per_draw", 0) or 0)
    tessellation_blocked_expected = bool(
        tessellation_fallback.get("native_tessellation_required") is True
        and tessellation_fallback.get("blocked_expected") is True
        and tessellation_fallback.get("fallback_draw_encoded") is False
        and int(tessellation_fallback.get("draw_calls", 0) or 0) == 0
    )
    tessellation_native_resolved = bool(
        tessellation_fallback.get("native_tessellation_required") is True
        and tessellation_fallback.get("native_tessellation_resolved") is True
        and tessellation_fallback.get("native_draw_encoded") is True
        and tessellation_fallback.get("native_indexed_draw_encoded") is True
        and tessellation_fallback.get("blocked_expected") is False
        and tessellation_fallback.get("fallback_draw_encoded") is False
        and int(tessellation_fallback.get("draw_calls", 0) or 0) >= visible_frames
        and int(tessellation_fallback.get("draw_indexed_calls", 0) or 0) >= visible_frames
    )
    indirect_draw = d3d12_json.get("indirect_draw", {}) if d3d12_json else {}
    indirect_vertices = int(indirect_draw.get("argument_vertex_count", 0) or 0)
    nanite_cluster = d3d12_json.get("nanite_cluster", {}) if d3d12_json else {}
    nanite_vertices = int((nanite_cluster.get("computed_draw_args") or [0])[0] or 0) if nanite_cluster else 0
    wave_ops = d3d12_json.get("wave_ops", {}) if d3d12_json else {}
    wave_cs_bytes = int(wave_ops.get("cs_bytes", 0) or 0)
    dxil_draws = re.findall(r"VKD3D swapchain DrawInstanced encoded v=3 i=1 .*?vs=([0-9a-f]{16}) ps=([0-9a-f]{16})", stderr_text)
    sm5_pattern = rf"VKD3D swapchain DrawInstanced encoded v={visible_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    sm5_draws = re.findall(sm5_pattern, stderr_text) if visible_vertices else []
    corpus_pattern = rf"VKD3D swapchain DrawInstanced encoded v={corpus_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}}).*?vs_cb=0.*?ps_cb=0"
    corpus_draws = re.findall(corpus_pattern, stderr_text) if corpus_vertices else []
    srv_pattern = rf"VKD3D swapchain DrawInstanced encoded v={srv_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    srv_draws = re.findall(srv_pattern, stderr_text) if srv_vertices else []
    texture_array_srv_pattern = rf"VKD3D swapchain DrawInstanced encoded v={texture_array_srv_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    texture_array_srv_draws = re.findall(texture_array_srv_pattern, stderr_text) if texture_array_srv_vertices else []
    textured_3d_pattern = rf"VKD3D swapchain DrawInstanced encoded v={textured_3d_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    textured_3d_draws = re.findall(textured_3d_pattern, stderr_text) if textured_3d_vertices else []
    cbv_pattern = rf"VKD3D swapchain DrawInstanced encoded v={cbv_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    cbv_draws = re.findall(cbv_pattern, stderr_text) if cbv_vertices else []
    indexed_pattern = rf"VKD3D swapchain DrawIndexedInstanced encoded idx={indexed_count} inst=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    indexed_draws = re.findall(indexed_pattern, stderr_text) if indexed_count else []
    tessellation_pattern = r"VKD3D native_tessellation_path draw encoded control_points=3 patches=\d+ instances=1"
    tessellation_draws = re.findall(tessellation_pattern, stderr_text) if tessellation_vertices else []
    tessellation_indexed_pattern = r"VKD3D native_tessellation_path indexed draw encoded control_points=3 patches=\d+ instances=1"
    tessellation_indexed_draws = re.findall(tessellation_indexed_pattern, stderr_text) if tessellation_vertices else []
    tessellation_fallback_trace_pattern = rf"VKD3D tessellation fallback draw .*?patch_control_points=3 .*?elements={tessellation_vertices} .*?tess_fallback=1"
    tessellation_fallback_traces = re.findall(tessellation_fallback_trace_pattern, stderr_text) if tessellation_vertices else []
    indirect_pattern = rf"VKD3D swapchain ExecuteIndirect DrawInstanced encoded v={indirect_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    indirect_draws = re.findall(indirect_pattern, stderr_text) if indirect_vertices else []
    nanite_pattern = rf"VKD3D swapchain ExecuteIndirect DrawInstanced encoded v={nanite_vertices} i=1 .*?vs=([0-9a-f]{{16}}) ps=([0-9a-f]{{16}})"
    nanite_draws = re.findall(nanite_pattern, stderr_text) if nanite_vertices else []
    dxil_unique_draws = sorted(set(dxil_draws))
    sm5_unique_draws = sorted(set(sm5_draws))
    corpus_unique_draws = sorted(set(corpus_draws))
    srv_unique_draws = sorted(set(srv_draws))
    texture_array_srv_unique_draws = sorted(set(texture_array_srv_draws))
    textured_3d_unique_draws = sorted(set(textured_3d_draws))
    cbv_unique_draws = sorted(set(cbv_draws))
    indexed_unique_draws = sorted(set(indexed_draws))
    tessellation_unique_draws = sorted(set(tessellation_draws))
    tessellation_indexed_unique_draws = sorted(set(tessellation_indexed_draws))
    indirect_unique_draws = sorted(set(indirect_draws))
    nanite_unique_draws = sorted(set(nanite_draws))
    if not dxil_draws:
        errors.append("missing_dxil_presented_draw_hashes")
    if not sm5_draws:
        errors.append("missing_sm5_presented_draw_hashes")
    if not corpus_draws:
        errors.append("missing_corpus_presented_draw_hashes")
    if not srv_draws:
        errors.append("missing_srv_sample_presented_draw_hashes")
    if not texture_array_srv_draws:
        errors.append("missing_texture_array_srv_sample_presented_draw_hashes")
    if not textured_3d_draws:
        errors.append("missing_textured_3d_presented_draw_hashes")
    if not cbv_draws:
        errors.append("missing_cbv_sample_presented_draw_hashes")
    if not indexed_draws:
        errors.append("missing_indexed_presented_draw_hashes")
    if not tessellation_native_resolved:
        errors.append("missing_native_tessellation_resolved_draw")
    if not tessellation_draws:
        errors.append("missing_native_tessellation_draw_log")
    if not tessellation_indexed_draws:
        errors.append("missing_native_tessellation_indexed_draw_log")
    if tessellation_fallback_traces:
        errors.append("unexpected_tessellation_fallback_trace")
    if tessellation_blocked_expected:
        errors.append("tessellation_blocked_expected_in_native_proof")
    if not indirect_draws:
        errors.append("missing_indirect_presented_draw_hashes")
    if not nanite_draws:
        errors.append("missing_nanite_cluster_presented_draw_hashes")
    if len(dxil_unique_draws) > 1:
        errors.append("unexpected_multiple_dxil_presented_shader_pairs")
    if len(sm5_unique_draws) > 1:
        errors.append("unexpected_multiple_sm5_presented_shader_pairs")
    if len(corpus_unique_draws) > 1:
        errors.append("unexpected_multiple_corpus_presented_shader_pairs")
    if len(srv_unique_draws) > 1:
        errors.append("unexpected_multiple_srv_sample_presented_shader_pairs")
    if len(texture_array_srv_unique_draws) > 1:
        errors.append("unexpected_multiple_texture_array_srv_sample_presented_shader_pairs")
    if len(textured_3d_unique_draws) > 1:
        errors.append("unexpected_multiple_textured_3d_presented_shader_pairs")
    if len(cbv_unique_draws) > 1:
        errors.append("unexpected_multiple_cbv_sample_presented_shader_pairs")
    if len(indexed_unique_draws) > 1:
        errors.append("unexpected_multiple_indexed_presented_shader_pairs")
    if len(tessellation_unique_draws) > 1:
        errors.append("unexpected_multiple_native_tessellation_draw_log_shapes")
    if len(tessellation_indexed_unique_draws) > 1:
        errors.append("unexpected_multiple_native_tessellation_indexed_draw_log_shapes")
    if len(indirect_unique_draws) > 1:
        errors.append("unexpected_multiple_indirect_presented_shader_pairs")
    if len(nanite_unique_draws) > 1:
        errors.append("unexpected_multiple_nanite_cluster_presented_shader_pairs")
    dxil_vs, dxil_ps = dxil_unique_draws[0] if dxil_unique_draws else ("", "")
    sm5_vs, sm5_ps = sm5_unique_draws[0] if sm5_unique_draws else ("", "")
    corpus_vs, corpus_ps = corpus_unique_draws[0] if corpus_unique_draws else ("", "")
    srv_vs, srv_ps = srv_unique_draws[0] if srv_unique_draws else ("", "")
    texture_array_srv_vs, texture_array_srv_ps = texture_array_srv_unique_draws[0] if texture_array_srv_unique_draws else ("", "")
    textured_3d_vs, textured_3d_ps = textured_3d_unique_draws[0] if textured_3d_unique_draws else ("", "")
    cbv_vs, cbv_ps = cbv_unique_draws[0] if cbv_unique_draws else ("", "")
    indexed_vs, indexed_ps = indexed_unique_draws[0] if indexed_unique_draws else ("", "")
    tessellation_vs, tessellation_ps = ("", "")
    indirect_vs, indirect_ps = indirect_unique_draws[0] if indirect_unique_draws else ("", "")
    nanite_vs, nanite_ps = nanite_unique_draws[0] if nanite_unique_draws else ("", "")
    if indexed_unique_draws and indirect_unique_draws and (indirect_vs, indirect_ps) == (indexed_vs, indexed_ps):
        errors.append("indirect_shader_pair_reused_indexed_pair")
    if texture_array_srv_unique_draws and srv_unique_draws and (texture_array_srv_vs, texture_array_srv_ps) == (srv_vs, srv_ps):
        errors.append("texture_array_srv_shader_pair_reused_srv_sample_pair")
    if textured_3d_unique_draws and srv_unique_draws and (textured_3d_vs, textured_3d_ps) == (srv_vs, srv_ps):
        errors.append("textured_3d_shader_pair_reused_srv_sample_pair")
    if textured_3d_unique_draws and texture_array_srv_unique_draws and (textured_3d_vs, textured_3d_ps) == (texture_array_srv_vs, texture_array_srv_ps):
        errors.append("textured_3d_shader_pair_reused_texture_array_srv_pair")
    if textured_3d_unique_draws and cbv_unique_draws and (textured_3d_vs, textured_3d_ps) == (cbv_vs, cbv_ps):
        errors.append("textured_3d_shader_pair_reused_cbv_sample_pair")
    if nanite_unique_draws and indirect_unique_draws and (nanite_vs, nanite_ps) == (indirect_vs, indirect_ps):
        errors.append("nanite_cluster_shader_pair_reused_indirect_pair")
    if nanite_unique_draws and indexed_unique_draws and (nanite_vs, nanite_ps) == (indexed_vs, indexed_ps):
        errors.append("nanite_cluster_shader_pair_reused_indexed_pair")

    required_paths: list[Path] = []
    for shader_hash in [dxil_vs, dxil_ps]:
        if shader_hash:
            required_paths.extend(
                shader_cache_dir / f"{shader_hash}{suffix}"
                for suffix in [".dxbc", ".module.txt", ".dxil_report.txt", ".msl"]
            )
    required_paths.extend([shader_cache_dir / "dxmt_sm50_vs_main.metallib", shader_cache_dir / "dxmt_sm50_ps_main.metallib"])
    required_files = [cache_file_record(path) for path in required_paths]
    for record in required_files:
        if not record["exists"] or int(record["size"] or 0) <= 0:
            errors.append(f"missing_or_empty_cache_file:{record['path']}")

    msl_checks: dict[str, Any] = {}
    if dxil_ps:
        ps_msl = shader_cache_dir / f"{dxil_ps}.msl"
        text = ps_msl.read_text(errors="replace") if ps_msl.exists() else ""
        msl_checks["dxil_ps_semantic_result_channels"] = all(
            token in text for token in ["result.x", "result.y", "result.z", "result.w"]
        )
        msl_checks["dxil_ps_scalar_vector_lowering"] = all(
            token in text
            for token in [
                "in.v0.x",
                "in.v0.y",
                "in.v0.z",
                "abs(",
                "clamp(",
                "min(",
                "max(",
                "result.x = v",
                "result.y = v",
                "result.z = v",
                "result.w = v",
            ]
        )
        code_text = _strip_cpp_line_comments(text)
        dxil_report = shader_cache_dir / f"{dxil_ps}.dxil_report.txt"
        report_text = dxil_report.read_text(errors="replace") if dxil_report.exists() else ""
        msl_checks["dxil_ps_synthetic_vector_bool_any"] = "any(" in code_text and "in.v0" in code_text
        msl_checks["dxil_ps_no_overarity_vector_constructors"] = not _has_overarity_vector_constructor(text)
        msl_checks["dxil_ps_msllowering_runtime_path"] = "MSLLowering runtime path active" in report_text
        if not msl_checks["dxil_ps_semantic_result_channels"]:
            errors.append("dxil_ps_msl_missing_result_channel_writes")
        if not msl_checks["dxil_ps_scalar_vector_lowering"]:
            errors.append("dxil_ps_msl_missing_scalar_vector_lowering")
        if not msl_checks["dxil_ps_no_overarity_vector_constructors"]:
            errors.append("dxil_ps_msl_overarity_vector_constructor")
        if not msl_checks["dxil_ps_msllowering_runtime_path"]:
            errors.append("dxil_ps_not_msllowering_runtime_path")
    if dxil_vs:
        vs_msl = shader_cache_dir / f"{dxil_vs}.msl"
        text = vs_msl.read_text(errors="replace") if vs_msl.exists() else ""
        msl_checks["dxil_vs_clip_w_one"] = "out.position.w = 1.0f;" in text
        msl_checks["dxil_vs_vertex_pull"] = "vkd3d_load_vertex_attr" in text
        if not msl_checks["dxil_vs_clip_w_one"]:
            errors.append("dxil_vs_msl_missing_clip_w_one")
        if not msl_checks["dxil_vs_vertex_pull"]:
            errors.append("dxil_vs_msl_missing_vertex_pull")

    hazard_replay = d3d12_json.get("dxil_hazard_replay", {}) if d3d12_json else {}
    expected_hazard_stems = ELDEN_DXIL_HAZARD_PIXEL_STEMS
    expected_hazard_count = len(expected_hazard_stems)
    hazard_entries = hazard_replay.get("entries", []) if isinstance(hazard_replay.get("entries", []), list) else []
    hazard_shader_records: list[dict[str, Any]] = []
    msl_checks["dxil_hazard_replay_runtime_ok"] = bool(
        hazard_replay.get("ok") is True
        and hazard_replay.get("proof_scope") == "exact_elden_dxil_pixel_shader_pso_replay"
        and int(hazard_replay.get("requested_count", 0) or 0) == expected_hazard_count
        and int(hazard_replay.get("replay_count", 0) or 0) == expected_hazard_count
        and int(hazard_replay.get("success_count", 0) or 0) == expected_hazard_count
        and len(hazard_entries) == expected_hazard_count
        and all(entry.get("ok") is True for entry in hazard_entries)
    )
    if not msl_checks["dxil_hazard_replay_runtime_ok"]:
        errors.append("dxil_hazard_replay_runtime_failed")
    for stem in expected_hazard_stems:
        msl_path = shader_cache_dir / f"{stem}.msl"
        report_path = shader_cache_dir / f"{stem}.dxil_report.txt"
        msl_text = msl_path.read_text(errors="replace") if msl_path.exists() else ""
        report_text = report_path.read_text(errors="replace") if report_path.exists() else ""
        code_text = _strip_cpp_line_comments(msl_text)
        record = {
            "stem": stem,
            "msl": cache_file_record(msl_path),
            "dxil_report": cache_file_record(report_path),
            "msllowering_runtime_path": "MSLLowering runtime path active" in report_text,
            "no_overarity_vector_constructors": not _has_overarity_vector_constructor(msl_text),
            "no_float2_pointer_coordinates": not _has_float2_pointer_coordinate(msl_text),
            "vector_bool_any": ("any(" in code_text and "in.v0" in code_text) if stem == "bcfd3010eba1f51d" else True,
        }
        record["ok"] = bool(
            record["msl"]["exists"]
            and int(record["msl"]["size"] or 0) > 0
            and record["dxil_report"]["exists"]
            and int(record["dxil_report"]["size"] or 0) > 0
            and record["msllowering_runtime_path"]
            and record["no_overarity_vector_constructors"]
            and record["no_float2_pointer_coordinates"]
            and record["vector_bool_any"]
        )
        if not record["ok"]:
            errors.append(f"dxil_hazard_shader_validation_failed:{stem}")
        hazard_shader_records.append(record)
    msl_checks["dxil_hazard_shader_records_ok"] = all(record["ok"] for record in hazard_shader_records)

    pso_files = sorted([*shader_cache_dir.glob("pso-render-*.json"), *shader_cache_dir.glob("pso-compute-*.json")])
    pso_records = [cache_file_record(path) for path in pso_files]
    pipelines: list[dict[str, Any]] = []
    for path in pso_files:
        try:
            for pipeline in json.loads(path.read_text()).get("pipelines", []):
                copy = dict(pipeline)
                copy["manifest"] = str(path)
                pipelines.append(copy)
        except Exception as error:  # noqa: BLE001 - report malformed cache evidence.
            errors.append(f"invalid_pso_manifest:{path}:{error}")
    waveops_compute_pso = next(
        (
            p
            for p in pipelines
            if wave_cs_bytes > 0
            and p.get("type") == "compute"
            and int(p.get("d3d12", {}).get("cs_bytes", 0) or 0) == wave_cs_bytes
        ),
        None,
    )
    waveops_cs_hash = str(waveops_compute_pso.get("d3d12", {}).get("cs_hash", "")) if waveops_compute_pso else ""
    waveops_msl = shader_cache_dir / f"{waveops_cs_hash}.msl" if waveops_cs_hash else Path("")
    waveops_metallib = Path(str(waveops_compute_pso.get("shader", {}).get("metallib", ""))) if waveops_compute_pso else Path("")
    if not waveops_compute_pso:
        errors.append("missing_waveops_compute_pso_manifest")
    elif not waveops_cs_hash:
        errors.append("missing_waveops_compute_cs_hash")
    if waveops_cs_hash:
        waveops_msl_text = waveops_msl.read_text(errors="replace") if waveops_msl.exists() else ""
        msl_checks["waveops_msl_simd_intrinsics"] = all(
            token in waveops_msl_text
            for token in [
                "simd_lane [[thread_index_in_simdgroup]]",
                "simd_count [[threads_per_simdgroup]]",
                "simd_broadcast_first(",
                "simd_broadcast(",
                "simd_any(",
                "simd_all(",
                "tex0.write(",
            ]
        )
        if not waveops_msl.exists() or waveops_msl.stat().st_size <= 0:
            errors.append("missing_waveops_msl")
        if waveops_compute_pso.get("threadgroup_size") != [32, 1, 1]:
            errors.append("waveops_compute_pso_missing_32x1x1_threadgroup_size")
        if int(waveops_compute_pso.get("metal", {}).get("compute_function", 0) or 0) == 0:
            errors.append("waveops_compute_pso_missing_compute_function")
        if not msl_checks["waveops_msl_simd_intrinsics"]:
            errors.append("waveops_msl_missing_simd_intrinsics")
    dxil_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == dxil_vs and p.get("d3d12", {}).get("ps_hash") == dxil_ps
        ),
        None,
    )
    sm5_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == sm5_vs and p.get("d3d12", {}).get("ps_hash") == sm5_ps
        ),
        None,
    )
    corpus_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == corpus_vs and p.get("d3d12", {}).get("ps_hash") == corpus_ps
        ),
        None,
    )
    srv_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == srv_vs and p.get("d3d12", {}).get("ps_hash") == srv_ps
        ),
        None,
    )
    texture_array_srv_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == texture_array_srv_vs
            and p.get("d3d12", {}).get("ps_hash") == texture_array_srv_ps
        ),
        None,
    )
    cbv_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == cbv_vs and p.get("d3d12", {}).get("ps_hash") == cbv_ps
        ),
        None,
    )
    textured_3d_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == textured_3d_vs and p.get("d3d12", {}).get("ps_hash") == textured_3d_ps
        ),
        None,
    )
    indexed_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == indexed_vs and p.get("d3d12", {}).get("ps_hash") == indexed_ps
        ),
        None,
    )
    tessellation_pso = None
    indirect_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == indirect_vs and p.get("d3d12", {}).get("ps_hash") == indirect_ps
        ),
        None,
    )
    nanite_pso = next(
        (
            p
            for p in pipelines
            if p.get("d3d12", {}).get("vs_hash") == nanite_vs and p.get("d3d12", {}).get("ps_hash") == nanite_ps
        ),
        None,
    )
    if not dxil_pso:
        errors.append("missing_dxil_presented_pso_manifest")
    if not sm5_pso:
        errors.append("missing_sm5_presented_pso_manifest")
    if not corpus_pso:
        errors.append("missing_corpus_presented_pso_manifest")
    if not srv_pso:
        errors.append("missing_srv_sample_presented_pso_manifest")
    if not texture_array_srv_pso:
        errors.append("missing_texture_array_srv_sample_presented_pso_manifest")
    if not cbv_pso:
        errors.append("missing_cbv_sample_presented_pso_manifest")
    if not textured_3d_pso:
        errors.append("missing_textured_3d_presented_pso_manifest")
    if not indexed_pso:
        errors.append("missing_indexed_presented_pso_manifest")
    if not indirect_pso:
        errors.append("missing_indirect_presented_pso_manifest")
    if not nanite_pso:
        errors.append("missing_nanite_cluster_presented_pso_manifest")
    if indirect_pso and indexed_pso and indirect_pso.get("manifest") == indexed_pso.get("manifest"):
        errors.append("indirect_pso_manifest_reused_indexed_manifest")
    if texture_array_srv_pso and srv_pso and texture_array_srv_pso.get("manifest") == srv_pso.get("manifest"):
        errors.append("texture_array_srv_pso_manifest_reused_srv_sample_manifest")
    if textured_3d_pso and srv_pso and textured_3d_pso.get("manifest") == srv_pso.get("manifest"):
        errors.append("textured_3d_pso_manifest_reused_srv_sample_manifest")
    if textured_3d_pso and texture_array_srv_pso and textured_3d_pso.get("manifest") == texture_array_srv_pso.get("manifest"):
        errors.append("textured_3d_pso_manifest_reused_texture_array_srv_manifest")
    if nanite_pso and indirect_pso and nanite_pso.get("manifest") == indirect_pso.get("manifest"):
        errors.append("nanite_cluster_pso_manifest_reused_indirect_manifest")
    if textured_3d_pso:
        if textured_3d_pso.get("depth_format") != "depth32float":
            errors.append("textured_3d_pso_missing_depth32float")
        if textured_3d_pso.get("stencil_format") != "invalid":
            errors.append("textured_3d_pso_unexpected_stencil_format")
        if int(textured_3d_pso.get("d3d12", {}).get("dsv_format", 0) or 0) != 40:
            errors.append("textured_3d_pso_missing_d32_dsv_format")
    metallib_policy: dict[str, Any] = {}
    sm5_metallibs = [shader_cache_dir / "dxmt_sm50_vs_main.metallib", shader_cache_dir / "dxmt_sm50_ps_main.metallib"]
    for name, pso in [
        ("dxil", dxil_pso),
        ("sm5", sm5_pso),
        ("corpus", corpus_pso),
        ("srv_sample", srv_pso),
        ("texture_array_srv_sample", texture_array_srv_pso),
        ("textured_3d", textured_3d_pso),
        ("cbv_sample", cbv_pso),
        ("indexed_draw", indexed_pso),
        ("indirect_draw", indirect_pso),
        ("nanite_cluster", nanite_pso),
    ]:
        if not pso:
            continue
        if pso.get("type") != "render":
            errors.append(f"{name}_pso_not_render")
        if pso.get("color_formats", [None])[0] != "rgba8unorm":
            errors.append(f"{name}_pso_not_rgba8unorm")
        if int(pso.get("d3d12", {}).get("input_elements", 0) or 0) != 2:
            errors.append(f"{name}_pso_missing_two_input_elements")
        if pso.get("metal", {}).get("rasterization_enabled") is not True:
            errors.append(f"{name}_pso_rasterization_disabled")
        if int(pso.get("metal", {}).get("vertex_function", 0) or 0) == 0:
            errors.append(f"{name}_pso_missing_vertex_function")
        if int(pso.get("metal", {}).get("fragment_function", 0) or 0) == 0:
            errors.append(f"{name}_pso_missing_fragment_function")
        referenced_metallibs = [
            Path(str(pso.get(stage, {}).get("metallib", "")))
            for stage in ["vertex", "fragment"]
            if pso.get(stage, {}).get("metallib")
        ]
        referenced_records = [cache_file_record(path) for path in referenced_metallibs]
        referenced_all_present = bool(referenced_records) and all(
            record["exists"] and int(record["size"] or 0) > 0 for record in referenced_records
        )
        if referenced_all_present:
            metallib_policy[name] = {"ok": True, "policy": "pso_referenced_metallibs_present", "referenced": referenced_records}
        elif name == "dxil" and msl_checks.get("dxil_ps_scalar_vector_lowering") and msl_checks.get("dxil_vs_clip_w_one"):
            metallib_policy[name] = {
                "ok": True,
                "policy": "runtime_source_compile_from_cached_msl_no_hash_metallib_file",
                "referenced": referenced_records,
                "required_msl_hashes": [dxil_vs, dxil_ps],
            }
        elif name in (
            "sm5",
            "corpus",
            "srv_sample",
            "texture_array_srv_sample",
            "textured_3d",
            "cbv_sample",
            "indexed_draw",
            "tessellation_fallback",
            "indirect_draw",
            "nanite_cluster",
        ) and all(path.exists() and path.stat().st_size > 0 for path in sm5_metallibs):
            metallib_policy[name] = {
                "ok": True,
                "policy": "runtime_sm5_metallib_files_present",
                "referenced": referenced_records,
                "metallibs": [cache_file_record(path) for path in sm5_metallibs],
            }
        else:
            metallib_policy[name] = {"ok": False, "policy": "unproven_metallib_source", "referenced": referenced_records}
            errors.append(f"{name}_metallib_policy_unproven")

    presented_log_required = min(visible_frames, 6)
    textured_3d_presented_log_required = min(visible_frames, 4)
    present_tie_ok = (
        len(dxil_draws) >= presented_log_required
        and len(sm5_draws) >= presented_log_required
        and len(dxil_unique_draws) == 1
        and len(sm5_unique_draws) == 1
        and len(corpus_draws) >= presented_log_required
        and len(corpus_unique_draws) == 1
        and len(srv_draws) >= presented_log_required
        and len(srv_unique_draws) == 1
        and len(texture_array_srv_draws) >= presented_log_required
        and len(texture_array_srv_unique_draws) == 1
        and len(textured_3d_draws) >= textured_3d_presented_log_required
        and len(textured_3d_unique_draws) == 1
        and len(cbv_draws) >= presented_log_required
        and len(cbv_unique_draws) == 1
        and len(indexed_draws) >= presented_log_required
        and len(indexed_unique_draws) == 1
        and tessellation_native_resolved
        and len(tessellation_draws) >= 1
        and len(tessellation_indexed_draws) >= 1
        and len(tessellation_fallback_traces) == 0
        and len(indirect_draws) >= presented_log_required
        and len(indirect_unique_draws) == 1
        and len(nanite_draws) >= presented_log_required
        and len(nanite_unique_draws) == 1
    )
    if not present_tie_ok:
        errors.append("insufficient_presented_shader_hash_logs")
    ok = not errors
    return {
        "ok": ok,
        "path": str(shader_cache_dir),
        "dxil_presented_hashes": {
            "vs": dxil_vs,
            "ps": dxil_ps,
            "logged_draws": len(dxil_draws),
            "unique_pairs": [[vs, ps] for vs, ps in dxil_unique_draws],
        },
        "sm5_presented_hashes": {
            "vs": sm5_vs,
            "ps": sm5_ps,
            "logged_draws": len(sm5_draws),
            "unique_pairs": [[vs, ps] for vs, ps in sm5_unique_draws],
            "vertices": visible_vertices,
        },
        "corpus_presented_hashes": {
            "vs": corpus_vs,
            "ps": corpus_ps,
            "logged_draws": len(corpus_draws),
            "unique_pairs": [[vs, ps] for vs, ps in corpus_unique_draws],
            "vertices": corpus_vertices,
        },
        "srv_sample_presented_hashes": {
            "vs": srv_vs,
            "ps": srv_ps,
            "logged_draws": len(srv_draws),
            "unique_pairs": [[vs, ps] for vs, ps in srv_unique_draws],
            "vertices": srv_vertices,
        },
        "texture_array_srv_sample_presented_hashes": {
            "vs": texture_array_srv_vs,
            "ps": texture_array_srv_ps,
            "logged_draws": len(texture_array_srv_draws),
            "unique_pairs": [[vs, ps] for vs, ps in texture_array_srv_unique_draws],
            "vertices": texture_array_srv_vertices,
        },
        "textured_3d_presented_hashes": {
            "vs": textured_3d_vs,
            "ps": textured_3d_ps,
            "logged_draws": len(textured_3d_draws),
            "unique_pairs": [[vs, ps] for vs, ps in textured_3d_unique_draws],
            "vertices": textured_3d_vertices,
        },
        "cbv_sample_presented_hashes": {
            "vs": cbv_vs,
            "ps": cbv_ps,
            "logged_draws": len(cbv_draws),
            "unique_pairs": [[vs, ps] for vs, ps in cbv_unique_draws],
            "vertices": cbv_vertices,
        },
        "indexed_presented_hashes": {
            "vs": indexed_vs,
            "ps": indexed_ps,
            "logged_draws": len(indexed_draws),
            "unique_pairs": [[vs, ps] for vs, ps in indexed_unique_draws],
            "indices": indexed_count,
        },
        "tessellation_fallback_presented_hashes": {
            "vs": tessellation_vs,
            "ps": tessellation_ps,
            "logged_draws": len(tessellation_draws),
            "fallback_trace_count": len(tessellation_fallback_traces),
            "unique_pairs": [],
            "unique_logs": tessellation_unique_draws,
            "vertices": tessellation_vertices,
            "native_tessellation_required": tessellation_fallback.get("native_tessellation_required") is True,
            "native_tessellation_resolved": tessellation_native_resolved,
            "native_draw_encoded": tessellation_fallback.get("native_draw_encoded") is True,
            "native_indexed_draw_encoded": tessellation_fallback.get("native_indexed_draw_encoded") is True,
            "logged_indexed_draws": len(tessellation_indexed_draws),
            "blocked_expected": tessellation_blocked_expected,
            "fallback_draw_encoded": tessellation_fallback.get("fallback_draw_encoded") is True,
        },
        "indirect_presented_hashes": {
            "vs": indirect_vs,
            "ps": indirect_ps,
            "logged_draws": len(indirect_draws),
            "unique_pairs": [[vs, ps] for vs, ps in indirect_unique_draws],
            "vertices": indirect_vertices,
        },
        "nanite_cluster_presented_hashes": {
            "vs": nanite_vs,
            "ps": nanite_ps,
            "logged_draws": len(nanite_draws),
            "unique_pairs": [[vs, ps] for vs, ps in nanite_unique_draws],
            "vertices": nanite_vertices,
        },
        "presented_log_required": presented_log_required,
        "textured_3d_presented_log_required": textured_3d_presented_log_required,
        "present_tie_ok": present_tie_ok,
        "required_files": required_files,
        "pso_manifests": pso_records,
        "dxil_pso_manifest": dxil_pso,
        "sm5_pso_manifest": sm5_pso,
        "corpus_pso_manifest": corpus_pso,
        "srv_sample_pso_manifest": srv_pso,
        "texture_array_srv_sample_pso_manifest": texture_array_srv_pso,
        "textured_3d_pso_manifest": textured_3d_pso,
        "cbv_sample_pso_manifest": cbv_pso,
        "indexed_pso_manifest": indexed_pso,
        "tessellation_fallback_pso_manifest": tessellation_pso,
        "indirect_pso_manifest": indirect_pso,
        "nanite_cluster_pso_manifest": nanite_pso,
        "waveops_compute_pso_manifest": waveops_compute_pso,
        "waveops_compute_hash": waveops_cs_hash,
        "waveops_msl": cache_file_record(waveops_msl) if waveops_cs_hash else {},
        "waveops_metallib_manifest_reference": cache_file_record(waveops_metallib) if waveops_cs_hash else {},
        "waveops_cache_policy": {
            "ok": bool(waveops_cs_hash and msl_checks.get("waveops_msl_simd_intrinsics") and waveops_compute_pso),
            "policy": "runtime_source_compile_from_cached_waveops_msl_with_compute_pso_manifest",
            "requires_hash_metallib_file": False,
        },
        "dxil_hazard_shader_records": hazard_shader_records,
        "msl_checks": msl_checks,
        "metallib_policy": metallib_policy,
        "errors": errors,
    }


def run_fresh_game(
    repo: Path,
    proof_dir: Path,
    wine: Path,
    wine_runtime: Path,
    prefix: Path,
    runtime_root: Path,
    staged_files: list[dict[str, Any]],
    corpus_tsv: Path,
    visible_frames: int,
) -> dict[str, Any]:
    sdk = repo / "tools" / "d3d12-metal-sdk"
    out_bin = sdk / "out" / "bin"
    exe = out_bin / "vkd3d_fresh_game.exe"
    run_dir = proof_dir / "phase1-visible-game"
    run_dir.mkdir(parents=True, exist_ok=True)

    dyld_parts = [
        str(runtime_root / "x86_64-unix"),
        str(wine_runtime / "lib" / "wine" / "x86_64-unix"),
    ]
    if os.environ.get("DYLD_LIBRARY_PATH"):
        dyld_parts.append(os.environ["DYLD_LIBRARY_PATH"])

    corpus_hash_validation = validate_fresh_game_corpus_tsv(corpus_tsv)
    corpus_input_dir = run_dir / "corpus-input"
    corpus_input_dir.mkdir(parents=True, exist_ok=True)
    copied_corpus_artifacts: list[dict[str, Any]] = []
    for artifact in [corpus_tsv, corpus_tsv.parent / "fresh-corpus-summary.json", corpus_tsv.parent / "fresh-corpus-manifest.json"]:
        if artifact.exists():
            destination = corpus_input_dir / artifact.name
            shutil.copy2(artifact, destination)
            copied_corpus_artifacts.append(
                {
                    "source": str(artifact),
                    "destination": str(destination),
                    "sha256": sha256(destination),
                    "size": destination.stat().st_size,
                }
            )

    dxil_artifacts = build_fresh_game_dxil(out_bin, run_dir, wine, prefix)
    dxil_hazard_artifacts = stage_elden_dxil_hazard_shaders(run_dir)
    waveops_artifacts = build_fresh_game_waveops_dxil(out_bin, run_dir, wine, prefix)

    shader_cache_dir = run_dir / "shader-cache-fresh"
    shader_cache_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DXMT_"):
            env.pop(key, None)
    env.update(
        {
            "WINEPREFIX": str(prefix),
            "WINEDLLPATH": str(runtime_root / "x86_64-windows"),
            "WINEDLLOVERRIDES": "d3d12,dxgi,dxgi_dxmt,winemetal=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
            "DYLD_LIBRARY_PATH": ":".join(dyld_parts),
            "DXMT_WINEMETAL_UNIXLIB": "winemetal.so",
            "DXMT_WINEMETAL_DEBUG": "1",
            "DXMT_LOG_LEVEL": "info",
            "DXMT_LOG_PATH": str(run_dir),
            "DXMT_SHADER_CACHE_PATH": str(shader_cache_dir),
            "DXMT_ASYNC_PIPELINE_COMPILE": "1",
            "DXMT_D3D12_PSO_WORKERS": "2",
            "DXMT_D3D12_TRACE": "1",
            "DXMT_D3D12_TRACE_STDERR": "1",
            "DXMT_D3D12_TRACE_MAX_MB": "128",
            "D3D12_METAL_SDK_PROFILE": "vkd3d-fresh-visible-game",
            "DXMT_D3D12_PRESENT_LOG_INTERVAL": "1",
            "VKD3D_FRESH_CORPUS_TSV": str(corpus_tsv),
            "VKD3D_FRESH_DXIL_VS": dxil_artifacts["vs_path"],
            "VKD3D_FRESH_DXIL_PS": dxil_artifacts["ps_path"],
            "VKD3D_FRESH_WAVEOPS_CS": waveops_artifacts["cs_path"],
            "VKD3D_FRESH_VISIBLE_FRAMES": str(visible_frames),
            "WINEDEBUG": "+loaddll",
        }
    )
    for index, entry in enumerate(dxil_hazard_artifacts.get("entries", [])):
        env[f"VKD3D_FRESH_DXIL_HAZARD_PS{index}"] = str(entry.get("destination", ""))
    cmd = [str(wine), exe.name]
    process_timeout_s = max(180, min(900, visible_frames * 5))
    process_timeout_ms = process_timeout_s * 1000
    timed_out = False
    proof_start_time = time.time()
    proof_start_mono = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            cwd=out_bin,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=process_timeout_s,
        )
    except subprocess.TimeoutExpired as error:
        timed_out = True
        timeout_stdout = error.stdout if isinstance(error.stdout, str) else (error.stdout or b"").decode("utf-8", "replace")
        timeout_stderr = error.stderr if isinstance(error.stderr, str) else (error.stderr or b"").decode("utf-8", "replace")
        timeout_stderr += (
            f"\nVKD3D proof timeout schema=metalsharp.vkd3d.proof-timeout.v1 "
            f"timeout_s={process_timeout_s} visible_frames={visible_frames}\n"
        )
        proc = subprocess.CompletedProcess(cmd, -9, timeout_stdout, timeout_stderr)
    proof_end_mono = time.monotonic()
    proof_end_time = time.time()
    process_elapsed_ms = int(round((proof_end_mono - proof_start_mono) * 1000.0))
    stdout_path = run_dir / "vkd3d_fresh_game.stdout.json"
    stderr_path = run_dir / "vkd3d_fresh_game.stderr.txt"
    stdout_path.write_text(proc.stdout)
    stderr_path.write_text(proc.stderr)
    diagnostic_log_paths = sorted(
        {
            *run_dir.glob("*dxmt-d3d12-trace.log"),
            *run_dir.glob("*dxmt-d3d12-pso.log"),
            *run_dir.glob("*d3d12.log"),
        }
    )
    diagnostic_log_records = [
        {"path": str(path), "size": path.stat().st_size, "sha256": sha256(path)}
        for path in diagnostic_log_paths
        if path.exists()
    ]
    diagnostic_log_text = merge_primary_log_text(proc.stderr, *(read_existing_text(path) for path in diagnostic_log_paths))

    parsed: dict[str, Any] | None = None
    parse_error = ""
    try:
        parsed = json.loads(proc.stdout)
    except json.JSONDecodeError as error:
        parse_error = str(error)
    loaddll_rows = parse_loaddll(proc.stderr)
    runtime_match = validate_loaddll_staged(loaddll_rows, staged_files)
    drawn_present_count = proc.stderr.count("classification=drawn")
    draw_line_count = len(re.findall(r"draws=[1-9]", proc.stderr))
    present_draw_counts = [
        int(match.group(1))
        for match in re.finditer(r"VKD3D present backbuffer work count=\d+ .*? draws=(\d+).*?classification=drawn", proc.stderr)
    ]
    present_indexed_counts = [
        int(match.group(1))
        for match in re.finditer(r"VKD3D present backbuffer work count=\d+ .*? indexed=(\d+).*?classification=drawn", proc.stderr)
    ]
    present_indirect_counts = [
        int(match.group(1))
        for match in re.finditer(r"VKD3D present backbuffer work count=\d+ .*? indirect=(\d+).*?classification=drawn", proc.stderr)
    ]
    native_vertex_resolved_count = len(re.findall(r"VKD3D native vertex path resolved", diagnostic_log_text))
    native_vertex_draw_indexed_resolved_count = len(
        re.findall(r"VKD3D native vertex path resolved draw=DrawIndexedInstanced", diagnostic_log_text)
    )
    native_vertex_execute_indirect_draw_indexed_resolved_count = len(
        re.findall(r"VKD3D native vertex path resolved draw=ExecuteIndirectDrawIndexed", diagnostic_log_text)
    )
    native_vertex_resolve_log_budget = 256
    native_vertex_resolved_required = min(visible_frames * 5, native_vertex_resolve_log_budget)
    native_vertex_draw_indexed_resolved_required = min(
        visible_frames * 4,
        (native_vertex_resolve_log_budget * 4) // 5,
    )
    native_vertex_execute_indirect_draw_indexed_resolved_required = min(
        visible_frames,
        native_vertex_resolve_log_budget // 5,
    )
    dxil_draw_encoded_count = len(re.findall(r"VKD3D swapchain DrawInstanced encoded v=3 i=1", diagnostic_log_text))
    dxil_vertex_pull_snapshot_count = len(
        re.findall(
            r"VKD3D vertex-pull snapshot draw=DrawInstanced v=3 i=1 .*?slot_mask=0x1.*?bound_vbs=1",
            diagnostic_log_text,
        )
    )
    dxil_draw_skipped = bool(
        re.search(r"VKD3D swapchain DrawInstanced skipped v=3\s+i=1|DrawInstanced\s+SKIPPED\s+v=3\s+i=1", diagnostic_log_text,
                  re.IGNORECASE)
    )
    indexed_draw_skipped = bool(
        re.search(r"VKD3D swapchain DrawIndexedInstanced skipped idx=6\s+inst=1|DrawIndexedInstanced\s+SKIPPED\s+idx=6\s+inst=1", diagnostic_log_text,
                  re.IGNORECASE)
    )
    indirect_draw_skipped = bool(
        re.search(r"VKD3D swapchain ExecuteIndirect DrawInstanced skipped v=24\s+i=1|ExecuteIndirectDraw\s+SKIPPED\s+v=24\s+i=1", diagnostic_log_text,
                  re.IGNORECASE)
    )
    execute_indirect_indexed_draw_skipped = bool(
        re.search(
            r"ExecuteIndirect DRAW_INDEXED SKIPPED idx=6\s+inst=1|"
            r"ExecuteIndirectDrawIndexed\s+SKIPPED\s+idx=6\s+inst=1|"
            r"VKD3D skipping unsafe ExecuteIndirectDrawIndexed",
            diagnostic_log_text,
            re.IGNORECASE,
        )
    )
    multi_slot_instance_root_draw_skipped = bool(
        re.search(
            r"DrawInstanced\s+SKIPPED\s+v=6\s+i=1.*start_inst=4|"
            r"VKD3D swapchain DrawInstanced skipped v=6\s+i=1.*start_inst=4|"
            r"VKD3D skipping unsafe DrawInstanced.*elems=6.*start_inst=4",
            diagnostic_log_text,
            re.IGNORECASE,
        )
    )
    tessellation_fallback_draw_skipped = bool(
        re.search(r"VKD3D swapchain DrawInstanced skipped v=51\s+i=1|DrawInstanced\s+SKIPPED\s+v=51\s+i=1", diagnostic_log_text,
                  re.IGNORECASE)
    )
    nanite_cluster_draw_skipped = bool(
        re.search(r"VKD3D swapchain ExecuteIndirect DrawInstanced skipped v=6\s+i=1|ExecuteIndirectDraw\s+SKIPPED\s+v=6\s+i=1", diagnostic_log_text,
                  re.IGNORECASE)
    )
    texture_array_srv_draw_skipped = bool(
        re.search(
            r"VKD3D swapchain DrawInstanced skipped v=30\s+i=1|DrawInstanced\s+SKIPPED\s+v=30\s+i=1",
            diagnostic_log_text,
            re.IGNORECASE,
        )
    )
    textured_3d_draw_skipped = bool(
        re.search(r"VKD3D swapchain DrawInstanced skipped v=33\s+i=1|DrawInstanced\s+SKIPPED\s+v=33\s+i=1", diagnostic_log_text,
                  re.IGNORECASE)
    )
    render_encoder_encode_failed = bool(re.search(r"VKD3D render encoder encode failed", diagnostic_log_text, re.IGNORECASE))
    stderr_assertion = "std::__condvar" in proc.stderr or "Assertion" in proc.stderr
    frames_presented = 0
    if parsed:
        frames_presented = int(parsed.get("d3d12_window", {}).get("frames_presented", 0) or 0)
    corpus_json = parsed.get("corpus", {}) if parsed else {}
    d3d12_json = parsed.get("d3d12_window", {}) if parsed else {}
    adapter_json = d3d12_json.get("adapter_report", {}) if d3d12_json else {}
    abi_semantics_json = d3d12_json.get("abi_semantics", {}) if d3d12_json else {}
    visible_scene_json = d3d12_json.get("visible_scene", {}) if d3d12_json else {}
    gpu_textures_json = d3d12_json.get("gpu_textures", {}) if d3d12_json else {}
    heap_alias_json = d3d12_json.get("heap_alias", {}) if d3d12_json else {}
    uav_barrier_json = d3d12_json.get("uav_barrier", {}) if d3d12_json else {}
    wave_ops_json = d3d12_json.get("wave_ops", {}) if d3d12_json else {}
    rtv_format_json = d3d12_json.get("rtv_format", {}) if d3d12_json else {}
    texture_array_subresources_json = d3d12_json.get("texture_array_subresources", {}) if d3d12_json else {}
    texture_array_srv_sample_json = d3d12_json.get("texture_array_srv_sample", {}) if d3d12_json else {}
    render_pass_json = d3d12_json.get("render_pass", {}) if d3d12_json else {}
    corpus_shader_json = d3d12_json.get("corpus_shader", {}) if d3d12_json else {}
    srv_sample_json = d3d12_json.get("srv_sample", {}) if d3d12_json else {}
    textured_3d_json = d3d12_json.get("textured_3d", {}) if d3d12_json else {}
    cbv_sample_json = d3d12_json.get("cbv_sample", {}) if d3d12_json else {}
    indexed_draw_json = d3d12_json.get("indexed_draw", {}) if d3d12_json else {}
    multi_slot_instance_root_json = d3d12_json.get("multi_slot_instance_root", {}) if d3d12_json else {}
    tessellation_fallback_json = d3d12_json.get("tessellation_fallback", {}) if d3d12_json else {}
    indirect_draw_json = d3d12_json.get("indirect_draw", {}) if d3d12_json else {}
    nanite_cluster_json = d3d12_json.get("nanite_cluster", {}) if d3d12_json else {}
    dxil_hazard_replay_json = d3d12_json.get("dxil_hazard_replay", {}) if d3d12_json else {}
    shader_cache_validation = validate_presented_shader_cache(shader_cache_dir, diagnostic_log_text, d3d12_json, visible_frames)
    hard_fail_gates = hard_fail_scan(diagnostic_log_text, shader_cache_dir)
    shader_replay_guardrail_result = shader_replay_guardrail(
        shader_cache_dir,
        shader_cache_validation,
        dxil_hazard_replay_json,
        hard_fail_gates,
    )
    write_json(run_dir / "shader_replay_guardrail.json", shader_replay_guardrail_result)
    texture_payload_bytes_required = 300 * 16 * 16 * 4
    rtv_format_expected_rgba = [32, 192, 96, 255]
    subresource_view_expected_slice0_rgba = [64, 48, 208, 255]
    subresource_view_expected_slice1_rgba = [208, 144, 64, 255]
    texture_array_srv_expected_slice0_rgba = [24, 96, 216, 255]
    texture_array_srv_expected_slice1_rgba = [216, 88, 40, 255]
    render_pass_expected_rgba = [224, 80, 176, 255]
    corpus_shader_expected_rgba = [64, 192, 32, 255]
    srv_sample_expected_rgba = [16, 144, 224, 255]
    cbv_sample_expected_rgba = [208, 48, 160, 255]
    indexed_draw_expected_rgba = [240, 200, 48, 255]
    tessellation_fallback_expected_rgba = [248, 96, 176, 255]
    indirect_draw_expected_rgba = [80, 224, 240, 255]
    nanite_cluster_expected_rgba = [176, 112, 232, 255]
    textured_3d_expected_rgba = [[232, 48, 56, 255], [48, 224, 96, 255], [64, 120, 240, 255]]
    textured_3d_require_all_faces = visible_frames >= 30
    textured_3d_faces = textured_3d_json.get("faces", []) if isinstance(textured_3d_json, dict) else []
    textured_3d_face_provenance = validate_textured_3d_face_provenance(corpus_tsv, textured_3d_faces)
    progressive_triangle_counts_raw = visible_scene_json.get("progressive_triangle_coverage_counts", [])
    progressive_triangle_counts = [
        int(value or 0) for value in progressive_triangle_counts_raw if isinstance(value, (int, float))
    ]
    progressive_triangle_counts_monotonic = all(
        progressive_triangle_counts[index] >= progressive_triangle_counts[index - 1]
        for index in range(1, len(progressive_triangle_counts))
    )
    progressive_triangle_seed_pixels = int(visible_scene_json.get("progressive_triangle_seed_pixels", 0) or 0)
    progressive_triangle_final_pixels = int(visible_scene_json.get("progressive_triangle_final_pixels", 0) or 0)
    progressive_triangle_ok = bool(
        visible_scene_json.get("progressive_triangle_proof_scope")
        == "progressive_rgb_vertex_triangle_seed_point_to_full_triangle"
        and visible_scene_json.get("progressive_triangle_ok") is True
        and visible_scene_json.get("progressive_triangle_seed_point_ok") is True
        and visible_scene_json.get("progressive_triangle_full_ok") is True
        and visible_scene_json.get("progressive_triangle_rgb_channels_present") is True
        and visible_scene_json.get("progressive_triangle_coverage_monotonic") is True
        and progressive_triangle_counts_monotonic
        and len(progressive_triangle_counts) == visible_frames
        and visible_frames >= 3
        and int(visible_scene_json.get("progressive_triangle_vertices_per_frame", 0) or 0) == 15
        and int(visible_scene_json.get("progressive_triangle_draw_calls", 0) or 0) == visible_frames
        and int(visible_scene_json.get("progressive_triangle_frames_validated", 0) or 0) == visible_frames
        and int(visible_scene_json.get("progressive_triangle_samples_checked", 0) or 0)
        == visible_frames * 144 * 112
        and progressive_triangle_counts[0] == progressive_triangle_seed_pixels
        and progressive_triangle_counts[-1] == progressive_triangle_final_pixels
        and 0 < progressive_triangle_seed_pixels <= 48
        and progressive_triangle_final_pixels >= 3000
        and progressive_triangle_final_pixels > progressive_triangle_seed_pixels * 100
        and int(visible_scene_json.get("progressive_triangle_peak_pixels", 0) or 0)
        == progressive_triangle_final_pixels
        and int(visible_scene_json.get("progressive_triangle_final_red_dominant_pixels", 0) or 0) >= 64
        and int(visible_scene_json.get("progressive_triangle_final_green_dominant_pixels", 0) or 0) >= 64
        and int(visible_scene_json.get("progressive_triangle_final_blue_dominant_pixels", 0) or 0) >= 64
    )
    tessellation_progressive_triangle_ok = bool(
        tessellation_fallback_json.get("progressive_triangle_proof_scope")
        == "progressive_rgb_tessellated_triangle_seed_point_to_full_triangle"
        and tessellation_fallback_json.get("progressive_triangle_ok") is True
        and tessellation_fallback_json.get("progressive_triangle_seed_point_ok") is True
        and tessellation_fallback_json.get("progressive_triangle_full_ok") is True
        and tessellation_fallback_json.get("progressive_triangle_rgb_channels_present") is True
        and tessellation_fallback_json.get("progressive_triangle_coverage_monotonic") is True
        and int(tessellation_fallback_json.get("progressive_triangle_frames_validated", 0) or 0) == visible_frames
        and len(tessellation_fallback_json.get("progressive_triangle_coverage_counts", [])) == visible_frames
        and int(tessellation_fallback_json.get("progressive_triangle_seed_pixels", 0) or 0) > 0
        and int(tessellation_fallback_json.get("progressive_triangle_seed_pixels", 0) or 0) < 96
        and int(tessellation_fallback_json.get("progressive_triangle_final_pixels", 0) or 0) >= 1800
        and int(tessellation_fallback_json.get("progressive_triangle_final_pixels", 0) or 0)
        > int(tessellation_fallback_json.get("progressive_triangle_seed_pixels", 0) or 0) * 8
    )
    textured_3d_ok = bool(
        textured_3d_json.get("ok") is True
        and textured_3d_json.get("present_ok") is True
        and textured_3d_json.get("proof_scope") == "rotating_rgb_triangular_pyramid_three_engine_texture_faces_depth_presented_readback"
        and textured_3d_json.get("D3DCompile_loaded") is True
        and textured_3d_json.get("vs_5_0_rotating_3d") == "0x00000000"
        and textured_3d_json.get("ps_5_0_three_Texture2D_Sample") == "0x00000000"
        and textured_3d_json.get("D3D12SerializeRootSignature_three_srv_table_static_sampler") == "0x00000000"
        and textured_3d_json.get("CreateRootSignature") == "0x00000000"
        and textured_3d_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and textured_3d_json.get("CreateVertexBuffer") == "0x00000000"
        and textured_3d_json.get("CreateDepthTexture_D32_FLOAT_ALLOW_DEPTH_STENCIL") == "0x00000000"
        and textured_3d_json.get("CreateDescriptorHeap_DSV") == "0x00000000"
        and textured_3d_json.get("depth_stencil_policy") == "D32_FLOAT_depth_only_stencil_invalid_expected"
        and textured_3d_json.get("CreateTexture2D_R8G8B8A8_UNORM") == ["0x00000000"] * 3
        and textured_3d_json.get("CreateUploadBuffer") == ["0x00000000"] * 3
        and textured_3d_json.get("CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE") == "0x00000000"
        and int(textured_3d_json.get("face_count", 0) or 0) == 3
        and int(textured_3d_json.get("unique_families", 0) or 0) == 3
        and int(textured_3d_json.get("textures_created", 0) or 0) == 3
        and int(textured_3d_json.get("upload_buffers_created", 0) or 0) == 3
        and int(textured_3d_json.get("uploads_filled", 0) or 0) == 3
        and int(textured_3d_json.get("CreateShaderResourceView_descriptors", 0) or 0) == 3
        and int(textured_3d_json.get("CreateDepthStencilView_descriptors", 0) or 0) == 1
        and int(textured_3d_json.get("ClearDepthStencilView_commands", 0) or 0) == visible_frames
        and textured_3d_json.get("depth_overlap_policy") == "front_unity_drawn_first_back_unreal_drawn_last_requires_D32_depth"
        and int(textured_3d_json.get("CopyTextureRegion_commands", 0) or 0) == 3
        and int(textured_3d_json.get("transition_barriers", 0) or 0) == 3
        and textured_3d_json.get("fence_wait_ok") is True
        and int(textured_3d_json.get("vertices_created", 0) or 0) == 33
        and int(textured_3d_json.get("vertices_per_draw", 0) or 0) == 33
        and int(textured_3d_json.get("vertex_buffer_updates", 0) or 0) == visible_frames
        and int(textured_3d_json.get("root_constant_sets", 0) or 0) == 0
        and int(textured_3d_json.get("draw_calls", 0) or 0) == visible_frames
        and int(textured_3d_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(textured_3d_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(textured_3d_json.get("depth_overlap_sample_x", 0) or 0) == 704
        and int(textured_3d_json.get("depth_overlap_sample_y", 0) or 0) == 156
        and textured_3d_json.get("depth_overlap_expected_rgba") == [48, 224, 96, 255]
        and textured_3d_json.get("depth_overlap_present_rgba") == [48, 224, 96, 255]
        and int(textured_3d_json.get("depth_overlap_samples_checked", 0) or 0) == visible_frames
        and int(textured_3d_json.get("depth_overlap_sample_matches", 0) or 0) == visible_frames
        and len(textured_3d_faces) == 3
        and [int(face.get("index", -1)) for face in textured_3d_faces] == [0, 1, 2]
        and [face.get("family") for face in textured_3d_faces] == ["unreal", "unity-sdk", "microsoft-sdk"]
        and textured_3d_face_provenance["ok"]
        and all(str(face.get("sha256", "")) for face in textured_3d_faces)
        and all(str(face.get("file_fnv1a64", "")) not in ("", "0000000000000000") for face in textured_3d_faces)
        and all(str(face.get("upload_fnv1a64", "")) not in ("", "0000000000000000") for face in textured_3d_faces)
        and all(int(face.get("declared_size", 0) or 0) > 0 for face in textured_3d_faces)
        and all(int(face.get("bytes_from_file", 0) or 0) > 0 for face in textured_3d_faces)
        and all(face.get("expected_rgba") == textured_3d_expected_rgba[int(face.get("index", -1))] for face in textured_3d_faces)
        and all(
            int(face.get("samples_checked", 0) or 0) == 0 or face.get("present_rgba") == face.get("expected_rgba")
            for face in textured_3d_faces
        )
        and sum(int(face.get("samples_checked", 0) or 0) for face in textured_3d_faces) == visible_frames
        and sum(int(face.get("sample_matches", 0) or 0) for face in textured_3d_faces) == visible_frames
        and (
            not textured_3d_require_all_faces
            or all(int(face.get("samples_checked", 0) or 0) > 0 for face in textured_3d_faces)
        )
        and (
            not textured_3d_require_all_faces
            or all(int(face.get("sample_matches", 0) or 0) > 0 for face in textured_3d_faces)
        )
    )
    game_json_ok = bool(
        parsed
        and parsed.get("pass") is True
        and corpus_json.get("ok") is True
        and int(corpus_json.get("texture_payloads_captured", 0) or 0) >= 300
        and int(corpus_json.get("texture_payload_bytes_from_files", 0) or 0) >= texture_payload_bytes_required
        and d3d12_json.get("ok") is True
        and adapter_json.get("ok") is True
        and adapter_json.get("EnumAdapters1") == "0x00000000"
        and adapter_json.get("GetDesc1") == "0x00000000"
        and int(adapter_json.get("vendor_id", 0) or 0) != 0
        and (int(adapter_json.get("dedicated_video_memory", 0) or 0) + int(adapter_json.get("shared_system_memory", 0) or 0)) > 0
        and adapter_json.get("adapter_luid_nonzero") is True
        and adapter_json.get("device_luid_nonzero") is True
        and adapter_json.get("luid_matches_device") is True
        and abi_semantics_json.get("ok") is True
        and abi_semantics_json.get("proof_scope") == "guid_com_abi_queryinterface_private_data_tied_to_presented_swapchain_run"
        and abi_semantics_json.get("IID_ID3D12Device") == "189819f1-1db6-4b57-be54-1821339b85f7"
        and abi_semantics_json.get("IID_IDXGIFactory4") == "1bc6ea02-ef36-464f-bf0c-21ca39e5168a"
        and abi_semantics_json.get("IID_IDXGIAdapter1") == "29038f61-3839-4626-91fd-086879011a05"
        and abi_semantics_json.get("IID_ID3D12CommandQueue") == "0ec870a6-5d7e-4c22-8cfc-5baae07616ed"
        and abi_semantics_json.get("IID_ID3D12CommandAllocator") == "6102dee4-af59-4b09-b999-b44d73f09b24"
        and abi_semantics_json.get("IID_ID3D12GraphicsCommandList") == "5b160d0f-ac1b-4185-8ba8-b3ae42a5a455"
        and abi_semantics_json.get("IID_ID3D12Fence") == "0a753dcf-c4d8-4b91-adf6-be5a60d95a76"
        and abi_semantics_json.get("IID_IDXGISwapChain3") == "94d99bdb-f1f8-4ab0-b236-7da0170edab1"
        and abi_semantics_json.get("guid_constants_ok") is True
        and abi_semantics_json.get("query_interface_ok") is True
        and abi_semantics_json.get("vtable_layout_ok") is True
        and abi_semantics_json.get("device_child_get_device_ok") is True
        and abi_semantics_json.get("device_child_identity_ok") is True
        and abi_semantics_json.get("private_data_copy_semantics") == "caller_buffer_mutated_after_SetPrivateData_before_GetPrivateData"
        and abi_semantics_json.get("private_data_roundtrip_ok") is True
        and abi_semantics_json.get("present_tie_ok") is True
        and int(abi_semantics_json.get("validated_frames", 0) or 0) == visible_frames
        and int(abi_semantics_json.get("private_data_size", 0) or 0) == 16
        and all(
            abi_semantics_json.get(key) == "0x00000000"
            for key in [
                "QueryInterface_IUnknown_device",
                "QueryInterface_ID3D12Device",
                "QueryInterface_IUnknown_factory",
                "QueryInterface_IUnknown_adapter",
                "QueryInterface_ID3D12CommandQueue",
                "QueryInterface_IUnknown_queue",
                "QueryInterface_ID3D12CommandAllocator",
                "QueryInterface_ID3D12GraphicsCommandList",
                "QueryInterface_ID3D12Fence",
                "QueryInterface_IDXGISwapChain3",
                "GetDevice_from_queue",
                "GetDevice_from_allocator",
                "GetDevice_from_list",
                "GetDevice_from_fence",
                "QueryInterface_IUnknown_queue_device",
                "QueryInterface_IUnknown_allocator_device",
                "QueryInterface_IUnknown_list_device",
                "QueryInterface_IUnknown_fence_device",
                "SetPrivateData",
                "GetPrivateData",
            ]
        )
        and visible_scene_json.get("ok") is True
        and visible_scene_json.get("present_ok") is True
        and visible_scene_json.get("sm5_stamp_source") == "DXBC_SM5_DYNAMIC_STAMP_SENTINEL_OVERWRITE"
        and int(visible_scene_json.get("sm5_stamp_quads_per_frame", 0) or 0) == 2
        and int(visible_scene_json.get("sm5_stamp_samples_checked", 0) or 0) == visible_frames
        and int(visible_scene_json.get("sm5_stamp_matches", 0) or 0) == visible_frames
        and progressive_triangle_ok
        and d3d12_json.get("dxil_scene", {}).get("ok") is True
        and d3d12_json.get("dxil_scene", {}).get("CreateDxilVertexBuffer") == "0x00000000"
        and d3d12_json.get("dxil_scene", {}).get("vertex_source") == "POSITION_NONDEGENERATE_COLOR_VERTEX_BUFFER_PS_SCALAR_VECTOR_SEMANTICS"
        and int(d3d12_json.get("dxil_scene", {}).get("draw_calls", 0) or 0) == visible_frames
        and int(d3d12_json.get("dxil_scene", {}).get("vertices_per_draw", 0) or 0) == 3
        and dxil_hazard_replay_json.get("ok") is True
        and dxil_hazard_replay_json.get("proof_scope") == "exact_elden_dxil_pixel_shader_pso_replay"
        and int(dxil_hazard_replay_json.get("requested_count", 0) or 0) == len(ELDEN_DXIL_HAZARD_PIXEL_STEMS)
        and int(dxil_hazard_replay_json.get("replay_count", 0) or 0) == len(ELDEN_DXIL_HAZARD_PIXEL_STEMS)
        and int(dxil_hazard_replay_json.get("success_count", 0) or 0) == len(ELDEN_DXIL_HAZARD_PIXEL_STEMS)
        and d3d12_json.get("dxil_readback", {}).get("ok") is True
        and d3d12_json.get("dxil_readback", {}).get("CreateReadbackBuffer") == "0x00000000"
        and int(d3d12_json.get("dxil_readback", {}).get("copy_commands", 0) or 0) == visible_frames
        and int(d3d12_json.get("dxil_readback", {}).get("sentinel_writes", 0) or 0) == visible_frames
        and int(d3d12_json.get("dxil_readback", {}).get("samples_checked", 0) or 0) == visible_frames
        and int(d3d12_json.get("dxil_readback", {}).get("semantic_samples", 0) or 0) == visible_frames
        and d3d12_json.get("dxil_readback", {}).get("expected_rgba") == [143, 128, 191, 191]
        and gpu_textures_json.get("ok") is True
        and gpu_textures_json.get("present_ok") is True
        and int(gpu_textures_json.get("texture_payloads_uploaded", 0) or 0) >= 300
        and int(gpu_textures_json.get("texture_payload_bytes_from_files", 0) or 0) >= texture_payload_bytes_required
        and int(gpu_textures_json.get("present_backbuffer_sentinel_copies", 0) or 0) == visible_frames
        and int(gpu_textures_json.get("present_copy_commands", 0) or 0) == visible_frames
        and int(gpu_textures_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(gpu_textures_json.get("present_sample_matches", 0) or 0) == visible_frames
        and gpu_textures_json.get("present_rgba") == gpu_textures_json.get("present_expected_rgba")
        and heap_alias_json.get("ok") is True
        and heap_alias_json.get("present_ok") is True
        and heap_alias_json.get("CreateHeap") == "0x00000000"
        and heap_alias_json.get("CreatePlacedResourceA") == "0x00000000"
        and heap_alias_json.get("CreatePlacedResourceB") == "0x00000000"
        and int(heap_alias_json.get("gpu_virtual_address_a", 0) or 0) != 0
        and int(heap_alias_json.get("gpu_virtual_address_b", 0) or 0) != 0
        and heap_alias_json.get("gpu_virtual_addresses_match") is True
        and int(heap_alias_json.get("copy_before_alias_commands", 0) or 0) == 1
        and int(heap_alias_json.get("aliasing_barriers", 0) or 0) == 1
        and int(heap_alias_json.get("copy_alias_overlap_commands", 0) or 0) == 1
        and int(heap_alias_json.get("copy_after_alias_commands", 0) or 0) == 1
        and int(heap_alias_json.get("transition_barriers", 0) or 0) == 4
        and heap_alias_json.get("readback_before_alias_ok") is True
        and heap_alias_json.get("readback_alias_overlap_ok") is True
        and heap_alias_json.get("readback_after_alias_ok") is True
        and int(heap_alias_json.get("present_copy_commands", 0) or 0) == visible_frames
        and int(heap_alias_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(heap_alias_json.get("present_sample_matches", 0) or 0) == visible_frames
        and heap_alias_json.get("present_rgba") == heap_alias_json.get("present_expected_rgba")
        and uav_barrier_json.get("ok") is True
        and uav_barrier_json.get("present_ok") is True
        and uav_barrier_json.get("proof_scope") == "dependent_uav_dispatch_visibility_with_explicit_uav_barriers"
        and uav_barrier_json.get("D3DCompile_loaded") is True
        and uav_barrier_json.get("CSWrite_cs_5_0") == "0x00000000"
        and uav_barrier_json.get("CSTransform_cs_5_0") == "0x00000000"
        and uav_barrier_json.get("D3D12SerializeRootSignature") == "0x00000000"
        and uav_barrier_json.get("CreateRootSignature") == "0x00000000"
        and uav_barrier_json.get("CreateComputePipelineStateWrite") == "0x00000000"
        and uav_barrier_json.get("CreateComputePipelineStateTransform") == "0x00000000"
        and uav_barrier_json.get("CreateUavBuffer") == "0x00000000"
        and uav_barrier_json.get("fixed_footprint_ok") is True
        and int(uav_barrier_json.get("row_pitch", 0) or 0) == 256
        and int(uav_barrier_json.get("footprint_bytes", 0) or 0) >= 4096
        and int(uav_barrier_json.get("uav_gpu_virtual_address", 0) or 0) != 0
        and int(uav_barrier_json.get("root_uav_sets", 0) or 0) == 2
        and int(uav_barrier_json.get("root_constant_sets", 0) or 0) == 0
        and int(uav_barrier_json.get("dispatch_commands", 0) or 0) == 2
        and int(uav_barrier_json.get("dispatch_write_commands", 0) or 0) == 1
        and int(uav_barrier_json.get("dispatch_read_transform_commands", 0) or 0) == 1
        and int(uav_barrier_json.get("dispatch_x", 0) or 0) == 16
        and int(uav_barrier_json.get("dispatch_y", 0) or 0) == 16
        and int(uav_barrier_json.get("uav_barriers", 0) or 0) == 2
        and int(uav_barrier_json.get("transition_barriers", 0) or 0) == 1
        and uav_barrier_json.get("compute_readback_ok") is True
        and int(uav_barrier_json.get("compute_pixels_checked", 0) or 0) == 256
        and int(uav_barrier_json.get("compute_pixel_matches", 0) or 0) == 256
        and int(uav_barrier_json.get("present_copy_commands", 0) or 0) == visible_frames
        and int(uav_barrier_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(uav_barrier_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(uav_barrier_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(uav_barrier_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and uav_barrier_json.get("present_rgba") == uav_barrier_json.get("present_expected_rgba")
        and wave_ops_json.get("ok") is True
        and wave_ops_json.get("present_ok") is True
        and wave_ops_json.get("proof_scope") == "sm6_dxil_waveops_compute_uav_presented_readback"
        and wave_ops_json.get("cs_loaded") is True
        and int(wave_ops_json.get("cs_bytes", 0) or 0) > 0
        and str(wave_ops_json.get("cs_fnv1a64", "")) not in ("", "0000000000000000")
        and wave_ops_json.get("CheckFeatureSupport_OPTIONS1") == "0x00000000"
        and wave_ops_json.get("wave_ops_reported") is True
        and int(wave_ops_json.get("wave_lane_count_min", 0) or 0) == 32
        and int(wave_ops_json.get("wave_lane_count_max", 0) or 0) == 32
        and wave_ops_json.get("D3D12SerializeRootSignature") == "0x00000000"
        and wave_ops_json.get("CreateRootSignature") == "0x00000000"
        and wave_ops_json.get("CreateComputePipelineStateWaveOps") == "0x00000000"
        and wave_ops_json.get("CreateUavBuffer") == "0x00000000"
        and wave_ops_json.get("CreateUavDescriptorHeap") == "0x00000000"
        and int(wave_ops_json.get("CreateUnorderedAccessView_descriptors", 0) or 0) == 1
        and wave_ops_json.get("CreateReadback") == "0x00000000"
        and wave_ops_json.get("CloseCommandList") == "0x00000000"
        and wave_ops_json.get("SignalFence") == "0x00000000"
        and wave_ops_json.get("MapReadback") == "0x00000000"
        and wave_ops_json.get("fixed_footprint_ok") is True
        and int(wave_ops_json.get("row_pitch", 0) or 0) == 256
        and int(wave_ops_json.get("footprint_bytes", 0) or 0) >= 4096
        and int(wave_ops_json.get("uav_descriptor_gpu_handle", 0) or 0) != 0
        and int(wave_ops_json.get("root_uav_sets", 0) or 0) == 1
        and int(wave_ops_json.get("dispatch_commands", 0) or 0) == 1
        and wave_ops_json.get("dispatch_groups") == [8, 1, 1]
        and int(wave_ops_json.get("uav_barriers", 0) or 0) == 1
        and int(wave_ops_json.get("transition_barriers", 0) or 0) == 1
        and int(wave_ops_json.get("compute_pixels_checked", 0) or 0) == 256
        and int(wave_ops_json.get("compute_pixel_matches", 0) or 0) == 256
        and wave_ops_json.get("compute_first_rgba") == [0, 32, 5, 14]
        and wave_ops_json.get("compute_center_rgba") == [8, 32, 5, 14]
        and wave_ops_json.get("compute_last_rgba") == [31, 32, 5, 14]
        and wave_ops_json.get("compute_readback_ok") is True
        and int(wave_ops_json.get("present_copy_commands", 0) or 0) == visible_frames
        and int(wave_ops_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(wave_ops_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(wave_ops_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(wave_ops_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and wave_ops_json.get("present_first_rgba") == [0, 32, 5, 14]
        and wave_ops_json.get("present_center_rgba") == [8, 32, 5, 14]
        and wave_ops_json.get("present_last_rgba") == [31, 32, 5, 14]
        and wave_ops_json.get("fence_wait_ok") is True
        and rtv_format_json.get("ok") is True
        and rtv_format_json.get("present_ok") is True
        and rtv_format_json.get("proof_scope") == "offscreen_r8g8b8a8_unorm_rtv_clear_presented_copy_readback"
        and rtv_format_json.get("CreateTexture2D_R8G8B8A8_UNORM_ALLOW_RENDER_TARGET") == "0x00000000"
        and rtv_format_json.get("CreateRtvDescriptorHeap") == "0x00000000"
        and rtv_format_json.get("CreateReadback") == "0x00000000"
        and int(rtv_format_json.get("CreateRenderTargetView_descriptors", 0) or 0) == 1
        and int(rtv_format_json.get("ClearRenderTargetView_commands", 0) or 0) == 1
        and int(rtv_format_json.get("transition_barriers", 0) or 0) == 1
        and rtv_format_json.get("fixed_footprint_ok") is True
        and int(rtv_format_json.get("row_pitch", 0) or 0) == 256
        and int(rtv_format_json.get("footprint_bytes", 0) or 0) >= 4096
        and rtv_format_json.get("offscreen_readback_ok") is True
        and int(rtv_format_json.get("offscreen_pixels_checked", 0) or 0) == 256
        and int(rtv_format_json.get("offscreen_pixel_matches", 0) or 0) == 256
        and int(rtv_format_json.get("present_copy_commands", 0) or 0) == visible_frames
        and int(rtv_format_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(rtv_format_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(rtv_format_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(rtv_format_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and rtv_format_json.get("expected_rgba") == rtv_format_expected_rgba
        and rtv_format_json.get("offscreen_first_rgba") == rtv_format_expected_rgba
        and rtv_format_json.get("offscreen_last_rgba") == rtv_format_expected_rgba
        and rtv_format_json.get("present_rgba") == rtv_format_expected_rgba
        and rtv_format_json.get("present_last_rgba") == rtv_format_expected_rgba
        and texture_array_subresources_json.get("ok") is True
        and texture_array_subresources_json.get("present_ok") is True
        and texture_array_subresources_json.get("proof_scope")
        == "texture2darray_subresource_upload_readback_slice1_presented_copy"
        and texture_array_subresources_json.get("CreateTexture2DArray_R8G8B8A8_UNORM") == "0x00000000"
        and texture_array_subresources_json.get("CreateUploadBuffer") == "0x00000000"
        and texture_array_subresources_json.get("CreateReadback") == "0x00000000"
        and texture_array_subresources_json.get("CreateDescriptorHeap_CBV_SRV_UAV") == "0x00000000"
        and texture_array_subresources_json.get("MapUpload") == "0x00000000"
        and texture_array_subresources_json.get("CloseCommandList") == "0x00000000"
        and texture_array_subresources_json.get("SignalFence") == "0x00000000"
        and texture_array_subresources_json.get("MapReadback") == "0x00000000"
        and int(texture_array_subresources_json.get("slice_count", 0) or 0) == 2
        and int(texture_array_subresources_json.get("footprint_bytes", 0) or 0) >= 8192
        and texture_array_subresources_json.get("row_pitch") == [256, 256]
        and isinstance(texture_array_subresources_json.get("slice_offsets"), list)
        and len(texture_array_subresources_json.get("slice_offsets", [])) == 2
        and int(texture_array_subresources_json.get("slice_offsets", [0, 0])[1] or 0)
        > int(texture_array_subresources_json.get("slice_offsets", [0, 0])[0] or 0)
        and texture_array_subresources_json.get("fixed_footprints_ok") is True
        and int(texture_array_subresources_json.get("CreateShaderResourceView_descriptors_inventory_only", 0) or 0)
        == 2
        and int(texture_array_subresources_json.get("upload_subresources_filled", 0) or 0) == 2
        and int(texture_array_subresources_json.get("readback_sentinel_fills", 0) or 0) == 2
        and int(texture_array_subresources_json.get("copy_upload_subresources", 0) or 0) == 2
        and int(texture_array_subresources_json.get("copy_readback_subresources", 0) or 0) == 2
        and int(texture_array_subresources_json.get("transition_barriers", 0) or 0) == 1
        and texture_array_subresources_json.get("subresource_readback_ok") is True
        and int(texture_array_subresources_json.get("subresource_pixels_checked", 0) or 0) == 512
        and int(texture_array_subresources_json.get("subresource_pixel_matches", 0) or 0) == 512
        and texture_array_subresources_json.get("expected_slice0_rgba") == subresource_view_expected_slice0_rgba
        and texture_array_subresources_json.get("expected_slice1_rgba") == subresource_view_expected_slice1_rgba
        and texture_array_subresources_json.get("slice0_first_rgba") == subresource_view_expected_slice0_rgba
        and texture_array_subresources_json.get("slice0_last_rgba") == subresource_view_expected_slice0_rgba
        and texture_array_subresources_json.get("slice1_first_rgba") == subresource_view_expected_slice1_rgba
        and texture_array_subresources_json.get("slice1_last_rgba") == subresource_view_expected_slice1_rgba
        and int(texture_array_subresources_json.get("present_copy_commands", 0) or 0) == visible_frames
        and int(texture_array_subresources_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(texture_array_subresources_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(texture_array_subresources_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(texture_array_subresources_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and texture_array_subresources_json.get("present_rgba") == subresource_view_expected_slice1_rgba
        and texture_array_subresources_json.get("present_last_rgba") == subresource_view_expected_slice1_rgba
        and texture_array_srv_sample_json.get("ok") is True
        and texture_array_srv_sample_json.get("present_ok") is True
        and texture_array_srv_sample_json.get("proof_scope")
        == "texture2darray_srv_descriptor_table_slice1_sample_presented_readback"
        and texture_array_srv_sample_json.get("D3DCompile_loaded") is True
        and texture_array_srv_sample_json.get("vs_5_0") == "0x00000000"
        and texture_array_srv_sample_json.get("ps_5_0_Texture2DArray_Sample") == "0x00000000"
        and texture_array_srv_sample_json.get("D3D12SerializeRootSignature_descriptor_table_static_sampler")
        == "0x00000000"
        and texture_array_srv_sample_json.get("CreateRootSignature") == "0x00000000"
        and texture_array_srv_sample_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and texture_array_srv_sample_json.get("CreateVertexBuffer") == "0x00000000"
        and texture_array_srv_sample_json.get("CreateTexture2DArray_R8G8B8A8_UNORM") == "0x00000000"
        and texture_array_srv_sample_json.get("CreateUploadBuffer") == "0x00000000"
        and texture_array_srv_sample_json.get("CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE") == "0x00000000"
        and int(texture_array_srv_sample_json.get("slice_count", 0) or 0) == 2
        and int(texture_array_srv_sample_json.get("footprint_bytes", 0) or 0) >= 8192
        and texture_array_srv_sample_json.get("row_pitch") == [256, 256]
        and isinstance(texture_array_srv_sample_json.get("slice_offsets"), list)
        and len(texture_array_srv_sample_json.get("slice_offsets", [])) == 2
        and int(texture_array_srv_sample_json.get("slice_offsets", [0, 0])[1] or 0)
        > int(texture_array_srv_sample_json.get("slice_offsets", [0, 0])[0] or 0)
        and texture_array_srv_sample_json.get("fixed_footprints_ok") is True
        and int(texture_array_srv_sample_json.get("CreateShaderResourceView_Texture2DArray_descriptors", 0) or 0)
        == 1
        and int(texture_array_srv_sample_json.get("upload_subresources_filled", 0) or 0) == 2
        and int(texture_array_srv_sample_json.get("CopyTextureRegion_commands", 0) or 0) == 2
        and int(texture_array_srv_sample_json.get("transition_barriers", 0) or 0) == 1
        and texture_array_srv_sample_json.get("fence_wait_ok") is True
        and int(texture_array_srv_sample_json.get("draw_calls", 0) or 0) == visible_frames
        and int(texture_array_srv_sample_json.get("vertices_per_draw", 0) or 0) == 30
        and int(texture_array_srv_sample_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(texture_array_srv_sample_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(texture_array_srv_sample_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(texture_array_srv_sample_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and texture_array_srv_sample_json.get("expected_slice0_rgba") == texture_array_srv_expected_slice0_rgba
        and texture_array_srv_sample_json.get("expected_slice1_rgba") == texture_array_srv_expected_slice1_rgba
        and texture_array_srv_sample_json.get("present_rgba") == texture_array_srv_expected_slice1_rgba
        and texture_array_srv_sample_json.get("present_last_rgba") == texture_array_srv_expected_slice1_rgba
        and render_pass_json.get("ok") is True
        and render_pass_json.get("present_ok") is True
        and render_pass_json.get("proof_scope") == "offscreen_render_pass_clear_presented_copy_readback"
        and render_pass_json.get("QueryInterface_ID3D12GraphicsCommandList4") == "0x00000000"
        and render_pass_json.get("CreateTexture2D_R8G8B8A8_UNORM_ALLOW_RENDER_TARGET") == "0x00000000"
        and render_pass_json.get("CreateRtvDescriptorHeap") == "0x00000000"
        and render_pass_json.get("CreateReadback") == "0x00000000"
        and int(render_pass_json.get("CreateRenderTargetView_descriptors", 0) or 0) == 1
        and int(render_pass_json.get("BeginRenderPass_commands", 0) or 0) == 1
        and int(render_pass_json.get("EndRenderPass_commands", 0) or 0) == 1
        and int(render_pass_json.get("transition_barriers", 0) or 0) == 1
        and render_pass_json.get("fixed_footprint_ok") is True
        and int(render_pass_json.get("row_pitch", 0) or 0) == 256
        and int(render_pass_json.get("footprint_bytes", 0) or 0) >= 4096
        and render_pass_json.get("offscreen_readback_ok") is True
        and int(render_pass_json.get("offscreen_pixels_checked", 0) or 0) == 256
        and int(render_pass_json.get("offscreen_pixel_matches", 0) or 0) == 256
        and int(render_pass_json.get("present_copy_commands", 0) or 0) == visible_frames
        and int(render_pass_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(render_pass_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(render_pass_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(render_pass_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and render_pass_json.get("expected_rgba") == render_pass_expected_rgba
        and render_pass_json.get("offscreen_first_rgba") == render_pass_expected_rgba
        and render_pass_json.get("offscreen_last_rgba") == render_pass_expected_rgba
        and render_pass_json.get("present_rgba") == render_pass_expected_rgba
        and render_pass_json.get("present_last_rgba") == render_pass_expected_rgba
        and corpus_shader_json.get("ok") is True
        and corpus_shader_json.get("present_ok") is True
        and corpus_shader_json.get("proof_scope") == "fresh_corpus_position_color_hlsl_graphics_pso_presented_readback"
        and str(corpus_shader_json.get("vs_path", "")).endswith("D3D12StateObjectDatabase/src/PositionColorVS.hlsl")
        and str(corpus_shader_json.get("ps_path", "")).endswith("D3D12StateObjectDatabase/src/PositionColorPS.hlsl")
        and corpus_shader_json.get("vs_loaded") is True
        and corpus_shader_json.get("ps_loaded") is True
        and int(corpus_shader_json.get("vs_bytes", 0) or 0) > 0
        and int(corpus_shader_json.get("ps_bytes", 0) or 0) > 0
        and str(corpus_shader_json.get("vs_fnv1a64", "")) not in ("", "0000000000000000")
        and str(corpus_shader_json.get("ps_fnv1a64", "")) not in ("", "0000000000000000")
        and corpus_shader_json.get("D3DCompile_loaded") is True
        and corpus_shader_json.get("PositionColorVS_vs_5_0") == "0x00000000"
        and corpus_shader_json.get("PositionColorPS_ps_5_0") == "0x00000000"
        and corpus_shader_json.get("D3D12SerializeRootSignature") == "0x00000000"
        and corpus_shader_json.get("CreateRootSignature") == "0x00000000"
        and corpus_shader_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and corpus_shader_json.get("CreateVertexBuffer") == "0x00000000"
        and int(corpus_shader_json.get("draw_calls", 0) or 0) == visible_frames
        and int(corpus_shader_json.get("vertices_per_draw", 0) or 0) == 6
        and int(corpus_shader_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(corpus_shader_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(corpus_shader_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(corpus_shader_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and corpus_shader_json.get("expected_rgba") == corpus_shader_expected_rgba
        and corpus_shader_json.get("present_rgba") == corpus_shader_expected_rgba
        and corpus_shader_json.get("present_last_rgba") == corpus_shader_expected_rgba
        and srv_sample_json.get("ok") is True
        and srv_sample_json.get("present_ok") is True
        and srv_sample_json.get("proof_scope") == "shader_visible_srv_descriptor_table_texture2d_sample_presented_readback"
        and srv_sample_json.get("D3DCompile_loaded") is True
        and srv_sample_json.get("vs_5_0") == "0x00000000"
        and srv_sample_json.get("ps_5_0_Texture2D_Sample") == "0x00000000"
        and srv_sample_json.get("D3D12SerializeRootSignature_descriptor_table_static_sampler") == "0x00000000"
        and srv_sample_json.get("CreateRootSignature") == "0x00000000"
        and srv_sample_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and srv_sample_json.get("CreateVertexBuffer") == "0x00000000"
        and srv_sample_json.get("CreateTexture2D_R8G8B8A8_UNORM") == "0x00000000"
        and srv_sample_json.get("CreateUploadBuffer") == "0x00000000"
        and srv_sample_json.get("CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE") == "0x00000000"
        and int(srv_sample_json.get("CreateShaderResourceView_descriptors", 0) or 0) == 1
        and int(srv_sample_json.get("CopyTextureRegion_commands", 0) or 0) == 1
        and int(srv_sample_json.get("transition_barriers", 0) or 0) == 1
        and srv_sample_json.get("upload_filled") is True
        and srv_sample_json.get("fence_wait_ok") is True
        and int(srv_sample_json.get("draw_calls", 0) or 0) == visible_frames
        and int(srv_sample_json.get("vertices_per_draw", 0) or 0) == 12
        and int(srv_sample_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(srv_sample_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(srv_sample_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(srv_sample_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and srv_sample_json.get("expected_rgba") == srv_sample_expected_rgba
        and srv_sample_json.get("present_rgba") == srv_sample_expected_rgba
        and srv_sample_json.get("present_last_rgba") == srv_sample_expected_rgba
        and textured_3d_ok
        and cbv_sample_json.get("ok") is True
        and cbv_sample_json.get("present_ok") is True
        and cbv_sample_json.get("proof_scope") == "shader_visible_cbv_descriptor_table_constant_buffer_presented_readback"
        and cbv_sample_json.get("D3DCompile_loaded") is True
        and cbv_sample_json.get("vs_5_0") == "0x00000000"
        and cbv_sample_json.get("ps_5_0_cbuffer") == "0x00000000"
        and cbv_sample_json.get("D3D12SerializeRootSignature_cbv_descriptor_table") == "0x00000000"
        and cbv_sample_json.get("CreateRootSignature") == "0x00000000"
        and cbv_sample_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and cbv_sample_json.get("CreateVertexBuffer") == "0x00000000"
        and cbv_sample_json.get("CreateConstantBuffer") == "0x00000000"
        and cbv_sample_json.get("CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE") == "0x00000000"
        and int(cbv_sample_json.get("CreateConstantBufferView_descriptors", 0) or 0) == 1
        and cbv_sample_json.get("constant_buffer_filled") is True
        and int(cbv_sample_json.get("draw_calls", 0) or 0) == visible_frames
        and int(cbv_sample_json.get("vertices_per_draw", 0) or 0) == 18
        and int(cbv_sample_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(cbv_sample_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(cbv_sample_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(cbv_sample_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and cbv_sample_json.get("expected_rgba") == cbv_sample_expected_rgba
        and cbv_sample_json.get("present_rgba") == cbv_sample_expected_rgba
        and cbv_sample_json.get("present_last_rgba") == cbv_sample_expected_rgba
        and indexed_draw_json.get("ok") is True
        and indexed_draw_json.get("present_ok") is True
        and indexed_draw_json.get("proof_scope")
        == "r16_r32_subrange_base_append_dynamic_stride_execute_indirect_indexed_presented_readback"
        and indexed_draw_json.get("D3DCompile_loaded") is True
        and indexed_draw_json.get("indexed_vs_vs_5_0") == "0x00000000"
        and indexed_draw_json.get("indexed_ps_ps_5_0") == "0x00000000"
        and indexed_draw_json.get("D3D12SerializeRootSignature") == "0x00000000"
        and indexed_draw_json.get("CreateRootSignature") == "0x00000000"
        and indexed_draw_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and indexed_draw_json.get("CreateVertexBuffer") == "0x00000000"
        and indexed_draw_json.get("CreateDynamicStrideVertexBuffer") == "0x00000000"
        and indexed_draw_json.get("CreateIndexBuffer") == "0x00000000"
        and indexed_draw_json.get("CreateIndexBufferR32") == "0x00000000"
        and indexed_draw_json.get("CreateIndexBufferNegativeBase") == "0x00000000"
        and indexed_draw_json.get("CreateIndexBufferExecuteIndirectIndexed") == "0x00000000"
        and indexed_draw_json.get("CreateExecuteIndirectIndexedArgumentBuffer") == "0x00000000"
        and indexed_draw_json.get("CreateExecuteIndirectIndexedCommandSignature") == "0x00000000"
        and indexed_draw_json.get("append_aligned_element") is True
        and int(indexed_draw_json.get("append_aligned_color_expected_offset", 0) or 0) == 12
        and int(indexed_draw_json.get("vertices_created", 0) or 0) == 12
        and int(indexed_draw_json.get("vertex_buffer_size", 0) or 0) == 336
        and int(indexed_draw_json.get("vertex_view_byte_offset", 0) or 0) == 28
        and int(indexed_draw_json.get("dynamic_stride_vertices_created", 0) or 0) == 4
        and int(indexed_draw_json.get("dynamic_stride_vertex_buffer_size", 0) or 0) == 128
        and int(indexed_draw_json.get("dynamic_stride", 0) or 0) == 32
        and int(indexed_draw_json.get("indices_created", 0) or 0) == 6
        and int(indexed_draw_json.get("index_format", 0) or 0) == 57
        and int(indexed_draw_json.get("index_buffer_size", 0) or 0) == 16
        and int(indexed_draw_json.get("index_view_byte_offset", 0) or 0) == 4
        and int(indexed_draw_json.get("start_index_location", 0) or 0) == 2
        and int(indexed_draw_json.get("r32_indices_created", 0) or 0) == 6
        and int(indexed_draw_json.get("r32_index_format", 0) or 0) == 42
        and int(indexed_draw_json.get("r32_index_buffer_size", 0) or 0) == 32
        and int(indexed_draw_json.get("r32_index_view_byte_offset", 0) or 0) == 8
        and int(indexed_draw_json.get("r32_start_index_location", 0) or 0) == 2
        and int(indexed_draw_json.get("r32_base_vertex_location", 0) or 0) == 4
        and int(indexed_draw_json.get("negative_base_indices_created", 0) or 0) == 6
        and int(indexed_draw_json.get("negative_base_index_format", 0) or 0) == 42
        and int(indexed_draw_json.get("negative_base_index_buffer_size", 0) or 0) == 24
        and int(indexed_draw_json.get("negative_base_start_index_location", 0) or 0) == 0
        and int(indexed_draw_json.get("negative_base_vertex_location", 0) or 0) == -4
        and int(indexed_draw_json.get("execute_indirect_indexed_indices_created", 0) or 0) == 6
        and int(indexed_draw_json.get("execute_indirect_indexed_index_format", 0) or 0) == 42
        and int(indexed_draw_json.get("execute_indirect_indexed_index_buffer_size", 0) or 0) == 32
        and int(indexed_draw_json.get("execute_indirect_indexed_argument_byte_stride", 0) or 0) == 20
        and int(indexed_draw_json.get("execute_indirect_indexed_command_signature_arguments", 0) or 0) == 1
        and int(indexed_draw_json.get("execute_indirect_indexed_max_command_count", 0) or 0) == 1
        and int(indexed_draw_json.get("execute_indirect_indexed_index_count", 0) or 0) == 6
        and int(indexed_draw_json.get("execute_indirect_indexed_instance_count", 0) or 0) == 1
        and int(indexed_draw_json.get("execute_indirect_indexed_start_index_location", 0) or 0) == 2
        and int(indexed_draw_json.get("execute_indirect_indexed_base_vertex_location", 0) or 0) == -6
        and int(indexed_draw_json.get("execute_indirect_indexed_start_instance_location", 0) or 0) == 0
        and int(indexed_draw_json.get("draw_indexed_calls", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("draw_indexed_r32_calls", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("draw_indexed_negative_base_calls", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("draw_indexed_dynamic_stride_calls", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("execute_indirect_indexed_calls", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_r32_samples_checked", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_r32_sample_matches", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_r32_pixels_checked", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_r32_pixel_matches", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_negative_base_samples_checked", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_negative_base_sample_matches", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_negative_base_pixels_checked", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_negative_base_pixel_matches", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_dynamic_stride_samples_checked", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_dynamic_stride_sample_matches", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_dynamic_stride_pixels_checked", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_dynamic_stride_pixel_matches", 0) or 0) == visible_frames * 256
        and int(indexed_draw_json.get("present_execute_indirect_indexed_samples_checked", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_execute_indirect_indexed_sample_matches", 0) or 0) == visible_frames
        and int(indexed_draw_json.get("present_execute_indirect_indexed_pixels_checked", 0) or 0)
        == visible_frames * 256
        and int(indexed_draw_json.get("present_execute_indirect_indexed_pixel_matches", 0) or 0)
        == visible_frames * 256
        and indexed_draw_json.get("expected_rgba") == indexed_draw_expected_rgba
        and indexed_draw_json.get("present_rgba") == indexed_draw_expected_rgba
        and indexed_draw_json.get("present_last_rgba") == indexed_draw_expected_rgba
        and indexed_draw_json.get("expected_r32_rgba") == [64, 220, 240, 255]
        and indexed_draw_json.get("present_r32_rgba") == [64, 220, 240, 255]
        and indexed_draw_json.get("present_r32_last_rgba") == [64, 220, 240, 255]
        and indexed_draw_json.get("expected_negative_base_rgba") == [200, 80, 240, 255]
        and indexed_draw_json.get("present_negative_base_rgba") == [200, 80, 240, 255]
        and indexed_draw_json.get("present_negative_base_last_rgba") == [200, 80, 240, 255]
        and indexed_draw_json.get("expected_dynamic_stride_rgba") == [96, 240, 120, 255]
        and indexed_draw_json.get("present_dynamic_stride_rgba") == [96, 240, 120, 255]
        and indexed_draw_json.get("present_dynamic_stride_last_rgba") == [96, 240, 120, 255]
        and indexed_draw_json.get("expected_execute_indirect_indexed_rgba") == [240, 144, 72, 255]
        and indexed_draw_json.get("present_execute_indirect_indexed_rgba") == [240, 144, 72, 255]
        and indexed_draw_json.get("present_execute_indirect_indexed_last_rgba") == [240, 144, 72, 255]
        and multi_slot_instance_root_json.get("ok") is True
        and multi_slot_instance_root_json.get("present_ok") is True
        and multi_slot_instance_root_json.get("proof_scope")
        == "two_slot_per_instance_step_rate_start_instance_root_constants_mutation_presented_readback"
        and multi_slot_instance_root_json.get("D3DCompile_loaded") is True
        and multi_slot_instance_root_json.get("multi_slot_vs_vs_5_0") == "0x00000000"
        and multi_slot_instance_root_json.get("multi_slot_ps_ps_5_0") == "0x00000000"
        and multi_slot_instance_root_json.get("D3D12SerializeRootSignature_root_constants") == "0x00000000"
        and multi_slot_instance_root_json.get("CreateRootSignature") == "0x00000000"
        and multi_slot_instance_root_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and multi_slot_instance_root_json.get("CreatePositionVertexBuffer") == "0x00000000"
        and multi_slot_instance_root_json.get("CreateInstanceVertexBuffer") == "0x00000000"
        and int(multi_slot_instance_root_json.get("input_slots", 0) or 0) == 2
        and int(multi_slot_instance_root_json.get("position_vertices_created", 0) or 0) == 12
        and int(multi_slot_instance_root_json.get("instance_records_created", 0) or 0) == 6
        and int(multi_slot_instance_root_json.get("selected_instance_record", 0) or 0) == 5
        and int(multi_slot_instance_root_json.get("per_instance_step_rate", 0) or 0) == 2
        and int(multi_slot_instance_root_json.get("start_instance_location", 0) or 0) == 4
        and int(multi_slot_instance_root_json.get("vertex_count_per_draw", 0) or 0) == 6
        and int(multi_slot_instance_root_json.get("instance_count_per_draw", 0) or 0) == 3
        and int(multi_slot_instance_root_json.get("slot0_stride", 0) or 0) == 12
        and int(multi_slot_instance_root_json.get("slot1_stride", 0) or 0) == 24
        and int(multi_slot_instance_root_json.get("root_constant_float_count", 0) or 0) == 8
        and int(multi_slot_instance_root_json.get("root_constant_sets", 0) or 0) == visible_frames * 2
        and int(multi_slot_instance_root_json.get("draw_calls", 0) or 0) == visible_frames * 2
        and int(multi_slot_instance_root_json.get("first_draw_calls", 0) or 0) == visible_frames
        and int(multi_slot_instance_root_json.get("second_draw_calls", 0) or 0) == visible_frames
        and int(multi_slot_instance_root_json.get("present_first_samples_checked", 0) or 0) == visible_frames
        and int(multi_slot_instance_root_json.get("present_first_sample_matches", 0) or 0) == visible_frames
        and int(multi_slot_instance_root_json.get("present_first_pixels_checked", 0) or 0) == visible_frames * 256
        and int(multi_slot_instance_root_json.get("present_first_pixel_matches", 0) or 0) == visible_frames * 256
        and int(multi_slot_instance_root_json.get("present_second_samples_checked", 0) or 0) == visible_frames
        and int(multi_slot_instance_root_json.get("present_second_sample_matches", 0) or 0) == visible_frames
        and int(multi_slot_instance_root_json.get("present_second_pixels_checked", 0) or 0) == visible_frames * 256
        and int(multi_slot_instance_root_json.get("present_second_pixel_matches", 0) or 0) == visible_frames * 256
        and multi_slot_instance_root_json.get("expected_first_rgba") == [104, 96, 20, 255]
        and multi_slot_instance_root_json.get("present_first_rgba") == [104, 96, 20, 255]
        and multi_slot_instance_root_json.get("present_first_last_rgba") == [104, 96, 20, 255]
        and multi_slot_instance_root_json.get("expected_second_rgba") == [36, 208, 120, 255]
        and multi_slot_instance_root_json.get("present_second_rgba") == [36, 208, 120, 255]
        and multi_slot_instance_root_json.get("present_second_last_rgba") == [36, 208, 120, 255]
        and tessellation_fallback_json.get("ok") is True
        and tessellation_fallback_json.get("present_ok") is True
        and tessellation_fallback_json.get("proof_scope")
        == "hs_ds_patch_topology_d3d12_native_metal_tessellation_presented_readback"
        and tessellation_fallback_json.get("D3DCompile_loaded") is True
        and tessellation_fallback_json.get("tess_vs_vs_5_0") == "0x00000000"
        and tessellation_fallback_json.get("tess_hs_hs_5_0") == "0x00000000"
        and tessellation_fallback_json.get("tess_ds_ds_5_0") == "0x00000000"
        and tessellation_fallback_json.get("tess_ps_ps_5_0") == "0x00000000"
        and tessellation_fallback_json.get("D3D12SerializeRootSignature") == "0x00000000"
        and tessellation_fallback_json.get("CreateRootSignature") == "0x00000000"
        and tessellation_fallback_json.get("CreateGraphicsPipelineState_PATCH_HS_DS") == "0x00000000"
        and tessellation_fallback_json.get("CreateVertexBuffer") == "0x00000000"
        and tessellation_fallback_json.get("CreateIndexBuffer") == "0x00000000"
        and tessellation_fallback_json.get("native_tessellation_required") is True
        and tessellation_fallback_json.get("native_tessellation_resolved") is True
        and tessellation_fallback_json.get("native_draw_encoded") is True
        and tessellation_fallback_json.get("native_indexed_draw_encoded") is True
        and tessellation_fallback_json.get("blocked_expected") is False
        and tessellation_fallback_json.get("fallback_draw_encoded") is False
        and int(tessellation_fallback_json.get("patch_control_points", 0) or 0) == 3
        and int(tessellation_fallback_json.get("vertices_created", 0) or 0) == 93
        and int(tessellation_fallback_json.get("vertices_per_draw", 0) or 0) == 51
        and int(tessellation_fallback_json.get("indices_created", 0) or 0) == 42
        and int(tessellation_fallback_json.get("indices_per_draw", 0) or 0) == 42
        and int(tessellation_fallback_json.get("draw_calls", 0) or 0) == visible_frames
        and int(tessellation_fallback_json.get("draw_indexed_calls", 0) or 0) == visible_frames
        and int(tessellation_fallback_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(tessellation_fallback_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(tessellation_fallback_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(tessellation_fallback_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and tessellation_fallback_json.get("expected_rgba") == tessellation_fallback_expected_rgba
        and tessellation_fallback_json.get("present_rgba") == tessellation_fallback_expected_rgba
        and tessellation_fallback_json.get("present_last_rgba") == tessellation_fallback_expected_rgba
        and tessellation_fallback_json.get("indexed_expected_rgba") == [32, 224, 216, 255]
        and tessellation_fallback_json.get("indexed_present_rgba") == [32, 224, 216, 255]
        and tessellation_fallback_json.get("indexed_present_last_rgba") == [32, 224, 216, 255]
        and tessellation_fallback_json.get("indexed_present_ok") is True
        and int(tessellation_fallback_json.get("indexed_present_samples_checked", 0) or 0) == visible_frames
        and int(tessellation_fallback_json.get("indexed_present_sample_matches", 0) or 0) == visible_frames
        and int(tessellation_fallback_json.get("indexed_present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(tessellation_fallback_json.get("indexed_present_pixel_matches", 0) or 0) == visible_frames * 256
        and tessellation_fallback_json.get("tessellation_factor_debug_dump", {}).get("edge_half") == "0x3c00"
        and tessellation_fallback_json.get("tessellation_factor_debug_dump", {}).get("inside_half") == "0x3c00"
        and len(tessellation_fallback_json.get("tessellation_factor_history", [])) >= visible_frames
        and tessellation_fallback_json.get("progressive_triangle_proof_scope")
        == "progressive_rgb_tessellated_triangle_seed_point_to_full_triangle"
        and tessellation_fallback_json.get("progressive_triangle_ok") is True
        and tessellation_fallback_json.get("progressive_triangle_seed_point_ok") is True
        and tessellation_fallback_json.get("progressive_triangle_full_ok") is True
        and tessellation_fallback_json.get("progressive_triangle_rgb_channels_present") is True
        and tessellation_fallback_json.get("progressive_triangle_coverage_monotonic") is True
        and int(tessellation_fallback_json.get("progressive_triangle_frames_validated", 0) or 0) == visible_frames
        and len(tessellation_fallback_json.get("progressive_triangle_coverage_counts", [])) == visible_frames
        and int(tessellation_fallback_json.get("progressive_triangle_seed_pixels", 0) or 0) > 0
        and int(tessellation_fallback_json.get("progressive_triangle_seed_pixels", 0) or 0) < 96
        and int(tessellation_fallback_json.get("progressive_triangle_final_pixels", 0) or 0) >= 1800
        and int(tessellation_fallback_json.get("progressive_triangle_final_pixels", 0) or 0)
        > int(tessellation_fallback_json.get("progressive_triangle_seed_pixels", 0) or 0) * 8
        and len(tessellation_fallback_json.get("last_control_points_ndc", [])) == 9
        and shader_cache_validation.get("tessellation_fallback_presented_hashes", {}).get("native_tessellation_resolved") is True
        and shader_cache_validation.get("tessellation_fallback_presented_hashes", {}).get("native_indexed_draw_encoded") is True
        and shader_cache_validation.get("tessellation_fallback_presented_hashes", {}).get("fallback_trace_count", 0) == 0
        and indirect_draw_json.get("ok") is True
        and indirect_draw_json.get("present_ok") is True
        and indirect_draw_json.get("proof_scope") == "command_signature_execute_indirect_draw_presented_readback"
        and indirect_draw_json.get("D3DCompile_loaded") is True
        and indirect_draw_json.get("indirect_vs_vs_5_0") == "0x00000000"
        and indirect_draw_json.get("indirect_ps_ps_5_0") == "0x00000000"
        and indirect_draw_json.get("D3D12SerializeRootSignature") == "0x00000000"
        and indirect_draw_json.get("CreateRootSignature") == "0x00000000"
        and indirect_draw_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and indirect_draw_json.get("CreateVertexBuffer") == "0x00000000"
        and indirect_draw_json.get("CreateArgumentBuffer") == "0x00000000"
        and indirect_draw_json.get("CreateCommandSignature") == "0x00000000"
        and int(indirect_draw_json.get("vertices_created", 0) or 0) == 24
        and int(indirect_draw_json.get("argument_byte_stride", 0) or 0) == 16
        and int(indirect_draw_json.get("command_signature_arguments", 0) or 0) == 1
        and int(indirect_draw_json.get("max_command_count", 0) or 0) == 1
        and int(indirect_draw_json.get("argument_vertex_count", 0) or 0) == 24
        and int(indirect_draw_json.get("argument_instance_count", 0) or 0) == 1
        and int(indirect_draw_json.get("argument_start_vertex", 0) or 0) == 0
        and int(indirect_draw_json.get("argument_start_instance", 0) or 0) == 0
        and int(indirect_draw_json.get("execute_indirect_calls", 0) or 0) == visible_frames
        and int(indirect_draw_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(indirect_draw_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(indirect_draw_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(indirect_draw_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and indirect_draw_json.get("expected_rgba") == indirect_draw_expected_rgba
        and indirect_draw_json.get("present_rgba") == indirect_draw_expected_rgba
        and indirect_draw_json.get("present_last_rgba") == indirect_draw_expected_rgba
        and nanite_cluster_json.get("ok") is True
        and nanite_cluster_json.get("present_ok") is True
        and nanite_cluster_json.get("proof_scope") == "nanite_style_compute_generated_args_readback_mirrored_execute_indirect_presented"
        and nanite_cluster_json.get("D3DCompile_loaded") is True
        and nanite_cluster_json.get("nanite_cull_cs_cs_5_0") == "0x00000000"
        and nanite_cluster_json.get("nanite_vs_vs_5_0") == "0x00000000"
        and nanite_cluster_json.get("nanite_ps_ps_5_0") == "0x00000000"
        and nanite_cluster_json.get("D3D12SerializeRootSignature_compute_uav") == "0x00000000"
        and nanite_cluster_json.get("D3D12SerializeRootSignature_graphics") == "0x00000000"
        and nanite_cluster_json.get("CreateComputeRootSignature") == "0x00000000"
        and nanite_cluster_json.get("CreateGraphicsRootSignature") == "0x00000000"
        and nanite_cluster_json.get("CreateComputePipelineState") == "0x00000000"
        and nanite_cluster_json.get("CreateGraphicsPipelineState") == "0x00000000"
        and nanite_cluster_json.get("CreateVertexBuffer") == "0x00000000"
        and nanite_cluster_json.get("CreateArgumentBuffer_UAV") == "0x00000000"
        and nanite_cluster_json.get("CreateArgumentReadback") == "0x00000000"
        and nanite_cluster_json.get("CreateIndirectArgumentUpload") == "0x00000000"
        and nanite_cluster_json.get("CreateCommandSignature") == "0x00000000"
        and nanite_cluster_json.get("CloseCommandList") == "0x00000000"
        and nanite_cluster_json.get("SignalFence") == "0x00000000"
        and nanite_cluster_json.get("MapReadback") == "0x00000000"
        and int(nanite_cluster_json.get("argument_gpu_virtual_address", 0) or 0) != 0
        and int(nanite_cluster_json.get("indirect_argument_gpu_virtual_address", 0) or 0) != 0
        and int(nanite_cluster_json.get("vertices_created", 0) or 0) == 6
        and int(nanite_cluster_json.get("argument_byte_stride", 0) or 0) == 16
        and int(nanite_cluster_json.get("command_signature_arguments", 0) or 0) == 1
        and int(nanite_cluster_json.get("max_command_count", 0) or 0) == 1
        and int(nanite_cluster_json.get("dispatch_commands", 0) or 0) == 1
        and int(nanite_cluster_json.get("uav_barriers", 0) or 0) == 1
        and int(nanite_cluster_json.get("copy_argument_readback_commands", 0) or 0) == 1
        and int(nanite_cluster_json.get("transition_to_copy_barriers", 0) or 0) == 1
        and int(nanite_cluster_json.get("transition_to_indirect_barriers", 0) or 0) == 1
        and nanite_cluster_json.get("computed_draw_args") == [6, 1, 0, 0]
        and nanite_cluster_json.get("argument_readback_ok") is True
        and nanite_cluster_json.get("indirect_argument_upload_filled") is True
        and int(nanite_cluster_json.get("execute_indirect_calls", 0) or 0) == visible_frames
        and int(nanite_cluster_json.get("present_samples_checked", 0) or 0) == visible_frames
        and int(nanite_cluster_json.get("present_sample_matches", 0) or 0) == visible_frames
        and int(nanite_cluster_json.get("present_pixels_checked", 0) or 0) == visible_frames * 256
        and int(nanite_cluster_json.get("present_pixel_matches", 0) or 0) == visible_frames * 256
        and nanite_cluster_json.get("expected_rgba") == nanite_cluster_expected_rgba
        and nanite_cluster_json.get("present_rgba") == nanite_cluster_expected_rgba
        and nanite_cluster_json.get("present_last_rgba") == nanite_cluster_expected_rgba
        and nanite_cluster_json.get("fence_wait_ok") is True
    )
    pso_ptr_pattern = r"(?:0x)?[0-9a-fA-F]+"
    async_scheduled_matches = re.findall(rf"PSO async compile scheduled pso=({pso_ptr_pattern}) compute=(\d)", diagnostic_log_text)
    async_worker_begin_matches = re.findall(
        rf"PSO async worker-owned compile begin worker=(\d+) pso=({pso_ptr_pattern}) compute=(\d)",
        diagnostic_log_text,
    )
    async_worker_complete_matches = re.findall(
        rf"PSO async worker-owned compile complete worker=(\d+) pso=({pso_ptr_pattern}) result=(\d+) state=(\d+) compute=(\d)",
        diagnostic_log_text,
    )
    async_inline_claim_matches = re.findall(rf"PSO async pending compile claimed inline pso=({pso_ptr_pattern}) compute=(\d)", diagnostic_log_text)
    async_worker_indices = sorted({int(worker) for worker, _pso, _compute in async_worker_begin_matches})
    async_scheduled_psos = sorted({pso for pso, _compute in async_scheduled_matches})
    async_worker_complete_success_psos = sorted(
        {pso for _worker, pso, result, state, _compute in async_worker_complete_matches if result == "1" and state == "3"}
    )
    async_worker_proof = {
        "ok": False,
        "enabled_env": env.get("DXMT_ASYNC_PIPELINE_COMPILE") == "1",
        "worker_env": env.get("DXMT_D3D12_PSO_WORKERS"),
        "scheduled_count": len(async_scheduled_matches),
        "scheduled_unique_psos": len(async_scheduled_psos),
        "worker_begin_count": len(async_worker_begin_matches),
        "worker_complete_count": len(async_worker_complete_matches),
        "worker_complete_success_count": len(async_worker_complete_success_psos),
        "worker_indices": async_worker_indices,
        "inline_claim_count": len(async_inline_claim_matches),
        "scheduled_psos": async_scheduled_psos[:24],
        "worker_complete_success_psos": async_worker_complete_success_psos[:24],
    }
    async_worker_proof["ok"] = bool(
        async_worker_proof["enabled_env"]
        and async_worker_proof["scheduled_unique_psos"] >= 6
        and async_worker_proof["worker_complete_success_count"] >= 6
        and len(async_worker_indices) >= 1
        and async_worker_proof["inline_claim_count"] == 0
    )
    native_compute_resolve = extract_native_compute_resolve(diagnostic_log_text)
    write_json(run_dir / "native_compute_resolve.json", native_compute_resolve)
    command_buffer_retention = extract_command_buffer_retention(diagnostic_log_text, min(visible_frames, 4))
    write_json(run_dir / "command_buffer_retention.json", command_buffer_retention)
    backpressure_window_safety = extract_backpressure_window_safety(
        diagnostic_log_text,
        frames_presented,
        visible_frames,
        process_elapsed_ms,
        hard_fail_gates,
        proof_start_time,
        proof_end_time,
        timed_out,
        process_timeout_ms,
    )
    write_json(run_dir / "backpressure_window_safety.json", backpressure_window_safety)

    result = {
        "command": cmd,
        "cwd": str(out_bin),
        "returncode": proc.returncode,
        "process_elapsed_ms": process_elapsed_ms,
        "process_timeout_ms": process_timeout_ms,
        "timed_out": timed_out,
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "diagnostic_logs": diagnostic_log_records,
        "async_worker_proof": async_worker_proof,
        "native_compute_resolve": native_compute_resolve,
        "command_buffer_retention": command_buffer_retention,
        "backpressure_window_safety": backpressure_window_safety,
        "json_parse_error": parse_error,
        "game_json": parsed,
        "loaddll": loaddll_rows,
        "runtime_match": runtime_match,
        "dxil_artifacts": dxil_artifacts,
        "dxil_hazard_artifacts": dxil_hazard_artifacts,
        "waveops_artifacts": waveops_artifacts,
        "shader_cache_dir": str(shader_cache_dir),
        "shader_cache_validation": shader_cache_validation,
        "shader_replay_guardrail": shader_replay_guardrail_result,
        "hard_fail_gates": hard_fail_gates,
        "no_fallback_pass": hard_fail_gates["no_fallback_pass"],
        "native_runtime_pass": hard_fail_gates["native_runtime_pass"],
        "copied_corpus_artifacts": copied_corpus_artifacts,
        "corpus_hash_validation": corpus_hash_validation,
        "drawn_present_count": drawn_present_count,
        "draw_line_count": draw_line_count,
        "present_draw_counts": present_draw_counts,
        "present_indexed_counts": present_indexed_counts,
        "present_indirect_counts": present_indirect_counts,
        "native_vertex_resolve_log_budget": native_vertex_resolve_log_budget,
        "native_vertex_resolved_count": native_vertex_resolved_count,
        "native_vertex_resolved_required": native_vertex_resolved_required,
        "native_vertex_draw_indexed_resolved_count": native_vertex_draw_indexed_resolved_count,
        "native_vertex_draw_indexed_resolved_required": native_vertex_draw_indexed_resolved_required,
        "native_vertex_execute_indirect_draw_indexed_resolved_count": native_vertex_execute_indirect_draw_indexed_resolved_count,
        "native_vertex_execute_indirect_draw_indexed_resolved_required": native_vertex_execute_indirect_draw_indexed_resolved_required,
        "dxil_draw_encoded_count": dxil_draw_encoded_count,
        "dxil_draw_encoded_required": min(frames_presented, 6),
        "dxil_draw_encoded_log_budget_note": "DXMT swapchain DrawInstanced/DrawIndexedInstanced/ExecuteIndirect/native-vertex resolver logs are capped samples; long runs additionally require every present to report draws>=10, indexed>=4, indirect>=3, capped native ExecuteIndirectDrawIndexed resolver output, and all JSON/readback lanes to pass.",
        "dxil_vertex_pull_snapshot_count": dxil_vertex_pull_snapshot_count,
        "dxil_vertex_pull_snapshot_required": min(frames_presented, 4),
        "dxil_vertex_pull_snapshot_note": "DXMT vertex-pull snapshot logs are capped; proof requires the DXIL overlay draw to have v=3, slot_mask=0x1, bound_vbs=1, plus per-frame JSON/readback validation.",
        "dxil_draw_skipped": dxil_draw_skipped,
        "indexed_draw_skipped": indexed_draw_skipped,
        "indirect_draw_skipped": indirect_draw_skipped,
        "execute_indirect_indexed_draw_skipped": execute_indirect_indexed_draw_skipped,
        "multi_slot_instance_root_draw_skipped": multi_slot_instance_root_draw_skipped,
        "tessellation_fallback_draw_skipped": tessellation_fallback_draw_skipped,
        "nanite_cluster_draw_skipped": nanite_cluster_draw_skipped,
        "texture_array_srv_draw_skipped": texture_array_srv_draw_skipped,
        "textured_3d_draw_skipped": textured_3d_draw_skipped,
        "progressive_triangle_ok": progressive_triangle_ok,
        "tessellation_progressive_triangle_ok": tessellation_progressive_triangle_ok,
        "progressive_triangle_coverage_counts": progressive_triangle_counts,
        "progressive_triangle_seed_pixels": progressive_triangle_seed_pixels,
        "progressive_triangle_final_pixels": progressive_triangle_final_pixels,
        "textured_3d_ok": textured_3d_ok,
        "textured_3d_face_provenance": textured_3d_face_provenance,
        "abi_semantics": abi_semantics_json,
        "render_encoder_encode_failed": render_encoder_encode_failed,
        "frames_presented": frames_presented,
        "stderr_assertion": stderr_assertion,
        "ok": not timed_out
        and proc.returncode == 0
        and dxil_artifacts["ok"]
        and waveops_artifacts["ok"]
        and game_json_ok
        and corpus_hash_validation["ok"]
        and shader_cache_validation["ok"]
        and shader_replay_guardrail_result["ok"]
        and hard_fail_gates["native_runtime_pass"]
        and async_worker_proof["ok"]
        and native_compute_resolve["ok"]
        and command_buffer_retention["ok"]
        and backpressure_window_safety["ok"]
        and runtime_match["ok"]
        and frames_presented == visible_frames
        and drawn_present_count == frames_presented
        and len(present_draw_counts) == frames_presented
        and len(present_indexed_counts) == frames_presented
        and len(present_indirect_counts) == frames_presented
        and all(draws >= 10 for draws in present_draw_counts)
        and all(indexed >= 4 for indexed in present_indexed_counts)
        and all(indirect >= 3 for indirect in present_indirect_counts)
        and native_vertex_resolved_count >= native_vertex_resolved_required
        and native_vertex_draw_indexed_resolved_count >= native_vertex_draw_indexed_resolved_required
        and native_vertex_execute_indirect_draw_indexed_resolved_count
        >= native_vertex_execute_indirect_draw_indexed_resolved_required
        and dxil_draw_encoded_count >= min(frames_presented, 6)
        and dxil_vertex_pull_snapshot_count >= min(frames_presented, 4)
        and not dxil_draw_skipped
        and not indexed_draw_skipped
        and not indirect_draw_skipped
        and not execute_indirect_indexed_draw_skipped
        and not multi_slot_instance_root_draw_skipped
        and not tessellation_fallback_draw_skipped
        and not nanite_cluster_draw_skipped
        and not texture_array_srv_draw_skipped
        and not textured_3d_draw_skipped
        and not render_encoder_encode_failed
        and draw_line_count >= visible_frames
        and not stderr_assertion,
    }
    write_json(run_dir / "phase1-visible-game-summary.json", result)
    return result


def build_vulkan_report_probe(repo: Path, run_dir: Path) -> dict[str, Any]:
    source = repo / "tools" / "d3d12-metal-sdk" / "probes" / "probe_vkd3d_vulkan_report" / "probe_vkd3d_vulkan_report.cpp"
    exe = run_dir / "probe_vkd3d_vulkan_report"
    command = [
        "xcrun",
        "--sdk",
        "macosx",
        "clang++",
        "-std=c++17",
        "-arch",
        "x86_64",
        str(source),
        "-ldl",
        "-o",
        str(exe),
    ]
    proc = subprocess.run(command, cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return {
        "command": command,
        "source": str(source),
        "exe": str(exe),
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "ok": proc.returncode == 0 and exe.exists(),
    }


def run_vulkan_report_probe(repo: Path, proof_dir: Path, wine_runtime: Path) -> dict[str, Any]:
    run_dir = proof_dir / "phase3-vulkan-report"
    run_dir.mkdir(parents=True, exist_ok=True)
    unix_dir = wine_runtime / "lib" / "wine" / "x86_64-unix"
    win_dir = wine_runtime / "lib" / "wine" / "x86_64-windows"
    paths = {
        "loader": unix_dir / "libvulkan.dylib",
        "icd": wine_runtime / "etc" / "vulkan" / "icd.d" / "MoltenVK_icd.json",
        "moltenvk": unix_dir / "libMoltenVK.dylib",
        "winevulkan_so": unix_dir / "winevulkan.so",
        "vulkan_dll": win_dir / "vulkan-1.dll",
        "winevulkan_dll": win_dir / "winevulkan.dll",
    }
    expected_formats = {
        "loader": "macho-x86_64",
        "icd": "json",
        "moltenvk": "macho-x86_64",
        "winevulkan_so": "macho-x86_64",
        "vulkan_dll": "pe-x86_64",
        "winevulkan_dll": "pe-x86_64",
    }
    path_records = {}
    required_pe_exports = {"vkCreateInstance", "vkEnumerateInstanceExtensionProperties", "vkGetInstanceProcAddr"}
    for name, path in paths.items():
        expected_format = expected_formats[name]
        arch_ok = True
        pe_exports: list[str] = []
        pe_required_exports_ok = True
        if expected_format == "macho-x86_64":
            arch_ok = macho_x86_64_ok(path)
        elif expected_format == "pe-x86_64":
            arch_ok = pe_x86_64_ok(path)
            pe_exports = pe_export_names(path)
            pe_required_exports_ok = required_pe_exports.issubset(set(pe_exports))
        resolved_path = str(path.resolve(strict=True)) if path.exists() else ""
        path_records[name] = {
            "path": str(path),
            "resolved_path": resolved_path,
            "exists": path.exists(),
            "size": path.stat().st_size if path.exists() else 0,
            "sha256": sha256(path) if path.exists() and path.is_file() else None,
            "expected_format": expected_format,
            "arch_ok": arch_ok,
            "resolved_under_wine_runtime": path_resolves_under(path, wine_runtime),
            "pe_required_exports_ok": pe_required_exports_ok,
            "pe_export_count": len(pe_exports),
            "pe_required_exports": sorted(required_pe_exports) if expected_format == "pe-x86_64" else [],
        }
    icd_data: dict[str, Any] = {}
    icd_parse_error = ""
    if paths["icd"].exists():
        try:
            icd_data = json.loads(paths["icd"].read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            icd_parse_error = str(error)
    icd_section = icd_data.get("ICD", {}) if isinstance(icd_data, dict) else {}
    icd_library_path = Path(str(icd_section.get("library_path", ""))).expanduser() if isinstance(icd_section, dict) else Path("")
    icd_validation = {
        "parse_ok": bool(icd_data) and not icd_parse_error,
        "file_format_version": icd_data.get("file_format_version") if isinstance(icd_data, dict) else None,
        "api_version": icd_section.get("api_version") if isinstance(icd_section, dict) else None,
        "is_portability_driver": icd_section.get("is_portability_driver") if isinstance(icd_section, dict) else None,
        "library_path": str(icd_library_path) if str(icd_library_path) else "",
        "library_path_matches_moltenvk": bool(
            str(icd_library_path) and paths["moltenvk"].exists() and icd_library_path.resolve() == paths["moltenvk"].resolve()
        ),
        "parse_error": icd_parse_error,
    }
    build = build_vulkan_report_probe(repo, run_dir)
    write_json(run_dir / "build-probe.json", build)
    stdout_path = run_dir / "probe_vkd3d_vulkan_report.stdout.json"
    stderr_path = run_dir / "probe_vkd3d_vulkan_report.stderr.txt"
    parsed: dict[str, Any] | None = None
    parse_error = ""
    proc_return = None
    if build["ok"]:
        probe_env = sanitized_vulkan_probe_env(paths["icd"], unix_dir)
        proc = subprocess.run(
            [
                build["exe"],
                "--loader",
                str(paths["loader"]),
                "--icd",
                str(paths["icd"]),
                "--moltenvk",
                str(paths["moltenvk"]),
                "--winevulkan-so",
                str(paths["winevulkan_so"]),
                "--vulkan-dll",
                str(paths["vulkan_dll"]),
                "--winevulkan-dll",
                str(paths["winevulkan_dll"]),
            ],
            cwd=run_dir,
            env=probe_env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        proc_return = proc.returncode
        stdout_path.write_text(proc.stdout)
        stderr_path.write_text(proc.stderr)
        try:
            parsed = json.loads(proc.stdout)
        except json.JSONDecodeError as error:
            parse_error = str(error)
    else:
        stdout_path.write_text("")
        stderr_path.write_text(build.get("stderr", ""))
    devices = parsed.get("physical_devices", []) if parsed else []
    physical_device_count = int(parsed.get("physical_device_count", 0) or 0) if parsed else 0
    physical_devices_report_ok = bool(devices) and len(devices) == physical_device_count and all(
        str(device.get("name", ""))
        and int(device.get("vendor_id", 0) or 0) != 0
        and str(device.get("device_type_name", "")) in {"integrated_gpu", "discrete_gpu", "virtual_gpu", "cpu", "other"}
        for device in devices
    )
    expected_probe_env = {
        "VK_ICD_FILENAMES": str(paths["icd"]),
        "VK_DRIVER_FILES": str(paths["icd"]),
        "DYLD_LIBRARY_PATH": str(unix_dir),
    }
    probe_env_ok = bool(parsed) and parsed.get("vk_icd_filenames") == str(paths["icd"]) and parsed.get("vk_driver_files") == str(paths["icd"])
    path_records_ok = all(
        record["exists"]
        and record["size"] > 0
        and record["arch_ok"]
        and record["resolved_under_wine_runtime"]
        and record["pe_required_exports_ok"]
        for record in path_records.values()
    )
    winevulkan_so_probe_ok = bool(parsed) and parsed.get("winevulkan_so_open_ok") is True and parsed.get("winevulkan_unix_call_funcs_ok") is True and parsed.get("winevulkan_unix_call_wow64_funcs_ok") is True
    result = {
        "schema": "metalsharp.vkd3d.fresh.phase3-vulkan-report.v1",
        "build": build,
        "paths": path_records,
        "icd_validation": icd_validation,
        "expected_probe_env": expected_probe_env,
        "probe_env_ok": probe_env_ok,
        "physical_devices_report_ok": physical_devices_report_ok,
        "path_records_ok": path_records_ok,
        "winevulkan_so_probe_ok": winevulkan_so_probe_ok,
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "returncode": proc_return,
        "json_parse_error": parse_error,
        "probe_json": parsed,
        "ok": bool(
            path_records_ok
            and winevulkan_so_probe_ok
            and probe_env_ok
            and icd_validation["parse_ok"]
            and icd_validation["is_portability_driver"] is True
            and icd_validation["library_path_matches_moltenvk"]
            and build["ok"]
            and proc_return == 0
            and parsed
            and parsed.get("ok") is True
            and parsed.get("process_arch") == "x86_64"
            and parsed.get("loader_open_ok") is True
            and parsed.get("vk_get_instance_proc_addr_ok") is True
            and parsed.get("moltenvk_extension_supported") is True
            and physical_device_count >= 1
            and physical_devices_report_ok
        ),
    }
    write_json(run_dir / "phase3-vulkan-report-summary.json", result)
    return result


def build_metal_archive_probe(repo: Path, run_dir: Path) -> dict[str, Any]:
    source = repo / "tools" / "d3d12-metal-sdk" / "probes" / "probe_vkd3d_fresh_metal_archive" / "probe_vkd3d_fresh_metal_archive.mm"
    exe = run_dir / "probe_vkd3d_fresh_metal_archive"
    command = [
        "xcrun",
        "--sdk",
        "macosx",
        "clang++",
        "-std=c++17",
        "-fobjc-arc",
        "-fblocks",
        str(source),
        "-framework",
        "Foundation",
        "-framework",
        "Metal",
        "-o",
        str(exe),
    ]
    proc = subprocess.run(command, cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return {
        "command": command,
        "source": str(source),
        "exe": str(exe),
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "ok": proc.returncode == 0 and exe.exists(),
    }


def metallib_path_is_fresh(path_text: str, shader_cache_dir: Path) -> bool:
    if not path_text:
        return False
    try:
        path = Path(path_text).resolve(strict=True)
        root = shader_cache_dir.resolve(strict=True)
    except FileNotFoundError:
        return False
    return str(path) == str(root) or str(path).startswith(str(root) + os.sep)


def dxil_sidecars_for_hash(shader_cache_dir: Path, shader_hash: str) -> list[str]:
    if not shader_hash:
        return []
    return [
        str(path)
        for path in [
            shader_cache_dir / f"{shader_hash}.dxil_report.txt",
            shader_cache_dir / f"{shader_hash}.module.txt",
            shader_cache_dir / f"{shader_hash}.msl",
        ]
        if path.exists()
    ]


def archive_manifest_stage_entries(manifest_data: dict[str, Any]) -> list[tuple[int, str, str, dict[str, Any]]]:
    entries: list[tuple[int, str, str, dict[str, Any]]] = []
    pipelines = manifest_data.get("pipelines")
    if not isinstance(pipelines, list):
        return entries
    for pipeline_index, pipeline in enumerate(pipelines):
        if not isinstance(pipeline, dict):
            continue
        pipeline_name = str(pipeline.get("name", ""))
        pipeline_type = pipeline.get("type")
        stage_keys: list[tuple[str, str]] = []
        if pipeline_type == "render":
            stage_keys = [("vertex", "vertex"), ("fragment", "fragment")]
        elif pipeline_type == "compute":
            stage_keys = [("shader", "compute")]
        for key, stage in stage_keys:
            stage_manifest = pipeline.get(key)
            if isinstance(stage_manifest, dict):
                entries.append((pipeline_index, pipeline_name, stage, stage_manifest))
    return entries


def exclude_archive_manifest_reason(manifest_data: dict[str, Any], shader_cache_dir: Path) -> dict[str, Any] | None:
    allowed_reason = "dxil_stage_has_fresh_sidecars_but_no_matching_per_hash_metallib_do_not_rewrite_to_sm50"
    problems: list[dict[str, Any]] = []
    for pipeline_index, pipeline_name, stage, stage_manifest in archive_manifest_stage_entries(manifest_data):
        metallib = str(stage_manifest.get("metallib", ""))
        shader_hash = str(stage_manifest.get("hash", ""))
        sidecars = dxil_sidecars_for_hash(shader_cache_dir, shader_hash)
        reason = ""
        if not metallib or not Path(metallib).exists():
            reason = "requested_per_hash_metallib_missing"
            if sidecars:
                reason = allowed_reason
        elif not metallib_path_is_fresh(metallib, shader_cache_dir):
            reason = "requested_metallib_exists_but_is_not_under_fresh_shader_cache"
        if reason:
            problems.append(
                {
                    "pipeline_index": pipeline_index,
                    "pipeline": pipeline_name,
                    "stage": stage,
                    "shader_hash": shader_hash,
                    "requested_metallib": metallib,
                    "dxil_sidecars": sidecars,
                    "reason": reason,
                }
            )
    for problem in problems:
        if problem["reason"] != allowed_reason:
            return {**problem, "all_problem_stages": problems}
    if problems:
        return {**problems[0], "all_problem_stages": problems}
    return None


def archive_manifest_metallib_paths(manifest_data: dict[str, Any]) -> list[str]:
    paths: list[str] = []
    for _, _, _, stage_manifest in archive_manifest_stage_entries(manifest_data):
        metallib = str(stage_manifest.get("metallib", ""))
        if metallib:
            paths.append(metallib)
    return paths


def run_metal_archive_proof(repo: Path, proof_dir: Path, visible_game_result: dict[str, Any]) -> dict[str, Any]:
    run_dir = proof_dir / "phase2-metal-archive-prewarm"
    run_dir.mkdir(parents=True, exist_ok=True)
    shader_cache_dir = Path(str(visible_game_result.get("shader_cache_dir", "")))
    manifests = sorted(shader_cache_dir.glob("pso-*.json")) if shader_cache_dir.exists() else []
    copied_manifest_records = []
    excluded_manifest_records = []
    normalized_manifests: list[Path] = []
    expected_metallib_paths: set[str] = set()
    accepted_stage_records: list[dict[str, Any]] = []
    manifest_copy_dir = run_dir / "manifest-copies"
    manifest_copy_dir.mkdir(parents=True, exist_ok=True)
    for manifest in manifests:
        manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
        exclusion = exclude_archive_manifest_reason(manifest_data, shader_cache_dir)
        if exclusion:
            excluded_manifest_records.append(
                {
                    "source": str(manifest),
                    "source_sha256": sha256(manifest),
                    "source_size": manifest.stat().st_size,
                    **exclusion,
                }
            )
            continue
        destination = manifest_copy_dir / manifest.name
        for pipeline_index, pipeline_name, stage, stage_manifest in archive_manifest_stage_entries(manifest_data):
            metallib_path = str(stage_manifest.get("metallib", ""))
            shader_hash = str(stage_manifest.get("hash", ""))
            resolved_metallib = Path(metallib_path).resolve(strict=True)
            expected_metallib_paths.add(str(resolved_metallib))
            accepted_stage_records.append(
                {
                    "manifest": str(manifest),
                    "pipeline_index": pipeline_index,
                    "pipeline": pipeline_name,
                    "stage": stage,
                    "shader_hash": shader_hash,
                    "metallib": str(resolved_metallib),
                    "metallib_name_matches_hash": resolved_metallib.name == f"{shader_hash}.metallib",
                    "metallib_under_shader_cache": metallib_path_is_fresh(str(resolved_metallib), shader_cache_dir),
                    "sha256": sha256(resolved_metallib),
                    "size": resolved_metallib.stat().st_size,
                }
            )
        write_json(destination, manifest_data)
        normalized_manifests.append(destination)
        copied_manifest_records.append(
            {
                "source": str(manifest),
                "destination": str(destination),
                "source_sha256": sha256(manifest),
                "source_size": manifest.stat().st_size,
                "normalized_sha256": sha256(destination),
                "normalized_size": destination.stat().st_size,
                "metallib_ref_rewrites": [],
            }
        )
    manifest_list = run_dir / "fresh-pso-manifests.txt"
    manifest_list.write_text("\n".join(str(path) for path in normalized_manifests) + ("\n" if normalized_manifests else ""))
    build = build_metal_archive_probe(repo, run_dir)
    write_json(run_dir / "build-probe.json", build)
    stdout_path = run_dir / "probe_vkd3d_fresh_metal_archive.stdout.json"
    stderr_path = run_dir / "probe_vkd3d_fresh_metal_archive.stderr.txt"
    parsed: dict[str, Any] | None = None
    parse_error = ""
    proc_return = None
    archive_path = run_dir / "fresh-pso-binary.archive"
    if build["ok"]:
        proc = subprocess.run(
            [build["exe"], "--manifest-list", str(manifest_list), "--archive", str(archive_path)],
            cwd=run_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        proc_return = proc.returncode
        stdout_path.write_text(proc.stdout)
        stderr_path.write_text(proc.stderr)
        try:
            parsed = json.loads(proc.stdout)
        except json.JSONDecodeError as error:
            parse_error = str(error)
    else:
        stdout_path.write_text("")
        stderr_path.write_text(build.get("stderr", ""))
    parsed_ok = bool(parsed and parsed.get("ok") is True)
    render_count = int(parsed.get("render_descriptor_count", 0) or 0) if parsed else 0
    compute_count = int(parsed.get("compute_descriptor_count", 0) or 0) if parsed else 0
    archive_size = int(parsed.get("archive_size", 0) or 0) if parsed else 0
    manifest_records = parsed.get("manifest_records", []) if parsed else []
    load_records = parsed.get("metallib_load_records", []) if parsed else []
    loaded_metallib_paths = {
        str(Path(str(record.get("metallib", ""))).resolve(strict=False)) for record in load_records if record.get("metallib")
    }
    loaded_metallib_records = [
        {
            "path": path,
            "sha256": sha256(Path(path)) if Path(path).exists() else None,
            "size": Path(path).stat().st_size if Path(path).exists() else 0,
        }
        for path in sorted(loaded_metallib_paths)
    ]
    manifest_records_ok = bool(manifest_records) and all(record.get("ok") is True for record in manifest_records)
    load_records_exact = bool(load_records) and all(
        record.get("requested_metallib") == record.get("metallib") for record in load_records
    )
    load_records_under_shader_cache = bool(load_records) and all(
        metallib_path_is_fresh(str(record.get("metallib", "")), shader_cache_dir) for record in load_records
    )
    load_records_match_normalized_manifests = loaded_metallib_paths == expected_metallib_paths
    accepted_stage_paths_are_fresh_per_hash = bool(accepted_stage_records) and all(
        record["metallib_under_shader_cache"] and record["metallib_name_matches_hash"] for record in accepted_stage_records
    )
    allowed_exclusion_reason = "dxil_stage_has_fresh_sidecars_but_no_matching_per_hash_metallib_do_not_rewrite_to_sm50"
    excluded_manifests_allowed = all(record.get("reason") == allowed_exclusion_reason for record in excluded_manifest_records)
    parsed_manifest_count_matches = bool(parsed and int(parsed.get("manifest_count", -1)) == len(normalized_manifests))
    result = {
        "schema": "metalsharp.vkd3d.fresh.phase2-metal-archive-prewarm.v1",
        "build": build,
        "shader_cache_dir": str(shader_cache_dir),
        "manifest_list": str(manifest_list),
        "source_manifest_count": len(manifests),
        "manifest_count": len(normalized_manifests),
        "copied_manifest_records": copied_manifest_records,
        "excluded_manifest_records": excluded_manifest_records,
        "excluded_manifests_allowed": excluded_manifests_allowed,
        "accepted_stage_records": accepted_stage_records,
        "accepted_stage_paths_are_fresh_per_hash": accepted_stage_paths_are_fresh_per_hash,
        "expected_metallib_paths": sorted(expected_metallib_paths),
        "loaded_metallib_records": loaded_metallib_records,
        "manifest_records_ok": manifest_records_ok,
        "load_records_exact": load_records_exact,
        "load_records_under_shader_cache": load_records_under_shader_cache,
        "load_records_match_normalized_manifests": load_records_match_normalized_manifests,
        "parsed_manifest_count_matches": parsed_manifest_count_matches,
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "returncode": proc_return,
        "json_parse_error": parse_error,
        "archive_path": str(archive_path),
        "archive_sha256": sha256(archive_path) if archive_path.exists() else None,
        "archive_size": archive_path.stat().st_size if archive_path.exists() else 0,
        "probe_json": parsed,
        "ok": bool(
            build["ok"]
            and proc_return == 0
            and parsed_ok
            and len(normalized_manifests) >= 8
            and parsed_manifest_count_matches
            and excluded_manifests_allowed
            and accepted_stage_paths_are_fresh_per_hash
            and manifest_records_ok
            and load_records_exact
            and load_records_under_shader_cache
            and load_records_match_normalized_manifests
            and render_count >= 6
            and compute_count >= 1
            and archive_size > 0
            and archive_path.exists()
            and archive_path.stat().st_size == archive_size
        ),
    }
    write_json(run_dir / "phase2-metal-archive-prewarm-summary.json", result)
    return result


def run_identity_probe(
    repo: Path,
    proof_dir: Path,
    wine: Path,
    wine_runtime: Path,
    prefix: Path,
    runtime_root: Path,
    staged_files: list[dict[str, Any]],
) -> dict[str, Any]:
    sdk = repo / "tools" / "d3d12-metal-sdk"
    out_bin = sdk / "out" / "bin"
    exe = out_bin / "probe_vkd3d_runtime_identity.exe"
    run_dir = proof_dir / "phase0-runtime-identity"
    run_dir.mkdir(parents=True, exist_ok=True)

    unix_bridge = (runtime_root / "x86_64-unix" / "winemetal.so").resolve()
    dyld_parts = [
        str(runtime_root / "x86_64-unix"),
        str(wine_runtime / "lib" / "wine" / "x86_64-unix"),
    ]
    if os.environ.get("DYLD_LIBRARY_PATH"):
        dyld_parts.append(os.environ["DYLD_LIBRARY_PATH"])

    env = os.environ.copy()
    env.update(
        {
            "WINEPREFIX": str(prefix),
            "WINEDLLPATH": str(runtime_root / "x86_64-windows"),
            "WINEDLLOVERRIDES": "d3d12,dxgi,dxgi_dxmt,winemetal=n,b;gameoverlayrenderer,gameoverlayrenderer64=d",
            "DYLD_LIBRARY_PATH": ":".join(dyld_parts),
            "DXMT_WINEMETAL_UNIXLIB": unix_bridge.name,
            "DXMT_WINEMETAL_DEBUG": "1",
            "DXMT_LOG_PATH": str(run_dir),
            "D3D12_METAL_SDK_PROFILE": "vkd3d-fresh-proof",
            "WINEDEBUG": "+loaddll",
        }
    )
    cmd = [str(wine), exe.name]
    proc = subprocess.run(cmd, cwd=out_bin, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout_path = run_dir / "probe_vkd3d_runtime_identity.stdout.json"
    stderr_path = run_dir / "probe_vkd3d_runtime_identity.stderr.txt"
    stdout_path.write_text(proc.stdout)
    stderr_path.write_text(proc.stderr)

    parsed: dict[str, Any] | None = None
    parse_error = ""
    try:
        parsed = json.loads(proc.stdout)
    except json.JSONDecodeError as error:
        parse_error = str(error)

    loaddll_rows = parse_loaddll(proc.stderr)
    runtime_match = validate_loaded_runtime(parsed, loaddll_rows, staged_files)
    unix_bridge_match = validate_unix_bridge(parsed, runtime_root, wine_runtime, run_dir / "winemetal-pe-debug.log")
    abi_semantics = parsed.get("abi_semantics", {}) if parsed else {}
    abi_semantics_ok = bool(
        abi_semantics.get("ok") is True
        and abi_semantics.get("proof_scope") == "runtime_guid_com_abi_queryinterface_private_data_bootstrap"
        and abi_semantics.get("IID_ID3D12Device") == "189819f1-1db6-4b57-be54-1821339b85f7"
        and abi_semantics.get("IID_IDXGIFactory2") == "50c83a1c-e072-4c48-87b0-3630fa36a6d0"
        and abi_semantics.get("IID_IDXGIAdapter1") == "29038f61-3839-4626-91fd-086879011a05"
        and abi_semantics.get("IID_ID3D12CommandQueue") == "0ec870a6-5d7e-4c22-8cfc-5baae07616ed"
        and abi_semantics.get("IID_ID3D12CommandAllocator") == "6102dee4-af59-4b09-b999-b44d73f09b24"
        and abi_semantics.get("IID_ID3D12GraphicsCommandList") == "5b160d0f-ac1b-4185-8ba8-b3ae42a5a455"
        and abi_semantics.get("IID_ID3D12Fence") == "0a753dcf-c4d8-4b91-adf6-be5a60d95a76"
        and abi_semantics.get("guid_constants_ok") is True
        and abi_semantics.get("create_objects_ok") is True
        and abi_semantics.get("query_interface_ok") is True
        and abi_semantics.get("vtable_layout_ok") is True
        and abi_semantics.get("device_child_get_device_ok") is True
        and abi_semantics.get("device_child_identity_ok") is True
        and abi_semantics.get("private_data_copy_semantics") == "caller_buffer_mutated_after_SetPrivateData_before_GetPrivateData"
        and abi_semantics.get("private_data_roundtrip_ok") is True
        and int(abi_semantics.get("private_data_size", 0) or 0) == 16
        and all(
            abi_semantics.get(key) == "0x00000000"
            for key in [
                "CreateCommandQueue",
                "CreateCommandAllocator",
                "CreateCommandList",
                "CreateFence",
                "QueryInterface_IDXGIFactory2",
                "QueryInterface_IDXGIAdapter1",
                "QueryInterface_ID3D12Device",
                "QueryInterface_IUnknown_device",
                "QueryInterface_ID3D12CommandQueue",
                "QueryInterface_ID3D12CommandAllocator",
                "QueryInterface_ID3D12GraphicsCommandList",
                "QueryInterface_ID3D12Fence",
                "GetDevice_from_queue",
                "GetDevice_from_allocator",
                "GetDevice_from_list",
                "GetDevice_from_fence",
                "QueryInterface_IUnknown_queue_device",
                "QueryInterface_IUnknown_allocator_device",
                "QueryInterface_IUnknown_list_device",
                "QueryInterface_IUnknown_fence_device",
                "SetPrivateData",
                "GetPrivateData",
            ]
        )
    )
    result = {
        "command": cmd,
        "cwd": str(out_bin),
        "returncode": proc.returncode,
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "json_parse_error": parse_error,
        "probe_json": parsed,
        "loaddll": loaddll_rows,
        "runtime_match": runtime_match,
        "unix_bridge_match": unix_bridge_match,
        "abi_semantics_ok": abi_semantics_ok,
        "ok": proc.returncode == 0
        and bool(parsed and parsed.get("pass") is True)
        and abi_semantics_ok
        and runtime_match["ok"]
        and unix_bridge_match["ok"],
    }
    write_json(run_dir / "phase0-runtime-identity-summary.json", result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the fresh VKD3D proof harness gates.")
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parents[3]))
    parser.add_argument("--proof-root", default="")
    parser.add_argument("--lab-root", default=str(DEFAULT_LAB_ROOT))
    parser.add_argument("--runtime-root", default=os.path.expanduser("~/.metalsharp/runtime/wine/lib/dxmt_vkd3d"))
    parser.add_argument("--wine-runtime", default=os.path.expanduser("~/.metalsharp/runtime/wine"))
    parser.add_argument("--wine", default=os.path.expanduser("~/.metalsharp/runtime/wine/bin/metalsharp-wine"))
    parser.add_argument("--prefix", default=os.path.expanduser("~/.metalsharp/prefix-steam"))
    parser.add_argument("--min-free-gib", type=int, default=50)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "x86_64-w64-mingw32-g++"))
    parser.add_argument("--corpus-tsv", default=os.environ.get("VKD3D_FRESH_CORPUS_TSV", ""))
    parser.add_argument("--visible-frames", type=int, default=600)
    parser.add_argument("--pr-number", type=int, default=230, help="GitHub PR number for final readiness CI accounting.")
    parser.add_argument("--skip-pr-ci", action="store_true", help="Skip PR CI accounting in final readiness output.")
    parser.add_argument("--skip-game", action="store_true", help="Skip the visible vkd3d_fresh_game.exe phase.")
    parser.add_argument(
        "--local-game-snapshot-root",
        action="append",
        default=[],
        metavar="TITLE=PATH",
        help="Override local game snapshot root for a title/appid, e.g. elden-ring=/path/to/shader-cache.",
    )
    parser.add_argument(
        "--skip-local-game-inventory",
        action="store_true",
        help="Skip the file-only Elden Ring/Subnautica 2 local snapshot inventory phase.",
    )
    parser.add_argument("--skip-run", action="store_true", help="Build and manifest only; do not execute Wine.")
    args = parser.parse_args()

    repo = Path(args.repo).expanduser().resolve()
    lab_root = Path(args.lab_root).expanduser()
    runtime_root = Path(args.runtime_root).expanduser()
    wine_runtime = Path(args.wine_runtime).expanduser()
    wine = Path(args.wine).expanduser()
    prefix = Path(args.prefix).expanduser()
    try:
        local_snapshot_overrides = parse_local_game_snapshot_overrides(args.local_game_snapshot_root)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    known_snapshot_keys = {
        key
        for source in LOCAL_GAME_SNAPSHOT_SOURCES
        for key in (str(source["title"]), str(source["appid"]))
    }
    unknown_snapshot_keys = sorted(set(local_snapshot_overrides) - known_snapshot_keys)
    if unknown_snapshot_keys:
        print(
            "unknown --local-game-snapshot-root key(s): "
            + ", ".join(unknown_snapshot_keys)
            + "; expected one of "
            + ", ".join(sorted(known_snapshot_keys)),
            file=sys.stderr,
        )
        return 2
    proof_root = Path(args.proof_root).expanduser() if args.proof_root else (
        lab_root / "06-results" / "in-progress" / f"vkd3d-fresh-proof-game-harness-{timestamp()}"
    )

    if not lab_root.exists():
        print(f"lab root is missing or unmounted: {lab_root}", file=sys.stderr)
        return 2
    free_gib = disk_free_gib(lab_root)
    disk_guard = {
        "path": str(lab_root),
        "free_gib": free_gib,
        "min_free_gib": args.min_free_gib,
        "ok": free_gib >= args.min_free_gib,
    }
    if not disk_guard["ok"]:
        print(f"disk guard failed: {free_gib} GiB free < {args.min_free_gib} GiB", file=sys.stderr)
        return 2

    lab_resolved = lab_root.resolve()
    proof_resolved = proof_root.resolve(strict=False)
    if not (str(proof_resolved) == str(lab_resolved) or str(proof_resolved).startswith(str(lab_resolved) + os.sep)):
        print(f"proof root must be under lab root: {proof_root} not under {lab_root}", file=sys.stderr)
        return 2
    proof_root.mkdir(parents=True, exist_ok=False)

    runtime_entries, missing_runtime = inspect_runtime(runtime_root)
    manifest = {
        "schema": "metalsharp.vkd3d.fresh.proof-run-manifest.v1",
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "policy": {
            "fresh_artifacts_only": True,
            "prior_proof_artifacts_allowed": False,
            "live_game_launch_requires_user_approval": True,
        },
        "repo": {
            "path": str(repo),
            "branch": git_text(repo, "branch", "--show-current"),
            "commit": git_text(repo, "rev-parse", "HEAD"),
            "status_short": git_text(repo, "status", "--short"),
        },
        "proof_root": str(proof_root),
        "disk_guard": disk_guard,
        "runtime_root": str(runtime_root),
        "wine_runtime": str(wine_runtime),
        "wine": str(wine),
        "prefix": str(prefix),
        "runtime_files": runtime_entries,
        "missing_runtime_files": missing_runtime,
    }
    write_json(proof_root / "proof-run-manifest.json", manifest)

    if missing_runtime:
        print("missing runtime files:", file=sys.stderr)
        for item in missing_runtime:
            print(f"  {item}", file=sys.stderr)
        return 3
    if not wine.exists():
        print(f"missing wine binary: {wine}", file=sys.stderr)
        return 4
    if not prefix.exists():
        print(f"missing prefix: {prefix}", file=sys.stderr)
        return 5

    build = build_identity_probe(repo, args.cxx)
    write_json(proof_root / "phase0-build-runtime-identity-probe.json", build)
    if not build["ok"]:
        print(build["stderr"], file=sys.stderr)
        return 6

    game_build = build_fresh_game(repo, args.cxx)
    write_json(proof_root / "phase1-build-vkd3d-fresh-game.json", game_build)
    if not game_build["ok"]:
        print(game_build["stderr"], file=sys.stderr)
        return 9

    staged = copy_runtime_to_probe_dir(runtime_root, repo / "tools" / "d3d12-metal-sdk" / "out" / "bin")
    write_json(
        proof_root / "phase0-staged-runtime-hashes.json",
        {"files": staged, "ok": all(x["hash_match"] for x in staged)},
    )
    if not all(x["hash_match"] for x in staged):
        return 7

    source_guard = d3d12_native_tessellation_source_guard(repo)
    write_json(proof_root / "phase0-d3d12-native-tessellation-source-guard.json", source_guard)

    identity_result: dict[str, Any] | None = None
    visible_game_result: dict[str, Any] | None = None
    metal_archive_result: dict[str, Any] | None = None
    vulkan_report_result: dict[str, Any] | None = None
    local_game_inventory_result: dict[str, Any] | None = None
    if not args.skip_run:
        identity_result = run_identity_probe(repo, proof_root, wine, wine_runtime, prefix, runtime_root, staged)
        vulkan_report_result = run_vulkan_report_probe(repo, proof_root, wine_runtime)
        if not args.skip_local_game_inventory:
            local_game_inventory_result = run_local_game_snapshot_inventory(proof_root, local_snapshot_overrides)
        if not args.skip_game:
            if not args.corpus_tsv:
                print("--corpus-tsv is required unless --skip-game or --skip-run is set", file=sys.stderr)
                return 10
            corpus_tsv = Path(args.corpus_tsv).expanduser().resolve()
            if not corpus_tsv.exists():
                print(f"missing corpus TSV: {corpus_tsv}", file=sys.stderr)
                return 11
            visible_game_result = run_fresh_game(
                repo,
                proof_root,
                wine,
                wine_runtime,
                prefix,
                runtime_root,
                staged,
                corpus_tsv,
                args.visible_frames,
            )
            if visible_game_result["ok"]:
                metal_archive_result = run_metal_archive_proof(repo, proof_root, visible_game_result)

    local_ok = (
        disk_guard["ok"]
        and not missing_runtime
        and build["ok"]
        and game_build["ok"]
        and all(x["hash_match"] for x in staged)
        and source_guard["ok"]
        and (args.skip_run or bool(identity_result and identity_result["ok"]))
        and (args.skip_run or bool(vulkan_report_result and vulkan_report_result["ok"]))
        and (args.skip_run or args.skip_game or bool(visible_game_result and visible_game_result["ok"]))
        and (args.skip_run or args.skip_local_game_inventory or bool(local_game_inventory_result and local_game_inventory_result["ok"]))
        and (args.skip_run or args.skip_game or bool(metal_archive_result and metal_archive_result["ok"]))
    )
    summary = {
        "schema": "metalsharp.vkd3d.fresh.phase0-summary.v1",
        "ok": local_ok,
        "local_ok": local_ok,
        "proof_root": str(proof_root),
        "disk_guard": disk_guard,
        "build_ok": build["ok"],
        "game_build_ok": game_build["ok"],
        "runtime_stage_ok": all(x["hash_match"] for x in staged),
        "d3d12_native_tessellation_source_guard_ok": source_guard["ok"],
        "identity_probe_ok": None if args.skip_run else bool(identity_result and identity_result["ok"]),
        "vulkan_report_ok": None if args.skip_run else bool(vulkan_report_result and vulkan_report_result["ok"]),
        "visible_game_ok": None
        if args.skip_run or args.skip_game
        else bool(visible_game_result and visible_game_result["ok"]),
        "hard_fail_gates": None
        if args.skip_run or args.skip_game or not visible_game_result
        else visible_game_result.get("hard_fail_gates"),
        "no_fallback_pass": None
        if args.skip_run or args.skip_game or not visible_game_result
        else visible_game_result.get("no_fallback_pass"),
        "native_runtime_pass": None
        if args.skip_run or args.skip_game or not visible_game_result
        else visible_game_result.get("native_runtime_pass"),
        "metal_archive_prewarm_ok": None
        if args.skip_run or args.skip_game
        else bool(metal_archive_result and metal_archive_result["ok"]),
        "local_game_snapshot_inventory_ok": None
        if args.skip_run or args.skip_local_game_inventory
        else bool(local_game_inventory_result and local_game_inventory_result["ok"]),
        "artifacts": {
            "manifest": str(proof_root / "proof-run-manifest.json"),
            "build": str(proof_root / "phase0-build-runtime-identity-probe.json"),
            "staged_hashes": str(proof_root / "phase0-staged-runtime-hashes.json"),
            "d3d12_native_tessellation_source_guard": str(
                proof_root / "phase0-d3d12-native-tessellation-source-guard.json"
            ),
            "identity": str(proof_root / "phase0-runtime-identity" / "phase0-runtime-identity-summary.json")
            if not args.skip_run
            else None,
            "vulkan_report": str(proof_root / "phase3-vulkan-report" / "phase3-vulkan-report-summary.json")
            if not args.skip_run
            else None,
            "visible_game": str(proof_root / "phase1-visible-game" / "phase1-visible-game-summary.json")
            if not args.skip_run and not args.skip_game
            else None,
            "metal_archive_prewarm": str(
                proof_root / "phase2-metal-archive-prewarm" / "phase2-metal-archive-prewarm-summary.json"
            )
            if not args.skip_run and not args.skip_game
            else None,
            "local_game_snapshot_inventory": str(
                proof_root
                / "phase4-local-game-snapshot-inventory"
                / "phase4-local-game-snapshot-inventory-summary.json"
            )
            if not args.skip_run and not args.skip_local_game_inventory
            else None,
        },
    }
    pr_ci_status = collect_pr_ci_status(repo, args.pr_number, git_text(repo, "rev-parse", "HEAD"), args.skip_pr_ci)
    readiness = write_readiness_bundle(proof_root, repo, summary, visible_game_result, pr_ci_status)
    summary["pr_ci_status"] = pr_ci_status
    summary["final_readiness"] = readiness
    summary["final_readiness_ok"] = readiness["ok"]
    summary["ok"] = readiness["ok"]
    summary["artifacts"]["final_readiness_json"] = str(proof_root / "final-readiness.json")
    summary["artifacts"]["readiness_md"] = str(proof_root / "READINESS.md")
    write_json(proof_root / "phase0-summary.json", summary)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["ok"] else 8


if __name__ == "__main__":
    raise SystemExit(main())
