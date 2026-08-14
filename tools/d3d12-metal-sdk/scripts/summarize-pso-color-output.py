#!/usr/bin/env python3
import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path


def load_first_json_object(path: Path) -> tuple[dict, str]:
    text = path.read_text(errors="replace")
    decoder = json.JSONDecoder()
    data, end = decoder.raw_decode(text)
    malformed = text[end:].strip()
    return data, malformed


def first_pipeline(path: Path) -> tuple[dict, str]:
    data, malformed_tail = load_first_json_object(path)
    pipelines = data.get("pipelines") or []
    return pipelines[0] if pipelines else {}, malformed_tail


def summarize_manifest(path: Path) -> dict:
    pipeline, malformed_tail = first_pipeline(path)
    d3d12 = pipeline.get("d3d12") or {}
    metal = pipeline.get("metal") or {}
    color_formats = pipeline.get("color_formats") or []
    return {
        "path": str(path),
        "name": pipeline.get("name", path.stem),
        "type": pipeline.get("type", ""),
        "color_formats": color_formats,
        "num_render_targets": d3d12.get("num_render_targets", 0),
        "depth_format": pipeline.get("depth_format", "unknown"),
        "stencil_format": pipeline.get("stencil_format", "unknown"),
        "sample_count": pipeline.get("sample_count", 0),
        "vs_hash": d3d12.get("vs_hash", ""),
        "ps_hash": d3d12.get("ps_hash", ""),
        "gs_hash": d3d12.get("gs_hash", ""),
        "has_pixel": bool(d3d12.get("ps_bytes", 0)),
        "has_geometry": bool(d3d12.get("gs_bytes", 0)),
        "rasterization_enabled": bool(metal.get("rasterization_enabled", False)),
        "uses_stage_in": bool(metal.get("uses_stage_in", False)),
        "uses_geometry_mesh": bool(metal.get("uses_geometry_mesh", False)),
        "malformed_tail": bool(malformed_tail),
        "malformed_tail_preview": malformed_tail[:200],
    }


def write_markdown(path: Path, rows: list[dict]) -> None:
    color_counter = Counter()
    rt_counter = Counter()
    no_pixel = 0
    invalid_color = 0
    geometry = 0
    malformed = 0
    for row in rows:
        color_tuple = tuple(row["color_formats"])
        color_counter[color_tuple] += 1
        rt_counter[row["num_render_targets"]] += 1
        no_pixel += 0 if row["has_pixel"] else 1
        invalid_color += 1 if any(fmt == "invalid" for fmt in row["color_formats"]) else 0
        geometry += 1 if row["has_geometry"] or row["uses_geometry_mesh"] else 0
        malformed += 1 if row["malformed_tail"] else 0

    lines = [
        "# Subnautica 2 PSO Color Output Summary",
        "",
        "Generated from captured `pso-render-*.json` manifests without launching the game.",
        "",
        "## Totals",
        "",
        f"- render PSOs: `{len(rows)}`",
        f"- no-pixel render PSOs: `{no_pixel}`",
        f"- PSOs with `invalid` color format entries: `{invalid_color}`",
        f"- geometry or geometry-mesh PSOs: `{geometry}`",
        f"- malformed/extra-tail manifests: `{malformed}`",
        "",
        "## Render Target Counts",
        "",
    ]
    for rt_count, count in sorted(rt_counter.items()):
        lines.append(f"- `{rt_count}` render targets: `{count}`")

    lines.extend(["", "## Top Color Format Tuples", ""])
    for color_tuple, count in color_counter.most_common(20):
        label = ", ".join(color_tuple) if color_tuple else "none"
        lines.append(f"- `{label}`: `{count}`")

    lines.extend(["", "## Latest Interesting PSOs", ""])
    for row in rows[:60]:
        interesting = (
            not row["has_pixel"]
            or any(fmt == "invalid" for fmt in row["color_formats"])
            or row["num_render_targets"] >= 3
            or row["has_geometry"]
            or row["uses_geometry_mesh"]
        )
        if not interesting:
            continue
        lines.append(
            "- "
            f"`{row['name']}` rt=`{row['num_render_targets']}` "
            f"colors=`{','.join(row['color_formats'])}` "
            f"depth=`{row['depth_format']}` sample=`{row['sample_count']}` "
            f"ps=`{row['ps_hash']}` gs=`{row['gs_hash']}` "
            f"stage_in=`{row['uses_stage_in']}` mesh=`{row['uses_geometry_mesh']}`"
        )
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize captured D3D12 render PSO color-output state for Subnautica/UE5 debugging."
    )
    parser.add_argument(
        "--corpus",
        default="/Volumes/AverySSD/SteamLibrary/steamapps/common/Subnautica2/.metalsharp-cache/shader-cache/vkd3d/1962700",
    )
    parser.add_argument("--profile", default="subnautica2")
    parser.add_argument("--results-dir", default=str(Path(__file__).resolve().parents[1] / "results"))
    args = parser.parse_args()

    corpus = Path(args.corpus)
    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    manifests = sorted(corpus.glob("pso-render-*.json"), key=lambda p: p.stat().st_mtime, reverse=True)
    rows = [summarize_manifest(path) for path in manifests]
    out = {
        "schema": "metalsharp.d3d12-metal.pso-color-output-summary.v1",
        "profile": args.profile,
        "corpus": str(corpus),
        "render_pso_count": len(rows),
        "rows": rows,
    }
    json_path = results_dir / f"pso-color-output-summary-{args.profile}.json"
    md_path = results_dir / f"pso-color-output-summary-{args.profile}.md"
    json_path.write_text(json.dumps(out, indent=2) + "\n")
    write_markdown(md_path, rows)
    print(md_path)
    print(json_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
