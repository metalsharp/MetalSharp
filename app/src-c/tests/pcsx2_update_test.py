#!/usr/bin/env python3
"""Security and transaction tests for the managed PCSX2 runtime."""

from __future__ import annotations

import hashlib
import io
import json
import os
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BACKEND = Path(sys.argv[1]).resolve()
TAG = "v2.6.3"
ASSET = f"pcsx2-{TAG}-macos-Qt.tar.xz"
URL = f"https://github.com/PCSX2/pcsx2/releases/download/{TAG}/{ASSET}"


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


def wait_progress(port: int, timeout: float = 30) -> dict:
    deadline = time.time() + timeout
    value: dict = {}
    while time.time() < deadline:
        value = request(port, "GET", "/sharp-library/pcsx2/update/progress")
        if value.get("status") in {"completed", "failed"}:
            return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for PCSX2 transaction: {value}")


def compile_runtime(app: Path, *, arch: str = "x86_64", valid_help: bool = True, data_path: bool = False) -> None:
    executable = app / "Contents/MacOS/PCSX2"
    executable.parent.mkdir(parents=True)
    docs = app / "Contents/Resources/docs"
    docs.mkdir(parents=True)
    (docs / "GPL.html").write_text("GPL-3.0-or-later synthetic fixture\n")
    (docs / "ThirdPartyLicenses.html").write_text("Synthetic third-party notices\n")
    help_text = "-batch -nogui -logfile -testconfig -setupwizard --"
    if data_path:
        help_text += " -datapath"
    if not valid_help:
        help_text = "incompatible command line"
    source = app.parent / "pcsx2-fixture.c"
    source.write_text(
        "#include <errno.h>\n#include <signal.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
        "#include <string.h>\n#include <sys/stat.h>\n#include <unistd.h>\n"
        "int main(int c,char**v){for(int i=1;i<c;i++){"
        "if(!strcmp(v[i],\"-version\")){puts(\"PCSX2 v2.6.3\");return 1;}"
        f"if(!strcmp(v[i],\"-help\")){{puts(\"{help_text}\");return 1;}}"
        "if(!strcmp(v[i],\"-testconfig\")){char p[4096];snprintf(p,sizeof(p),\"%s/Library/Application Support/PCSX2/inis\",getenv(\"HOME\"));mkdir(p,0700);strcat(p,\"/PCSX2.ini\");if(access(p,F_OK)!=0){FILE*f=fopen(p,\"w\");if(!f)return 2;fputs(\"[UI]\\nSetupWizardIncomplete = true\\n\",f);fclose(f);}return 0;}}"
        "signal(SIGTERM,SIG_DFL);sleep(30);return 0;}\n"
    )
    subprocess.run(
        ["cc", "-arch", arch, "-mmacosx-version-min=11.0", str(source), "-o", str(executable)], check=True
    )


def make_archive(root: Path, *, arch: str = "x86_64", valid_help: bool = True, data_path: bool = False) -> Path:
    top = root / f"PCSX2-{TAG}.app"
    compile_runtime(top, arch=arch, valid_help=valid_help, data_path=data_path)
    archive = root / ASSET
    env = os.environ.copy()
    env["COPYFILE_DISABLE"] = "1"
    subprocess.run(["/usr/bin/tar", "-cJf", str(archive), "-C", str(root), top.name], check=True, env=env)
    return archive


def write_release(
    archive: Path,
    output: Path,
    *,
    size_delta: int = 0,
    digest: str | None = None,
    draft: bool = False,
    prerelease: bool = False,
    duplicate: bool = False,
) -> None:
    sha = digest or hashlib.sha256(archive.read_bytes()).hexdigest()
    asset = {
        "name": ASSET,
        "browser_download_url": URL,
        "size": archive.stat().st_size + size_delta,
        "digest": f"sha256:{sha}",
    }
    output.write_text(
        json.dumps(
            {
                "tag_name": TAG,
                "published_at": "2026-01-28T18:00:00Z",
                "draft": draft,
                "prerelease": prerelease,
                "assets": [asset, dict(asset)] if duplicate else [asset],
            }
        )
    )


def make_unsafe_archive(root: Path, kind: str) -> Path:
    archive = root / ASSET
    with tarfile.open(archive, "w:xz") as tar:
        if kind == "traversal":
            info = tarfile.TarInfo("../escape")
            info.size = 1
            tar.addfile(info, io.BytesIO(b"x"))
        elif kind == "symlink":
            directory = tarfile.TarInfo(f"PCSX2-{TAG}.app")
            directory.type = tarfile.DIRTYPE
            tar.addfile(directory)
            link = tarfile.TarInfo(f"PCSX2-{TAG}.app/escape")
            link.type = tarfile.SYMTYPE
            link.linkname = "/tmp"
            tar.addfile(link)
        elif kind == "multiple":
            for name in (f"PCSX2-{TAG}.app/file", "other/file"):
                info = tarfile.TarInfo(name)
                info.size = 1
                tar.addfile(info, io.BytesIO(b"x"))
        elif kind == "hardlink":
            first = tarfile.TarInfo(f"PCSX2-{TAG}.app/file")
            first.size = 1
            tar.addfile(first, io.BytesIO(b"x"))
            link = tarfile.TarInfo(f"PCSX2-{TAG}.app/hard")
            link.type = tarfile.LNKTYPE
            link.linkname = first.name
            tar.addfile(link)
        elif kind == "fifo":
            fifo = tarfile.TarInfo(f"PCSX2-{TAG}.app/pipe")
            fifo.type = tarfile.FIFOTYPE
            tar.addfile(fifo)
        elif kind == "case-duplicate":
            for name in (f"PCSX2-{TAG}.app/File", f"PCSX2-{TAG}.app/file"):
                info = tarfile.TarInfo(name)
                info.size = 1
                tar.addfile(info, io.BytesIO(b"x"))
        elif kind == "absolute":
            info = tarfile.TarInfo("/tmp/escape")
            info.size = 1
            tar.addfile(info, io.BytesIO(b"x"))
        else:
            raise AssertionError(kind)
    return archive


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
                "METALSHARP_PCSX2_RELEASE_JSON": str(release),
                "METALSHARP_PCSX2_DOWNLOAD_FILE": str(archive),
                "METALSHARP_PCSX2_HOST_ARCH": "arm64",
                "METALSHARP_PCSX2_HOST_MACOS": "27",
                "METALSHARP_PCSX2_ROSETTA": "1",
                "METALSHARP_PCSX2_SKIP_SIGNATURE_FOR_TESTS": "1",
            }
        )
        env.update(extra)
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


def run_failure_case(kind: str) -> None:
    with tempfile.TemporaryDirectory(prefix=f"pcsx2-{kind}-") as temporary:
        root = Path(temporary)
        if kind in {"traversal", "symlink", "multiple", "hardlink", "fifo", "case-duplicate", "absolute"}:
            archive = make_unsafe_archive(root, kind)
        else:
            archive = make_archive(
                root,
                arch="arm64" if kind == "wrong-arch" else "x86_64",
                valid_help=kind != "cli-drift",
            )
        release = root / "release.json"
        write_release(
            archive,
            release,
            size_delta=1 if kind == "wrong-size" else 0,
            digest="0" * 64 if kind == "wrong-digest" else None,
            draft=kind == "draft",
            prerelease=kind == "prerelease",
            duplicate=kind == "duplicate",
        )
        with Backend(root, archive, release) as backend:
            started = request(backend.port, "POST", "/sharp-library/pcsx2/update/install")
            if kind in {"draft", "prerelease", "duplicate"}:
                assert not started["ok"], started
            else:
                assert started["ok"] and started["running"], started
                result = wait_progress(backend.port)
                assert result["status"] == "failed", result
            assert not (backend.home / "emulators/pcsx2/current").exists()
            assert not list((backend.home / "emulators/pcsx2/downloads").glob("*.part"))


def test_host_gates() -> None:
    cases = (
        ({"METALSHARP_PCSX2_HOST_ARCH": "x86_64", "METALSHARP_PCSX2_SSE41": "0"}, "sse41_missing"),
        ({"METALSHARP_PCSX2_ROSETTA": "0"}, "rosetta_missing"),
        ({"METALSHARP_PCSX2_HOST_MACOS": "10"}, "macos_too_old"),
        ({"METALSHARP_PCSX2_HOST_ARCH": "riscv64"}, "unsupported_architecture"),
    )
    for index, (environment, reason) in enumerate(cases):
        with tempfile.TemporaryDirectory(prefix=f"pcsx2-host-{index}-") as temporary:
            root = Path(temporary)
            archive = make_archive(root)
            release = root / "release.json"
            write_release(archive, release)
            with Backend(root, archive, release, **environment) as backend:
                status = request(backend.port, "GET", "/sharp-library/pcsx2/status")
                assert not status["supported"] and status["unsupportedReason"] == reason, status
                update = request(backend.port, "POST", "/sharp-library/pcsx2/update/install")
                assert not update["ok"], update


def test_disc_image_selection_registers_parent_folder() -> None:
    with tempfile.TemporaryDirectory(prefix="pcsx2-disc-image-") as temporary:
        root = Path(temporary)
        archive = make_archive(root)
        release = root / "release.json"
        write_release(archive, release)
        games = root / "owned-games"
        games.mkdir()
        disc = games / "Selected Game.iso"
        disc.write_bytes(b"synthetic owned disc image")
        unsupported = games / "notes.txt"
        unsupported.write_text("not a game")
        with Backend(root, archive, release) as backend:
            added = request(
                backend.port,
                "POST",
                "/sharp-library/pcsx2/add-root",
                {"path": str(disc)},
            )
            assert added["ok"] and added["roots"] == [str(games.resolve())], added
            assert any(game["path"] == str(disc.resolve()) for game in added["games"]), added
            rejected = request(
                backend.port,
                "POST",
                "/sharp-library/pcsx2/add-root",
                {"path": str(unsupported)},
            )
            assert not rejected["ok"] and "supported disc image" in rejected["error"], rejected


def test_data_path_capability_probe() -> None:
    with tempfile.TemporaryDirectory(prefix="pcsx2-datapath-") as temporary:
        root = Path(temporary)
        archive = make_archive(root, data_path=True)
        release = root / "release.json"
        write_release(archive, release)
        with Backend(root, archive, release) as backend:
            assert request(backend.port, "POST", "/sharp-library/pcsx2/update/install")["ok"]
            assert wait_progress(backend.port)["status"] == "completed"
            status = request(backend.port, "GET", "/sharp-library/pcsx2/status")
            assert status["dataPathFlag"] is True
            capabilities = json.loads(
                (backend.home / "emulators/pcsx2/current/capabilities.json").read_text()
            )
            assert capabilities["dataPathFlag"] is True


def test_success_repair_rollback_and_preservation() -> None:
    with tempfile.TemporaryDirectory(prefix="pcsx2-success-") as temporary:
        root = Path(temporary)
        archive = make_archive(root)
        release = root / "release.json"
        write_release(archive, release)
        with Backend(root, archive, release) as backend:
            started = request(backend.port, "POST", "/sharp-library/pcsx2/update/install")
            assert started["ok"] and started["running"]
            progress = wait_progress(backend.port)
            assert progress["status"] == "completed", progress
            environment = backend.home / "emulators/pcsx2"
            version = environment / "versions" / TAG
            assert os.readlink(environment / "current") == f"versions/{TAG}"
            assert (version / "PCSX2.app/Contents/MacOS/PCSX2").exists()
            assert (version / "LICENSE").read_text().startswith("GPL-3.0")
            assert (version / "THIRD_PARTY_LICENSES.html").exists()
            assert (version.stat().st_mode & 0o777) == 0o555
            source = json.loads((version / "source.json").read_text())
            assert source["teamIdentifier"] == "PTMR35SWS3" and source["upstreamSignaturePreserved"]
            capabilities = json.loads((version / "capabilities.json").read_text())
            assert capabilities["dataPathFlag"] is False and capabilities["runtimeArchitecture"] == "x86_64"
            ini = environment / "home/Library/Application Support/PCSX2/inis/PCSX2.ini"
            assert "CheckAtStartup = false" in ini.read_text()
            marker = environment / "home/Library/Application Support/PCSX2/memcards/preserve.ps2"
            marker.parent.mkdir(parents=True)
            marker.write_text("preserve")

            # Same-tag repair waits for an active UI process and replaces a damaged runtime.
            opened = request(backend.port, "POST", "/sharp-library/pcsx2/open-ui", {})
            assert opened["ok"] and opened["pid"] > 0
            time.sleep(0.2)
            assert request(backend.port, "GET", "/sharp-library/pcsx2/status")["state"] == "running"
            os.chmod(version, 0o755)
            executable = version / "PCSX2.app/Contents/MacOS/PCSX2"
            os.chmod(executable.parent, 0o755)
            executable.unlink()
            repaired = request(backend.port, "POST", "/sharp-library/pcsx2/update/install")
            assert repaired["ok"] and repaired["running"]
            deadline = time.time() + 20
            waiting: dict = {}
            while time.time() < deadline:
                waiting = request(backend.port, "GET", "/sharp-library/pcsx2/update/progress")
                if waiting.get("status") == "waiting_for_exit":
                    break
                assert waiting.get("status") not in {"failed", "completed"}, waiting
                time.sleep(0.05)
            assert waiting.get("status") == "waiting_for_exit", waiting
            blocked_remove = request(
                backend.port, "POST", "/sharp-library/pcsx2/remove-runtime", {"confirm": True}
            )
            assert not blocked_remove["ok"] and "transaction" in blocked_remove["error"]
            stopped = request(backend.port, "POST", "/sharp-library/pcsx2/stop", {})
            assert stopped["ok"]
            assert wait_progress(backend.port)["status"] == "completed"
            assert executable.exists() and marker.read_text() == "preserve"
            backups = list((environment / "backups").glob("config-v2.6.3-*/backup.json"))
            assert backups and json.loads(backups[-1].read_text())["fromTag"] == TAG

            # A valid manually staged previous version can be selected and rolled forward again.
            previous = environment / "versions/previous-fixture"
            os.chmod(version, 0o755)
            subprocess.run(["/bin/cp", "-R", str(version), str(previous)], check=True)
            (environment / "previous").unlink(missing_ok=True)
            (environment / "previous").symlink_to("versions/previous-fixture")
            rolled = request(backend.port, "POST", "/sharp-library/pcsx2/update/rollback", {})
            assert rolled["ok"] and rolled["currentTag"] == "previous-fixture"
            rolled_forward = request(backend.port, "POST", "/sharp-library/pcsx2/update/rollback", {})
            assert rolled_forward["ok"] and rolled_forward["currentTag"] == TAG

            removed = request(backend.port, "POST", "/sharp-library/pcsx2/remove-runtime", {"confirm": True})
            assert removed["ok"] and removed["preservedData"]
            assert marker.read_text() == "preserve"
            assert not (environment / "current").exists()


def test_interrupted_activation_preserves_current() -> None:
    with tempfile.TemporaryDirectory(prefix="pcsx2-activation-") as temporary:
        root = Path(temporary)
        archive = make_archive(root)
        release = root / "release.json"
        write_release(archive, release)
        with Backend(root, archive, release, METALSHARP_PCSX2_FAIL_ACTIVATION="1") as backend:
            environment = backend.home / "emulators/pcsx2"
            old = environment / "versions/old"
            executable = old / "PCSX2.app/Contents/MacOS/PCSX2"
            executable.parent.mkdir(parents=True)
            subprocess.run(
                ["cc", "-arch", "x86_64", "-x", "c", "-o", str(executable), "-"],
                input=b"int main(void){return 0;}\n",
                check=True,
            )
            (environment / "current").symlink_to("versions/old")
            assert request(backend.port, "POST", "/sharp-library/pcsx2/update/install")["ok"]
            progress = wait_progress(backend.port)
            assert progress["status"] == "failed", progress
            assert os.readlink(environment / "current") == "versions/old"
            assert executable.exists()


def main() -> None:
    for kind in (
        "wrong-size",
        "wrong-digest",
        "traversal",
        "symlink",
        "multiple",
        "hardlink",
        "fifo",
        "case-duplicate",
        "absolute",
        "wrong-arch",
        "cli-drift",
        "draft",
        "prerelease",
        "duplicate",
    ):
        run_failure_case(kind)
    test_host_gates()
    test_disc_image_selection_registers_parent_folder()
    test_data_path_capability_probe()
    test_success_repair_rollback_and_preservation()
    test_interrupted_activation_preserves_current()
    print("PCSX2 update transaction tests passed")


if __name__ == "__main__":
    main()
