#!/usr/bin/env python3
"""Evaluate the behavior-backed D3D12 FL12_2 promotion gate.

This script intentionally returns non-zero while the target is incomplete.  A
failed result is useful: it contains the exact query, behavior, identity, and
provenance blockers instead of allowing a query-only feature report to pass.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[3]
SDK_DIR = ROOT_DIR / "tools" / "d3d12-metal-sdk"
DEFAULT_RESULTS_DIR = SDK_DIR / "results"
DEFAULT_CONTRACT = SDK_DIR / "contracts" / "fl12-2-gate-contract.json"
MISSING = "<missing>"
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


@dataclass
class Check:
    kind: str
    check_id: str
    api: str
    result: str
    path: str
    operator: str
    expected: Any
    observed: Any
    passed: bool
    detail: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "id": self.check_id,
            "api": self.api,
            "result": self.result,
            "path": self.path,
            "operator": self.operator,
            "expected": self.expected,
            "observed": self.observed,
            "pass": self.passed,
            "detail": self.detail,
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Evaluate the behavior-backed D3D12 FL12_2 promotion gate."
    )
    parser.add_argument(
        "--profile",
        default="metalsharp-isolated",
        help="Result profile suffix (default: metalsharp-isolated).",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=DEFAULT_RESULTS_DIR,
        help="Directory containing probe and identity JSON results.",
    )
    parser.add_argument(
        "--contract",
        type=Path,
        default=DEFAULT_CONTRACT,
        help="FL12_2 gate contract JSON.",
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=ROOT_DIR,
        help="Repository root used to verify the recorded source identity.",
    )
    parser.add_argument("--json-out", type=Path, help="Write the full gate result here.")
    parser.add_argument("--markdown-out", type=Path, help="Write a Markdown summary here.")
    return parser.parse_args()


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def result_path(results_dir: Path, result_stem: str, profile: str) -> Path:
    return results_dir / f"{result_stem}-{profile}.json"


def safe_load_result(results_dir: Path, result_stem: str, profile: str) -> tuple[Any, str | None]:
    path = result_path(results_dir, result_stem, profile)
    if not path.exists():
        return None, f"result file is missing: {path}"
    try:
        return load_json(path), None
    except (OSError, json.JSONDecodeError) as exc:
        return None, f"result file cannot be read as JSON: {path}: {exc}"


def get_path(value: Any, path: str) -> Any:
    if path == "$status":
        if not isinstance(value, dict):
            return MISSING
        for key in ("pass", "passed", "ok"):
            if key in value:
                return value[key]
        return MISSING
    current = value
    for component in path.split(".") if path else []:
        if not isinstance(current, dict) or component not in current:
            return MISSING
        current = current[component]
    return current


def parse_version(value: Any) -> tuple[int, ...] | None:
    if not isinstance(value, str):
        return None
    parts = value.split("_")
    if not parts or any(not part.isdigit() for part in parts):
        return None
    return tuple(int(part) for part in parts)


def parse_hresult(value: Any) -> int | None:
    if isinstance(value, int):
        return value & 0xFFFFFFFF
    if not isinstance(value, str):
        return None
    text = value.strip().lower()
    try:
        return int(text, 16) if text.startswith("0x") else int(text, 10)
    except ValueError:
        return None


def evaluate(operator: str, observed: Any, expected: Any) -> tuple[bool, str]:
    if observed == MISSING:
        return False, "required evidence field is missing"
    if operator == "equals":
        passed = type(observed) is type(expected) and observed == expected
        return passed, "exact value matched" if passed else "exact value did not match"
    if operator == "at_least":
        observed_version = parse_version(observed)
        expected_version = parse_version(expected)
        if observed_version is not None and expected_version is not None:
            passed = observed_version >= expected_version
        elif isinstance(observed, (int, float)) and isinstance(expected, (int, float)) and not isinstance(observed, bool):
            passed = observed >= expected
        else:
            return False, "values are not comparable with at_least"
        return passed, "minimum value matched" if passed else "observed value is below the target"
    if operator == "bitmask_all":
        if not isinstance(observed, int) or isinstance(observed, bool) or not isinstance(expected, int):
            return False, "bitmask values are not integers"
        passed = (observed & expected) == expected
        return passed, "all required bits are set" if passed else "one or more required bits are missing"
    if operator == "hr_success":
        hr = parse_hresult(observed)
        if hr is None:
            return False, "observed value is not an HRESULT"
        passed = (hr & 0x80000000) == 0
        return passed, "HRESULT succeeded" if passed else "HRESULT failed"
    return False, f"unknown gate operator: {operator}"


def make_check(kind: str, requirement: dict[str, Any], value: Any, error: str | None) -> Check:
    check_id = str(requirement.get("id", MISSING))
    api = str(requirement.get("api", MISSING))
    result = str(requirement.get("probe", MISSING))
    path = str(requirement.get("path", MISSING))
    operator = str(requirement.get("operator", MISSING))
    expected = requirement.get("expected", MISSING)
    if error:
        return Check(kind, check_id, api, result, path, operator, expected, MISSING, False, error)
    passed, detail = evaluate(operator, value, expected)
    return Check(kind, check_id, api, result, path, operator, expected, value, passed, detail)


def validate_requirement_shape(contract: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    target = contract.get("target")
    if not isinstance(target, dict):
        errors.append("contract target must be an object")
    elif target.get("feature_level") != "12_2":
        errors.append("contract target.feature_level must be 12_2")

    for section in ("query_requirements", "behavior_requirements"):
        entries = contract.get(section)
        if not isinstance(entries, list) or not entries:
            errors.append(f"contract {section} must be a non-empty list")
            continue
        seen: set[str] = set()
        for index, entry in enumerate(entries):
            if not isinstance(entry, dict):
                errors.append(f"contract {section}[{index}] must be an object")
                continue
            check_id = entry.get("id")
            if not isinstance(check_id, str) or not check_id:
                errors.append(f"contract {section}[{index}] is missing id")
            elif check_id in seen:
                errors.append(f"contract {section} contains duplicate id: {check_id}")
            else:
                seen.add(check_id)
            for field in ("api", "probe", "path", "operator", "expected"):
                if field not in entry:
                    errors.append(f"contract {section}[{index}] is missing {field}")
    identity = contract.get("identity")
    if not isinstance(identity, dict):
        errors.append("contract identity must be an object")
    return errors


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_identity(source_root: Path) -> tuple[str, str, str | None]:
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(source_root), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        listed = subprocess.check_output(
            [
                "git",
                "-C",
                str(source_root),
                "ls-files",
                "-co",
                "--exclude-standard",
                "vendor/dxmt",
                "tools/d3d12-metal-sdk",
            ],
            text=True,
            stderr=subprocess.DEVNULL,
        ).splitlines()
    except (OSError, subprocess.CalledProcessError) as exc:
        return "unknown", "unknown", f"unable to enumerate source identity: {exc}"

    def is_source_path(relative: str) -> bool:
        return not (
            relative.startswith("vendor/dxmt/build-")
            or relative.startswith("tools/d3d12-metal-sdk/results/")
            or relative.startswith("tools/d3d12-metal-sdk/out/")
            or relative.startswith("tools/d3d12-metal-sdk/cache/")
            or "/__pycache__/" in relative
            or relative.endswith("/__pycache__")
            or relative.endswith(".pyc")
        )

    digest = hashlib.sha256()
    for relative in sorted(path for path in listed if is_source_path(path) and (source_root / path).is_file()):
        path = source_root / relative
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return commit, digest.hexdigest(), None


def identity_checks(
    contract: dict[str, Any], results_dir: Path, profile: str, source_root: Path
) -> tuple[list[dict[str, Any]], list[str]]:
    identity = contract["identity"]
    checks: list[dict[str, Any]] = []
    blockers: list[str] = []

    environment_stem = str(identity["environment_result"])
    environment, error = safe_load_result(results_dir, environment_stem, profile)
    env_pass = error is None and isinstance(environment, dict)
    checks.append(
        {
            "id": "isolated_environment_result",
            "result": environment_stem,
            "pass": env_pass,
            "detail": error or "isolated environment result loaded",
        }
    )
    if not env_pass:
        blockers.append("isolated_environment_result")
        environment = {}

    host_stem = str(identity["host_runtime_result"])
    host, error = safe_load_result(results_dir, host_stem, profile)
    host_pass = error is None and isinstance(host, dict)
    checks.append(
        {
            "id": "host_runtime_result",
            "result": host_stem,
            "pass": host_pass,
            "detail": error or "host runtime result loaded",
        }
    )
    if not host_pass:
        blockers.append("host_runtime_result")
        host = {}

    if environment.get("schema") != "metalsharp.d3d12-metal.isolated-probe-environment.v1":
        checks.append({"id": "environment_schema", "pass": False, "detail": "unexpected isolated environment schema"})
        blockers.append("environment_schema")
    else:
        checks.append({"id": "environment_schema", "pass": True, "detail": "isolated environment schema matched"})

    if host.get("schema") != "metalsharp.d3d12-metal.host-runtime.v1":
        checks.append({"id": "host_runtime_schema", "pass": False, "detail": "unexpected host runtime schema"})
        blockers.append("host_runtime_schema")
    else:
        checks.append({"id": "host_runtime_schema", "pass": True, "detail": "host runtime schema matched"})

    profile_checks = [
        ("environment_profile", environment.get("profile"), profile),
        ("host_runtime_profile", host.get("profile"), profile),
    ]
    for check_id, observed, expected in profile_checks:
        passed = observed == expected
        checks.append({"id": check_id, "observed": observed, "expected": expected, "pass": passed, "detail": "profile matched" if passed else "profile mismatch"})
        if not passed:
            blockers.append(check_id)

    wine_version = environment.get("wine_version")
    passed = wine_version == identity["wine_version"]
    checks.append({"id": "wine_version", "observed": wine_version, "expected": identity["wine_version"], "pass": passed, "detail": "Wine version matched" if passed else "Wine version mismatch"})
    if not passed:
        blockers.append("wine_version")

    for artifact_name in identity.get("required_artifacts", []):
        artifact = environment.get(artifact_name)
        passed = isinstance(artifact, dict) and artifact.get("exists") is True and isinstance(artifact.get("sha256"), str) and SHA256_RE.fullmatch(artifact["sha256"]) is not None
        checks.append({"id": f"artifact_{artifact_name}", "artifact": artifact_name, "observed": artifact, "pass": passed, "detail": "captured artifact existence and SHA-256 are valid" if passed else "artifact is missing, was not present at capture, or has no valid SHA-256"})
        if not passed:
            blockers.append(f"artifact_{artifact_name}")

    if identity.get("require_xcode_and_metal"):
        for key in ("xcode", "metal"):
            observed = environment.get(key)
            passed = isinstance(observed, str) and bool(observed.strip()) and observed.strip().lower() != "unknown"
            checks.append({"id": f"{key}_identity", "observed": observed, "pass": passed, "detail": f"{key} identity captured" if passed else f"{key} identity missing"})
            if not passed:
                blockers.append(f"{key}_identity")

    if identity.get("require_source_identity"):
        expected_commit, expected_tree, source_error = source_identity(source_root)
        observed_commit = host.get("source_commit")
        observed_tree = host.get("source_tree_sha256")
        commit_pass = source_error is None and isinstance(observed_commit, str) and observed_commit == expected_commit
        tree_pass = source_error is None and isinstance(observed_tree, str) and observed_tree == expected_tree and SHA256_RE.fullmatch(observed_tree) is not None
        checks.append({"id": "source_commit", "observed": observed_commit, "expected": expected_commit, "pass": commit_pass, "detail": source_error or ("source commit matched" if commit_pass else "source commit differs from the current tree")})
        checks.append({"id": "source_tree_sha256", "observed": observed_tree, "expected": expected_tree, "pass": tree_pass, "detail": source_error or ("source tree digest matched" if tree_pass else "source tree digest differs from the current tree")})
        if not commit_pass:
            blockers.append("source_commit")
        if not tree_pass:
            blockers.append("source_tree_sha256")

    host_path = result_path(results_dir, host_stem, profile)
    dependency_stems = sorted(
        {
            str(requirement["probe"])
            for section in ("query_requirements", "behavior_requirements")
            for requirement in contract_requirements(contract, section)
        }
    )
    stale: list[str] = []
    if host_pass and host_path.exists():
        host_mtime = host_path.stat().st_mtime_ns
        for stem in dependency_stems:
            dependency_path = result_path(results_dir, stem, profile)
            if not dependency_path.exists() or dependency_path.stat().st_mtime_ns < host_mtime:
                stale.append(stem)
    else:
        stale = dependency_stems
    freshness_pass = host_pass and isinstance(host.get("run_started_at"), int) and host.get("run_started_at", 0) > 0 and not stale
    checks.append(
        {
            "id": "results_fresh_for_host_run",
            "observed": stale,
            "expected": "all query/behavior result files newer than host-runtime",
            "pass": freshness_pass,
            "detail": "all dependency results were captured after the host-runtime identity" if freshness_pass else "dependency results are missing or stale relative to host-runtime",
        }
    )
    if not freshness_pass:
        blockers.append("results_fresh_for_host_run")

    return checks, blockers


def contract_requirements(contract: dict[str, Any], section: str) -> list[dict[str, Any]]:
    entries = contract.get(section, [])
    return entries if isinstance(entries, list) else []


def evaluate_requirements(
    contract: dict[str, Any], results_dir: Path, profile: str
) -> tuple[list[Check], list[str]]:
    checks: list[Check] = []
    blockers: list[str] = []
    for kind, section in (("query", "query_requirements"), ("behavior", "behavior_requirements")):
        for requirement in contract[section]:
            result_stem = str(requirement["probe"])
            result, error = safe_load_result(results_dir, result_stem, profile)
            value = MISSING if error else get_path(result, str(requirement["path"]))
            check = make_check(kind, requirement, value, error)
            checks.append(check)
            if not check.passed:
                blockers.append(check.check_id)
    return checks, blockers


def build_markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# D3D12 FL12_2 Gate",
        "",
        f"- Profile: `{summary['profile']}`",
        f"- Promotion ready: `{'PASS' if summary['pass'] else 'FAIL'}`",
        f"- Identity checks: `{summary['identity']['passed']}/{summary['identity']['total']}`",
        f"- Query checks: `{summary['queries']['passed']}/{summary['queries']['total']}`",
        f"- Behavior checks: `{summary['behaviors']['passed']}/{summary['behaviors']['total']}`",
        "",
        "## Blockers",
    ]
    if summary["blockers"]:
        lines.extend(f"- `{blocker['id']}`: {blocker['detail']}" for blocker in summary["blockers"])
    else:
        lines.append("- none")

    for title, key in (("Identity", "identity_checks"), ("Queries", "query_checks"), ("Behavior", "behavior_checks")):
        lines.extend(["", f"## {title}"])
        for check in summary[key]:
            status = "PASS" if check["pass"] else "FAIL"
            lines.append(f"- `{check['id']}`: `{status}` — {check.get('detail', '')}")
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    try:
        contract = load_json(args.contract)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[FAIL] cannot load FL12_2 gate contract: {exc}")
        return 2
    if not isinstance(contract, dict):
        print("[FAIL] FL12_2 gate contract must be a JSON object")
        return 2

    shape_errors = validate_requirement_shape(contract)
    if shape_errors:
        for error in shape_errors:
            print(f"[FAIL] {error}")
        return 2

    identity_results, identity_blockers = identity_checks(contract, args.results_dir, args.profile, args.source_root)
    checks, requirement_blockers = evaluate_requirements(contract, args.results_dir, args.profile)
    query_checks = [check.as_dict() for check in checks if check.kind == "query"]
    behavior_checks = [check.as_dict() for check in checks if check.kind == "behavior"]

    blocker_ids = set(identity_blockers + requirement_blockers)
    blocker_rows: list[dict[str, Any]] = []
    for row in identity_results:
        if not row.get("pass") and row.get("id") in blocker_ids:
            blocker_rows.append({"id": row["id"], "detail": row.get("detail", "identity check failed")})
    for check in checks:
        if not check.passed:
            blocker_rows.append({"id": check.check_id, "detail": f"{check.api}: {check.detail}; observed={check.observed!r}, expected={check.expected!r}"})

    identity_passed = sum(1 for row in identity_results if row.get("pass"))
    query_passed = sum(1 for check in checks if check.kind == "query" and check.passed)
    behavior_passed = sum(1 for check in checks if check.kind == "behavior" and check.passed)
    passed = not blocker_rows
    summary = {
        "schema": "metalsharp.d3d12-metal.fl12-2-gate-result.v1",
        "profile": args.profile,
        "contract": str(args.contract),
        "pass": passed,
        "promotion_ready": passed,
        "identity": {"passed": identity_passed, "total": len(identity_results)},
        "queries": {"passed": query_passed, "total": sum(1 for check in checks if check.kind == "query")},
        "behaviors": {"passed": behavior_passed, "total": sum(1 for check in checks if check.kind == "behavior")},
        "identity_checks": identity_results,
        "query_checks": query_checks,
        "behavior_checks": behavior_checks,
        "blockers": blocker_rows,
    }

    json_out = args.json_out or (args.results_dir / f"fl12-2-gate-{args.profile}.json")
    markdown_out = args.markdown_out or (args.results_dir / f"fl12-2-gate-{args.profile}.md")
    write_json(json_out, summary)
    write_text(markdown_out, build_markdown(summary))
    print(markdown_out)
    print(json_out)
    if passed:
        print("[PASS] FL12_2 promotion gate passed")
        return 0
    print(f"[FAIL] FL12_2 promotion gate has {len(blocker_rows)} blocker(s)")
    for blocker in blocker_rows:
        print(f"  - {blocker['id']}: {blocker['detail']}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
