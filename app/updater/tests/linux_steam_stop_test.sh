#!/bin/bash
# update.sh: Linux Steam's virtual machine (the FEX build's experimental Steam client) is stopped with its own command,
# with MetalSharp's folders, before Steam and Wine are killed; with no Linux Steam, or one that never ran, nothing runs.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/update.sh" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
MS_DIR="$WORK/home"
COMMAND="$MS_DIR/runtime/MetalSharp-Wine.app/Contents/share/steamvm/metalsharp-steam"

# the updater's own function, as update.sh defines it
eval "$(sed -n '/^stop_linux_steam() {$/,/^}$/p' "$1")"
declare -F stop_linux_steam >/dev/null

# no Linux Steam: nothing
stop_linux_steam

mkdir -p "$(dirname "$COMMAND")"
cat > "$COMMAND" <<'STUB'
#!/bin/bash
printf '%s|%s|%s|%s|%s|%s\n' "$*" "$METALSHARP_VM_DIR" "$METALSHARP_VM_RUN" "$METALSHARP_STEAM_PREFIX" \
    "$METALSHARP_GAME_PREFIX" "$METALSHARP_WINE" >> "$(dirname "$0")/calls"
STUB
chmod 755 "$COMMAND"

# never started (no run folder): nothing
stop_linux_steam
[ ! -e "$(dirname "$COMMAND")/calls" ]

mkdir -p "$MS_DIR/run/linux-steam"
stop_linux_steam
expected="stop|$MS_DIR/linux-steam|$MS_DIR/run/linux-steam|$MS_DIR/prefix-steam|$MS_DIR/prefix-steam|$MS_DIR/runtime/MetalSharp-Wine.app/Contents/MacOS/wine"
[ "$(cat "$(dirname "$COMMAND")/calls")" = "$expected" ]

# the Steam and Wine sweep runs it first, and never names the virtual machine's programs
grep -q '^    stop_linux_steam$' <(sed -n '/^stop_steam_and_wine() {$/,/^}$/p' "$1")
if sed -n '/^stop_steam_and_wine() {$/,/^}$/p' "$1" | grep -Eq 'krunkit|gvproxy|metalsharp-vmd|LinuxSteam'; then
    echo "stop_steam_and_wine kills the virtual machine's programs by name" >&2
    exit 1
fi
echo "updater Linux Steam stop tests passed"
