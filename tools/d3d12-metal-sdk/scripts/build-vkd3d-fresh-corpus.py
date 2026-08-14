#!/usr/bin/env python3
"""Build the fresh Phase 1 corpus/source manifest for the VKD3D proof harness.

This is provenance tooling, not a legacy-result importer. It inventories and
copies source assets from local engine/vendor trees and optional fresh sparse SDK
clones into a new proof root. Every copied file receives a hash and source label.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


DEFAULT_LAB_ROOT = Path("/Volumes/AverySSD/MetalSharp-SM6-UE-Lab")
DEFAULT_UNREAL_ROOT = DEFAULT_LAB_ROOT / "02-unreal-full" / "release" / "UnrealEngine"
MAX_DEFAULT_FILE_BYTES = 64 * 1024 * 1024

SHADER_EXTENSIONS = {
    ".hlsl",
    ".hlsli",
    ".fx",
    ".fxh",
    ".usf",
    ".ush",
    ".metal",
    ".msl",
    ".dxil",
    ".dxbc",
    ".cso",
}
TEXTURE_EXTENSIONS = {
    ".dds",
    ".png",
    ".jpg",
    ".jpeg",
    ".tga",
    ".bmp",
    ".exr",
    ".hdr",
    ".tif",
    ".tiff",
    ".uasset",
}
SDK_EXTENSIONS = SHADER_EXTENSIONS | TEXTURE_EXTENSIONS | {".h", ".hpp", ".idl", ".json", ".props", ".targets"}


@dataclass(frozen=True)
class SourceSpec:
    family: str
    label: str
    root: Path
    relative_paths: tuple[str, ...]
    extensions: frozenset[str]
    required: bool = False
    license_note: str = "inventory/corpus use only; verify project license before redistribution"


@dataclass(frozen=True)
class RepoSpec:
    family: str
    label: str
    url: str
    sparse_paths: tuple[str, ...]
    extensions: frozenset[str] = field(default_factory=lambda: frozenset(SDK_EXTENSIONS))
    license_note: str = "public SDK/sample repository; verify upstream license before redistribution"


SDK_REPOS: tuple[RepoSpec, ...] = (
    RepoSpec(
        family="microsoft-sdk",
        label="DirectX-Graphics-Samples",
        url="https://github.com/microsoft/DirectX-Graphics-Samples.git",
        sparse_paths=("Samples/Desktop", "Libraries", "MiniEngine", "TechniqueDemos"),
    ),
    RepoSpec(
        family="microsoft-sdk",
        label="DirectXShaderCompiler",
        url="https://github.com/microsoft/DirectXShaderCompiler.git",
        sparse_paths=("tools/clang/test", "projects/dxilconv/test", "docs", "include"),
    ),
    RepoSpec(
        family="microsoft-sdk",
        label="DirectX-Headers",
        url="https://github.com/microsoft/DirectX-Headers.git",
        sparse_paths=("include", "src", "test"),
    ),
    RepoSpec(
        family="microsoft-sdk",
        label="DirectXTex",
        url="https://github.com/microsoft/DirectXTex.git",
        sparse_paths=("DirectXTex", "Texconv", "Texassemble", "DDSTextureLoader", "WICTextureLoader"),
    ),
    RepoSpec(
        family="microsoft-sdk",
        label="DirectXTK12",
        url="https://github.com/microsoft/DirectXTK12.git",
        sparse_paths=("Inc", "Src", "Shaders", "MakeSpriteFont"),
    ),
    RepoSpec(
        family="microsoft-sdk",
        label="WinPixEventRuntime",
        url="https://github.com/microsoft/WinPixEventRuntime.git",
        sparse_paths=("Include", "bin", "Samples"),
    ),
    RepoSpec(
        family="microsoft-sdk",
        label="D3D12MemoryAllocator",
        url="https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git",
        sparse_paths=("include", "src", "samples"),
    ),
    RepoSpec(
        family="unity-sdk",
        label="Unity-Graphics",
        url="https://github.com/Unity-Technologies/Graphics.git",
        sparse_paths=(
            "Packages/com.unity.render-pipelines.core/ShaderLibrary",
            "Packages/com.unity.render-pipelines.universal/Shaders",
            "Packages/com.unity.render-pipelines.high-definition/Runtime/RenderPipeline",
            "Tests/SRPTests/Projects",
        ),
    ),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def timestamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def disk_free_gib(path: Path) -> int:
    return int(shutil.disk_usage(path).free // (1024**3))


def run(command: list[str], cwd: Path | None = None, timeout: int = 900) -> dict[str, Any]:
    try:
        proc = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return {
            "command": command,
            "cwd": str(cwd) if cwd else None,
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "ok": proc.returncode == 0,
        }
    except (FileNotFoundError, subprocess.TimeoutExpired) as error:
        return {
            "command": command,
            "cwd": str(cwd) if cwd else None,
            "returncode": None,
            "stdout": "",
            "stderr": str(error),
            "ok": False,
        }


def safe_leaf(text: str) -> str:
    out = []
    for char in text:
        if char.isalnum() or char in {"-", "_", "."}:
            out.append(char)
        else:
            out.append("-")
    return "".join(out).strip("-") or "source"


def iter_files(root: Path, extensions: frozenset[str], max_bytes: int) -> Iterable[Path]:
    if not root.exists():
        return
    for current, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in {".git", "Binaries", "Saved", "DerivedDataCache", "Intermediate"}]
        current_path = Path(current)
        for name in sorted(files):
            path = current_path / name
            if path.suffix.lower() not in extensions:
                continue
            try:
                if path.stat().st_size > max_bytes:
                    continue
            except OSError:
                continue
            yield path


def category_for_extension(extension: str) -> str:
    if extension in SHADER_EXTENSIONS:
        return "shader"
    if extension in TEXTURE_EXTENSIONS:
        return "texture"
    return "sdk_metadata"


def copy_entry(
    source: Path,
    source_root: Path,
    proof_root: Path,
    family: str,
    label: str,
    license_note: str,
) -> dict[str, Any]:
    rel = source.relative_to(source_root) if source.is_relative_to(source_root) else Path(source.name)
    destination = proof_root / "01-corpus" / safe_leaf(family) / safe_leaf(label) / rel
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return {
        "family": family,
        "label": label,
        "source_path": str(source),
        "source_root": str(source_root),
        "relative_path": str(rel),
        "destination": str(destination),
        "extension": source.suffix.lower(),
        "category": category_for_extension(source.suffix.lower()),
        "size": destination.stat().st_size,
        "sha256": sha256(destination),
        "license_note": license_note,
    }


def collect_from_spec(spec: SourceSpec, proof_root: Path, limit: int, max_bytes: int) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    attempts: list[dict[str, Any]] = []
    for rel in spec.relative_paths:
        root = spec.root / rel
        attempts.append({"path": str(root), "exists": root.exists()})
        if not root.exists():
            continue
        for path in iter_files(root, spec.extensions, max_bytes):
            entries.append(copy_entry(path, spec.root, proof_root, spec.family, spec.label, spec.license_note))
            if limit > 0 and len(entries) >= limit:
                break
        if limit > 0 and len(entries) >= limit:
            break
    return {
        "family": spec.family,
        "label": spec.label,
        "root": str(spec.root),
        "required": spec.required,
        "attempts": attempts,
        "file_count": len(entries),
        "entries": entries,
        "ok": len(entries) > 0 or not spec.required,
    }


def clone_or_update_sparse(repo: RepoSpec, clones_root: Path) -> dict[str, Any]:
    target = clones_root / safe_leaf(repo.label)
    commands: list[dict[str, Any]] = []
    if not target.exists():
        commands.append(
            run(
                [
                    "git",
                    "clone",
                    "--depth",
                    "1",
                    "--filter=blob:none",
                    "--sparse",
                    repo.url,
                    str(target),
                ],
                timeout=1800,
            )
        )
    else:
        commands.append(run(["git", "fetch", "--depth", "1", "origin", "HEAD"], cwd=target, timeout=900))
    if target.exists():
        commands.append(run(["git", "sparse-checkout", "set", *repo.sparse_paths], cwd=target, timeout=900))
        commands.append(run(["git", "rev-parse", "HEAD"], cwd=target, timeout=60))
    head = commands[-1]["stdout"].strip() if commands and commands[-1]["ok"] else ""
    return {
        "family": repo.family,
        "label": repo.label,
        "url": repo.url,
        "target": str(target),
        "sparse_paths": list(repo.sparse_paths),
        "commands": commands,
        "head": head,
        "ok": target.exists() and bool(head),
    }


def inventory_repo(repo: RepoSpec, clone_result: dict[str, Any], proof_root: Path, limit: int, max_bytes: int) -> dict[str, Any]:
    target = Path(clone_result["target"])
    entries: list[dict[str, Any]] = []
    if target.exists():
        for rel in repo.sparse_paths:
            root = target / rel
            if not root.exists():
                continue
            for path in iter_files(root, repo.extensions, max_bytes):
                entries.append(copy_entry(path, target, proof_root, repo.family, repo.label, repo.license_note))
                if limit > 0 and len(entries) >= limit:
                    break
            if limit > 0 and len(entries) >= limit:
                break
    return {
        "family": repo.family,
        "label": repo.label,
        "url": repo.url,
        "clone": clone_result,
        "file_count": len(entries),
        "entries": entries,
        "ok": clone_result.get("ok", False) and len(entries) > 0,
    }


def sdk_probe(repo: RepoSpec) -> dict[str, Any]:
    result = run(["git", "ls-remote", "--heads", repo.url], timeout=120)
    heads = []
    if result["ok"]:
        for line in result["stdout"].splitlines()[:20]:
            parts = line.split()
            if len(parts) == 2:
                heads.append({"sha": parts[0], "ref": parts[1]})
    return {
        "family": repo.family,
        "label": repo.label,
        "url": repo.url,
        "reachable": result["ok"],
        "heads_sample": heads,
        "stderr": result["stderr"],
    }


def default_unity_roots() -> list[str]:
    roots: list[str] = []
    app_support = Path.home() / "Library" / "Application Support"
    if app_support.exists():
        for child in sorted(app_support.iterdir()):
            if child.is_dir() and "unity" in child.name.lower():
                roots.append(str(child))
    for candidate in (Path("/Applications") / "Unity", Path("/Applications") / "Unity Hub.app"):
        if candidate.exists():
            roots.append(str(candidate))
    return roots


def build_local_specs(args: argparse.Namespace) -> list[SourceSpec]:
    unreal = Path(args.unreal_root).expanduser()
    microsoft_roots = [Path(path).expanduser() for path in args.microsoft_root]
    unity_roots = [Path(path).expanduser() for path in args.unity_root]

    specs: list[SourceSpec] = [
        SourceSpec(
            family="unreal",
            label="UnrealEngine-SM5-SM6-Shaders",
            root=unreal,
            relative_paths=(
                "Engine/Shaders",
                "Engine/Plugins/PCG/Shaders",
                "Engine/Plugins/TextureGraph/Shaders",
                "Engine/Plugins/Developer/ShaderToolkit",
                "Engine/Intermediate/ShaderAutogen",
            ),
            extensions=frozenset(SHADER_EXTENSIONS),
            required=True,
            license_note="local Unreal Engine repository; proof inventory only",
        ),
        SourceSpec(
            family="unreal",
            label="UnrealEngine-Textures",
            root=unreal,
            relative_paths=(
                "Samples/StarterContent/Content/StarterContent/Textures",
                "Samples",
                "Templates",
                "Engine/Content/StarterContent/Textures",
                "Engine/Content/Engine_MI_Shaders/Textures",
                "Engine/Content/EditorShapes/Textures",
                "Engine/Content/MaterialTemplates/Textures",
                "Engine/Content",
                "Engine/Plugins",
            ),
            extensions=frozenset(TEXTURE_EXTENSIONS),
            required=True,
            license_note="local Unreal Engine repository texture assets; proof inventory only",
        ),
    ]
    for index, root in enumerate(microsoft_roots):
        specs.append(
            SourceSpec(
                family="microsoft-local",
                label=f"Microsoft-Local-{index + 1}",
                root=root,
                relative_paths=(".",),
                extensions=frozenset(SDK_EXTENSIONS),
                required=False,
                license_note="local Microsoft/DirectX/DXC material; proof inventory only",
            )
        )
    for index, root in enumerate(unity_roots):
        specs.append(
            SourceSpec(
                family="unity-local",
                label=f"Unity-Local-{index + 1}",
                root=root,
                relative_paths=(".",),
                extensions=frozenset(SDK_EXTENSIONS),
                required=False,
                license_note="local Unity material if present; proof inventory only",
            )
        )
    return specs


def main() -> int:
    parser = argparse.ArgumentParser(description="Build fresh VKD3D corpus and SDK provenance manifests.")
    parser.add_argument("--lab-root", default=str(DEFAULT_LAB_ROOT))
    parser.add_argument("--proof-root", default="")
    parser.add_argument("--unreal-root", default=str(DEFAULT_UNREAL_ROOT))
    parser.add_argument("--microsoft-root", action="append", default=[])
    parser.add_argument("--unity-root", action="append", default=[])
    parser.add_argument("--fetch-sdks", action="store_true", help="Sparse-clone configured public SDK/sample repositories.")
    parser.add_argument("--sdk-probe-only", action="store_true", help="Only run git ls-remote reachability for online SDKs.")
    parser.add_argument("--limit-per-source", type=int, default=500)
    parser.add_argument("--target-shaders", type=int, default=300)
    parser.add_argument("--target-textures", type=int, default=300)
    parser.add_argument("--target-total", type=int, default=600)
    parser.add_argument("--max-file-mib", type=int, default=64)
    parser.add_argument("--min-free-gib", type=int, default=50)
    args = parser.parse_args()

    lab_root = Path(args.lab_root).expanduser()
    if not lab_root.exists():
        print(f"lab root missing: {lab_root}", file=sys.stderr)
        return 2
    free_gib = disk_free_gib(lab_root)
    if free_gib < args.min_free_gib:
        print(f"disk guard failed: {free_gib} GiB free < {args.min_free_gib} GiB", file=sys.stderr)
        return 2

    proof_root = Path(args.proof_root).expanduser() if args.proof_root else (
        lab_root / "06-results" / "in-progress" / f"vkd3d-fresh-corpus-{timestamp()}"
    )
    lab_resolved = lab_root.resolve()
    proof_resolved = proof_root.resolve(strict=False)
    if not (str(proof_resolved) == str(lab_resolved) or str(proof_resolved).startswith(str(lab_resolved) + os.sep)):
        print(f"proof root must be under lab root: {proof_root}", file=sys.stderr)
        return 2
    proof_root.mkdir(parents=True, exist_ok=False)

    max_bytes = args.max_file_mib * 1024 * 1024
    if not args.microsoft_root:
        args.microsoft_root.extend(
            [
                str(Path.home() / "metalsharp-vkd3d-lab" / "tools" / "d3d12-metal-sdk" / "cache" / "dxc"),
                str(DEFAULT_LAB_ROOT / "03-dxc"),
            ]
        )
    if not args.unity_root:
        args.unity_root.extend(default_unity_roots())

    sdk_reachability = [sdk_probe(repo) for repo in SDK_REPOS]
    clone_results: list[dict[str, Any]] = []
    sdk_inventory: list[dict[str, Any]] = []
    if args.fetch_sdks and not args.sdk_probe_only:
        clones_root = proof_root / "00-sdk-clones"
        for repo in SDK_REPOS:
            clone = clone_or_update_sparse(repo, clones_root)
            clone_results.append(clone)
            sdk_inventory.append(inventory_repo(repo, clone, proof_root, args.limit_per_source, max_bytes))

    local_inventory = [
        collect_from_spec(spec, proof_root, args.limit_per_source, max_bytes)
        for spec in build_local_specs(args)
    ]

    all_entries: list[dict[str, Any]] = []
    for group in local_inventory + sdk_inventory:
        all_entries.extend(group["entries"])

    source_families = sorted({entry["family"] for entry in all_entries})
    extension_counts: dict[str, int] = {}
    category_counts: dict[str, int] = {"shader": 0, "texture": 0, "sdk_metadata": 0}
    family_category_counts: dict[str, dict[str, int]] = {}
    for entry in all_entries:
        extension_counts[entry["extension"]] = extension_counts.get(entry["extension"], 0) + 1
        category = entry.get("category", "sdk_metadata")
        category_counts[category] = category_counts.get(category, 0) + 1
        family_counts = family_category_counts.setdefault(entry["family"], {"shader": 0, "texture": 0, "sdk_metadata": 0})
        family_counts[category] = family_counts.get(category, 0) + 1

    target_status = {
        "target_total": args.target_total,
        "target_shaders": args.target_shaders,
        "target_textures": args.target_textures,
        "entry_count": len(all_entries),
        "shader_count": category_counts.get("shader", 0),
        "texture_count": category_counts.get("texture", 0),
        "total_ok": len(all_entries) >= args.target_total,
        "shader_ok": category_counts.get("shader", 0) >= args.target_shaders,
        "texture_ok": category_counts.get("texture", 0) >= args.target_textures,
        "note": "Hundreds-scale targets are required for final game-harness acceptance when source material is available.",
    }

    manifest = {
        "schema": "metalsharp.vkd3d.fresh.corpus-manifest.v1",
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "proof_root": str(proof_root),
        "policy": {
            "fresh_artifacts_only": True,
            "prior_proof_results_imported": False,
            "requires_hash_provenance": True,
        },
        "disk_guard": {"path": str(lab_root), "free_gib": free_gib, "min_free_gib": args.min_free_gib, "ok": True},
        "settings": {
            "limit_per_source": args.limit_per_source,
            "target_shaders": args.target_shaders,
            "target_textures": args.target_textures,
            "target_total": args.target_total,
            "max_file_mib": args.max_file_mib,
            "fetch_sdks": args.fetch_sdks,
            "sdk_probe_only": args.sdk_probe_only,
        },
        "sdk_reachability": sdk_reachability,
        "sdk_clones": clone_results,
        "local_inventory": local_inventory,
        "sdk_inventory": sdk_inventory,
        "entry_count": len(all_entries),
        "source_families": source_families,
        "extension_counts": extension_counts,
        "category_counts": category_counts,
        "family_category_counts": family_category_counts,
        "target_status": target_status,
        "entries": all_entries,
    }
    write_json(proof_root / "fresh-corpus-manifest.json", manifest)

    tsv_lines = ["family\tlabel\tcategory\textension\tsize\tsha256\tdestination\tsource_path"]
    for entry in all_entries:
        tsv_lines.append(
            "\t".join(
                [
                    entry["family"],
                    entry["label"],
                    entry["category"],
                    entry["extension"],
                    str(entry["size"]),
                    entry["sha256"],
                    entry["destination"],
                    entry["source_path"],
                ]
            )
        )
    write_text(proof_root / "fresh-corpus-files.tsv", "\n".join(tsv_lines) + "\n")

    summary = {
        "schema": "metalsharp.vkd3d.fresh.corpus-summary.v1",
        "ok": len(all_entries) > 0
        and target_status["total_ok"]
        and target_status["shader_ok"]
        and target_status["texture_ok"]
        and any(group["family"] == "unreal" and group["file_count"] > 0 for group in local_inventory)
        and any(probe["reachable"] for probe in sdk_reachability),
        "proof_root": str(proof_root),
        "entry_count": len(all_entries),
        "source_families": source_families,
        "extension_counts": extension_counts,
        "category_counts": category_counts,
        "family_category_counts": family_category_counts,
        "target_status": target_status,
        "sdk_reachable_count": sum(1 for probe in sdk_reachability if probe["reachable"]),
        "sdk_fetched_count": sum(1 for clone in clone_results if clone.get("ok")),
        "manifest": str(proof_root / "fresh-corpus-manifest.json"),
        "files_tsv": str(proof_root / "fresh-corpus-files.tsv"),
    }
    write_json(proof_root / "fresh-corpus-summary.json", summary)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
