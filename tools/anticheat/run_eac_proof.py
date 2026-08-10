#!/usr/bin/env python3
"""Run the explicit Elden Ring EAC substrate proof.

This command is deliberately opt-in.  It never launches Steam or
``eldenring.exe``; it starts only the supplied ``Start_protected_game.exe``
under the already-installed MetalSharp Wine 11.5 runtime, waits at most thirty
seconds, and then tears down the Wine process group and its wineserver.  The
launcher is expected to remain alive while it waits for the protected game.

The success result is limited to what this probe actually demonstrates:
the real downloaded EAC Linux ELF was mapped, relocated, initialized, and its
public export ``a`` returned success through the Darwin substrate.  It does
not claim that an online session or a protected game transition occurred.
Those are separate gates and are reported explicitly in the JSON evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import stat
import subprocess
import sys
import time
from pathlib import Path


MAX_TIMEOUT_SECONDS = 30
EAC_PROCESS_NAMES = {
    "wine",
    "wine64",
    "wine-preloader",
    "winedevice",
    "winedbg",
    "wineserver",
    "conhost",
    "explorer",
}
# On macOS, Wine's Windows processes often expose a truncated Mach `comm`
# value (for example `C:\\windows\\syste`) instead of `winedevice` or
# `services.exe`.  Their command line still contains the Windows drive path.
# Treat that path as the authoritative process-shape marker during teardown;
# leaving a detached `steamwebhelper.exe`, `explorer.exe`, or `winedbg.exe`
# behind defeats the bounded proof and can keep a Wine window alive.
WINE_COMMAND_MARKERS = ("C:\\", "Z:\\", "winedbg", "wineserver", "wine-preloader")
REQUIRED_LOG_MARKERS = (
    "Linux ABI substrate initialized; virtual /proc maps is active",
    "validated EAC Linux ELF",
    "EAC RELA relocation pass complete=1",
    "EAC PLT relocation pass complete=1",
    "applied EAC Linux PT_LOAD protections",
    "calling Linux EAC DT_INIT",
    "EAC_PROOF module_loaded=1",
    "protections=0x1",
    "EAC_PROOF export_a_success=1",
)
FORBIDDEN_LOG_MARKERS = (
    "unresolved Linux ABI symbol",
    "Linux relocation unresolved",
    "unsupported Linux relocation",
    "invalid EAC Linux ELF",
    "cannot map EAC Linux module",
)


def parser() -> argparse.ArgumentParser:
    return argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)


def add_arguments(args: argparse.ArgumentParser) -> None:
    home = Path.home()
    repo = Path(__file__).resolve().parents[2]
    metalsharp_home = Path(os.environ.get("METALSHARP_HOME", home / ".metalsharp")).expanduser()
    configured_game_dir = os.environ.get("METALSHARP_ELDEN_RING_GAME_DIR")
    args.add_argument(
        "--wine",
        type=Path,
        default=metalsharp_home / "runtime" / "wine" / "bin" / "wine",
        help="exact MetalSharp Wine 11.5 launcher (default: %(default)s)",
    )
    args.add_argument(
        "--prefix",
        type=Path,
        default=metalsharp_home / "prefix-steam",
        help="Steam Wine prefix (default: %(default)s)",
    )
    args.add_argument(
        "--game-dir",
        type=Path,
        default=Path(configured_game_dir).expanduser() if configured_game_dir else None,
        help="Elden Ring Game directory on the external Steam library",
    )
    args.add_argument("--launcher", type=Path, help="Start_protected_game.exe; defaults below --game-dir")
    args.add_argument(
        "--substrate",
        type=Path,
        default=repo / "app" / "native" / "metalsharp_eac_substrate.dylib",
        help="built MetalSharp substrate dylib; this command does not build it",
    )
    args.add_argument(
        "--libc",
        type=Path,
        default=repo / "app" / "native" / "metalsharp_eac_libc.so.6",
        help="generated ELF symbol image; this command does not generate it",
    )
    args.add_argument("--module-dump", type=Path, default=Path("/tmp/metalsharp-eac-module.bin"))
    args.add_argument("--log", type=Path, default=Path("/tmp/metalsharp-eac-substrate-proof.log"))
    args.add_argument("--stdout-log", type=Path, default=Path("/tmp/metalsharp-eac-proof-wine.out"))
    args.add_argument("--evidence", type=Path, default=Path("/tmp/metalsharp-eac-proof.json"))
    args.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="maximum launcher lifetime in seconds (must be <= 30)",
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fail(message: str) -> "NoReturn":
    print(f"eac-proof: error: {message}", file=sys.stderr)
    raise SystemExit(2)


def validate_path(path: Path, label: str, *, regular_file: bool = True) -> None:
    if not path.exists():
        fail(f"{label} does not exist: {path}")
    if regular_file and not path.is_file():
        fail(f"{label} is not a regular file: {path}")


def validate_inputs(options: argparse.Namespace) -> tuple[Path, Path]:
    if options.timeout <= 0 or options.timeout > MAX_TIMEOUT_SECONDS:
        fail(f"--timeout must be between 0 and {MAX_TIMEOUT_SECONDS} seconds")
    if options.game_dir is None:
        fail("--game-dir (or METALSHARP_ELDEN_RING_GAME_DIR) is required")
    options.wine = options.wine.expanduser().resolve()
    options.prefix = options.prefix.expanduser().resolve()
    options.game_dir = options.game_dir.expanduser().resolve()
    options.launcher = (options.launcher or options.game_dir / "Start_protected_game.exe").expanduser().resolve()
    options.substrate = options.substrate.expanduser().resolve()
    options.libc = options.libc.expanduser().resolve()
    options.module_dump = options.module_dump.expanduser().resolve()
    options.log = options.log.expanduser().resolve()
    options.stdout_log = options.stdout_log.expanduser().resolve()
    options.evidence = options.evidence.expanduser().resolve()

    expected_wine = options.prefix.parent / "runtime" / "wine" / "bin" / "wine"
    # The path is checked structurally, not by trusting a version string from
    # another Wine binary.  It prevents GPTK, Proton, or a second Wine build
    # from accidentally becoming the test runtime.
    if options.wine != expected_wine:
        fail(f"--wine must be the selected MetalSharp runtime binary, not {options.wine}")
    if ".metalsharp" not in options.wine.parts:
        fail(f"--wine is outside the MetalSharp home: {options.wine}")
    validate_path(options.wine, "MetalSharp Wine 11.5 binary")
    validate_path(options.prefix, "Wine prefix", regular_file=False)
    validate_path(options.game_dir, "Elden Ring game directory", regular_file=False)
    validate_path(options.launcher, "Start_protected_game.exe")
    validate_path(options.substrate, "MetalSharp EAC substrate")
    validate_path(options.libc, "MetalSharp ELF symbol image")
    if options.launcher.read_bytes()[:2] != b"MZ":
        fail(f"launcher is not a PE image: {options.launcher}")
    if options.libc.read_bytes()[:4] != b"\x7fELF":
        fail(f"ELF symbol image has no ELF header: {options.libc}")
    return expected_wine.parent / "wineserver", options.game_dir


def process_rows() -> list[tuple[int, str, str]]:
    try:
        output = subprocess.check_output(
            ["/bin/ps", "-axo", "pid=,comm=,args="], text=True, stderr=subprocess.DEVNULL
        )
    except (OSError, subprocess.SubprocessError):
        return []
    rows: list[tuple[int, str, str]] = []
    for line in output.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) != 3:
            continue
        try:
            pid = int(fields[0])
        except ValueError:
            continue
        rows.append((pid, fields[1], fields[2]))
    return rows


def wine_rows(wine_root: Path, prefix: Path) -> list[tuple[int, str, str]]:
    root_text = str(wine_root)
    prefix_text = str(prefix)
    rows = []
    for pid, name, command in process_rows():
        process_name = Path(name).name.lower()
        command_lower = command.lower()
        known_name = process_name in EAC_PROCESS_NAMES
        windows_command = any(marker.lower() in command_lower for marker in WINE_COMMAND_MARKERS)
        selected_runtime = root_text in command or prefix_text in command
        if (known_name or windows_command) and (
            selected_runtime or windows_command or process_name == "wineserver"
        ):
            rows.append((pid, name, command))
    return rows


def terminate_group(process: subprocess.Popen[bytes], wine_root: Path, prefix: Path, wineserver: Path) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass

    # wineserver -k is the runtime-supported teardown path.  Use the exact
    # runtime binary and prefix, then give descendant helper windows a short
    # grace period before a final targeted kill.
    try:
        subprocess.run(
            [str(wineserver), "-k"],
            env={**os.environ, "WINEPREFIX": str(prefix)},
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        pass
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline and wine_rows(wine_root, prefix):
        time.sleep(0.1)
    for pid, _name, _command in wine_rows(wine_root, prefix):
        if pid == os.getpid():
            continue
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def run_launcher(options: argparse.Namespace, wineserver: Path) -> tuple[int | None, bool]:
    for path in (options.log, options.module_dump, options.stdout_log):
        path.unlink(missing_ok=True)
        path.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(
        {
            "WINEPREFIX": str(options.prefix),
            "WINEDEBUG": "-all,+seh",
            "DYLD_INSERT_LIBRARIES": str(options.substrate),
            "METALSHARP_EAC_SUBSTRATE_LIBC": str(options.libc),
            "METALSHARP_EAC_SUBSTRATE_LOG": str(options.log),
            "METALSHARP_EAC_SUBSTRATE_MAPS": "/tmp/metalsharp-eac-maps",
            "METALSHARP_EAC_MODULE_DUMP": str(options.module_dump),
        }
    )
    started = time.monotonic()
    with options.stdout_log.open("wb") as output:
        process = subprocess.Popen(
            [str(options.wine), str(options.launcher)],
            cwd=options.game_dir,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        timed_out = False
        try:
            process.wait(timeout=options.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
        finally:
            terminate_group(process, options.wine.parent, options.prefix, wineserver)
    elapsed = time.monotonic() - started
    if elapsed > MAX_TIMEOUT_SECONDS + 5:
        fail(f"Wine teardown exceeded the hard cleanup bound: {elapsed:.2f}s")
    return process.returncode, timed_out


def line_set(log: str) -> dict[str, bool]:
    markers = {marker: marker in log for marker in REQUIRED_LOG_MARKERS}
    markers.update(
        {f"calling Linux EAC init_array[{index}]": f"calling Linux EAC init_array[{index}]" in log for index in range(6)}
    )
    markers.update({f"__libc_dlsym({name})": f"__libc_dlsym({name})" in log for name in ("a", "b", "c", "d")})
    return markers


def module_description(path: Path) -> dict[str, object]:
    if not path.exists():
        return {"exists": False, "path": str(path)}
    blob = path.read_bytes()
    elf64_x86_64 = len(blob) >= 20 and blob[:4] == b"\x7fELF" and blob[4] == 2 and blob[5] == 1 and blob[18:20] == b">\x00"
    return {
        "exists": True,
        "path": str(path),
        "bytes": len(blob),
        "sha256": sha256(path),
        "format": "elf64-x86_64" if elf64_x86_64 else "unknown",
        "elf64_x86_64": elf64_x86_64,
    }


def latest_eac_log(prefix: Path) -> tuple[Path | None, str]:
    root = prefix / "drive_c" / "users"
    candidates = []
    if root.is_dir():
        for path in root.rglob("*.log"):
            if path.is_file() and "easyanticheat" in str(path).lower():
                try:
                    candidates.append((path.stat().st_mtime_ns, path))
                except OSError:
                    continue
    if not candidates:
        return None, ""
    path = max(candidates, key=lambda item: item[0])[1]
    try:
        return path, path.read_text(errors="replace")
    except OSError:
        return path, ""


def wine_description(path: Path, eac_log: str) -> dict[str, object]:
    version = None
    marker = "Starting Wine module mapping, Wine version: "
    for line in eac_log.splitlines():
        if marker in line:
            version = line.split(marker, 1)[1].rstrip(".").strip()
    return {
        "path": str(path),
        "sha256": sha256(path),
        "wineVersionFromEac": version,
        "expectedVersion": "11.5",
        "versionMatches": version == "11.5",
    }


def eac_log_checks(eac_log: str) -> dict[str, bool]:
    return {
        "systemLinux64": "System name: 'linux64'." in eac_log,
        "moduleRequestSucceeded": "Response Code: 200" in eac_log,
        "wineMappingStarted": "Starting Wine module mapping, Wine version: 11.5." in eac_log,
        "mappingDidNotReportFailure": "Failed to map the anti-cheat module" not in eac_log,
    }


def build_evidence(options: argparse.Namespace, returncode: int | None, timed_out: bool) -> dict[str, object]:
    log = options.log.read_text(errors="replace") if options.log.exists() else ""
    eac_log_path, eac_log = latest_eac_log(options.prefix)
    markers = line_set(log)
    forbidden = [marker for marker in FORBIDDEN_LOG_MARKERS if marker in log]
    module = module_description(options.module_dump)
    wine = wine_description(options.wine, eac_log)
    eac_checks = eac_log_checks(eac_log)
    residual = [
        {"pid": pid, "name": name, "command": command}
        for pid, name, command in wine_rows(options.wine.parent, options.prefix)
        if pid != os.getpid()
    ]
    module_proof = (
        all(markers.values())
        and not forbidden
        and module.get("elf64_x86_64") is True
        and module.get("bytes", 0) > 0
        and wine.get("versionMatches") is True
        and all(eac_checks.values())
        and not residual
    )
    return {
        "schema": "metalsharp.eac-proof.v1",
        "ok": module_proof,
        "proofLevel": "real_eac_linux_module_relocated_initialized_exported" if module_proof else "not_proven",
        "protectedGameTransitionObserved": False,
        "onlineSessionObserved": False,
        "launcher": {
            "path": str(options.launcher),
            "sha256": sha256(options.launcher),
            "returnCode": returncode,
            "timedOutAtThirtySeconds": timed_out,
        },
        "runtime": wine,
        "eacLogChecks": eac_checks,
        "prefix": str(options.prefix),
        "gameDir": str(options.game_dir),
        "substrate": {"path": str(options.substrate), "sha256": sha256(options.substrate)},
        "symbolImage": {"path": str(options.libc), "sha256": sha256(options.libc), "format": "elf64"},
        "module": module,
        "requiredMarkers": markers,
        "forbiddenMarkers": forbidden,
        "residualWineProcesses": residual,
        "logs": {
            "substrate": str(options.log),
            "launcherOutput": str(options.stdout_log),
            "eacLauncher": str(eac_log_path) if eac_log_path is not None else None,
        },
        "interpretation": (
            "The exact MetalSharp Wine 11.5 runtime loaded the real EAC Linux ELF, "
            "completed relocation and constructors, and received export-a success. "
            "This proof intentionally does not claim a game transition or online support."
            if module_proof
            else "The required real-module proof markers were not all present."
        ),
    }


def main() -> int:
    options_parser = parser()
    add_arguments(options_parser)
    options = options_parser.parse_args()
    wineserver, _ = validate_inputs(options)
    returncode, timed_out = run_launcher(options, wineserver)
    evidence = build_evidence(options, returncode, timed_out)
    options.evidence.parent.mkdir(parents=True, exist_ok=True)
    options.evidence.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps(evidence, indent=2, sort_keys=True))
    return 0 if evidence["ok"] else 1


if __name__ == "__main__":
    main()
