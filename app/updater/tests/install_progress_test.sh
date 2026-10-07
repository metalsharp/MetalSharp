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

STATUS_LOG="$(mktemp)"
DMG_PATH="$(mktemp)"
trap 'rm -f "$STATUS_LOG" "$DMG_PATH"' EXIT
export APP_PID=0 BACKEND_PID=0 TARGET_VERSION=0.78.0

write_status() { printf '%s %s\n' "$1" "$2" >> "$STATUS_LOG"; }
pkill() { return 0; }
sleep() { :; }
kill_pid() { return 0; }
force_kill_process_names() { :; }
unmount_stale_metalsharp_images() { :; }
hdiutil() { return 0; }

prepare_install

# The app shows each status percent as-is. Recovery hands off at 20%, so every
# pre-install step must report once, above that, and never move backwards.
python3 - "$STATUS_LOG" <<'PY'
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    steps = [(phase, int(percent)) for phase, percent in (line.split() for line in stream)]
phases = [phase for phase, _ in steps]
percents = [percent for _, percent in steps]
assert phases == [
    "starting",
    "stopping_old_runtime",
    "unmounting_old_runtime",
    "killing_steam",
    "verifying_dmg",
], steps
assert percents == sorted(percents), steps
assert len(set(percents)) == len(percents), steps
assert percents[0] > 20, steps
PY

echo "updater install progress tests passed"
