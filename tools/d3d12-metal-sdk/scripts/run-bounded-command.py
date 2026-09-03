#!/usr/bin/env python3
"""Run one command with process-group timeout and deterministic cleanup."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


def terminate_group(process: subprocess.Popen[bytes], grace: float) -> None:
    try:
        group = os.getpgid(process.pid)
    except ProcessLookupError:
        return
    try:
        os.killpg(group, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + grace
    while process.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)
    if process.poll() is None:
        try:
            os.killpg(group, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stderr", type=Path)
    parser.add_argument("--cwd", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("a command is required after --")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    stderr_handle = None
    try:
        if args.stderr:
            args.stderr.parent.mkdir(parents=True, exist_ok=True)
            stderr_handle = args.stderr.open("wb")
        with args.output.open("wb") as stdout_handle:
            process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=stdout_handle,
                stderr=stderr_handle if stderr_handle else subprocess.DEVNULL,
                cwd=str(args.cwd) if args.cwd else None,
                start_new_session=True,
            )
            try:
                process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                terminate_group(process, min(5.0, max(0.5, args.timeout / 4.0)))
                return 124
            return process.returncode if process.returncode is not None else 1
    finally:
        if stderr_handle:
            stderr_handle.close()


if __name__ == "__main__":
    raise SystemExit(main())
