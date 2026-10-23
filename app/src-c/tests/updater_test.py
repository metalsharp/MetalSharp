#!/usr/bin/env python3
"""Release selection and download tests for baseline and FEX app updates."""

from __future__ import annotations

import json
import http.client
import http.server
import threading
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BACKEND = Path(sys.argv[1]).resolve()
# The FEX build of the backend (make VARIANT=fex); built next to BACKEND when not given.
FEX_BACKEND = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
SRC_C = Path(__file__).resolve().parent.parent
# MetalSharp: keep the synthetic release ahead of the app version; the
# set-version script bumps this alongside the app version.
VERSION = "0.80.1"  # Synthetic release must be newer than the current app.


class UnixHTTPConnection(http.client.HTTPConnection):
    """HTTP over the backend's owner-only Unix socket (<home>/run/backend.sock)."""

    def __init__(self, socket_path: Path, timeout: float):
        super().__init__("localhost", timeout=timeout)
        self.socket_path = str(socket_path)

    def connect(self) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.socket_path)


def request(sock: Path, method: str, path: str, body: dict | None = None) -> dict:
    payload = None if body is None else json.dumps(body).encode()
    connection = UnixHTTPConnection(sock, timeout=30)
    try:
        connection.request(method, path, body=payload, headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        if response.status >= 400:
            raise OSError(f"HTTP {response.status} for {method} {path}")
        return json.load(response)
    finally:
        connection.close()


def wait_for_download(sock: Path, timeout: float = 10) -> dict:
    deadline = time.time() + timeout
    progress: dict = {}
    while time.time() < deadline:
        progress = request(sock, "GET", "/update/progress")
        if progress.get("status") in {"downloaded", "error"}:
            return progress
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for update download: {progress}")


def build_fex_backend(version: str) -> Path:
    """The fex backend, in its own BUILD_DIR so its objects never mix with the regular build's."""
    build_dir = BACKEND.parent / "updater-test-fex"
    target = build_dir / "metalsharp-backend"
    subprocess.run(
        [
            "make",
            "-s",
            "-C",
            str(SRC_C),
            "VARIANT=fex",
            f"VERSION={version}",
            f"BUILD_DIR={build_dir}",
            f"TARGET={target}",
            str(target),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return target


class Backend:
    def __init__(self, root: Path, release: Path, macos: str, backend: Path | None = None):
        home = root / "metalsharp"
        user_home = root / "home"
        home.mkdir()
        user_home.mkdir()
        self.sock = home / "run" / "backend.sock"
        env = os.environ.copy()
        env.update(
            {
                "HOME": str(user_home),
                "METALSHARP_HOME": str(home),
                "METALSHARP_UPDATE_RELEASE_JSON": str(release),
                "METALSHARP_UPDATE_HOST_MACOS": str(macos),
            }
        )
        self.log = open(root / "backend.log", "wb")
        self.process = subprocess.Popen([str(backend or BACKEND)], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                if request(self.sock, "GET", "/status").get("ok"):
                    return
            except OSError:
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


def write_release(root: Path, version: str = VERSION, fex_asset: bool = True) -> tuple[Path, bytes, bytes]:
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
                "tag_name": f"v{version}",
                "name": f"MetalSharp v{version}",
                "body": "Synthetic updater release",
                "assets": [
                    {
                        "name": f"MetalSharp-{version}.dmg",
                        "browser_download_url": regular_fallback.as_uri(),
                        "size": regular_fallback.stat().st_size,
                    },
                    *(
                        [
                            {
                                "name": f"MetalSharp-{version}-arm64-FEX.dmg",
                                "browser_download_url": fex.as_uri(),
                                "size": fex.stat().st_size,
                            }
                        ]
                        if fex_asset
                        else []
                    ),
                    {
                        "name": f"MetalSharp-{version}-arm64.dmg",
                        "browser_download_url": regular.as_uri(),
                        "size": regular.stat().st_size,
                    },
                ],
            }
        )
    )
    return release, regular_bytes, fex_bytes


def test_download_progress(root: Path) -> None:
    payload = b"x" * (1024 * 1024)

    class SlowDownload(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            for offset in range(0, len(payload), 65536):
                self.wfile.write(payload[offset:offset + 65536])
                self.wfile.flush()
                time.sleep(0.25)

        def log_message(self, *_: object) -> None:
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SlowDownload)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        release, _, _ = write_release(root)
        data = json.loads(release.read_text())
        for asset in data["assets"]:
            asset["browser_download_url"] = f"http://127.0.0.1:{server.server_port}/download.dmg"
            asset["size"] = len(payload)
        release.write_text(json.dumps(data))
        with Backend(root, release, "27.0") as backend:
            assert request(backend.sock, "POST", "/update/start", {"variant": "regular"})["ok"]
            observed = set()
            deadline = time.time() + 20
            while time.time() < deadline:
                progress = request(backend.sock, "GET", "/update/progress")
                if progress["status"] == "downloading":
                    observed.add(progress["percent"])
                    assert progress["percent"] < 80
                if progress["status"] == "downloaded":
                    break
                assert progress["status"] != "error", progress
                time.sleep(0.1)
            else:
                raise AssertionError("slow download did not complete")
            assert len([p for p in observed if 10 < p < 80]) >= 2, observed
            result = request(backend.sock, "GET", "/update/dmg-path")
            assert Path(result["path"]).read_bytes() == payload
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


def release_variant(root: Path, name: str, version: str, fex_asset: bool = True) -> tuple[Path, bytes, bytes]:
    release_root = root / name
    release_root.mkdir()
    return write_release(release_root, version, fex_asset)


def test_regular_build_checks(root: Path, current: str) -> None:
    """The regular build keeps its old answers and offers the FEX build as a switch."""
    release, regular_bytes, fex_bytes = release_variant(root, "regular-newer-release", VERSION)
    backend_root = root / "regular-newer"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0") as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["variant"] == "regular" and status["available"] is True, status
        assert status["download_variant"] == "regular", status
        assert status["download_url"].endswith("regular-arm64.dmg"), status
        assert status["regular_download_url"] == status["download_url"], status
        assert status["regular_download_size"] == len(regular_bytes), status
        assert status["switch_variant"] == "fex" and status["switch_available"] is True, status
        assert status["switch_download_url"].endswith("fex-arm64.dmg"), status
        assert status["switch_download_size"] == len(fex_bytes), status
        # no body: the build this backend is
        started = request(backend.sock, "POST", "/update/start")
        assert started["ok"] is True and started["variant"] == "regular", started
        assert wait_for_download(backend.sock)["status"] == "downloaded"
        assert Path(request(backend.sock, "GET", "/update/dmg-path")["path"]).read_bytes() == regular_bytes
        # "build" names the build too
        started = request(backend.sock, "POST", "/update/start", {"build": "fex"})
        assert started["ok"] is True and started["variant"] == "fex", started
        assert wait_for_download(backend.sock)["status"] == "downloaded"
        downloaded = request(backend.sock, "GET", "/update/dmg-path?variant=fex")
        assert Path(downloaded["path"]).read_bytes() == fex_bytes, downloaded
        invalid = request(backend.sock, "POST", "/update/start", {"build": "preview"})
        assert invalid["ok"] is False and "variant" in invalid["error"], invalid

    # the same version: no update, but switching to the FEX build is offered
    release, _, _ = release_variant(root, "regular-same-release", current)
    backend_root = root / "regular-same"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0") as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["available"] is False and status["switch_available"] is True, status

    # an older release offers neither
    release, _, _ = release_variant(root, "regular-older-release", "0.1.0")
    backend_root = root / "regular-older"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0") as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["available"] is False and status["switch_available"] is False, status

    # a newer release without a FEX asset: the regular build's answer is unchanged
    release, _, _ = release_variant(root, "regular-nofex-release", VERSION, fex_asset=False)
    backend_root = root / "regular-nofex"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0") as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["available"] is True and status["fex_available"] is False, status
        assert status["switch_available"] is False, status


def test_fex_build_checks(root: Path, fex_backend: Path, current: str) -> None:
    """The FEX build stays on FEX: its update and default download are the FEX asset."""
    release, regular_bytes, fex_bytes = release_variant(root, "fex-newer-release", VERSION)
    backend_root = root / "fex-newer"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0", fex_backend) as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["variant"] == "fex" and status["current_version"] == current, status
        assert status["available"] is True and status["download_variant"] == "fex", status
        assert status["download_url"].endswith("fex-arm64.dmg"), status
        assert status["download_size"] == len(fex_bytes), status
        assert status["fex_download_url"] == status["download_url"], status
        assert status["regular_download_url"].endswith("regular-arm64.dmg"), status
        assert status["switch_variant"] == "regular" and status["switch_available"] is True, status
        assert status["switch_download_url"] == status["regular_download_url"], status
        # no body and an empty body: the FEX build
        for body in (None, {}):
            started = request(backend.sock, "POST", "/update/start", body)
            assert started["ok"] is True and started["variant"] == "fex", started
            assert wait_for_download(backend.sock)["status"] == "downloaded"
            downloaded = request(backend.sock, "GET", "/update/dmg-path?variant=fex")
            assert downloaded["version"] == VERSION, downloaded
            assert Path(downloaded["path"]).read_bytes() == fex_bytes, downloaded
        # switching to the regular build is asked for by name
        started = request(backend.sock, "POST", "/update/start", {"variant": "regular"})
        assert started["ok"] is True and started["variant"] == "regular", started
        assert wait_for_download(backend.sock)["status"] == "downloaded"
        downloaded = request(backend.sock, "GET", "/update/dmg-path?variant=regular")
        assert Path(downloaded["path"]).read_bytes() == regular_bytes, downloaded

    # a newer release without a FEX asset is not an update for the FEX build
    release, _, _ = release_variant(root, "fex-nofex-release", VERSION, fex_asset=False)
    backend_root = root / "fex-nofex"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0", fex_backend) as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["available"] is False and status["download_url"] == "", status
        assert status["switch_available"] is True, status

    # the same version: switching to the regular build is offered, nothing else
    release, _, _ = release_variant(root, "fex-same-release", current)
    backend_root = root / "fex-same"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0", fex_backend) as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["available"] is False and status["switch_available"] is True, status

    release, _, _ = release_variant(root, "fex-older-release", "0.1.0")
    backend_root = root / "fex-older"
    backend_root.mkdir()
    with Backend(backend_root, release, "27.0", fex_backend) as backend:
        status = request(backend.sock, "GET", "/update/check")
        assert status["available"] is False and status["switch_available"] is False, status


def main() -> None:
    # /tmp keeps <home>/run/backend.sock inside sockaddr_un.sun_path.
    with tempfile.TemporaryDirectory(prefix="msu-", dir="/tmp") as temporary:
        root = Path(temporary)
        slow_root = root / "slow"
        slow_root.mkdir()
        test_download_progress(slow_root)
        release, regular_bytes, fex_bytes = write_release(root)
        supported_root = root / "supported"
        supported_root.mkdir()
        with Backend(supported_root, release, "27.0") as backend:
            status = request(backend.sock, "GET", "/update/check")
            assert status["ok"] is True and status["available"] is True, status
            assert status["download_url"].endswith("regular-arm64.dmg"), status
            assert status["download_size"] == len(regular_bytes), status
            assert status["fex_available"] is True, status
            assert status["fex_download_url"].endswith("fex-arm64.dmg"), status
            assert status["fex_download_size"] == len(fex_bytes), status
            assert status["fex_recommended"] is True and status["macos_version"] == "27.0", status
            assert status["macos_major"] == 27 and status["fex_min_macos"] == "26.4", status

            started = request(backend.sock, "POST", "/update/start", {"variant": "fex"})
            assert started["ok"] is True, started
            progress = wait_for_download(backend.sock)
            assert progress["status"] == "downloaded", progress
            downloaded = request(backend.sock, "GET", "/update/dmg-path?variant=fex")
            assert downloaded["ok"] is True and downloaded["version"] == VERSION, downloaded
            assert Path(downloaded["path"]).read_bytes() == fex_bytes
            assert request(backend.sock, "GET", "/update/dmg-path")["ok"] is False

            started = request(backend.sock, "POST", "/update/start", {"variant": "regular"})
            assert started["ok"] is True, started
            progress = wait_for_download(backend.sock)
            assert progress["status"] == "downloaded", progress
            downloaded = request(backend.sock, "GET", "/update/dmg-path")
            assert downloaded["ok"] is True and downloaded["version"] == VERSION, downloaded
            assert Path(downloaded["path"]).read_bytes() == regular_bytes

        # 26.4 is the oldest macOS the Arm64 build with FEX is meant for
        minimum_root = root / "minimum"
        minimum_root.mkdir()
        with Backend(minimum_root, release, "26.4") as backend:
            status = request(backend.sock, "GET", "/update/check")
            assert status["fex_recommended"] is True and status["macos_version"] == "26.4", status

        # older systems are warned in the app, not refused: the download still starts
        older_root = root / "older"
        older_root.mkdir()
        with Backend(older_root, release, "26.3.1") as backend:
            status = request(backend.sock, "GET", "/update/check")
            assert status["fex_available"] is True and status["fex_recommended"] is False, status
            assert status["macos_version"] == "26.3.1" and status["macos_major"] == 26, status
            started = request(backend.sock, "POST", "/update/start", {"variant": "fex"})
            assert started["ok"] is True, started
            assert wait_for_download(backend.sock)["status"] == "downloaded"
            invalid = request(backend.sock, "POST", "/update/start", {"variant": "preview"})
            assert invalid["ok"] is False and "variant" in invalid["error"], invalid
            current = status["current_version"]

        builds_root = root / "builds"
        builds_root.mkdir()
        test_regular_build_checks(builds_root, current)
        test_fex_build_checks(builds_root, FEX_BACKEND or build_fex_backend(current), current)

    print("baseline and FEX updater tests passed")


if __name__ == "__main__":
    main()
