#!/usr/bin/env python3
"""Index VKD3D D3D12/Metal render evidence without launching a game."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path


INTERESTING_LOG_PATTERNS = {
    "d3d12_device": re.compile(r"D3D12 device created|D3D12CreateDevice", re.IGNORECASE),
    "unknown_qi": re.compile(r"QueryInterface: unknown IID", re.IGNORECASE),
    "command_queue": re.compile(r"D3D12CommandQueue|CommandQueue|ExecuteCommand", re.IGNORECASE),
    "descriptor_heap": re.compile(r"D3D12DescriptorHeap|descriptor", re.IGNORECASE),
    "root_signature": re.compile(r"D3D12RootSignature|root signature", re.IGNORECASE),
    "resource": re.compile(r"D3D12Resource|Resource:", re.IGNORECASE),
    "dxil_msl_compile_failed": re.compile(r"DXIL MSL compilation failed", re.IGNORECASE),
    "metal_pso_error": re.compile(r"(newRenderPipelineState|newComputePipelineState|pso|pipeline).*failed", re.IGNORECASE),
    "readback_zero": re.compile(r"nonzero_pixels=0"),
    "readback_nonzero": re.compile(r"nonzero_pixels=([1-9][0-9]*)"),
    "present": re.compile(r"\bPresent\b|\bpresent\b|present blit", re.IGNORECASE),
    "rtv": re.compile(r"\bRTV\b|render target", re.IGNORECASE),
    "barrier": re.compile(r"barrier|RESOURCE_STATE", re.IGNORECASE),
    "crash": re.compile(r"crashreport|Unhandled exception|Backtrace:", re.IGNORECASE),
    "magenta_hint": re.compile(r"magenta|pink|diagnostic fragment|force.*color", re.IGNORECASE),
}


def tail(path: Path, limit: int = 4000) -> str:
    try:
        data = path.read_text(errors="replace")
    except OSError:
        return ""
    return data[-limit:]


def default_log_roots(appid: int) -> list[Path]:
    home = Path.home()
    return [
        home / ".metalsharp" / "pipeline-cache" / "vkd3d" / str(appid),
        home / ".metalsharp" / "compatdata" / str(appid) / "logs",
        home / ".metalsharp" / "bottles" / f"steam_{appid}" / "logs",
    ]


def default_cache_roots(appid: int) -> list[Path]:
    home = Path.home()
    return [
        home / ".metalsharp" / "shader-cache" / "vkd3d" / str(appid),
        home / ".metalsharp" / "pipeline-cache" / "vkd3d" / str(appid),
    ]


def collect_logs(roots: list[Path], limit: int) -> list[dict]:
    candidates: list[Path] = []
    for root in roots:
        if root.is_file():
            candidates.append(root)
        elif root.exists():
            candidates.extend(path for path in root.rglob("*") if path.is_file() and path.suffix in {".log", ".txt"})

    unique = sorted(set(candidates), key=lambda p: p.stat().st_mtime, reverse=True)
    rows = []
    for path in unique[:limit]:
        text = tail(path, 120000)
        counts = {key: len(pattern.findall(text)) for key, pattern in INTERESTING_LOG_PATTERNS.items()}
        snippets = []
        for line in text.splitlines():
            if any(pattern.search(line) for pattern in INTERESTING_LOG_PATTERNS.values()):
                snippets.append(line[-500:])
        rows.append(
            {
                "path": str(path),
                "mtime": path.stat().st_mtime,
                "size": path.stat().st_size,
                "counts": counts,
                "snippets_tail": snippets[-80:],
            }
        )
    return rows


def collect_shader_sidecars(roots: list[Path]) -> list[dict]:
    sidecars = []
    for root in roots:
        if not root.exists():
            continue
        for pattern in ("*.msl.err.txt", "*.msc.fail", "*.dxil_report.txt", "pso-*.json", "*.metallib", "*.msl"):
            for path in root.rglob(pattern):
                entry = {
                    "path": str(path),
                    "kind": pattern,
                    "size": path.stat().st_size,
                    "mtime": path.stat().st_mtime,
                }
                if path.suffix in {".txt", ".fail"} or path.name.endswith(".fail"):
                    entry["tail"] = tail(path, 3000)
                sidecars.append(entry)
    return sorted(sidecars, key=lambda item: item["mtime"], reverse=True)


def write_markdown(path: Path, result: dict) -> None:
    log_totals = Counter()
    for log in result["logs"]:
        log_totals.update(log["counts"])

    lines = [
        f"# VKD3D Failure Index: {result['profile']}",
        "",
        "Generated from local VKD3D logs and shader sidecars without launching a game.",
        "",
        "## Totals",
        "",
    ]
    for key, count in sorted(log_totals.items()):
        lines.append(f"- `{key}`: {count}")
    lines.extend(
        [
            f"- `shader_sidecars`: {len(result['shader_sidecars'])}",
            "",
            "## Latest Logs",
            "",
        ]
    )
    for log in result["logs"][:8]:
        lines.append(f"### `{log['path']}`")
        active = {key: value for key, value in log["counts"].items() if value}
        lines.append("")
        lines.append(f"- counts: `{json.dumps(active, sort_keys=True)}`")
        if log["snippets_tail"]:
            lines.append("- tail evidence:")
            for snippet in log["snippets_tail"][-10:]:
                lines.append(f"  - `{snippet}`")
        lines.append("")

    lines.extend(["## Latest Shader Sidecars", ""])
    for sidecar in result["shader_sidecars"][:30]:
        lines.append(f"- `{sidecar['kind']}` `{sidecar['path']}`")

    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Index VKD3D D3D12/Metal failures from logs and shader sidecars.")
    parser.add_argument("--appid", type=int, default=2050650)
    parser.add_argument("--profile", default="")
    parser.add_argument("--log-root", action="append", default=[])
    parser.add_argument("--cache-root", action="append", default=[])
    parser.add_argument("--limit", type=int, default=16)
    parser.add_argument("--results-dir", default=str(Path(__file__).resolve().parents[1] / "results"))
    args = parser.parse_args()

    profile = args.profile or str(args.appid)
    log_roots = [Path(root) for root in args.log_root] if args.log_root else default_log_roots(args.appid)
    cache_roots = [Path(root) for root in args.cache_root] if args.cache_root else default_cache_roots(args.appid)
    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    result = {
        "schema": "metalsharp.d3d12-metal.vkd3d-failure-index.v1",
        "profile": profile,
        "appid": args.appid,
        "log_roots": [str(root) for root in log_roots],
        "cache_roots": [str(root) for root in cache_roots],
        "logs": collect_logs(log_roots, args.limit),
        "shader_sidecars": collect_shader_sidecars(cache_roots),
    }

    json_path = results_dir / f"vkd3d-failure-index-{profile}.json"
    md_path = results_dir / f"vkd3d-failure-index-{profile}.md"
    json_path.write_text(json.dumps(result, indent=2) + "\n")
    write_markdown(md_path, result)
    print(md_path)
    print(json_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
