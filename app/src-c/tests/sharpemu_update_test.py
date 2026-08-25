#!/usr/bin/env python3
"""Transaction, isolation, discovery, supervision, and hardening tests for SharpEmu."""

from __future__ import annotations

import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zlib


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(port: int, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
    payload = json.dumps(body).encode() if body is not None else None
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=40)
    headers = {"Content-Type": "application/json"} if payload is not None else {}
    connection.request(method, path, body=payload, headers=headers)
    response = connection.getresponse()
    raw = response.read()
    status = response.status
    connection.close()
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"invalid JSON from {path}: {raw!r}") from exc
    return status, parsed


def wait_ready(port: int) -> None:
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            status, result = request(port, "GET", "/status")
            if status == 200 and result.get("ok"):
                return
        except (OSError, TimeoutError):
            pass
        time.sleep(0.05)
    raise AssertionError("backend did not become ready")


def wait_update(port: int) -> dict:
    deadline = time.time() + 120
    while time.time() < deadline:
        _, result = request(port, "GET", "/sharp-library/sharpemu/update/progress")
        if result.get("status") in {"completed", "failed"}:
            return result
        time.sleep(0.1)
    raise AssertionError("SharpEmu update did not finish")


def png() -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    scanline = b"\x00\xff\x20\x40\xff"
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanline))
        + chunk(b"IEND", b"")
    )


def synthetic_gen5_elf() -> bytes:
    """Match upstream Gen5NativeReturnSmokeTests: xor eax,eax; ret."""
    image = bytearray(0x1003)
    image[0:4] = b"\x7fELF"
    image[4:9] = bytes([2, 1, 1, 9, 2])
    struct.pack_into("<H", image, 0x10, 3)
    struct.pack_into("<H", image, 0x12, 0x3E)
    struct.pack_into("<I", image, 0x14, 1)
    struct.pack_into("<Q", image, 0x18, 0x1000)
    struct.pack_into("<Q", image, 0x20, 0x40)
    struct.pack_into("<H", image, 0x34, 0x40)
    struct.pack_into("<H", image, 0x36, 0x38)
    struct.pack_into("<H", image, 0x38, 1)
    offset = 0x40
    struct.pack_into("<I", image, offset, 1)
    struct.pack_into("<I", image, offset + 4, 5)
    struct.pack_into("<Q", image, offset + 8, 0x1000)
    struct.pack_into("<Q", image, offset + 0x10, 0x1000)
    struct.pack_into("<Q", image, offset + 0x18, 0x1000)
    struct.pack_into("<Q", image, offset + 0x20, 3)
    struct.pack_into("<Q", image, offset + 0x28, 3)
    struct.pack_into("<Q", image, offset + 0x30, 0x1000)
    image[0x1000:] = b"\x31\xc0\xc3"
    return bytes(image)


def write_release(path: Path, archive: Path, tag: str, *, digest: str | None = None) -> None:
    value = digest or hashlib.sha256(archive.read_bytes()).hexdigest()
    version = tag.removeprefix("v")
    path.write_text(
        json.dumps(
            {
                "id": 1001,
                "tag_name": tag,
                "draft": False,
                "prerelease": False,
                "published_at": "2026-08-24T00:00:00Z",
                "metalsharp_source_commit": "d9b599a1fdf105187156b9baad1b3737c093a46a",
                "assets": [
                    {
                        "id": 2002,
                        "name": f"sharpemu-{version}-osx-x64.tar.gz",
                        "browser_download_url": (
                            f"https://github.com/sharpemu/sharpemu/releases/download/{tag}/"
                            f"sharpemu-{version}-osx-x64.tar.gz"
                        ),
                        "size": archive.stat().st_size,
                        "digest": f"sha256:{value}",
                    }
                ],
            }
        )
    )


def build_runtime(payload: Path, archive: Path) -> None:
    payload.mkdir()
    (payload / "plugins").mkdir()
    (payload / "licenses").mkdir()
    source = payload / "sleep.c"
    source.write_text("#include <unistd.h>\nint main(void){ sleep(60); return 0; }\n")
    subprocess.run(
        ["/usr/bin/clang", "-arch", "x86_64", "-mmacosx-version-min=26.0", str(source), "-o", str(payload / "SharpEmu")],
        check=True,
    )
    source.unlink()
    shutil.copyfile("/usr/bin/true", payload / "libMoltenVK.dylib")
    shutil.copyfile("/usr/bin/true", payload / "libvulkan.1.dylib")
    (payload / "libMoltenVK.dylib").chmod(0o755)
    (payload / "libvulkan.1.dylib").chmod(0o755)
    (payload / "LICENSE.txt").write_text("GPL-2.0-or-later test fixture\n")
    (payload / "licenses" / "Apache-2.0.txt").write_text("test fixture\n")
    with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT) as output:
        for item in sorted(payload.rglob("*")):
            output.add(item, arcname=f"./{item.relative_to(payload)}", recursive=False)


def build_bad_archive(payload: Path, archive: Path) -> None:
    shutil.rmtree(payload)
    payload.mkdir()
    (payload / "SharpEmu").symlink_to("../../../../etc/passwd")
    with tarfile.open(archive, "w:gz") as output:
        output.add(payload / "SharpEmu", arcname="./SharpEmu", recursive=False)


def verify_network_sandbox(temp: Path) -> None:
    source = temp / "network-probe.c"
    executable = temp / "network-probe"
    source.write_text(
        """
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int escaped = 0;
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (tcp >= 0) {
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = 0;
        if (bind(tcp, (struct sockaddr*)&a, sizeof(a)) == 0 || listen(tcp, 1) == 0) escaped = 1;
        close(tcp);
    }
    if (udp >= 0) {
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(9); a.sin_addr.s_addr = htonl(0x7f000001);
        char byte = 0;
        if (sendto(udp, &byte, 1, 0, (struct sockaddr*)&a, sizeof(a)) >= 0) escaped = 1;
        close(udp);
    }
    struct addrinfo* result = 0;
    if (getaddrinfo("metalsharp-network-probe.invalid", "443", 0, &result) == 0) {
        escaped = 1; freeaddrinfo(result);
    }
    return escaped;
}
"""
    )
    subprocess.run(["/usr/bin/clang", str(source), "-o", str(executable)], check=True)
    denied = subprocess.run(
        [
            "/usr/bin/sandbox-exec",
            "-p",
            "(version 1)(allow default)(deny network*)",
            str(executable),
        ],
        check=False,
        timeout=10,
    )
    assert denied.returncode == 0, "sandbox allowed a TCP, UDP, DNS, bind, or listen operation"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: sharpemu_update_test.py BACKEND", file=sys.stderr)
        return 2
    backend = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="metalsharp-sharpemu-test-") as temporary:
        temp = Path(temporary)
        metalsharp_home = temp / "metalsharp"
        host_home = temp / "home"
        payload = temp / "payload"
        archive = temp / "sharpemu-0.0.4-release.1-osx-x64.tar.gz"
        release = temp / "release.json"
        games = temp / "games"
        runtime_state = metalsharp_home / "emulators" / "sharpemu" / "state"
        host_home.mkdir()
        games.mkdir()
        verify_network_sandbox(temp)
        build_runtime(payload, archive)
        write_release(release, archive, "v0.0.4-release.1")

        port = free_port()
        environment = os.environ.copy()
        environment.update(
            {
                "HOME": str(host_home),
                "METALSHARP_HOME": str(metalsharp_home),
                "METALSHARP_PORT": str(port),
                "METALSHARP_SHARPEMU_RELEASE_JSON": str(release),
                "METALSHARP_SHARPEMU_DOWNLOAD_FILE": str(archive),
                "METALSHARP_SHARPEMU_HOST_ARCH": "arm64",
                "METALSHARP_SHARPEMU_HOST_MACOS": "27",
                "METALSHARP_SHARPEMU_ROSETTA": "1",
                "METALSHARP_SHARPEMU_SANDBOX": "1",
                "METALSHARP_SHARPEMU_SKIP_PROBE_FOR_TESTS": "1",
            }
        )
        process = subprocess.Popen([str(backend)], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_ready(port)
            _, providers = request(port, "GET", "/emulators")
            sharpemu = next(provider for provider in providers["providers"] if provider["id"] == "sharpemu")
            assert sharpemu == {
                "id": "sharpemu",
                "name": "SharpEmu",
                "platform": "PlayStation 5",
                "supported": True,
                "experimental": True,
            }

            _, status = request(port, "GET", "/sharp-library/sharpemu/status")
            assert status["state"] == "missing_runtime" and status["networkDefault"] == "denied"
            _, update = request(port, "GET", "/sharp-library/sharpemu/update/check")
            assert update["available"] and update["assetMutable"] and not update["upstreamNotarized"]
            _, started = request(port, "POST", "/sharp-library/sharpemu/update/install", {})
            assert started["ok"] and started["running"]
            completed = wait_update(port)
            assert completed["status"] == "completed", completed
            _, status = request(port, "GET", "/sharp-library/sharpemu/status")
            assert status["installed"] and status["currentTag"] == "v0.0.4-release.1"
            assert status["locallyAdHocSigned"] and status["state"] == "no_game_roots"
            version = metalsharp_home / "emulators" / "sharpemu" / "versions" / "v0.0.4-release.1"
            assert (version / "source-manifest.json").is_file()
            assert (version / "activation-manifest.json").is_file()
            assert not os.access(version, os.W_OK)
            subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(version / "SharpEmu")], check=True)

            version.chmod(0o700)
            (version / "LICENSE.txt").chmod(0o600)
            (version / "LICENSE.txt").write_text("damaged runtime\n")
            _, damaged = request(port, "GET", "/sharp-library/sharpemu/status")
            assert damaged["installed"] and not damaged["runtimeValid"] and damaged["state"] == "runtime_probe_failed"
            _, repairing = request(port, "POST", "/sharp-library/sharpemu/update/install", {})
            assert repairing["ok"] and repairing["running"]
            repaired = wait_update(port)
            assert repaired["status"] == "completed", repaired
            _, repaired_status = request(port, "GET", "/sharp-library/sharpemu/status")
            assert repaired_status["runtimeValid"] and repaired_status["state"] == "no_game_roots"

            write_release(release, archive, "v0.0.4-release.2")
            _, started = request(port, "POST", "/sharp-library/sharpemu/update/install", {})
            assert started["ok"] and started["running"]
            completed = wait_update(port)
            assert completed["status"] == "completed", completed
            _, status = request(port, "GET", "/sharp-library/sharpemu/status")
            assert status["currentTag"] == "v0.0.4-release.2" and status["rollbackAvailable"]
            _, rolled_back = request(port, "POST", "/sharp-library/sharpemu/update/rollback", {})
            assert rolled_back["ok"] and rolled_back["currentTag"] == "v0.0.4-release.1"

            game = games / "Synthetic Game"
            (game / "sce_sys").mkdir(parents=True)
            (game / "eboot.bin").write_bytes(synthetic_gen5_elf())
            (game / "sce_sys" / "param.json").write_text(
                json.dumps(
                    {
                        "titleId": "PPSA12345",
                        "contentVersion": "01.002.000",
                        "masterVersion": "01.000.000",
                        "localizedParameters": {
                            "defaultLanguage": "en-US",
                            "en-US": {"titleName": "Synthetic PS5 Test"},
                        },
                    }
                )
            )
            icon = game / "sce_sys" / "icon0.png"
            icon.write_bytes(png())
            _, library = request(port, "POST", "/sharp-library/sharpemu/add-root", {"path": str(games)})
            assert library["ok"] and len(library["games"]) == 1 and not library["truncated"]
            indexed = library["games"][0]
            assert indexed["title"] == "Synthetic PS5 Test" and indexed["titleId"] == "PPSA12345"
            assert indexed["hasArtwork"] and indexed["contentVersion"] == "01.002.000"

            malformed = games / "Malformed Metadata"
            (malformed / "sce_sys").mkdir(parents=True)
            (malformed / "eboot.bin").write_bytes(synthetic_gen5_elf())
            (malformed / "sce_sys" / "param.json").write_text("{not-json")
            deep = games / "deep"
            for index in range(10):
                deep /= str(index)
            deep.mkdir(parents=True)
            (deep / "eboot.bin").write_bytes(synthetic_gen5_elf())
            (games / "outside-link").symlink_to(temp, target_is_directory=True)
            _, rescanned = request(port, "POST", "/sharp-library/sharpemu/scan", {})
            assert rescanned["ok"] and len(rescanned["games"]) == 2
            fallback = next(item for item in rescanned["games"] if item["id"].startswith("ps5-"))
            assert fallback["title"] == "Malformed Metadata" and not fallback["hasArtwork"]

            symlink_root = temp / "games-link"
            symlink_root.symlink_to(games, target_is_directory=True)
            _, rejected = request(port, "POST", "/sharp-library/sharpemu/add-root", {"path": str(symlink_root)})
            assert not rejected["ok"]
            _, rejected = request(
                port, "POST", "/sharp-library/sharpemu/launch", {"id": indexed["id"], "unknown": True}
            )
            assert not rejected["ok"] and "unknown" in rejected["error"].lower()

            _, launched = request(
                port,
                "POST",
                "/sharp-library/sharpemu/launch",
                {"id": indexed["id"], "fullscreen": False, "allowNetwork": False},
            )
            assert launched["ok"] and not launched["networkEnabled"] and launched["pid"] > 0
            _, sessions = request(port, "GET", "/sharp-library/sharpemu/sessions")
            assert sessions["ok"] and sessions["sessions"][0]["networkEnabled"] is False
            _, blocked = request(port, "POST", "/sharp-library/sharpemu/remove-runtime", {"confirm": True})
            assert not blocked["ok"] and "stop" in blocked["error"].lower()
            _, stopped = request(port, "POST", "/sharp-library/sharpemu/stop", {"id": indexed["id"]})
            assert stopped["ok"]

            _, network_launch = request(
                port,
                "POST",
                "/sharp-library/sharpemu/launch",
                {"id": indexed["id"], "fullscreen": False, "allowNetwork": True},
            )
            if not network_launch.get("ok"):
                logs = list((metalsharp_home / "emulators" / "sharpemu" / "logs").glob("*.log"))
                details = [(str(item), item.read_text(errors="replace")) for item in logs]
                raise AssertionError((network_launch, details))
            assert network_launch["networkEnabled"] is True
            _, stopped = request(port, "POST", "/sharp-library/sharpemu/stop", {"id": indexed["id"]})
            assert stopped["ok"]

            replacement = game / "eboot.replacement"
            replacement.write_bytes(synthetic_gen5_elf())
            replacement.replace(game / "eboot.bin")
            _, changed = request(
                port,
                "POST",
                "/sharp-library/sharpemu/launch",
                {"id": indexed["id"], "allowNetwork": True},
            )
            assert not changed["ok"] and "changed" in changed["error"].lower()

            icon.unlink()
            icon.symlink_to("/etc/passwd")
            cover_path = f"/sharp-library/sharpemu/cover?id={indexed['id']}"
            cover_status, cover = request(port, "GET", cover_path)
            assert cover_status == 404 and not cover["ok"]

            runtime_state.mkdir(parents=True, exist_ok=True)
            marker = runtime_state / "preserved-save.bin"
            marker.write_bytes(b"preserve")
            original_release = release.read_text()
            mutated = json.loads(original_release)
            mutated["assets"][0]["digest"] = "sha256:" + "0" * 64
            release.write_text(json.dumps(mutated))
            _, quarantined = request(port, "POST", "/sharp-library/sharpemu/update/refresh", {})
            assert not quarantined["ok"] and "changed upstream" in quarantined["error"]
            release.write_text(original_release)

            _, removed = request(port, "POST", "/sharp-library/sharpemu/remove-runtime", {"confirm": True})
            assert removed["ok"] and removed["preservedData"] and marker.read_bytes() == b"preserve"
            assert game.is_dir() and (game / "eboot.bin").is_file()

            build_bad_archive(payload, archive)
            write_release(release, archive, "v0.0.5-release.1")
            _, started = request(port, "POST", "/sharp-library/sharpemu/update/install", {})
            assert started["ok"] and started["running"]
            failed = wait_update(port)
            assert failed["status"] == "failed" and "path-safety" in failed["error"]
            assert marker.read_bytes() == b"preserve"
            _, final_status = request(port, "GET", "/sharp-library/sharpemu/status")
            assert not final_status["installed"]
        finally:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            # Runtime versions are deliberately read-only.
            for directory, directories, files in os.walk(metalsharp_home, topdown=False):
                for name in files:
                    try:
                        os.chmod(Path(directory) / name, 0o600)
                    except FileNotFoundError:
                        pass
                for name in directories:
                    try:
                        os.chmod(Path(directory) / name, 0o700)
                    except FileNotFoundError:
                        pass
    print("SharpEmu transaction and hardening tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
