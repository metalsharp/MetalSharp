#!/bin/bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/update.sh" >&2
    exit 2
fi

export METALSHARP_UPDATE_TEST_SOURCE_ONLY=1
# shellcheck source=../update.sh
source "$1"
unset METALSHARP_UPDATE_TEST_SOURCE_ONLY

STATUS_FILE="$(mktemp)"
OSASCRIPT_PID_FILE="$(mktemp)"
export OSASCRIPT_PID_FILE
OSASCRIPT_STUB_DIR="$(mktemp -d)"
trap 'rm -f "$STATUS_FILE" "$OSASCRIPT_PID_FILE"; rm -rf "$OSASCRIPT_STUB_DIR"' EXIT
APP_PID=424242
BACKEND_PID=424243
TARGET_VERSION=0.74.0
DMG_PATH=/tmp/metalsharp-test.dmg
APP_IS_ALIVE=1
UNMOUNT_REACHED=0

pid_alive() {
    if [ "$1" = "$APP_PID" ]; then
        [ "$APP_IS_ALIVE" -eq 1 ]
    else
        builtin kill -0 "$1" 2>/dev/null
    fi
}

kill_pid() {
    if [ "$1" = "$APP_PID" ]; then APP_IS_ALIVE=0; fi
    return 0
}

pkill() { return 0; }
force_kill_process_names() { :; }
unmount_stale_metalsharp_images() { UNMOUNT_REACHED=1; }

# Use an external executable so its PID is exactly the background PID tracked
# by update.sh. This also works with the stock macOS Bash 3.2 (no $BASHPID).
cat > "$OSASCRIPT_STUB_DIR/osascript" <<'SH'
#!/bin/sh
printf '%s\n' "$$" > "$OSASCRIPT_PID_FILE"
exec /bin/sleep 30
SH
chmod +x "$OSASCRIPT_STUB_DIR/osascript"

started=$SECONDS
PATH="$OSASCRIPT_STUB_DIR:$PATH" METALSHARP_APP_QUIT_TIMEOUT_SECONDS=1 force_stop_old_runtime
elapsed=$((SECONDS - started))

[ -s "$OSASCRIPT_PID_FILE" ] || { echo "hung osascript stub was never started" >&2; exit 1; }
quit_pid="$(cat "$OSASCRIPT_PID_FILE")"
case "$quit_pid" in
    ''|*[!0-9]*) echo "invalid osascript PID: $quit_pid" >&2; exit 1 ;;
esac
[ "$APP_IS_ALIVE" -eq 0 ] || { echo "app termination fallback did not run" >&2; exit 1; }
[ "$UNMOUNT_REACHED" -eq 1 ] || { echo "installer did not continue after the quit timeout" >&2; exit 1; }
[ "$elapsed" -lt 5 ] || { echo "quit request was not bounded (${elapsed}s)" >&2; exit 1; }
if builtin kill -0 "$quit_pid" 2>/dev/null; then
    echo "timed-out osascript process was left running: $quit_pid" >&2
    exit 1
fi

python3 - "$STATUS_FILE" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    status = json.load(stream)
assert status["phase"] == "unmounting_old_runtime", status
PY

# A responsive Apple Event should not pay the full timeout before TERM fallback.
cat > "$OSASCRIPT_STUB_DIR/osascript" <<'SH'
#!/bin/sh
exit 0
SH
chmod +x "$OSASCRIPT_STUB_DIR/osascript"
APP_IS_ALIVE=1
UNMOUNT_REACHED=0
started=$SECONDS
PATH="$OSASCRIPT_STUB_DIR:$PATH" METALSHARP_APP_QUIT_TIMEOUT_SECONDS=10 force_stop_old_runtime
elapsed=$((SECONDS - started))
[ "$UNMOUNT_REACHED" -eq 1 ] || { echo "responsive quit did not continue to unmount" >&2; exit 1; }
[ "$elapsed" -lt 5 ] || { echo "responsive osascript was treated as hung (${elapsed}s)" >&2; exit 1; }

# Leading-zero values must be normalized as decimal, not rejected as octal.
APP_IS_ALIVE=1
UNMOUNT_REACHED=0
started=$SECONDS
PATH="$OSASCRIPT_STUB_DIR:$PATH" METALSHARP_APP_QUIT_TIMEOUT_SECONDS=08 force_stop_old_runtime
elapsed=$((SECONDS - started))
[ "$UNMOUNT_REACHED" -eq 1 ] || { echo "leading-zero timeout prevented installer progress" >&2; exit 1; }
[ "$elapsed" -lt 5 ] || { echo "responsive quit with leading-zero timeout waited too long (${elapsed}s)" >&2; exit 1; }

echo "legacy updater shutdown timeout tests passed"
