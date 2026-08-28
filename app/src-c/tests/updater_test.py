#!/usr/bin/env python3
"""Release selection and download tests for baseline and FEX app updates."""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BACKEND = Path(sys.argv[1]).resolve()
VERSION = "0.61.0"


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


def wait_for_download(port: int, timeout: float = 10) -> dict:
    deadline = time.time() + timeout
    progress: dict = {}
    while time.time() < deadline:
        progress = request(port, "GET", "/update/progress")
        if progress.get("status") in {"downloaded", "error"}:
            return progress
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for update download: {progress}")


class Backend:
    def __init__(self, root: Path, release: Path, macos: int):
        home = root / "metalsharp"
        user_home = root / "home"
        home.mkdir()
        user_home.mkdir()
        self.port = free_port()
        env = os.environ.copy()
        env.update(
            {
                "HOME": str(user_home),
                "METALSHARP_HOME": str(home),
                "METALSHARP_PORT": str(self.port),
                "METALSHARP_UPDATE_RELEASE_JSON": str(release),
                "METALSHARP_UPDATE_HOST_MACOS": str(macos),
            }
        )
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

    def __enter__(self) -> "Backend":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def write_release(root: Path) -> tuple[Path, bytes, bytes]:
    regular_fallback = root / "regular-universal.dmg"
    regular = root / "regular-arm64.dmg"
    fex = root / "fex-arm64.dmg"
    regular_fallback.write_bytes(b"universal baseline fixture")
    regular_bytes = b"preferred arm64 baseline fixture"
    fex_bytes = b"experimental fex fixture"
    regular.write_bytes(regular_bytes)
    fex.write_bytes(fex_bytes)
    release = root / "release.json"
    release.write_text(
        json.dumps(
            {
                "tag_name": f"v{VERSION}",
                "name": f"MetalSharp v{VERSION}",
                "body": "Synthetic updater release",
                "assets": [
                    {
                        "name": f"MetalSharp-{VERSION}.dmg",
                        "browser_download_url": regular_fallback.as_uri(),
                        "size": regular_fallback.stat().st_size,
                    },
                    {
                        "name": f"MetalSharp-{VERSION}-FEX-arm64.dmg",
                        "browser_download_url": fex.as_uri(),
                        "size": fex.stat().st_size,
                    },
                    {
                        "name": f"MetalSharp-{VERSION}-arm64.dmg",
                        "browser_download_url": regular.as_uri(),
                        "size": regular.stat().st_size,
                    },
                ],
            }
        )
    )
    return release, regular_bytes, fex_bytes


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="metalsharp-updater-") as temporary:
        root = Path(temporary)
        release, regular_bytes, fex_bytes = write_release(root)
        supported_root = root / "supported"
        supported_root.mkdir()
        with Backend(supported_root, release, 27) as backend:
            status = request(backend.port, "GET", "/update/check")
            assert status["ok"] is True and status["available"] is True, status
            assert status["download_url"].endswith("regular-arm64.dmg"), status
            assert status["download_size"] == len(regular_bytes), status
            assert status["fex_available"] is True, status
            assert status["fex_download_url"].endswith("fex-arm64.dmg"), status
            assert status["fex_download_size"] == len(fex_bytes), status
            assert status["fex_supported"] is True and status["macos_major"] == 27, status

            started = request(backend.port, "POST", "/update/start", {"variant": "fex"})
            assert started["ok"] is True, started
            progress = wait_for_download(backend.port)
            assert progress["status"] == "downloaded", progress
            downloaded = request(backend.port, "GET", "/update/dmg-path?variant=fex")
            assert downloaded["ok"] is True and downloaded["version"] == VERSION, downloaded
            assert Path(downloaded["path"]).read_bytes() == fex_bytes
            assert request(backend.port, "GET", "/update/dmg-path")["ok"] is False

            started = request(backend.port, "POST", "/update/start", {"variant": "regular"})
            assert started["ok"] is True, started
            progress = wait_for_download(backend.port)
            assert progress["status"] == "downloaded", progress
            downloaded = request(backend.port, "GET", "/update/dmg-path")
            assert downloaded["ok"] is True and downloaded["version"] == VERSION, downloaded
            assert Path(downloaded["path"]).read_bytes() == regular_bytes

        unsupported_root = root / "unsupported"
        unsupported_root.mkdir()
        with Backend(unsupported_root, release, 26) as backend:
            status = request(backend.port, "GET", "/update/check")
            assert status["fex_available"] is True and status["fex_supported"] is False, status
            blocked = request(backend.port, "POST", "/update/start", {"variant": "fex"})
            assert blocked["ok"] is False and "macOS 27" in blocked["error"], blocked
            invalid = request(backend.port, "POST", "/update/start", {"variant": "preview"})
            assert invalid["ok"] is False and "variant" in invalid["error"], invalid

    print("baseline and FEX updater tests passed")


if __name__ == "__main__":
    main()
