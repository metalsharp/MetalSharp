#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[3]
SDK_DIR = ROOT_DIR / "tools" / "d3d12-metal-sdk"
DEFAULT_CONTRACT = SDK_DIR / "contracts" / "winemetal-bridge-contract.json"

STEAM_WRAPPER_EXPORTS = [
    "WMTSetMetalShaderCachePath",
    "WMTBootstrapRegister",
    "WMTBootstrapLookUp",
    "MTLDevice_newSharedTexture",
    "MTLSharedEvent_createMachPort",
    "MTLDevice_newSharedEventWithMachPort",
    "CreateMetalViewFromHWND",
    "ReleaseMetalView",
]

DXMT_EXPORTS = STEAM_WRAPPER_EXPORTS + [
    "MTLLibrary_newFunctionWithConstants",
    "MTLLibrary_newFunctionWithDescriptor",
    "MTLDevice_newRenderPipelineState",
    "MTLDevice_newComputePipelineState",
    "MTLDevice_newMeshRenderPipelineState",
    "MTLDevice_newTileRenderPipelineState",
    "MTLDevice_newLibraryWithData",
    "MTLDevice_newLibrary",
    "MTLDevice_newLibraryWithSource",
]

UNIXLIB_EXPORTS = [
    "__wine_unix_call_funcs",
    "__wine_unix_call_wow64_funcs",
]


def sha256(path: Path) -> str | None:
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str]) -> str:
    return subprocess.check_output(command, text=True, stderr=subprocess.DEVNULL)


def load_contract(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def pe_exports(path: Path) -> set[str]:
    if not path.exists():
        return set()
    try:
        output = run(["x86_64-w64-mingw32-objdump", "-p", str(path)])
    except (FileNotFoundError, subprocess.CalledProcessError):
        try:
            output = run(["llvm-objdump", "-p", str(path)])
        except (FileNotFoundError, subprocess.CalledProcessError):
            return set()

    exports: set[str] = set()
    for line in output.splitlines():
        parts = line.strip().split()
        if len(parts) >= 4 and re.match(r"^\[\s*\d+\]$", " ".join(parts[:2])):
            exports.add(parts[-1])
        elif parts and parts[-1].startswith(("WMT", "MTL", "Metal", "CreateMetal", "ReleaseMetal", "NSString", "NS")):
            exports.add(parts[-1])
    return exports


def unix_exports(path: Path) -> set[str]:
    if not path.exists():
        return set()
    candidates = [
        ["nm", "-g", str(path)],
        ["llvm-nm", "-g", str(path)],
    ]
    output = ""
    for command in candidates:
        try:
            output = run(command)
            break
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue

    exports: set[str] = set()
    for line in output.splitlines():
        parts = line.strip().split()
        if parts:
            name = parts[-1]
            exports.add(name)
            if name.startswith("__"):
                exports.add(name[1:])
                exports.add(name[2:])
            elif name.startswith("_"):
                exports.add(name[1:])
    return exports


def read_source(relative_path: str) -> str:
    return (ROOT_DIR / relative_path).read_text(encoding="utf-8")


def exported_api_names(header_text: str) -> set[str]:
    names: set[str] = set()
    pattern = re.compile(r"WINEMETAL_API\s+(?:[\w:<>]+\s+)+(?P<name>[A-Za-z_]\w*)\s*\(")
    for match in pattern.finditer(header_text):
        names.add(match.group("name"))
    return names


def thunk_definition_names(thunk_text: str) -> set[str]:
    names: set[str] = set()
    pattern = re.compile(r"WINEMETAL_API\s+(?:[\w:<>]+\s+)+(?P<name>[A-Za-z_]\w*)\s*\(", re.MULTILINE)
    for match in pattern.finditer(thunk_text):
        names.add(match.group("name"))
    return names


def unix_call_codes(thunk_text: str) -> set[int]:
    codes: set[int] = set()
    for pattern in (r"UNIX_CALL\((\d+)", r"winemetal_unix_call_ok\((\d+)", r"WINE_UNIX_CALL\((\d+)"):
        for match in re.finditer(pattern, thunk_text):
            codes.add(int(match.group(1)))
    return codes


def unix_call_table(unix_text: str, name: str) -> list[str]:
    match = re.search(rf"{re.escape(name)}\[\]\s*=\s*\{{(?P<body>.*?)\n\}};", unix_text, re.S)
    if not match:
        return []
    entries: list[str] = []
    for line in match.group("body").splitlines():
        line = line.split("//", 1)[0].strip().rstrip(",")
        if not line:
            continue
        if line == "NULL":
            entries.append("NULL")
            continue
        if line.startswith("&"):
            entries.append(line[1:])
    return entries


def normalize_wow64_entry(name: str) -> str:
    if name.startswith("thunk32_"):
        return "thunk_" + name[len("thunk32_") :]
    return name


def compiler() -> str | None:
    for name in ("c++", "clang++", "g++"):
        path = shutil_which(name)
        if path:
            return path
    return None


def shutil_which(name: str) -> str | None:
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = Path(directory) / name
        if candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def compute_struct_sizes(structs: list[str]) -> tuple[dict[str, int], str | None]:
    cxx = compiler()
    if not cxx:
        return {}, "no C++ compiler found for ABI size probe"
    with tempfile.TemporaryDirectory(prefix="winemetal-abi-") as tmp:
        tmp_path = Path(tmp)
        source = tmp_path / "sizes.cpp"
        binary = tmp_path / "sizes"
        body = [
            "#include <cstdio>",
            '#include "vendor/dxmt/src/winemetal/winemetal_thunks.h"',
            "int main() {",
            '  std::printf("{\\n");',
        ]
        for index, struct_name in enumerate(structs):
            comma = "," if index + 1 < len(structs) else ""
            body.append(f'  std::printf("  \\"{struct_name}\\": %zu{comma}\\n", sizeof({struct_name}));')
        body.extend(['  std::printf("}\\n");', "  return 0;", "}"])
        source.write_text("\n".join(body) + "\n", encoding="utf-8")
        try:
            subprocess.check_call([cxx, "-std=c++17", "-I", str(ROOT_DIR), str(source), "-o", str(binary)])
            output = subprocess.check_output([str(binary)], text=True)
        except (subprocess.CalledProcessError, FileNotFoundError) as exc:
            return {}, f"failed to compile ABI size probe: {exc}"
    return json.loads(output), None


def inspect_sources(contract: dict) -> dict:
    source_audit = contract.get("source_audit", {})
    header_text = read_source(source_audit["pe_header"])
    thunk_text = read_source(source_audit["pe_thunks"])
    unix_text = read_source(source_audit["unix_bridge"])

    required_pe = list(contract.get("required_pe_exports", []))
    required_unix = list(contract.get("required_unix_call_entries", []))
    header_exports = exported_api_names(header_text)
    thunk_defs = thunk_definition_names(thunk_text)
    normal_table = unix_call_table(unix_text, "__wine_unix_call_funcs")
    wow64_table = unix_call_table(unix_text, "__wine_unix_call_wow64_funcs")
    normalized_wow64_table = [normalize_wow64_entry(name) for name in wow64_table]
    call_codes = unix_call_codes(thunk_text)
    max_code = max(call_codes) if call_codes else -1

    expected_sizes = contract.get("critical_unixcall_struct_sizes", {})
    actual_sizes, size_error = compute_struct_sizes(list(expected_sizes))
    size_mismatches = {
        name: {"expected": expected, "actual": actual_sizes.get(name)}
        for name, expected in expected_sizes.items()
        if actual_sizes.get(name) != expected
    }

    source_failures = {
        "missing_required_header_exports": [name for name in required_pe if name not in header_exports],
        "missing_required_pe_thunk_defs": [name for name in required_pe if name not in thunk_defs],
        "missing_required_unix_entries": [name for name in required_unix if name not in normal_table],
        "missing_required_wow64_entries": [name for name in required_unix if name not in wow64_table],
        "unix_call_codes_without_table_entry": [code for code in sorted(call_codes) if code >= len(normal_table)],
        "wow64_table_mismatch": []
        if normal_table == normalized_wow64_table
        else ["__wine_unix_call_funcs and __wine_unix_call_wow64_funcs differ after thunk32 normalization"],
        "size_probe_error": [size_error] if size_error else [],
        "critical_struct_size_mismatches": size_mismatches,
    }
    failure_count = sum(
        len(value) if isinstance(value, list) else len(value.keys()) for value in source_failures.values()
    )
    return {
        "contract": str(DEFAULT_CONTRACT),
        "abi_version": contract.get("abi_version"),
        "required_pe_exports": required_pe,
        "required_unix_call_entries": required_unix,
        "header_export_count": len(header_exports),
        "pe_thunk_definition_count": len(thunk_defs),
        "unix_call_count": len(normal_table),
        "wow64_call_count": len(wow64_table),
        "max_pe_call_code": max_code,
        "critical_unixcall_struct_sizes": actual_sizes,
        "failures": source_failures,
        "ok": failure_count == 0,
    }


def inspect(path: Path, role: str, required: list[str], binary_type: str) -> dict:
    exports = unix_exports(path) if binary_type == "unix" else pe_exports(path)
    missing = [name for name in required if name not in exports]
    exists = path.exists()
    return {
        "role": role,
        "path": str(path),
        "binary_type": binary_type,
        "exists": exists,
        "size": path.stat().st_size if exists else 0,
        "sha256": sha256(path),
        "required_exports": required,
        "missing_exports": missing,
        "export_count": len(exports),
        "ok": exists and not missing,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate Winemetal ABI/export compatibility before Steam or game launch."
    )
    parser.add_argument("--profile", default="metalsharp")
    parser.add_argument(
        "--dxmt-runtime",
        default=os.path.expanduser("~/.metalsharp/runtime/wine/lib/dxmt"),
        help="DXMT runtime root containing x86_64-windows and x86_64-unix.",
    )
    parser.add_argument(
        "--wine-runtime",
        default=os.path.expanduser("~/.metalsharp/runtime/wine"),
        help="Wine runtime root containing lib/wine.",
    )
    parser.add_argument(
        "--prefix",
        default=os.path.expanduser("~/.metalsharp/prefix-steam"),
        help="Wine prefix used for Steam/game launches.",
    )
    parser.add_argument("--game-dir", default="", help="Optional game Win64 directory containing staged DLLs.")
    parser.add_argument("--results-dir", default=str(Path(__file__).resolve().parents[1] / "results"))
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    parser.add_argument(
        "--optional-prefix",
        action="store_true",
        help="Do not fail if prefix system32/syswow64 winemetal.dll copies do not exist.",
    )
    args = parser.parse_args()
    contract = load_contract(args.contract)
    required_dxmt_exports = sorted(set(DXMT_EXPORTS) | set(contract.get("required_pe_exports", [])))
    required_steam_exports = sorted(set(STEAM_WRAPPER_EXPORTS))

    dxmt_runtime = Path(args.dxmt_runtime)
    wine_runtime = Path(args.wine_runtime)
    prefix = Path(args.prefix)
    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    entries: list[dict] = [
        inspect(dxmt_runtime / "x86_64-windows" / "winemetal.dll", "dxmt_windows", required_dxmt_exports, "pe"),
        inspect(dxmt_runtime / "x86_64-unix" / "winemetal.so", "dxmt_unixlib", UNIXLIB_EXPORTS, "unix"),
        inspect(
            wine_runtime / "lib" / "wine" / "x86_64-windows" / "winemetal.dll",
            "wine_builtin_windows",
            required_dxmt_exports,
            "pe",
        ),
        inspect(
            wine_runtime / "lib" / "wine" / "x86_64-unix" / "winemetal.so",
            "wine_builtin_unixlib",
            UNIXLIB_EXPORTS,
            "unix",
        ),
        inspect(
            prefix / "drive_c" / "windows" / "system32" / "winemetal.dll",
            "prefix_system32",
            required_dxmt_exports,
            "pe",
        ),
        inspect(
            prefix / "drive_c" / "windows" / "syswow64" / "winemetal.dll",
            "prefix_syswow64",
            required_steam_exports,
            "pe",
        ),
    ]

    if args.optional_prefix:
        for entry in entries:
            if entry["role"] in {"prefix_system32", "prefix_syswow64"} and not entry["exists"]:
                entry["ok"] = True
                entry["optional_absent"] = True

    if args.game_dir:
        entries.append(inspect(Path(args.game_dir) / "winemetal.dll", "game_local_winemetal", required_dxmt_exports, "pe"))

    active_windows_roles = {"dxmt_windows", "prefix_system32", "game_local_winemetal"}
    active_windows_ok = any(entry["role"] in active_windows_roles and entry["ok"] for entry in entries)
    if active_windows_ok:
        for entry in entries:
            if entry["role"] == "wine_builtin_windows" and not entry["ok"]:
                entry["ok"] = True
                entry["advisory_only"] = True
                entry["advisory_reason"] = (
                    "Active DXMT/prefix winemetal.dll export surface passed; "
                    "global Wine builtin copy is not the launch-authoritative VKD3D DLL."
                )

    source_audit = inspect_sources(contract)
    failures = [entry for entry in entries if not entry["ok"]]
    result = {
        "schema": "metalsharp.d3d12-metal.winemetal-abi.v1",
        "profile": args.profile,
        "ok": not failures and source_audit["ok"],
        "failure_count": len(failures) + (0 if source_audit["ok"] else 1),
        "required_groups": {
            "steam_wrapper_exports": required_steam_exports,
            "dxmt_exports": required_dxmt_exports,
            "unixlib_exports": UNIXLIB_EXPORTS,
        },
        "source_audit": source_audit,
        "entries": entries,
        "notes": [
            "Steam/global Wine x86_64 copies must preserve Steam wrapper exports plus D3D12 shader/PSO bridge exports.",
            "The 32-bit syswow64 copy is still checked for legacy wrapper exports only.",
            "DXMT/game-local copies must preserve Steam wrapper exports plus D3D12 shader/PSO bridge exports.",
            "A red result here means do not launch Steam or a game; repair DLL staging first.",
        ],
    }
    out_path = results_dir / f"winemetal-abi-{args.profile}.json"
    out_path.write_text(json.dumps(result, indent=2) + "\n")
    print(out_path)

    if failures or not source_audit["ok"]:
        for failure in failures:
            print(
                f"winemetal ABI failed: {failure['role']} {failure['path']} "
                f"missing={failure['missing_exports']}",
                file=sys.stderr,
            )
        if not source_audit["ok"]:
            print(f"winemetal source ABI audit failed: {source_audit['failures']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
