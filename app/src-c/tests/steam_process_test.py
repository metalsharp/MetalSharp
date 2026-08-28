#!/usr/bin/env python3
"""Regression coverage for migrated Wine Steam process detection and shutdown."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request_json(port: int, path: str, method: str = "GET") -> dict[str, object]:
    request = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method)
    with urllib.request.urlopen(request, timeout=20) as response:
        value = json.load(response)
    assert isinstance(value, dict)
    return value


def wait_status(port: int, expected: bool, timeout: float = 10) -> dict[str, object]:
    deadline = time.monotonic() + timeout
    last: dict[str, object] = {}
    while time.monotonic() < deadline:
        try:
            last = request_json(port, "/steam/status")
            if last.get("running") is expected:
                return last
        except Exception:
            pass
        time.sleep(0.1)
    raise AssertionError(f"Steam running never became {expected}: {last}")


def fake_process(cwd: Path, argv0: str) -> subprocess.Popen[bytes]:
    code = "import os,sys; os.chdir(sys.argv[1]); os.execv('/bin/sleep',[sys.argv[2],'120'])"
    return subprocess.Popen([sys.executable, "-c", code, str(cwd), argv0])


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: steam_process_test.py BACKEND")
    backend = Path(sys.argv[1]).resolve()
    assert backend.is_file()

    with tempfile.TemporaryDirectory(prefix="metalsharp-steam-process-") as directory:
        root = Path(directory).resolve()
        home = root / "home"
        host_home = root / "host-home"
        steam_dir = home / "prefix-steam/drive_c/Program Files (x86)/Steam"
        wine_bin = home / "runtime/wine/bin"
        steam_dir.mkdir(parents=True)
        host_home.mkdir()
        wine_bin.mkdir(parents=True)
        (steam_dir / "Steam.exe").write_bytes(b"test")
        (steam_dir / "steamui.dll").write_bytes(b"test")
        for name in ("wine", "metalsharp-wine"):
            target = wine_bin / name
            target.write_text("#!/bin/sh\nexit 0\n")
            target.chmod(0o755)

        port = free_port()
        env = {
            **os.environ,
            "HOME": str(host_home),
            "METALSHARP_HOME": str(home),
            "METALSHARP_PORT": str(port),
        }
        server = subprocess.Popen([str(backend)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        native = fake_process(steam_dir, "/Applications/Steam.app/Contents/MacOS/steam_osx")
        foreign_wine = fake_process(host_home, r"C:\Program Files (x86)\Steam\steam.exe")
        managed_service = fake_process(steam_dir, "wineserver")
        wine_steam: subprocess.Popen[bytes] | None = None
        try:
            wait_status(port, False)
            assert native.poll() is None, "native macOS Steam stand-in exited unexpectedly"
            assert foreign_wine.poll() is None, "foreign Wine Steam stand-in exited unexpectedly"

            wine_steam = fake_process(steam_dir, r"C:\Program Files (x86)\Steam\steam.exe")
            wait_status(port, True)

            stopped = request_json(port, "/steam/stop", method="POST")
            assert stopped.get("ok") is True, stopped
            assert stopped.get("running") is False, stopped
            wine_steam.wait(timeout=5)
            managed_service.wait(timeout=5)
            assert native.poll() is None, "Wine Steam shutdown targeted native macOS Steam"
            assert foreign_wine.poll() is None, "Wine Steam shutdown targeted another Wine prefix"
            wait_status(port, False)
        finally:
            processes = (wine_steam, managed_service, foreign_wine, native, server)
            for process in processes:
                if process is not None and process.poll() is None:
                    process.terminate()
            for process in processes:
                if process is None:
                    continue
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    print("Steam process detection and migration handoff shutdown verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
