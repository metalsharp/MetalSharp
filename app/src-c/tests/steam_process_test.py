#!/usr/bin/env python3
"""Regression coverage for migrated Wine Steam process detection and shutdown."""

from __future__ import annotations

import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


class UnixHTTPConnection(http.client.HTTPConnection):
    """HTTP over the backend's owner-only Unix socket (<home>/run/backend.sock)."""

    def __init__(self, socket_path: Path, timeout: float):
        super().__init__("localhost", timeout=timeout)
        self.socket_path = str(socket_path)

    def connect(self) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.socket_path)


def request_json(sock: Path, path: str, method: str = "GET") -> dict[str, object]:
    connection = UnixHTTPConnection(sock, timeout=20)
    try:
        connection.request(method, path)
        response = connection.getresponse()
        if response.status >= 400:
            raise OSError(f"HTTP {response.status} for {method} {path}")
        value = json.load(response)
    finally:
        connection.close()
    assert isinstance(value, dict)
    return value


def wait_status(sock: Path, expected: bool, timeout: float = 10) -> dict[str, object]:
    deadline = time.monotonic() + timeout
    last: dict[str, object] = {}
    while time.monotonic() < deadline:
        try:
            last = request_json(sock, "/steam/status")
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

    # /tmp keeps <home>/run/backend.sock inside sockaddr_un.sun_path.
    with tempfile.TemporaryDirectory(prefix="mss-", dir="/tmp") as directory:
        root = Path(directory).resolve()
        home = root / "home"
        host_home = root / "host-home"
        steam_dir = home / "prefix-steam/drive_c/Program Files (x86)/Steam"
        steamapps = steam_dir / "steamapps"
        game_dir = steamapps / "common/Test Game"
        wine_bin = home / "runtime/wine/bin"
        game_dir.mkdir(parents=True)
        host_home.mkdir()
        wine_bin.mkdir(parents=True)
        (steam_dir / "Steam.exe").write_bytes(b"test")
        (steam_dir / "steamui.dll").write_bytes(b"test")
        (steamapps / "appmanifest_1.acf").write_text(
            '"AppState"\n{\n\t"appid"\t"1"\n\t"name"\t"Test Game"\n\t"installdir"\t"Test Game"\n}\n'
        )
        # A cold Steam start (-no-cef-sandbox) becomes a long-lived stand-in
        # for the Windows client; every invocation's first argument is logged.
        steam_stand_in = (
            f'exec "{sys.executable}" -c \'import os; '
            'os.execv("/bin/sleep", [r"C:\\Program Files (x86)\\Steam\\steam.exe", "120"])\''
        )
        for name in ("wine", "metalsharp-wine"):
            target = wine_bin / name
            target.write_text(
                "#!/bin/sh\n"
                'printf "%s\\n" "$@" > "$METALSHARP_HOME/steam-launch.args"\n'
                'printf "%s\\n" "$1" >> "$METALSHARP_HOME/wine-calls.log"\n'
                f'case "$2" in -no-cef-sandbox) {steam_stand_in};; esac\n'
            )
            target.chmod(0o755)

        sock = home / "run" / "backend.sock"
        env = {
            **os.environ,
            "HOME": str(host_home),
            "METALSHARP_HOME": str(home),
        }
        server = subprocess.Popen([str(backend)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        native = fake_process(steam_dir, "/Applications/Steam.app/Contents/MacOS/steam_osx")
        foreign_wine = fake_process(host_home, r"C:\Program Files (x86)\Steam\steam.exe")
        managed_service = fake_process(steam_dir, "wineserver")
        wine_steam: subprocess.Popen[bytes] | None = None
        try:
            wait_status(sock, False)
            library = request_json(sock, "/steam/library")
            game = next(item for item in library["games"] if item["appid"] == 1)
            assert game["embedded_icon_path"] is None
            assert not (game_dir / ".metalsharp").exists(), "ordinary library reads must not scan game contents"

            icon = game_dir / ".metalsharp/steam-embedded-icon.png"
            icon.parent.mkdir()
            icon.write_bytes(b"cached")
            library = request_json(sock, "/steam/library")
            game = next(item for item in library["games"] if item["appid"] == 1)
            assert game["embedded_icon_path"] == str(icon)

            assert native.poll() is None, "native macOS Steam stand-in exited unexpectedly"
            assert foreign_wine.poll() is None, "foreign Wine Steam stand-in exited unexpectedly"

            wine_steam = fake_process(steam_dir, r"C:\Program Files (x86)\Steam\steam.exe")
            wait_status(sock, True)

            launched = request_json(sock, "/steam/launch", method="POST")
            assert launched.get("ok") is True, launched
            launch_args = home / "steam-launch.args"
            for _ in range(50):
                if launch_args.exists():
                    break
                time.sleep(0.1)
            assert "steam://open/library" in launch_args.read_text().splitlines()

            stopped = request_json(sock, "/steam/stop", method="POST")
            assert stopped.get("ok") is True, stopped
            assert stopped.get("running") is False, stopped
            wine_steam.wait(timeout=5)
            managed_service.wait(timeout=5)
            assert native.poll() is None, "Wine Steam shutdown targeted native macOS Steam"
            assert foreign_wine.poll() is None, "Wine Steam shutdown targeted another Wine prefix"
            wait_status(sock, False)

            # Leftover helpers with no live client (e.g. after an interrupted
            # session) must not be "activated": the session restarts cleanly.
            # A user.reg that already carries the Steam registry seed skips
            # the slow `wine reg import`.
            seed = "\n".join(
                f"[Software\\\\Wine\\\\AppDefaults\\\\{app}\\\\DllOverrides] 1\n"
                '"d3d12"="builtin"\n"d3d12core"="builtin"\n"d3d12SDKLayers"="builtin"\n"dxcore"="builtin"\n'
                for app in ("Steam.exe", "steamwebhelper.exe", "steamwebhelper_real.exe")
            )
            seed += "".join(
                f'\n[Software\\\\Wine\\\\AppDefaults\\\\{app}\\\\OpenGL] 1\n"LaterEntryPoints"="Y"\n'
                for app in ("deadcells_gl.exe", "Mosa Lina.exe")
            )
            (home / "prefix-steam/user.reg").write_text(
                seed + '\n[Software\\\\Wine\\\\Mac Driver] 1\n"RetinaMode"="N"\n'
                '\n[Control Panel\\\\Desktop] 1\n"LogPixels"=dword:00000060\n'
            )
            calls = home / "wine-calls.log"
            calls.unlink(missing_ok=True)
            stale_helper = fake_process(steam_dir, r"C:\Program Files (x86)\Steam\bin\cef\steamwebhelper.exe")
            wait_status(sock, True)
            launched = request_json(sock, "/steam/launch", method="POST")
            assert launched.get("ok") is True, launched
            stale_helper.wait(timeout=5)
            args = launch_args.read_text().splitlines()
            assert "-no-cef-sandbox" in args and "steam://open/library" not in args, args
            assert "reg" not in calls.read_text().splitlines(), calls.read_text()
            stopped = request_json(sock, "/steam/stop", method="POST")
            assert stopped.get("ok") is True and stopped.get("running") is False, stopped
            wait_status(sock, False)
        finally:
            # Backend-spawned Wine stand-ins are not our children; a failed
            # assertion must not leak them, so let the backend tear them down.
            if server.poll() is None:
                try:
                    request_json(sock, "/steam/stop", method="POST")
                except Exception:
                    pass
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
    print("Steam process detection, activation, stale-session restart, and migration handoff shutdown verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
