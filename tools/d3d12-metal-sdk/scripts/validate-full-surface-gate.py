#!/usr/bin/env python3
"""Evaluate the full-surface aggregate gate without weakening open phases.

The gate is deliberately fail-closed.  It validates the synchronized contract
validators, checks the latest behavior result for required positive evidence,
and requires the phase coverage manifest to be explicitly closed.  A manifest
row marked open is a blocker even when its focused probe passes.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[3]
SDK_DIR = ROOT_DIR / "tools" / "d3d12-metal-sdk"
CONTRACT_DIR = SDK_DIR / "contracts"
DEFAULT_RESULTS_DIR = SDK_DIR / "results"
DEFAULT_MANIFEST = CONTRACT_DIR / "phase3-exhaustive-coverage.json"
PHASE4_MANIFEST = CONTRACT_DIR / "phase4-command-coverage.json"
PHASE5_MANIFEST = CONTRACT_DIR / "phase5-shader-coverage.json"
PHASE7_MANIFEST = CONTRACT_DIR / "phase7-mesh-workgraph-coverage.json"
PHASE8_MANIFEST = CONTRACT_DIR / "phase8-dxr-coverage.json"
PHASE9_MANIFEST = CONTRACT_DIR / "phase9-video-coverage.json"
PHASE12_MANIFEST = CONTRACT_DIR / "phase12-display-coverage.json"
PHASE13_MANIFEST = CONTRACT_DIR / "phase13-diagnostics-coverage.json"
VALIDATORS = (
    "validate-contracts.py",
    "validate-probe-matrix.py",
    "validate-interface-census.py",
    "validate-full-surface-contract.py",
    "validate-shader-engine.py",
)
MISSING = object()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Evaluate the fail-closed D3D12 full-surface aggregate gate."
    )
    parser.add_argument(
        "--profile", default="metalsharp-isolated", help="Probe result suffix."
    )
    parser.add_argument(
        "--results-dir", type=Path, default=DEFAULT_RESULTS_DIR
    )
    parser.add_argument("--manifest", type=Path,
                        help="Coverage manifest (defaults to the selected phase).")
    parser.add_argument(
        "--phase", choices=("3", "4", "5", "7", "8", "9", "12", "13", "all"), default="all",
        help="Gate Phase 3, Phase 4, Phase 5, Phase 7, Phase 8, Phase 9, Phase 12, Phase 13, or all currently declared phases (default: all).",
    )
    parser.add_argument(
        "--format", choices=("text", "json"), default="text"
    )
    parser.add_argument("--json-out", type=Path)
    parser.add_argument(
        "--allow-open", action="store_true",
        help="Return success after reporting blockers; never reports pass=true.",
    )
    return parser.parse_args()


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def get_path(value: Any, components: list[str]) -> Any:
    current = value
    for component in components:
        if isinstance(current, dict):
            if component not in current:
                return MISSING
            current = current[component]
        elif isinstance(current, list) and component.isdigit():
            index = int(component)
            if index >= len(current):
                return MISSING
            current = current[index]
        else:
            return MISSING
    return current


def result_file(results_dir: Path, stem: str, profile: str) -> Path:
    return results_dir / f"{stem}-{profile}.json"


def result_specs(row: dict[str, Any], default_profile: str) -> list[tuple[str, str]]:
    """Return the explicitly declared result stem/profile pairs for a row.

    Most rows use one result and inherit the gate profile.  A few aggregate
    rows intentionally combine independent evidence files; those rows may
    provide a semicolon-separated result string and a matching
    ``result_profiles`` list.  Never discover arbitrary matching files here:
    the manifest must name every evidence source and profile.
    """
    declared = row.get("result", "")
    if isinstance(declared, str):
        stems = [stem.strip() for stem in declared.split(";") if stem.strip()]
    elif isinstance(declared, list):
        stems = [str(stem).strip() for stem in declared if str(stem).strip()]
    else:
        stems = []

    declared_profiles = row.get("result_profiles")
    if isinstance(declared_profiles, list):
        profiles = [str(profile) for profile in declared_profiles]
        if len(profiles) != len(stems):
            return [(stem, "") for stem in stems]
    else:
        row_profile = str(row.get("profile", default_profile))
        profiles = [row_profile] * len(stems)
    return list(zip(stems, profiles))


def run_validators() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for validator in VALIDATORS:
        path = SDK_DIR / "scripts" / validator
        completed = subprocess.run(
            ["python3", str(path)],
            cwd=ROOT_DIR,
            text=True,
            capture_output=True,
            check=False,
        )
        output = (completed.stdout + completed.stderr).strip()
        rows.append(
            {
                "id": validator,
                "pass": completed.returncode == 0,
                "returncode": completed.returncode,
                "output": output,
            }
        )
    return rows


def valid_loader_audit(audit: Any) -> bool:
    """Check retained copy evidence, not just its self-reported match flag.

    Temporary aliases are removed after a run, so validate the captured
    metadata without requiring historical paths to remain live.
    """
    required = {"d3d12.dll", "d3d11.dll", "d3d10core.dll", "dxgi.dll",
                "dxgi_dxmt.dll", "winemetal.dll"}
    if not isinstance(audit, dict) or not required.issubset(audit):
        return False
    for record in audit.values():
        if not isinstance(record, dict) or record.get("match") is not True:
            return False
        selected, copied = record.get("selected"), record.get("probe_copy")
        for artifact in (selected, copied):
            if not isinstance(artifact, dict):
                return False
            digest = artifact.get("sha256")
            if (not isinstance(artifact.get("path"), str) or not artifact["path"] or
                    type(artifact.get("size")) is not int or artifact["size"] <= 0 or
                    not isinstance(digest, str) or len(digest) != 64 or
                    any(character not in "0123456789abcdef" for character in digest)):
                return False
        if selected["size"] != copied["size"] or selected["sha256"] != copied["sha256"]:
            return False
    return True


def check_result(
    results_dir: Path, profile: str, row: dict[str, Any]
) -> tuple[dict[str, Any], list[str]]:
    specs = result_specs(row, profile)
    if not specs or any(not result_profile for _, result_profile in specs):
        return ({"id": row.get("id"), "pass": False,
                 "detail": "missing result specification or profile"}, [str(row.get("id"))])
    paths = [result_file(results_dir, stem, result_profile)
             for stem, result_profile in specs]
    blockers: list[str] = []
    missing = [str(path) for path in paths if not path.exists()]
    if missing:
        return (
            {"id": row.get("id"), "result": ";".join(str(path) for path in paths),
             "pass": False, "detail": f"behavior result is missing: {', '.join(missing)}"},
            [str(row.get("id"))],
        )

    # Each probe invocation writes a host-runtime identity record before any
    # evidence result.  Require every named result to be newer than the
    # identity record for its declared profile; otherwise a narrowed or
    # interrupted run could silently reuse a previous passing JSON file.
    stale: list[str] = []
    identity_errors: list[str] = []
    for path, (_, result_profile) in zip(paths, specs):
        identity_path = result_file(results_dir, "host-runtime", result_profile)
        try:
            identity = load_json(identity_path)
            if not isinstance(identity, dict):
                identity_errors.append(f"{identity_path}: run identity must be an object")
                continue
            run_started_at = identity.get("run_started_at")
            loader_audit = identity.get("loader_pe_copy_audit")
            loader_ok = valid_loader_audit(loader_audit)
            if (identity.get("profile") != result_profile or
                    type(run_started_at) not in (int, float) or
                    not math.isfinite(run_started_at) or run_started_at <= 0 or not loader_ok):
                identity_errors.append(
                    f"{identity_path}: missing matching profile/run_started_at/loader_pe_copy_audit"
                )
            elif path.stat().st_mtime + 1e-6 < float(run_started_at):
                stale.append(f"{path} (before run_started_at={run_started_at})")
        except (OSError, json.JSONDecodeError, OverflowError) as exc:
            identity_errors.append(f"{identity_path}: {exc}")
    if identity_errors or stale:
        detail_parts = []
        if identity_errors:
            detail_parts.append("invalid or missing run identity: " + ", ".join(identity_errors))
        if stale:
            detail_parts.append("stale behavior result: " + ", ".join(stale))
        return (
            {"id": row.get("id"), "result": ";".join(str(path) for path in paths),
             "pass": False, "detail": "; ".join(detail_parts)},
            [str(row.get("id"))],
        )

    results: list[Any] = []
    invalid: list[str] = []
    for path in paths:
        try:
            results.append(load_json(path))
        except (OSError, json.JSONDecodeError) as exc:
            invalid.append(f"{path}: {exc}")
    if invalid:
        return (
            {"id": row.get("id"), "result": ";".join(str(path) for path in paths),
             "pass": False, "detail": f"behavior result is invalid: {', '.join(invalid)}"},
            [str(row.get("id"))],
        )

    checks: list[dict[str, Any]] = []
    row_pass = True
    for check in row.get("checks", []):
        if not isinstance(check, list) or len(check) < 1:
            row_pass = False
            checks.append({"path": check, "pass": False,
                           "detail": "invalid check declaration"})
            continue

        # A one-element declaration means that the named field must be true.
        # For a two-or-more element declaration, first try the complete list
        # as a nested boolean path (the existing contract form), then fall
        # back to an exact value assertion on the first path component.  This
        # supports exact integers, arrays, strings, and explicit false values
        # without weakening boolean checks.
        nested_parts = [str(part) for part in check]
        observed = MISSING
        passed = False
        matched_path = ".".join(nested_parts)
        for result in results:
            candidate = get_path(result, nested_parts)
            if candidate is True:
                observed = candidate
                passed = True
                break
            if observed is MISSING and candidate is not MISSING:
                observed = candidate

        expected = MISSING
        if not passed and len(check) >= 2:
            expected = check[-1]
            value_parts = [str(check[0])] if len(check) == 2 else [str(part) for part in check[:-1]]
            matched_path = ".".join(value_parts)
            for result in results:
                candidate = get_path(result, value_parts)
                if candidate is not MISSING and candidate == expected:
                    observed = candidate
                    passed = True
                    break
                if observed is MISSING and candidate is not MISSING:
                    observed = candidate

        row_pass = row_pass and passed
        check_result_row: dict[str, Any] = {
            "path": matched_path,
            "observed": None if observed is MISSING else observed,
            "pass": passed,
            "detail": "positive behavior evidence present"
            if passed else "required positive behavior evidence is missing or false",
        }
        if expected is not MISSING:
            check_result_row["expected"] = expected
        checks.append(check_result_row)

    if not row_pass:
        blockers.append(str(row.get("id")))
    return (
        {"id": row.get("id"), "result": ";".join(str(path) for path in paths),
         "pass": row_pass, "manifest_status": row.get("status"),
         "checks": checks, "remaining": row.get("remaining", "")},
        blockers,
    )


def build_summary(args: argparse.Namespace) -> dict[str, Any]:
    manifest_path = args.manifest or (
        PHASE4_MANIFEST if args.phase == "4" else
        PHASE5_MANIFEST if args.phase == "5" else
        PHASE7_MANIFEST if args.phase == "7" else
        PHASE8_MANIFEST if args.phase == "8" else
        PHASE9_MANIFEST if args.phase == "9" else
        PHASE12_MANIFEST if args.phase == "12" else
        PHASE13_MANIFEST if args.phase == "13" else DEFAULT_MANIFEST
    )
    blockers: list[dict[str, str]] = []
    validator_rows = run_validators()
    for row in validator_rows:
        if not row["pass"]:
            blockers.append({"id": row["id"], "detail": row["output"] or "validator failed"})

    manifest_error: str | None = None
    manifest: dict[str, Any] = {}
    try:
        value = load_json(manifest_path)
        if not isinstance(value, dict):
            manifest_error = "coverage manifest must be a JSON object"
        else:
            manifest = value
    except (OSError, json.JSONDecodeError) as exc:
        manifest_error = f"coverage manifest cannot be loaded: {exc}"
    if manifest_error:
        blockers.append({"id": f"phase{args.phase}-coverage-manifest",
                         "detail": manifest_error})

    if args.phase == "all":
        full_contract_path = CONTRACT_DIR / "d3d12-full-surface-contract.json"
        try:
            full_contract = load_json(full_contract_path)
        except (OSError, json.JSONDecodeError) as exc:
            full_contract = {}
            blockers.append({"id": "full-surface-contract", "detail": f"cannot load full-surface contract: {exc}"})
        if isinstance(full_contract, dict):
            if full_contract.get("summary", {}).get("promotion_ready") is not True:
                blockers.append({"id": "full-surface-promotion", "detail": "full-surface contract promotion_ready is not true"})
        for ledger_name in ("unsupported-api-ledger.json", "risky-stub-ledger.json"):
            ledger_path = CONTRACT_DIR / ledger_name
            try:
                ledger = load_json(ledger_path)
            except (OSError, json.JSONDecodeError) as exc:
                blockers.append({"id": ledger_name, "detail": f"cannot load ledger: {exc}"})
                continue
            entries = ledger.get("entries", []) if isinstance(ledger, dict) else []
            open_entries = [entry.get("api", "<unnamed>") for entry in entries
                            if isinstance(entry, dict) and entry.get("state") in
                            {"unsupported", "limited_to_proven_probe", "stub_risky"}]
            if open_entries:
                blockers.append({"id": ledger_name, "detail": f"open ledger entries: {', '.join(open_entries)}"})

    manifest_rows: list[dict[str, Any]] = []
    if manifest:
        expected_schema = {
            "3": "metalsharp.d3d12.phase3-exhaustive-coverage.v1",
            "4": "metalsharp.d3d12.phase4-command-coverage.v1",
            "5": "metalsharp.d3d12.phase5-shader-coverage.v1",
            "7": "metalsharp.d3d12.phase7-mesh-workgraph-coverage.v1",
            "8": "metalsharp.d3d12.phase8-dxr-coverage.v1",
            "9": "metalsharp.d3d12.phase9-video-coverage.v1",
            "12": "metalsharp.d3d12.phase12-display-coverage.v1",
            "13": "metalsharp.d3d12.phase13-diagnostics-coverage.v1",
        }.get(args.phase)
        if args.phase == "all":
            expected_schema = "metalsharp.d3d12.phase3-exhaustive-coverage.v1"
        if expected_schema and manifest.get("schema") != expected_schema:
            blockers.append({"id": f"phase{args.phase}-coverage-manifest-schema",
                             "detail": "unexpected coverage manifest schema"})
        if args.phase in ("3", "4", "5", "7", "8", "9", "12", "13", "all"):
            if manifest.get("status") != "closed":
                blockers.append({"id": f"phase{args.phase}-coverage-status",
                                 "detail": f"manifest status is {manifest.get('status')!r}, not 'closed'"})
            rows = manifest.get("rows")
            if not isinstance(rows, list) or not rows:
                blockers.append({"id": f"phase{args.phase}-coverage-rows",
                                 "detail": "coverage manifest rows are missing or empty"})
            else:
                for row in rows:
                    if not isinstance(row, dict) or not row.get("id"):
                        blockers.append({"id": f"phase{args.phase}-coverage-row-shape",
                                         "detail": "coverage row is not a named object"})
                        continue
                    evidence, row_blockers = check_result(args.results_dir, args.profile, row)
                    manifest_rows.append(evidence)
                    for row_id in row_blockers:
                        blockers.append({"id": row_id, "detail": "required behavior evidence failed"})
                    if row.get("status") != "closed":
                        blockers.append({"id": str(row["id"]), "detail": row.get("remaining", "coverage row is still open")})

    additional_manifests: list[str] = []
    if args.phase == "all":
        # Keep the historical Phase 3 manifest as the primary input while
        # making an all-phase gate fail closed when a later declared phase has
        # an open manifest of its own.
        additional_manifests.extend((str(PHASE4_MANIFEST), str(PHASE5_MANIFEST), str(PHASE7_MANIFEST)))
        try:
            phase4_manifest = load_json(PHASE4_MANIFEST)
        except (OSError, json.JSONDecodeError) as exc:
            phase4_manifest = {}
            blockers.append({"id": "phase4-coverage-manifest",
                             "detail": f"coverage manifest cannot be loaded: {exc}"})
        if isinstance(phase4_manifest, dict):
            if phase4_manifest.get("schema") != "metalsharp.d3d12.phase4-command-coverage.v1":
                blockers.append({"id": "phase4-coverage-manifest-schema",
                                 "detail": "unexpected Phase 4 coverage manifest schema"})
            if phase4_manifest.get("status") != "closed":
                blockers.append({"id": "phase4-coverage-status",
                                 "detail": f"manifest status is {phase4_manifest.get('status')!r}, not 'closed'"})
            phase4_rows = phase4_manifest.get("rows")
            if not isinstance(phase4_rows, list) or not phase4_rows:
                blockers.append({"id": "phase4-coverage-rows",
                                 "detail": "coverage manifest rows are missing or empty"})
            else:
                for row in phase4_rows:
                    if not isinstance(row, dict) or not row.get("id"):
                        blockers.append({"id": "phase4-coverage-row-shape",
                                         "detail": "coverage row is not a named object"})
                        continue
                    evidence, row_blockers = check_result(
                        args.results_dir, args.profile, row
                    )
                    evidence["phase"] = 4
                    manifest_rows.append(evidence)
                    for row_id in row_blockers:
                        blockers.append({"id": f"phase4:{row_id}",
                                         "detail": "required behavior evidence failed"})
                    if row.get("status") != "closed":
                        blockers.append({"id": f"phase4:{row['id']}",
                                         "detail": row.get("remaining", "coverage row is still open")})

        try:
            phase5_manifest = load_json(PHASE5_MANIFEST)
        except (OSError, json.JSONDecodeError) as exc:
            phase5_manifest = {}
            blockers.append({"id": "phase5-coverage-manifest",
                             "detail": f"coverage manifest cannot be loaded: {exc}"})
        if isinstance(phase5_manifest, dict):
            if phase5_manifest.get("schema") != "metalsharp.d3d12.phase5-shader-coverage.v1":
                blockers.append({"id": "phase5-coverage-manifest-schema",
                                 "detail": "unexpected Phase 5 coverage manifest schema"})
            if phase5_manifest.get("status") != "closed":
                blockers.append({"id": "phase5-coverage-status",
                                 "detail": f"coverage manifest status is {phase5_manifest.get('status')!r}, not 'closed'"})
            phase5_rows = phase5_manifest.get("rows")
            if not isinstance(phase5_rows, list) or not phase5_rows:
                blockers.append({"id": "phase5-coverage-rows",
                                 "detail": "coverage manifest rows are missing or empty"})
            else:
                for row in phase5_rows:
                    if not isinstance(row, dict) or not row.get("id"):
                        blockers.append({"id": "phase5-coverage-row-shape",
                                         "detail": "coverage row is not a named object"})
                        continue
                    evidence, row_blockers = check_result(
                        args.results_dir, args.profile, row
                    )
                    evidence["phase"] = 5
                    manifest_rows.append(evidence)
                    for row_id in row_blockers:
                        blockers.append({"id": f"phase5:{row_id}",
                                         "detail": "required behavior evidence failed"})
                    if row.get("status") != "closed":
                        blockers.append({"id": f"phase5:{row['id']}",
                                         "detail": row.get("remaining", "coverage row is still open")})

        try:
            phase7_manifest = load_json(PHASE7_MANIFEST)
        except (OSError, json.JSONDecodeError) as exc:
            phase7_manifest = {}
            blockers.append({"id": "phase7-coverage-manifest",
                             "detail": f"coverage manifest cannot be loaded: {exc}"})
        if isinstance(phase7_manifest, dict):
            if phase7_manifest.get("schema") != "metalsharp.d3d12.phase7-mesh-workgraph-coverage.v1":
                blockers.append({"id": "phase7-coverage-manifest-schema",
                                 "detail": "unexpected Phase 7 coverage manifest schema"})
            if phase7_manifest.get("status") != "closed":
                blockers.append({"id": "phase7-coverage-status",
                                 "detail": f"manifest status is {phase7_manifest.get('status')!r}, not 'closed'"})
            phase7_rows = phase7_manifest.get("rows")
            if not isinstance(phase7_rows, list) or not phase7_rows:
                blockers.append({"id": "phase7-coverage-rows",
                                 "detail": "coverage manifest rows are missing or empty"})
            else:
                for row in phase7_rows:
                    if not isinstance(row, dict) or not row.get("id"):
                        blockers.append({"id": "phase7-coverage-row-shape",
                                         "detail": "coverage row is not a named object"})
                        continue
                    evidence, row_blockers = check_result(
                        args.results_dir, args.profile, row
                    )
                    evidence["phase"] = 7
                    manifest_rows.append(evidence)
                    for row_id in row_blockers:
                        blockers.append({"id": f"phase7:{row_id}",
                                         "detail": "required behavior evidence failed"})
                    if row.get("status") != "closed":
                        blockers.append({"id": f"phase7:{row['id']}",
                                         "detail": row.get("remaining", "coverage row is still open")})

    result = {
        "schema": "metalsharp.d3d12-metal.full-surface-gate-result.v1",
        "profile": args.profile,
        "phase": args.phase,
        "pass": not blockers,
        "promotion_ready": not blockers,
        "validators": validator_rows,
        "coverage_manifest": str(manifest_path),
        "additional_coverage_manifests": additional_manifests,
        "coverage_rows": manifest_rows,
        "blockers": blockers,
    }
    return result


def print_summary(summary: dict[str, Any], output_format: str) -> None:
    if output_format == "json":
        print(json.dumps(summary, indent=2, sort_keys=True))
        return
    print(f"[{'PASS' if summary['pass'] else 'FAIL'}] full-surface aggregate gate")
    for blocker in summary["blockers"]:
        print(f"  - {blocker['id']}: {blocker['detail']}")


def main() -> int:
    args = parse_args()
    summary = build_summary(args)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print_summary(summary, args.format)
    if summary["pass"]:
        return 0
    return 0 if args.allow_open else 1


if __name__ == "__main__":
    raise SystemExit(main())
