#!/usr/bin/env python3
"""Security and transaction tests for the managed shadPS4 runtime."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import warnings
import zipfile
from pathlib import Path

BACKEND = Path(sys.argv[1]).resolve()
OFFICIAL_URL = (
    "https://github.com/shadps4-emu/shadPS4/releases/download/"
    "v.0.18.0/shadps4-macos-sdl-0.18.0.zip"
)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(port: int, method: str, path: str, body: dict | None = None) -> dict:
    payload = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=payload,
        method=method,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def wait_progress(port: int, expected: str = "failed", timeout: float = 20) -> dict:
    deadline = time.time() + timeout
    value: dict = {}
    while time.time() < deadline:
        value = request(port, "GET", "/sharp-library/shadps4/update/progress")
        if value.get("status") == expected:
            return value
        if value.get("status") in {"completed", "failed"} and value.get("status") != expected:
            return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {expected}: {value}")


def compile_runtime(directory: Path, *, arch: str = "x86_64", valid_help: bool = True) -> None:
    help_text = "--fullscreen --add-game-folder --config-global" if valid_help else "incompatible help"
    source = directory / "core.c"
    source.write_text(
        "#include <signal.h>\n#include <stdio.h>\n#include <string.h>\n#include <unistd.h>\n"
        "int main(int argc,char**argv){"
        "for(int i=1;i<argc;i++)if(!strcmp(argv[i],\"--help\")){puts(\"%s\");return 0;}"
        "signal(SIGTERM,SIG_DFL);sleep(30);return 0;}\n" % help_text
    )
    subprocess.run(
        ["cc", "-arch", arch, "-mmacosx-version-min=13.0", str(source), "-o", str(directory / "shadps4")],
        check=True,
    )
    dylib_source = directory / "driver.c"
    dylib_source.write_text("int metalsharp_shadps4_fixture(void){return 1;}\n")
    for name in ("libvulkan.dylib", "libvulkan_kosmickrisp.dylib"):
        subprocess.run(
            [
                "cc",
                "-arch",
                arch,
                "-dynamiclib",
                "-mmacosx-version-min=13.0",
                "-install_name",
                f"@rpath/{name}",
                str(dylib_source),
                "-o",
                str(directory / name),
            ],
            check=True,
        )
    (directory / "kosmickrisp_mesa_icd.json").write_text(
        json.dumps(
            {
                "ICD": {"api_version": "1.3.353", "library_path": "./libvulkan_kosmickrisp.dylib"},
                "file_format_version": "1.0.1",
            }
        )
    )


def zip_runtime(source: Path, output: Path, *, names: tuple[str, ...] | None = None) -> None:
    selected = names or (
        "shadps4",
        "libvulkan.dylib",
        "libvulkan_kosmickrisp.dylib",
        "kosmickrisp_mesa_icd.json",
    )
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in selected:
            info = zipfile.ZipInfo(name)
            mode = 0o100755 if name == "shadps4" else 0o100644
            info.external_attr = mode << 16
            archive.writestr(info, (source / name).read_bytes())


def release_json(archive: Path, output: Path, *, size_delta: int = 0, digest: str | None = None) -> None:
    sha = digest or hashlib.sha256(archive.read_bytes()).hexdigest()
    output.write_text(
        json.dumps(
            {
                "tag_name": "v.0.18.0",
                "published_at": "2026-08-18T08:23:46Z",
                "assets": [
                    {
                        "name": "shadps4-macos-sdl-0.18.0.zip",
                        "browser_download_url": OFFICIAL_URL,
                        "size": archive.stat().st_size + size_delta,
                        "digest": f"sha256:{sha}",
                    }
                ],
            }
        )
    )


class Backend:
    def __init__(self, root: Path, archive: Path, release: Path, **extra: str):
        self.home = root / "metalsharp"
        self.user_home = root / "home"
        self.home.mkdir()
        self.user_home.mkdir()
        self.port = free_port()
        env = os.environ.copy()
        env.update(
            {
                "HOME": str(self.user_home),
                "METALSHARP_HOME": str(self.home),
                "METALSHARP_PORT": str(self.port),
                "METALSHARP_SHADPS4_RELEASE_JSON": str(release),
                "METALSHARP_SHADPS4_DOWNLOAD_FILE": str(archive),
                "METALSHARP_SHADPS4_LICENSE_FILE": str(root / "LICENSE"),
                "METALSHARP_SHADPS4_HOST_ARCH": "arm64",
                "METALSHARP_SHADPS4_HOST_MACOS": "27",
                "METALSHARP_SHADPS4_ROSETTA": "1",
            }
        )
        env.update(extra)
        (root / "LICENSE").write_text("GPL-2.0-or-later test fixture\n")
        self.log = open(root / "backend.log", "wb")
        self.process = subprocess.Popen([str(BACKEND)], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                if request(self.port, "GET", "/status").get("ok"):
                    return
            except (OSError, urllib.error.URLError):
                time.sleep(0.05)
        raise AssertionError("backend did not become ready")

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()


def install(port: int) -> dict:
    started = request(port, "POST", "/sharp-library/shadps4/update/install")
    assert started.get("ok") and started.get("running"), started
    return wait_progress(port, "completed")


def make_sfo(path: Path) -> None:
    values = [("TITLE_ID", "CUSA99999"), ("TITLE", "Transaction Test"), ("APP_VER", "01.00")]
    keys = b""
    entries = []
    data = b""
    for key, value in values:
        key_offset = len(keys)
        keys += key.encode() + b"\0"
        raw = value.encode() + b"\0"
        entries.append((key_offset, 0x0204, len(raw), len(raw), len(data)))
        data += raw
    key_start = 20 + 16 * len(entries)
    data_start = key_start + len(keys)
    blob = struct.pack("<4sIIII", b"\0PSF", 0x101, key_start, data_start, len(entries))
    blob += b"".join(struct.pack("<HHIII", *entry) for entry in entries) + keys + data
    path.write_bytes(blob)


def failure_case(base: Path, name: str, archive: Path, release: Path, expected: str, **env: str) -> None:
    root = base / name
    root.mkdir()
    backend = Backend(root, archive, release, **env)
    try:
        started = request(backend.port, "POST", "/sharp-library/shadps4/update/install")
        if started.get("running"):
            progress = wait_progress(backend.port, "failed")
            assert expected.lower() in str(progress.get("error", "")).lower(), progress
        else:
            assert expected.lower() in str(started.get("error", "")).lower(), started
        environment = backend.home / "emulators" / "shadps4"
        assert not (environment / "current").exists()
        assert not list((environment / "downloads").glob("*.part"))
    finally:
        backend.close()


def main() -> None:
    if sys.platform != "darwin":
        print("shadPS4 update tests skipped: macOS required")
        return
    with tempfile.TemporaryDirectory(prefix="metalsharp-shadps4-update-") as temp:
        base = Path(temp)
        runtime = base / "runtime"
        runtime.mkdir()
        compile_runtime(runtime)
        valid_zip = base / "valid.zip"
        valid_release = base / "valid.json"
        zip_runtime(runtime, valid_zip)
        release_json(valid_zip, valid_release)

        # Complete install, active-session update handoff, rollback, and preservation.
        valid_root = base / "valid-case"
        valid_root.mkdir()
        backend = Backend(valid_root, valid_zip, valid_release)
        try:
            progress = install(backend.port)
            assert progress["status"] == "completed", progress
            environment = backend.home / "emulators" / "shadps4"
            current = environment / "current"
            assert current.is_symlink()
            assert (current / "source.json").is_file()
            assert (current / "LICENSE").is_file()
            capabilities = json.loads((current / "capabilities.json").read_text())
            assert capabilities["runtimeTag"] == "v.0.18.0"
            assert {"--game", "--fullscreen", "--config-global"} <= set(capabilities["cli"])
            assert "libSceFont.sprx" in capabilities["supportedModules"]
            assert capabilities["packageExtraction"] is False and capabilities["zarDiscovery"] is False
            subprocess.run(["codesign", "--verify", "--strict", str(current / "shadps4")], check=True)

            game = valid_root / "games" / "CUSA99999"
            (game / "sce_sys").mkdir(parents=True)
            (game / "eboot.bin").write_bytes(b"owned-fixture")
            make_sfo(game / "sce_sys" / "param.sfo")
            games = request(backend.port, "POST", "/sharp-library/shadps4/add-root", {"path": str(valid_root / "games")})
            game_id = games["games"][0]["id"]

            old = environment / "versions" / "v-old"
            shutil.copytree(environment / "versions" / "v.0.18.0", old)
            current.unlink()
            current.symlink_to("versions/v-old")
            launched = request(backend.port, "POST", "/sharp-library/shadps4/launch", {"id": game_id})
            assert launched.get("ok"), launched
            started = request(backend.port, "POST", "/sharp-library/shadps4/update/install")
            assert started.get("running"), started
            deadline = time.time() + 20
            while time.time() < deadline:
                state = request(backend.port, "GET", "/sharp-library/shadps4/update/progress")
                if state["status"] == "waiting_for_exit":
                    break
                if state["status"] == "failed":
                    raise AssertionError(state)
                time.sleep(0.05)
            else:
                raise AssertionError("update did not wait for active shadPS4 session")
            stopped = request(backend.port, "POST", "/sharp-library/shadps4/stop", {"id": game_id})
            assert stopped.get("ok"), stopped
            assert wait_progress(backend.port, "completed", 20)["status"] == "completed"
            rolled = request(backend.port, "POST", "/sharp-library/shadps4/update/rollback", {})
            assert rolled.get("currentTag") == "v-old", rolled
            rolled = request(backend.port, "POST", "/sharp-library/shadps4/update/rollback", {})
            assert rolled.get("currentTag") == "v.0.18.0", rolled

            marker = environment / "home" / "Library" / "Application Support" / "shadPS4" / "home" / "savedata"
            marker.mkdir(parents=True)
            (marker / "preserve").write_text("keep")
            removed = request(backend.port, "POST", "/sharp-library/shadps4/remove-runtime", {"confirm": True})
            assert removed.get("ok") and removed.get("preservedData"), removed
            assert (marker / "preserve").read_text() == "keep"
            assert game.is_dir()
        finally:
            backend.close()

        # A stale same-tag pointer with a missing executable is repaired rather than treated as current.
        repair_root = base / "repair-same-tag"
        repair_root.mkdir()
        backend = Backend(repair_root, valid_zip, valid_release)
        try:
            environment = backend.home / "emulators" / "shadps4"
            (environment / "versions" / "v.0.18.0").mkdir(parents=True)
            (environment / "current").symlink_to("versions/v.0.18.0")
            repaired = install(backend.port)
            assert repaired["status"] == "completed", repaired
            assert (environment / "current" / "shadps4").is_file()
        finally:
            backend.close()

        # Metadata integrity failures.
        bad_digest_json = base / "bad-digest.json"
        release_json(valid_zip, bad_digest_json, digest="0" * 64)
        failure_case(base, "bad-digest", valid_zip, bad_digest_json, "digest")
        bad_size_json = base / "bad-size.json"
        release_json(valid_zip, bad_size_json, size_delta=1)
        failure_case(base, "bad-size", valid_zip, bad_size_json, "size")

        # Archive traversal and duplicate normalized path rejection.
        traversal_zip = base / "traversal.zip"
        with zipfile.ZipFile(traversal_zip, "w") as archive:
            archive.writestr("../escape", b"bad")
        traversal_json = base / "traversal.json"
        release_json(traversal_zip, traversal_json)
        failure_case(base, "traversal", traversal_zip, traversal_json, "path-safety")

        duplicate_zip = base / "duplicate.zip"
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with zipfile.ZipFile(duplicate_zip, "w") as archive:
                archive.writestr("shadps4", b"one")
                archive.writestr("shadps4", b"two")
        duplicate_json = base / "duplicate.json"
        release_json(duplicate_zip, duplicate_json)
        failure_case(base, "duplicate", duplicate_zip, duplicate_json, "path-safety")

        symlink_zip = base / "symlink.zip"
        with zipfile.ZipFile(symlink_zip, "w") as archive:
            info = zipfile.ZipInfo("shadps4")
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, "/tmp/escape")
        symlink_json = base / "symlink.json"
        release_json(symlink_zip, symlink_json)
        failure_case(base, "symlink", symlink_zip, symlink_json, "extraction")

        # Runtime structure, architecture, ICD, signature, CLI, and activation failures.
        missing_zip = base / "missing.zip"
        zip_runtime(runtime, missing_zip, names=("shadps4",))
        missing_json = base / "missing.json"
        release_json(missing_zip, missing_json)
        failure_case(base, "missing", missing_zip, missing_json, "missing")

        arm_runtime = base / "arm-runtime"
        arm_runtime.mkdir()
        compile_runtime(arm_runtime, arch="arm64")
        arm_zip = base / "arm.zip"
        arm_json = base / "arm.json"
        zip_runtime(arm_runtime, arm_zip)
        release_json(arm_zip, arm_json)
        failure_case(base, "wrong-arch", arm_zip, arm_json, "architecture")

        bad_icd_runtime = base / "bad-icd-runtime"
        shutil.copytree(runtime, bad_icd_runtime)
        (bad_icd_runtime / "kosmickrisp_mesa_icd.json").write_text('{"ICD":{"library_path":"/tmp/escape"}}')
        bad_icd_zip = base / "bad-icd.zip"
        bad_icd_json = base / "bad-icd.json"
        zip_runtime(bad_icd_runtime, bad_icd_zip)
        release_json(bad_icd_zip, bad_icd_json)
        failure_case(base, "bad-icd", bad_icd_zip, bad_icd_json, "vulkan")

        bad_help_runtime = base / "bad-help-runtime"
        bad_help_runtime.mkdir()
        compile_runtime(bad_help_runtime, valid_help=False)
        bad_help_zip = base / "bad-help.zip"
        bad_help_json = base / "bad-help.json"
        zip_runtime(bad_help_runtime, bad_help_zip)
        release_json(bad_help_zip, bad_help_json)
        failure_case(base, "bad-help", bad_help_zip, bad_help_json, "capability")

        failure_case(
            base,
            "signing",
            valid_zip,
            valid_release,
            "sign",
            METALSHARP_SHADPS4_CODESIGN_BIN="/usr/bin/false",
        )
        failure_case(
            base,
            "activation",
            valid_zip,
            valid_release,
            "activation",
            METALSHARP_SHADPS4_FAIL_ACTIVATION="1",
        )

        unsupported_root = base / "unsupported"
        unsupported_root.mkdir()
        backend = Backend(
            unsupported_root,
            valid_zip,
            valid_release,
            METALSHARP_SHADPS4_HOST_ARCH="x86_64",
        )
        try:
            status_value = request(backend.port, "GET", "/sharp-library/shadps4/status")
            assert not status_value["supported"] and status_value["unsupportedReason"] == "intel_mac"
            refused = request(backend.port, "POST", "/sharp-library/shadps4/update/install")
            assert not refused["ok"] and "Apple Silicon" in refused["error"]
        finally:
            backend.close()

        old_macos_root = base / "old-macos"
        old_macos_root.mkdir()
        backend = Backend(
            old_macos_root,
            valid_zip,
            valid_release,
            METALSHARP_SHADPS4_HOST_MACOS="25",
        )
        try:
            status_value = request(backend.port, "GET", "/sharp-library/shadps4/status")
            assert not status_value["supported"] and status_value["unsupportedReason"] == "macos_too_old"
            refused = request(backend.port, "POST", "/sharp-library/shadps4/update/install")
            assert not refused["ok"] and "macOS 26" in refused["error"]
        finally:
            backend.close()

        no_rosetta_root = base / "no-rosetta"
        no_rosetta_root.mkdir()
        backend = Backend(
            no_rosetta_root,
            valid_zip,
            valid_release,
            METALSHARP_SHADPS4_ROSETTA="0",
        )
        try:
            status_value = request(backend.port, "GET", "/sharp-library/shadps4/status")
            assert not status_value["supported"] and status_value["unsupportedReason"] == "rosetta_missing"
            refused = request(backend.port, "POST", "/sharp-library/shadps4/update/install")
            assert not refused["ok"] and "Rosetta" in refused["error"]
        finally:
            backend.close()

    print("shadPS4 update transaction tests passed")


if __name__ == "__main__":
    main()
