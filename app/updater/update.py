#!/usr/bin/env python3
"""MetalSharp Updater — handles app update installation after download.

Spawned by the Electron app with detached=true so it survives the app quitting.
Performs: kill processes → mount DMG → install → relaunch → verify.

Communication: writes JSON status to ~/.metalsharp/update_install_status.json
The new MetalSharp instance reads this file on launch to show success/failure.

Usage:
    python3 update.py --dmg <path> --backend-pid <pid> --target-version <ver> \
                      [--status-file <path>] [--python <path>]
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    os.setsid()
except OSError:
    pass

APP_PATH = "/Applications/MetalSharp.app"
BACKEND_PORT = 9274
UPDATE_DMG_PATH = None


def write_status(status_file, phase, percent, message, error=None, new_version=None):
    data = {
        "phase": phase,
        "percent": percent,
        "message": message,
        "error": error,
        "new_version": new_version,
        "dmg_path": UPDATE_DMG_PATH,
        "timestamp": time.time(),
    }
    try:
        with open(status_file, "w") as f:
            json.dump(data, f)
    except Exception:
        pass


def read_status(status_file):
    try:
        with open(status_file, "r") as f:
            return json.load(f)
    except Exception:
        return None


def run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=30, **kwargs)


def is_pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError, OSError):
        return False


def kill_pid(pid, timeout=10):
    try:
        os.kill(pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError, OSError):
        return True
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not is_pid_alive(pid):
            return True
        time.sleep(0.3)
    try:
        os.kill(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        pass
    time.sleep(0.5)
    return not is_pid_alive(pid)


def kill_by_patterns(patterns):
    for pat in patterns:
        try:
            subprocess.run(["pkill", "-x", pat], capture_output=True, timeout=5)
        except Exception:
            pass
    for pat in patterns:
        try:
            subprocess.run(["pkill", "-f", pat], capture_output=True, timeout=5)
        except Exception:
            pass
    time.sleep(1)


def verify_all_dead(patterns, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        all_dead = True
        for pat in patterns:
            r = subprocess.run(["pgrep", "-x", pat], capture_output=True, text=True)
            if r.returncode == 0 and r.stdout.strip():
                all_dead = False
                for pid_str in r.stdout.strip().split("\n"):
                    try:
                        os.kill(int(pid_str.strip()), signal.SIGKILL)
                    except Exception:
                        pass
        if all_dead:
            return True
        time.sleep(0.5)
    return False


def force_kill_process_names(patterns, grace=2):
    for pat in patterns:
        try:
            subprocess.run(["pkill", "-x", pat], capture_output=True, timeout=5)
        except Exception:
            pass
    time.sleep(grace)
    for pat in patterns:
        try:
            subprocess.run(["pkill", "-9", "-x", pat], capture_output=True, timeout=5)
        except Exception:
            pass


def unmount_stale_metalsharp_images(current_mount=None):
    try:
        result = subprocess.run(["mount"], capture_output=True, text=True, timeout=10)
    except Exception:
        return
    for line in result.stdout.splitlines():
        if "MetalSharp" not in line and "metalsharp" not in line:
            continue
        try:
            mount_point = line.split(" on ", 1)[1].split(" (", 1)[0]
        except Exception:
            continue
        if current_mount and mount_point == current_mount:
            continue
        subprocess.run(["hdiutil", "detach", mount_point, "-quiet"], capture_output=True, timeout=30)
        subprocess.run(["diskutil", "unmount", "force", mount_point], capture_output=True, timeout=30)


def force_stop_old_runtime(status_file, backend_pid, app_pid, target_version):
    write_status(status_file, "stopping_old_runtime", 5, "Force-stopping the old MetalSharp app and backend...", new_version=target_version)
    kill_pid(backend_pid, timeout=5)
    force_kill_process_names(["metalsharp-backend"], grace=1)
    subprocess.run(["osascript", "-e", 'tell application id "com.metalsharp.app" to quit'], capture_output=True, timeout=10)
    if app_pid:
        kill_pid(app_pid, timeout=5)
    force_kill_process_names([
        "MetalSharp",
        "MetalSharp Helper",
        "MetalSharp Helper (GPU)",
        "MetalSharp Helper (Renderer)",
        "MetalSharp Helper (Plugin)",
    ])
    write_status(status_file, "unmounting_old_runtime", 28, "Unmounting stale MetalSharp disk images...", new_version=target_version)
    unmount_stale_metalsharp_images()


def mount_dmg(dmg_path):
    mount_dir = tempfile.mkdtemp(prefix="metalsharp-update-mount-")
    r = run(["hdiutil", "attach", "-nobrowse", "-quiet", "-mountpoint", mount_dir, dmg_path])
    if r.returncode == 0 and os.path.isdir(mount_dir) and os.listdir(mount_dir):
        return mount_dir

    apple = (
        'do shell script "hdiutil attach -nobrowse -quiet -mountpoint '
        '\\"' + mount_dir + '\\" '
        '\\"' + dmg_path + '\\""'
        " with administrator privileges"
    )
    r = run(["osascript", "-e", apple])
    if r.returncode == 0:
        time.sleep(2)
        if os.path.isdir(mount_dir) and os.listdir(mount_dir):
            return mount_dir

    try:
        os.rmdir(mount_dir)
    except Exception:
        pass
    return None


def detach_mount(mount_point):
    if not mount_point:
        return
    run(["hdiutil", "detach", mount_point, "-quiet"])
    try:
        os.rmdir(mount_point)
    except Exception:
        pass


def _parse_mount(output):
    for line in output.strip().split("\n"):
        parts = line.split()
        if parts and parts[-1].startswith("/Volumes/"):
            return parts[-1]
    return None


def _parse_mount_info(output, dmg_path):
    lines = output.split("\n")
    found = False
    for line in lines:
        if dmg_path in line:
            found = True
        if found and "/Volumes/" in line:
            idx = line.index("/Volumes/")
            rest = line[idx:].strip()
            return rest
    return None


def find_app_in_mount(mount_point):
    try:
        for entry in os.listdir(mount_point):
            if entry.endswith(".app") and "metalsharp" in entry.lower():
                return os.path.join(mount_point, entry)
    except Exception:
        pass
    return None


def verify_app_bundle(app_path):
    required = [
        os.path.join(app_path, "Contents", "Info.plist"),
        os.path.join(app_path, "Contents", "MacOS", "MetalSharp"),
        os.path.join(app_path, "Contents", "Resources", "runtime", "metalsharp-backend"),
        os.path.join(
            app_path,
            "Contents",
            "Resources",
            "scripts",
            "tools",
            "native",
            "metalsharp_eac_substrate.dylib",
        ),
        os.path.join(
            app_path,
            "Contents",
            "Resources",
            "scripts",
            "tools",
            "native",
            "metalsharp_eac_libc.so.6",
        ),
        os.path.join(app_path, "Contents", "Resources", "scripts", "tools", "updater", "update.sh"),
    ]
    if not all(os.path.isfile(path) and os.path.getsize(path) > 0 for path in required):
        return False

    substrate = os.path.join(
        app_path,
        "Contents",
        "Resources",
        "scripts",
        "tools",
        "native",
        "metalsharp_eac_substrate.dylib",
    )
    symbol_image = os.path.join(
        app_path,
        "Contents",
        "Resources",
        "scripts",
        "tools",
        "native",
        "metalsharp_eac_libc.so.6",
    )
    try:
        with open(substrate, "rb") as handle:
            substrate_magic = handle.read(4)
        with open(symbol_image, "rb") as handle:
            symbol_magic = handle.read(4)
    except OSError:
        return False
    if substrate_magic not in {
        b"\xcf\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xca\xfe\xba\xbe",
    } or symbol_magic != b"\x7fELF":
        return False
    try:
        substrate_type = subprocess.run(
            ["file", "-b", substrate], capture_output=True, text=True, timeout=10, check=False
        ).stdout
        symbol_type = subprocess.run(
            ["file", "-b", symbol_image], capture_output=True, text=True, timeout=10, check=False
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return "Mach-O" in substrate_type and "x86_64" in substrate_type and "ELF 64-bit" in symbol_type and "x86-64" in symbol_type


def admin_rm_rf(path):
    r = run(["rm", "-rf", path])
    if r.returncode == 0:
        return True
    apple = 'do shell script "rm -rf \\"' + path + '\\"" with administrator privileges'
    r = run(["osascript", "-e", apple])
    return r.returncode == 0


def admin_mv(src, dst):
    r = run(["mv", src, dst])
    if r.returncode == 0:
        return True
    apple = 'do shell script "mv \\\"' + src + '\\\" \\\"' + dst + '\\\"" with administrator privileges'
    r = run(["osascript", "-e", apple])
    return r.returncode == 0


def admin_cp_r(src, dst):
    r = run(["cp", "-R", src, dst])
    if r.returncode == 0:
        return True
    apple = (
        'do shell script "cp -R \\"'
        + src
        + '\\" \\"'
        + dst
        + '\\"" with administrator privileges'
    )
    r = run(["osascript", "-e", apple])
    return r.returncode == 0


def wait_for_backend(timeout=45):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            req = urllib.request.Request(
                "http://127.0.0.1:" + str(BACKEND_PORT) + "/status"
            )
            with urllib.request.urlopen(req, timeout=3) as resp:
                data = json.loads(resp.read())
                return data.get("version"), data.get("pid")
        except Exception:
            pass
        time.sleep(1)
    return None, None


def main():
    parser = argparse.ArgumentParser(description="MetalSharp Updater")
    parser.add_argument("--dmg", required=True)
    parser.add_argument("--backend-pid", required=True, type=int)
    parser.add_argument("--target-version", required=True)
    parser.add_argument(
        "--status-file",
        default=os.path.expanduser("~/.metalsharp/update_install_status.json"),
    )
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--app-pid", default=0, type=int)
    args = parser.parse_args()

    sf = args.status_file
    tv = args.target_version
    global UPDATE_DMG_PATH
    dmg = args.dmg
    UPDATE_DMG_PATH = dmg
    bpid = args.backend_pid
    app_pid = args.app_pid

    write_status(sf, "starting", 0, "Starting update...", new_version=tv)

    # ── 1. Force-stop old app/backend before touching the update image ───────
    force_stop_old_runtime(sf, bpid, app_pid, tv)

    # ── 2. Kill steam/wine/webhelper ─────────────────────────────────
    write_status(
        sf, "killing_steam", 15, "Stopping Steam and Wine processes...", new_version=tv
    )
    wine_patterns = [
        "steam",
        "steam.exe",
        "steamwebhelper",
        "steamwebhelper.exe",
        "wine",
        "wine64",
        "wineserver",
    ]
    kill_by_patterns(wine_patterns)
    verify_all_dead(wine_patterns, timeout=15)
    write_status(
        sf, "killed_steam", 20, "Steam/Wine processes stopped.", new_version=tv
    )

    # ── 3. Verify DMG exists ─────────────────────────────────────────
    if not os.path.exists(dmg):
        write_status(
            sf,
            "error",
            20,
            "DMG not found: {}".format(dmg),
            error="dmg_not_found",
            new_version=tv,
        )
        sys.exit(1)

    # ── 4. Force-stop again immediately before mounting in case the app respawned ──
    force_stop_old_runtime(sf, bpid, app_pid, tv)

    # ── 5. Mount DMG (may prompt for password via osascript) ─────────
    write_status(sf, "mounting", 35, "Mounting update disk image...", new_version=tv)
    mount_point = mount_dmg(dmg)
    if not mount_point:
        write_status(
            sf,
            "error",
            35,
            "Failed to mount DMG.",
            error="mount_failed",
            new_version=tv,
        )
        sys.exit(1)
    write_status(sf, "mounted", 45, "Mounted at {}".format(mount_point), new_version=tv)

    # ── 6. Install ───────────────────────────────────────────────────
    write_status(sf, "installing", 50, "Installing new version...", new_version=tv)

    app_source = find_app_in_mount(mount_point)
    if not app_source:
        detach_mount(mount_point)
        write_status(
            sf,
            "error",
            50,
            "MetalSharp.app not found in update.",
            error="app_not_found",
            new_version=tv,
        )
        sys.exit(1)
    if not verify_app_bundle(app_source):
        detach_mount(mount_point)
        write_status(
            sf,
            "error",
            50,
            "MetalSharp.app is missing required runtime or EAC substrate assets.",
            error="app_bundle_invalid",
            new_version=tv,
        )
        sys.exit(1)

    backup_app_path = APP_PATH + ".previous." + str(os.getpid())
    if not admin_rm_rf(backup_app_path):
        detach_mount(mount_point)
        write_status(
            sf,
            "error",
            55,
            "Failed to remove a stale update backup.",
            error="backup_cleanup_failed",
            new_version=tv,
        )
        sys.exit(1)

    had_previous_app = os.path.exists(APP_PATH)
    if had_previous_app:
        if not admin_mv(APP_PATH, backup_app_path):
            detach_mount(mount_point)
            write_status(
                sf,
                "error",
                55,
                "Failed to stage the old app for rollback.",
                error="backup_stage_failed",
                new_version=tv,
            )
            sys.exit(1)

    write_status(
        sf, "installing", 60, "Copying new version to Applications...", new_version=tv
    )
    if not admin_cp_r(app_source, APP_PATH):
        admin_mv(backup_app_path, APP_PATH) if had_previous_app else None
        detach_mount(mount_point)
        write_status(
            sf,
            "error",
            65,
            "Failed to copy new version.",
            error="copy_failed",
            new_version=tv,
        )
        sys.exit(1)

    if not verify_app_bundle(APP_PATH):
        admin_rm_rf(APP_PATH)
        admin_mv(backup_app_path, APP_PATH) if had_previous_app else None
        write_status(
            sf,
            "error",
            75,
            "Installed MetalSharp.app is missing required EAC substrate assets.",
            error="installed_bundle_invalid",
            new_version=tv,
        )
        sys.exit(1)

    if not admin_rm_rf(backup_app_path):
        write_status(
            sf,
            "error",
            78,
            "New version installed, but the previous app backup could not be removed.",
            error="backup_cleanup_failed",
            new_version=tv,
        )
        sys.exit(1)

    write_status(sf, "installed", 80, "New version installed.", new_version=tv)

    ms_dir = os.path.expanduser("~/.metalsharp")
    os.makedirs(ms_dir, exist_ok=True)
    migration_marker = os.path.join(ms_dir, ".post-update-migration")
    try:
        with open(migration_marker, "w") as f:
            json.dump({"needed": True, "target_version": tv, "timestamp": time.time()}, f)
    except Exception:
        pass

    # ── 7. Unmount ───────────────────────────────────────────────────
    write_status(sf, "unmounting", 82, "Unmounting update disk...", new_version=tv)
    detach_mount(mount_point)

    # ── 8. Relaunch ──────────────────────────────────────────────────
    write_status(sf, "relaunching", 85, "Launching MetalSharp...", new_version=tv)
    time.sleep(1)
    run(["open", APP_PATH])

    # ── 9. Verify version ────────────────────────────────────────────
    write_status(sf, "verifying", 90, "Verifying installation...", new_version=tv)
    version, new_pid = wait_for_backend(timeout=45)

    if version and version != tv:
        write_status(
            sf,
            "deploying_backend",
            92,
            "Backend version mismatch, redeploying...",
            new_version=tv,
        )
        if new_pid:
            kill_pid(new_pid, timeout=10)
        time.sleep(1)
        run(["pkill", "-x", "metalsharp-backend"])
        time.sleep(1)
        run(["open", "-a", "MetalSharp"])
        time.sleep(3)
        version, new_pid = wait_for_backend(timeout=30)

    # ── 10. Report result ────────────────────────────────────────────
    if version == tv:
        write_status(
            sf,
            "complete",
            100,
            "Update installed. Opening migration wizard...",
            new_version=tv,
        )
    else:
        write_status(
            sf,
            "complete",
            100,
            "Update installed. Backend: v{} (expected v{})".format(version or "?", tv),
            new_version=tv,
        )


if __name__ == "__main__":
    main()
