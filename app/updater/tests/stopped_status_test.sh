#!/bin/bash
# update.sh: an installer stopped by a signal before it finishes reports an error naming the step it was at, and
# keeps a log (metalsharp/MetalSharp#767: 0.80's installer stopped at "stopping_old_runtime" and left that status).
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/update.sh" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/home"
: > "$WORK/MetalSharp-9.9.9-FEX.dmg"
# The install itself (prepare_install, which closes MetalSharp) is replaced by a step that reports 24% and waits
sed 's/^prepare_install$/write_status stopping_old_runtime 24 "Force-stopping the old MetalSharp app and backend..."; while :; do \/bin\/sleep 0.1; done/' \
    "$1" > "$WORK/update.sh"
grep -q 'while :; do' "$WORK/update.sh"

METALSHARP_UPDATER_NO_LAUNCHD=1 /bin/bash "$WORK/update.sh" --dmg "$WORK/MetalSharp-9.9.9-FEX.dmg" --backend-pid 0 \
    --target-version 9.9.9 --status-file "$WORK/home/update_install_status.json" --metalsharp-home "$WORK/home" \
    --app-pid 0 &
pid=$!
for _ in $(seq 100); do
    grep -q stopping_old_runtime "$WORK/home/update_install_status.json" 2>/dev/null && break
    /bin/sleep 0.1
done
kill -TERM "$pid"
code=0
wait "$pid" || code=$?
[ "$code" -ne 0 ]

python3 - "$WORK/home/update_install_status.json" <<'PY'
import json, sys
status = json.load(open(sys.argv[1]))
assert status["phase"] == "error", status
assert status["error"] == "updater_stopped", status
assert status["percent"] == 24, status
assert "Force-stopping" in status["message"], status
PY
grep -q '^updater got SIGTERM$' "$WORK/home/logs/updater.log"
grep -q '^updater exited with status 1 during stopping_old_runtime$' "$WORK/home/logs/updater.log"

echo "updater stopped-status tests passed"
