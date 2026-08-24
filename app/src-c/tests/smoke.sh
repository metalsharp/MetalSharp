#!/bin/sh
set -eu

backend=$1
port=${METALSHARP_PORT:-19274}
home=${METALSHARP_HOME:?METALSHARP_HOME must be set}

"$backend" >/tmp/metalsharp-c-backend.stdout.$$ 2>/tmp/metalsharp-c-backend.stderr.$$ &
pid=$!
cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "/tmp/metalsharp-c-backend.stdout.$$" "/tmp/metalsharp-c-backend.stderr.$$"
    rm -rf "$home"
}
trap cleanup EXIT INT TERM

ready=0
i=0
while [ "$i" -lt 50 ]; do
    if response=$(curl --silent --fail "http://127.0.0.1:$port/status" 2>/dev/null); then
        ready=1
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
[ "$ready" -eq 1 ]

printf '%s' "$response" | python3 -c '
import json, os, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert v["version"] == "0.60.0"
assert v["dev_mode"] is False
assert v["metalsharp_home"] == os.environ["METALSHARP_HOME"]
assert isinstance(v["pid"], int) and v["pid"] > 0
'

config=$(curl --silent --fail "http://127.0.0.1:$port/config")
printf '%s' "$config" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert v["controllerInput"] == "off"
assert v["msync"] is True
'

curl --silent --fail --request POST --header 'Content-Type: application/json' \
    --data '{"graphicsRuntimeLogs":true,"controllerInput":"X","msync":false}' \
    "http://127.0.0.1:$port/config" >/dev/null
config_after=$(curl --silent --fail "http://127.0.0.1:$port/config")
printf '%s' "$config_after" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["graphicsRuntimeLogs"] is True
assert v["graphics_runtime_logs"] is True
assert v["controllerInput"] == "x"
assert v["msync"] is False
'

progress=$(curl --silent --fail "http://127.0.0.1:$port/update/progress")
printf '%s' "$progress" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["status"] == "idle" and v["percent"] == 0'
migration_check=$(curl --silent --fail "http://127.0.0.1:$port/update/migrate/check")
printf '%s' "$migration_check" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v == {"ok": True, "needed": False, "reason": "fresh_install"}'
migration_progress=$(curl --silent --fail "http://127.0.0.1:$port/update/migrate/progress")
printf '%s' "$migration_progress" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["status"] == "idle" and v["total"] == 0'
migration_report=$(curl --silent --fail "http://127.0.0.1:$port/update/migrate/report")
printf '%s' "$migration_report" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["status"] == "idle" and v["entries"] == []'
dmg=$(curl --silent --fail "http://127.0.0.1:$port/update/dmg-path")
printf '%s' "$dmg" | python3 -c 'import json, sys; assert json.load(sys.stdin)["ok"] is False'
cleanup_updates=$(curl --silent --fail --request POST "http://127.0.0.1:$port/update/cleanup")
printf '%s' "$cleanup_updates" | python3 -c 'import json, sys; assert json.load(sys.stdin)["ok"] is True'

setup=$(curl --silent --fail "http://127.0.0.1:$port/setup/state")
printf '%s' "$setup" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert v["completed"] is False
assert v["savedCompleted"] is False
assert v["step"] == 0
assert v["deviceName"] == ""
assert v["dxmtRuntime"]["current"] is False
'
curl --silent --fail --request POST --header 'Content-Type: application/json' \
    --data '{"step":2,"completed":true,"deviceName":"Test Host","steamApiKeySet":true}' \
    "http://127.0.0.1:$port/setup/save" >/dev/null
setup_after=$(curl --silent --fail "http://127.0.0.1:$port/setup/state")
printf '%s' "$setup_after" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["savedCompleted"] is True
assert v["completed"] is False
assert v["step"] == 2
assert v["deviceName"] == "Test Host"
assert v["steamApiKeySet"] is True
assert v["runtimeMigrationRequired"] is True
'
install_progress=$(curl --silent --fail "http://127.0.0.1:$port/setup/install-progress")
printf '%s' "$install_progress" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["status"] == "idle" and v["step"] == 0 and v["total"] == 0'
installing=$(curl --silent --fail "http://127.0.0.1:$port/setup/installing")
printf '%s' "$installing" | python3 -c 'import json, sys; assert json.load(sys.stdin) == {"installing": False}'
mtsp=$(curl --silent --request POST --header 'Content-Type: application/json' --data '{"appid":1234,"pipeline":"m12"}' "http://127.0.0.1:$port/mtsp/prepare")
printf '%s' "$mtsp" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] is False and v["appid"] == 1234'
bottles=$(curl --silent --fail "http://127.0.0.1:$port/bottles")
printf '%s' "$bottles" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and isinstance(v["bottles"], list)'
mkdir -p "$home/smoke-input"
printf 'smoke' > "$home/smoke-input/Game.exe"
printf 'cover' > "$home/smoke-input/cover.png"
sharp_install=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"srcPath\":\"$home/smoke-input/Game.exe\",\"name\":\"Smoke Game\"}" "http://127.0.0.1:$port/sharp-library/install")
printf '%s' "$sharp_install" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["app"]["name"] == "Smoke Game"'
sharp_id=$(printf '%s' "$sharp_install" | python3 -c 'import json, sys; print(json.load(sys.stdin)["app"]["id"])')
cover_set=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$sharp_id\",\"coverPath\":\"$home/smoke-input/cover.png\"}" "http://127.0.0.1:$port/sharp-library/set-cover")
printf '%s' "$cover_set" | python3 -c 'import json, sys; assert json.load(sys.stdin)["ok"]'
curl --silent --fail "http://127.0.0.1:$port/sharp-library/cover?id=$sharp_id" -o "$home/smoke-cover.png"
cmp "$home/smoke-cover.png" "$home/sharp-library/$sharp_id.png"
sharp_library=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library")
printf '%s' "$sharp_library" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and len(v["apps"]) == 1'
mkdir -p "$home/GameJolt/Smoke Windows Game/bin" "$home/GameJolt/Smoke Mac.app"
printf 'smoke' > "$home/GameJolt/Smoke Windows Game/bin/game.exe"
printf 'cover' > "$home/GameJolt/Smoke Windows Game/cover.png"
gamejolt=$(curl --silent --fail "http://127.0.0.1:$port/gamejolt")
printf '%s' "$gamejolt" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and len(v["games"]) == 2 and any(g["native"] for g in v["games"]) and any(g["cover_path"] for g in v["games"])'
gog_import=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"productId":"smoke-gog","title":"Smoke GOG"}' "http://127.0.0.1:$port/sharp-library/gog/import")
printf '%s' "$gog_import" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["game"]["productId"] == "smoke-gog"'
mkdir -p "$home/tools" "$home/gog-play/Game" "$home/gog"
printf '{}' > "$home/gog-play/Game/goggame-424242.info"
cat > "$home/tools/gogdl" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$METALSHARP_HOME/gog-launch-args"
EOF
chmod +x "$home/tools/gogdl"
python3 - <<'PY'
import json
import os
from pathlib import Path

home = Path(os.environ["METALSHARP_HOME"])
game = home / "gog-play" / "Game"
(home / "gog" / "library.json").write_text(json.dumps({
    "games": [{
        "productId": "424242",
        "title": "GOG Launch Regression",
        "platform": "windows",
        "slug": "gog_launch_regression",
        "imageUrl": "https://example.invalid/cover.jpg",
        "iconUrl": None,
        "installRoot": str(home / "gog-play"),
        "gameFolder": str(game),
        "primaryExe": "Game.exe",
        "primaryTaskName": "Play",
        "installed": True,
        "running": False,
        "status": "installed",
        "downloadSizeBytes": 12,
        "diskSizeBytes": 34,
        "lastInstallPid": None,
        "lastLaunchPid": None,
        "lastLogPath": None,
        "lastError": None,
    }],
    "lastSyncAt": 1,
}))
PY
gog_play=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"productId":"424242","engine":"auto"}' "http://127.0.0.1:$port/sharp-library/gog/play")
printf '%s' "$gog_play" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["game"]["title"] == "GOG Launch Regression" and v["game"]["gameFolder"].endswith("/gog-play/Game")'
i=0
while [ ! -f "$home/gog-launch-args" ] && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.02
done
[ -f "$home/gog-launch-args" ]
python3 - <<'PY'
import json
import os
from pathlib import Path

home = Path(os.environ["METALSHARP_HOME"])
args = (home / "gog-launch-args").read_text().splitlines()
assert args[:3] == ["--auth-config-path", str(home / "gog_store" / "auth.json"), "launch"]
assert args[3:5] == [str(home / "gog-play" / "Game"), "424242"]
game = json.loads((home / "gog" / "library.json").read_text())["games"][0]
assert game["slug"] == "gog_launch_regression"
assert game["primaryExe"] == "Game.exe"
assert game["primaryTaskName"] == "Play"
assert game["downloadSizeBytes"] == 12 and game["diskSizeBytes"] == 34
PY
gog_launch_log=$(printf '%s' "$gog_play" | python3 -c 'import json, sys; print(json.load(sys.stdin)["logPath"])')
i=0
while ! grep -q 'gogdl exited' "$gog_launch_log" 2>/dev/null && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.02
done
grep -q 'gogdl exited with Some(0)' "$gog_launch_log"
rm -rf "$home/logs/gog" "$home/gog-launch-args"

emulators=$(curl --silent --fail "http://127.0.0.1:$port/emulators")
printf '%s' "$emulators" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert [p["id"] for p in v["providers"]] == ["pcsx2", "rpcs3", "shadps4", "sharpemu"] and all(p["supported"] for p in v["providers"]); assert v["providers"][2]["experimental"] and v["providers"][3]["experimental"] and v["providers"][3]["platform"] == "PlayStation 5"'
sharpemu_status=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/sharpemu/status")
printf '%s' "$sharpemu_status" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["experimental"] and not v["installed"] and v["state"] == "missing_runtime" and v["runtimeArchitecture"] == "x86_64" and v["networkDefault"] == "denied"'

pcsx2_status=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/pcsx2/status")
printf '%s' "$pcsx2_status" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["supported"] and not v["installed"] and v["state"] == "missing_runtime" and v["runtimeArchitecture"] == "x86_64" and not v["biosInstalled"]'
pcsx2_update=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/pcsx2/update/check")
printf '%s' "$pcsx2_update" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["available"] and v["latestVersion"] == "v2.6.3" and v["downloadSize"] == 4 and v["digest"].startswith("sha256:")'
mkdir -p "$home/pcsx2-games"
printf 'disc-data-SLUS_123.45' > "$home/pcsx2-games/Smoke Game.iso"
printf '\177ELFhomebrew' > "$home/pcsx2-games/Homebrew.elf"
printf 'unsupported' > "$home/pcsx2-games/Track.cue"
ln -s /tmp "$home/pcsx2-games/outside"
pcsx2_add=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/pcsx2-games\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/add-root")
printf '%s' "$pcsx2_add" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and len(v["roots"]) == 1 and len(v["games"]) == 2; g=next(x for x in v["games"] if x["format"] == "iso"); assert g["title"] == "Smoke Game" and g["serial"] == "SLUS-12345" and g["size"] > 0'
pcsx2_id=$(printf '%s' "$pcsx2_add" | python3 -c 'import json, sys; print(next(x for x in json.load(sys.stdin)["games"] if x["format"] == "iso")["id"])')
pcsx2_root=$(printf '%s' "$pcsx2_add" | python3 -c 'import json, sys; print(json.load(sys.stdin)["roots"][0])')
protected_pcsx2_root=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"path":"/"}' "http://127.0.0.1:$port/sharp-library/pcsx2/add-root")
printf '%s' "$protected_pcsx2_root" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'

pcsx2_env="$home/emulators/pcsx2"
mkdir -p "$pcsx2_env/versions/smoke-build/PCSX2.app/Contents/MacOS" "$pcsx2_env/home/Library/Application Support/PCSX2/inis"
printf '#include <signal.h>\n#include <stdio.h>\n#include <string.h>\n#include <unistd.h>\nint main(int c,char**v){for(int i=1;i<c;i++){if(!strcmp(v[i],"-version")){puts("PCSX2 v2.6.3");return 0;}if(!strcmp(v[i],"-help")){puts("-batch -nogui -logfile -testconfig -setupwizard --");return 0;}if(!strcmp(v[i],"-testconfig"))return 0;}signal(SIGTERM,SIG_DFL);sleep(30);return 0;}\n' > "$home/pcsx2-smoke.c"
${CC:-cc} -arch x86_64 -mmacosx-version-min=11.0 "$home/pcsx2-smoke.c" -o "$pcsx2_env/versions/smoke-build/PCSX2.app/Contents/MacOS/PCSX2"
printf '{"schemaVersion":1,"provider":"pcsx2","runtimeTag":"smoke-build","dataPathFlag":false}' > "$pcsx2_env/versions/smoke-build/capabilities.json"
ln -s "versions/smoke-build" "$pcsx2_env/current"
printf '[UI]\nSetupWizardIncomplete = false\n\n[AutoUpdater]\nCheckAtStartup = true\n' > "$pcsx2_env/home/Library/Application Support/PCSX2/inis/PCSX2.ini"
pcsx2_initialized=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/pcsx2/initialize")
printf '%s' "$pcsx2_initialized" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["upstreamUpdaterDisabled"] and not v["biosInstalled"]'
grep -F "RecursivePaths = $pcsx2_root" "$pcsx2_env/home/Library/Application Support/PCSX2/inis/PCSX2.ini" >/dev/null
mkdir -p "$pcsx2_env/home/Library/Application Support/PCSX2/covers"
printf 'not-png' > "$pcsx2_env/home/Library/Application Support/PCSX2/covers/SLUS-12345.png"
pcsx2_bad_cover_scan=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/pcsx2/scan")
printf '%s' "$pcsx2_bad_cover_scan" | python3 -c 'import json, sys; g=next(x for x in json.load(sys.stdin)["games"] if x["format"] == "iso"); assert not g["hasArtwork"]'
PCSX2_COVER="$pcsx2_env/home/Library/Application Support/PCSX2/covers/SLUS-12345.png" python3 - <<'PY'
import base64, os
open(os.environ['PCSX2_COVER'],'wb').write(base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII='))
PY
pcsx2_cover_scan=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/pcsx2/scan")
printf '%s' "$pcsx2_cover_scan" | python3 -c 'import json, sys; g=next(x for x in json.load(sys.stdin)["games"] if x["format"] == "iso"); assert g["hasArtwork"] and g["region"] == "USA"'
curl --silent --fail "http://127.0.0.1:$port/sharp-library/pcsx2/cover?id=$pcsx2_id" -o "$home/pcsx2-cover.png"
cmp "$home/pcsx2-cover.png" "$pcsx2_env/home/Library/Application Support/PCSX2/covers/SLUS-12345.png"
rm "$pcsx2_env/home/Library/Application Support/PCSX2/covers/SLUS-12345.png"
ln -s /etc/passwd "$pcsx2_env/home/Library/Application Support/PCSX2/covers/SLUS-12345.png"
if curl --silent --fail "http://127.0.0.1:$port/sharp-library/pcsx2/cover?id=$pcsx2_id" -o "$home/pcsx2-cover-escaped"; then
    echo "PCSX2 cover endpoint followed a replaced symlink" >&2
    exit 1
fi
rm "$pcsx2_env/home/Library/Application Support/PCSX2/covers/SLUS-12345.png"
PCSX2_BIOS="$home/pcsx2-bios.bin" python3 - <<'PY'
import os, struct
p=os.environ['PCSX2_BIOS']; data=bytearray(4*1024*1024)
data[0x20:0x20+14]=b'0230AC20260101'
def entry(name,size): return name.encode().ljust(10,b'\0')+struct.pack('<HI',0,size)
data[0x1000:0x1010]=entry('RESET',0x20)
data[0x1010:0x1020]=entry('ROMVER',14)
open(p,'wb').write(data)
PY
dd if=/dev/zero of="$home/pcsx2-invalid-bios.bin" bs=1048576 count=4 status=none
pcsx2_invalid_bios=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/pcsx2-invalid-bios.bin\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/import-bios")
printf '%s' "$pcsx2_invalid_bios" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'
ln -s "$home/pcsx2-bios.bin" "$home/pcsx2-bios-link.bin"
pcsx2_linked_bios=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/pcsx2-bios-link.bin\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/import-bios")
printf '%s' "$pcsx2_linked_bios" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'
pcsx2_bios=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/pcsx2-bios.bin\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/import-bios")
printf '%s' "$pcsx2_bios" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["biosCount"] == 1 and v["region"] == "USA"'
pcsx2_failed_replacement=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/pcsx2-invalid-bios.bin\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/import-bios")
printf '%s' "$pcsx2_failed_replacement" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'
[ -f "$pcsx2_env/home/Library/Application Support/PCSX2/bios/pcsx2-bios.bin" ]
pcsx2_ready=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/pcsx2/status")
printf '%s' "$pcsx2_ready" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["installed"] and v["runtimeValid"] and v["setupComplete"] and v["biosInstalled"] and v["state"] == "ready" and v["gameRootCount"] == 1'
mv "$home/pcsx2-games/Smoke Game.iso" "$home/pcsx2-games/Smoke Game.iso.saved"
ln -s /etc/passwd "$home/pcsx2-games/Smoke Game.iso"
pcsx2_replaced_launch=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$pcsx2_id\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/launch")
printf '%s' "$pcsx2_replaced_launch" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert not v["ok"] and "changed" in v["error"]'
rm "$home/pcsx2-games/Smoke Game.iso"
mv "$home/pcsx2-games/Smoke Game.iso.saved" "$home/pcsx2-games/Smoke Game.iso"
pcsx2_launch=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$pcsx2_id\",\"fullscreen\":true}" "http://127.0.0.1:$port/sharp-library/pcsx2/launch")
printf '%s' "$pcsx2_launch" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pid"] > 0 and v["logPath"].endswith(".log")'
sleep 0.1
pcsx2_running=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/pcsx2/games")
printf '%s' "$pcsx2_running" | python3 -c 'import json, sys; assert any(g["running"] for g in json.load(sys.stdin)["games"])'
pcsx2_remove_running=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"confirm":true}' "http://127.0.0.1:$port/sharp-library/pcsx2/remove-runtime")
printf '%s' "$pcsx2_remove_running" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert not v["ok"] and "stop PCSX2" in v["error"]'
pcsx2_stop=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$pcsx2_id\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/stop")
printf '%s' "$pcsx2_stop" | python3 -c 'import json, sys; assert json.load(sys.stdin)["ok"]'
pcsx2_remove=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"confirm":true}' "http://127.0.0.1:$port/sharp-library/pcsx2/remove-runtime")
printf '%s' "$pcsx2_remove" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["preservedData"]'
[ -f "$pcsx2_env/home/Library/Application Support/PCSX2/bios/pcsx2-bios.bin" ]
[ -f "$home/pcsx2-games/Smoke Game.iso" ]
pcsx2_bad_update=$(curl --silent --fail --request POST "http://127.0.0.1:$port/sharp-library/pcsx2/update/install")
printf '%s' "$pcsx2_bad_update" | python3 -c 'import json, sys; assert json.load(sys.stdin)["running"]'
for _ in $(seq 1 100); do
    pcsx2_bad_progress=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/pcsx2/update/progress")
    pcsx2_bad_status=$(printf '%s' "$pcsx2_bad_progress" | python3 -c 'import json, sys; print(json.load(sys.stdin)["status"])')
    [ "$pcsx2_bad_status" = "failed" ] && break
    sleep 0.05
done
printf '%s' "$pcsx2_bad_progress" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["status"] == "failed" and "archive" in v["error"].lower()'
[ ! -e "$pcsx2_env/current" ]
pcsx2_remove_root=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/pcsx2-games\"}" "http://127.0.0.1:$port/sharp-library/pcsx2/remove-root")
printf '%s' "$pcsx2_remove_root" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["roots"] == [] and v["games"] == []'
[ -d "$home/pcsx2-games" ]

shadps4_status=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/status")
printf '%s' "$shadps4_status" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["supported"] and v["experimental"] and not v["installed"] and v["state"] == "missing_runtime" and v["runtimeArchitecture"] == "x86_64"'
mkdir -p "$home/emulators/shadps4/staging/update-interrupted"
printf 'partial' > "$home/emulators/shadps4/downloads/interrupted.zip.part"
curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/status" >/dev/null
[ ! -e "$home/emulators/shadps4/staging/update-interrupted" ]
[ ! -e "$home/emulators/shadps4/downloads/interrupted.zip.part" ]
shadps4_update=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/update/check")
printf '%s' "$shadps4_update" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["available"] and v["latestVersion"] == "v.0.18.0" and v["downloadSize"] == 4 and v["digest"].startswith("sha256:")'

mkdir -p "$home/shadps4-games/CUSA12345/sce_sys"
printf 'fake-eboot' > "$home/shadps4-games/CUSA12345/eboot.bin"
SHADPS4_SFO="$home/shadps4-games/CUSA12345/sce_sys/param.sfo" python3 - <<'PY'
import os, struct
values = [("TITLE_ID", "CUSA12345"), ("TITLE", "Smoke shadPS4 Game"), ("APP_VER", "01.23"), ("CATEGORY", "gd")]
keys = b""
entries = []
data = b""
for key, value in values:
    key_offset = len(keys)
    keys += key.encode() + b"\0"
    raw = value.encode() + b"\0"
    entries.append((key_offset, 0x0204, len(raw), len(raw), len(data)))
    data += raw
key_start = 20 + 16 * len(entries)
data_start = key_start + len(keys)
blob = struct.pack("<4sIIII", b"\0PSF", 0x101, key_start, data_start, len(entries))
blob += b"".join(struct.pack("<HHIII", *entry) for entry in entries) + keys + data
with open(os.environ["SHADPS4_SFO"], "wb") as handle:
    handle.write(blob)
PY
printf 'png-smoke' > "$home/shadps4-games/CUSA12345/sce_sys/icon0.png"
mkdir -p "$home/shadps4-games/CUSA12345-patch/sce_sys" "$home/shadps4-games/Malformed/sce_sys" "$home/shadps4-outside/CUSA77777/sce_sys"
cp "$home/shadps4-games/CUSA12345/sce_sys/param.sfo" "$home/shadps4-games/CUSA12345-patch/sce_sys/param.sfo"
printf 'patch-eboot' > "$home/shadps4-games/CUSA12345-patch/eboot.bin"
printf 'not-an-sfo' > "$home/shadps4-games/Malformed/sce_sys/param.sfo"
printf 'malformed-eboot' > "$home/shadps4-games/Malformed/eboot.bin"
cp "$home/shadps4-games/CUSA12345/sce_sys/param.sfo" "$home/shadps4-outside/CUSA77777/sce_sys/param.sfo"
printf 'outside-eboot' > "$home/shadps4-outside/CUSA77777/eboot.bin"
ln -s "$home/shadps4-outside" "$home/shadps4-games/symlinked-outside"
shadps4_add=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/shadps4-games\"}" "http://127.0.0.1:$port/sharp-library/shadps4/add-root")
printf '%s' "$shadps4_add" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and len(v["roots"]) == 1 and len(v["games"]) == 1; g=v["games"][0]; assert g["title"] == "Smoke shadPS4 Game" and g["titleId"] == "CUSA12345" and g["version"] == "01.23" and g["hasArtwork"] and g["hasUpdate"]'
shadps4_id=$(printf '%s' "$shadps4_add" | python3 -c 'import json, sys; print(json.load(sys.stdin)["games"][0]["id"])')
curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/cover?id=$shadps4_id" -o "$home/shadps4-cover.png"
cmp "$home/shadps4-cover.png" "$home/shadps4-games/CUSA12345/sce_sys/icon0.png"
protected_shadps4_root=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"path":"/"}' "http://127.0.0.1:$port/sharp-library/shadps4/add-root")
printf '%s' "$protected_shadps4_root" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'
protected_shadps4_system=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"path":"/System/Library"}' "http://127.0.0.1:$port/sharp-library/shadps4/add-root")
printf '%s' "$protected_shadps4_system" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'

mkdir -p "$home/shadps4-modules" "$home/shadps4-fonts/font" "$home/shadps4-fonts/font2"
printf '\177ELFmodule-smoke' > "$home/shadps4-modules/libSceFont.sprx"
printf '\177ELFunknown' > "$home/shadps4-modules/unknown.sprx"
printf 'font-smoke' > "$home/shadps4-fonts/font/test.ttf"
printf 'font2-smoke' > "$home/shadps4-fonts/font2/test.otf"
shadps4_modules=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/shadps4-modules\"}" "http://127.0.0.1:$port/sharp-library/shadps4/import-modules")
printf '%s' "$shadps4_modules" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["imported"] == 1 and v["rejected"] == 1'
shadps4_fonts=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/shadps4-fonts\"}" "http://127.0.0.1:$port/sharp-library/shadps4/import-fonts")
printf '%s' "$shadps4_fonts" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["files"] == 2'
mkdir -p "$home/shadps4-bad-fonts"
ln -s /tmp "$home/shadps4-bad-fonts/font"
shadps4_bad_fonts=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/shadps4-bad-fonts\"}" "http://127.0.0.1:$port/sharp-library/shadps4/import-fonts")
printf '%s' "$shadps4_bad_fonts" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'
[ -f "$home/emulators/shadps4/home/Library/Application Support/shadPS4/fonts/font/test.ttf" ]

shadps4_env="$home/emulators/shadps4"
mkdir -p "$shadps4_env/versions/smoke-build"
printf '#include <signal.h>\n#include <unistd.h>\nint main(void){signal(SIGTERM, SIG_DFL); sleep(30); return 0;}\n' > "$home/shadps4-smoke.c"
${CC:-cc} -arch x86_64 -mmacosx-version-min=13.0 "$home/shadps4-smoke.c" -o "$shadps4_env/versions/smoke-build/shadps4"
printf '{"ICD":{"api_version":"1.3.0","library_path":"./libvulkan_kosmickrisp.dylib"},"file_format_version":"1.0.1"}' > "$shadps4_env/versions/smoke-build/kosmickrisp_mesa_icd.json"
printf 'loader' > "$shadps4_env/versions/smoke-build/libvulkan.dylib"
printf 'driver' > "$shadps4_env/versions/smoke-build/libvulkan_kosmickrisp.dylib"
ln -s "versions/smoke-build" "$shadps4_env/current"
shadps4_ready=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/status")
printf '%s' "$shadps4_ready" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["installed"] and v["state"] == "ready" and v["moduleCount"] == 1 and v["modulesReady"] and v["fontsReady"] and v["gameRootCount"] == 1'
shadps4_pin=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/shadps4/pin-current")
printf '%s' "$shadps4_pin" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pinnedTag"] == "smoke-build" and v["suppressed"] == "pinned"'
shadps4_unpin=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/shadps4/unpin")
printf '%s' "$shadps4_unpin" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pinnedTag"] is None'
shadps4_launch=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$shadps4_id\",\"fullscreen\":true}" "http://127.0.0.1:$port/sharp-library/shadps4/launch")
printf '%s' "$shadps4_launch" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pid"] > 0 and v["logPath"].endswith(".log")'
sleep 0.2
shadps4_running=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/games")
printf '%s' "$shadps4_running" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["games"][0]["running"] and v["games"][0]["lastLogPath"].endswith(".log")'
shadps4_remove_running=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"confirm":true}' "http://127.0.0.1:$port/sharp-library/shadps4/remove-runtime")
printf '%s' "$shadps4_remove_running" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert not v["ok"] and "stop" in v["error"].lower()'
shadps4_stop=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$shadps4_id\"}" "http://127.0.0.1:$port/sharp-library/shadps4/stop")
printf '%s' "$shadps4_stop" | python3 -c 'import json, sys; assert json.load(sys.stdin)["ok"]'
shadps4_stopped=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/games")
printf '%s' "$shadps4_stopped" | python3 -c 'import json, sys; g=json.load(sys.stdin)["games"][0]; assert not g["running"] and g["lastExitSignal"] == 15'
shadps4_remove=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"confirm":true}' "http://127.0.0.1:$port/sharp-library/shadps4/remove-runtime")
printf '%s' "$shadps4_remove" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["preservedData"]'
[ -f "$shadps4_env/home/Library/Application Support/shadPS4/sys_modules/libSceFont.sprx" ]
[ -f "$shadps4_env/home/Library/Application Support/shadPS4/fonts/font/test.ttf" ]
[ -f "$home/shadps4-games/CUSA12345/eboot.bin" ]
shadps4_bad_update=$(curl --silent --fail --request POST "http://127.0.0.1:$port/sharp-library/shadps4/update/install")
printf '%s' "$shadps4_bad_update" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["running"]'
i=0
while [ "$i" -lt 50 ]; do
    shadps4_bad_progress=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/shadps4/update/progress")
    shadps4_bad_status=$(printf '%s' "$shadps4_bad_progress" | python3 -c 'import json, sys; print(json.load(sys.stdin)["status"])')
    [ "$shadps4_bad_status" = "failed" ] && break
    i=$((i + 1))
    sleep 0.1
done
printf '%s' "$shadps4_bad_progress" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["status"] == "failed" and "digest" in v["error"]'
[ ! -e "$shadps4_env/current" ]
[ -z "$(find "$shadps4_env/downloads" -name '*.part' -print -quit)" ]
shadps4_remove_root=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/shadps4-games\"}" "http://127.0.0.1:$port/sharp-library/shadps4/remove-root")
printf '%s' "$shadps4_remove_root" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["roots"] == [] and v["games"] == []'
[ -d "$home/shadps4-games" ]

rpcs3_status=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/rpcs3/status")
printf '%s' "$rpcs3_status" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and not v["installed"] and v["state"] == "not_installed" and not v["firmwareInstalled"]'
rpcs3_update=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/rpcs3/update/check")
printf '%s' "$rpcs3_update" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["available"] and v["latestVersion"] == "0.0.42-19000-01234567" and v["downloadSize"] == 123 and v["digest"].startswith("sha256:")'
rpcs3_refresh=$(curl --silent --fail --request POST "http://127.0.0.1:$port/sharp-library/rpcs3/update/refresh")
printf '%s' "$rpcs3_refresh" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["latestTag"].startswith("build-")'

mkdir -p "$home/rpcs3-games/Smoke Disc/PS3_GAME"
RPCS3_SFO="$home/rpcs3-games/Smoke Disc/PS3_GAME/PARAM.SFO" python3 - <<'PY'
import os, struct
values = [("TITLE_ID", "TEST12345"), ("TITLE", "Smoke RPCS3 Game"), ("APP_VER", "01.23"), ("CATEGORY", "DG")]
keys = b""
entries = []
data = b""
for key, value in values:
    key_offset = len(keys)
    keys += key.encode() + b"\0"
    raw = value.encode() + b"\0"
    entries.append((key_offset, 0x0204, len(raw), len(raw), len(data)))
    data += raw
key_start = 20 + 16 * len(entries)
data_start = key_start + len(keys)
blob = struct.pack("<4sIIII", b"\0PSF", 0x101, key_start, data_start, len(entries))
blob += b"".join(struct.pack("<HHIII", *entry) for entry in entries) + keys + data
with open(os.environ["RPCS3_SFO"], "wb") as handle:
    handle.write(blob)
PY
printf 'png-smoke' > "$home/rpcs3-games/Smoke Disc/PS3_GAME/ICON0.PNG"
rpcs3_add=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/rpcs3-games\"}" "http://127.0.0.1:$port/sharp-library/rpcs3/add-root")
printf '%s' "$rpcs3_add" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and len(v["roots"]) == 1 and len(v["games"]) == 1; g=v["games"][0]; assert g["title"] == "Smoke RPCS3 Game" and g["titleId"] == "TEST12345" and g["version"] == "01.23" and g["hasArtwork"]'
rpcs3_id=$(printf '%s' "$rpcs3_add" | python3 -c 'import json, sys; print(json.load(sys.stdin)["games"][0]["id"])')
curl --silent --fail "http://127.0.0.1:$port/sharp-library/rpcs3/cover?id=$rpcs3_id" -o "$home/rpcs3-cover.png"
cmp "$home/rpcs3-cover.png" "$home/rpcs3-games/Smoke Disc/PS3_GAME/ICON0.PNG"
invalid_rpcs3_root=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"path":"/definitely/not/a/real/rpcs3/root"}' "http://127.0.0.1:$port/sharp-library/rpcs3/add-root")
printf '%s' "$invalid_rpcs3_root" | python3 -c 'import json, sys; assert not json.load(sys.stdin)["ok"]'

rpcs3_env="$home/emulators/rpcs3"
mkdir -p "$rpcs3_env/versions/smoke-build/RPCS3.app/Contents/MacOS" "$rpcs3_env/home/Library/Application Support/rpcs3/dev_flash/vsh/module" "$rpcs3_env/home/Library/Application Support/rpcs3/dev_hdd0/home/00000001/savedata"
printf 'firmware-smoke' > "$rpcs3_env/home/Library/Application Support/rpcs3/dev_flash/vsh/module/vsh.self"
printf '#!/bin/sh\ntrap "exit 0" TERM INT\nsleep 30\n' > "$rpcs3_env/versions/smoke-build/RPCS3.app/Contents/MacOS/rpcs3"
chmod +x "$rpcs3_env/versions/smoke-build/RPCS3.app/Contents/MacOS/rpcs3"
ln -s "versions/smoke-build" "$rpcs3_env/current"
printf 'preserve-me' > "$rpcs3_env/home/Library/Application Support/rpcs3/dev_hdd0/home/00000001/savedata/marker"
rpcs3_ready=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/rpcs3/status")
printf '%s' "$rpcs3_ready" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["installed"] and v["firmwareInstalled"] and v["state"] == "ready" and v["currentTag"] == "smoke-build"'
rpcs3_pin=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/rpcs3/pin-current")
printf '%s' "$rpcs3_pin" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pinnedTag"] == "smoke-build" and v["suppressed"] == "pinned" and not v["available"]'
rpcs3_unpin=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/rpcs3/unpin")
printf '%s' "$rpcs3_unpin" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pinnedTag"] is None and v["available"]'
rpcs3_skip=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"tag":"build-0123456789abcdef0123456789abcdef01234567"}' "http://127.0.0.1:$port/sharp-library/rpcs3/skip-update")
printf '%s' "$rpcs3_skip" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["suppressed"] == "skipped" and not v["available"]'
rpcs3_clear_skip=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/rpcs3/clear-skip")
printf '%s' "$rpcs3_clear_skip" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["skippedTag"] is None and v["available"]'
mkdir -p "$rpcs3_env/versions/old-build"
cp -R "$rpcs3_env/versions/smoke-build/RPCS3.app" "$rpcs3_env/versions/old-build/RPCS3.app"
ln -s "versions/old-build" "$rpcs3_env/previous"
rpcs3_rollback=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/rpcs3/update/rollback")
printf '%s' "$rpcs3_rollback" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["currentTag"] == "old-build" and v["rollbackAvailable"]'
rpcs3_rollforward=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/sharp-library/rpcs3/update/rollback")
printf '%s' "$rpcs3_rollforward" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["currentTag"] == "smoke-build"'
rpcs3_launch=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$rpcs3_id\",\"fullscreen\":true}" "http://127.0.0.1:$port/sharp-library/rpcs3/launch")
printf '%s' "$rpcs3_launch" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pid"] > 0 and v["logPath"].endswith(".log")'
sleep 0.2
rpcs3_running=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/rpcs3/games")
printf '%s' "$rpcs3_running" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["games"][0]["running"] and v["games"][0]["pid"] > 0 and v["games"][0]["lastLogPath"].endswith(".log")'
rpcs3_remove_running=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"confirm":true}' "http://127.0.0.1:$port/sharp-library/rpcs3/remove-runtime")
printf '%s' "$rpcs3_remove_running" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert not v["ok"] and "stop RPCS3" in v["error"]'
rpcs3_stop=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"id\":\"$rpcs3_id\"}" "http://127.0.0.1:$port/sharp-library/rpcs3/stop")
printf '%s' "$rpcs3_stop" | python3 -c 'import json, sys; assert json.load(sys.stdin)["ok"]'
rpcs3_remove=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"confirm":true}' "http://127.0.0.1:$port/sharp-library/rpcs3/remove-runtime")
printf '%s' "$rpcs3_remove" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["preservedData"]'
[ -f "$rpcs3_env/home/Library/Application Support/rpcs3/dev_hdd0/home/00000001/savedata/marker" ]
[ -f "$home/rpcs3-games/Smoke Disc/PS3_GAME/PARAM.SFO" ]
rpcs3_removed=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/rpcs3/status")
printf '%s' "$rpcs3_removed" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert not v["installed"] and v["firmwareInstalled"] and v["state"] == "not_installed"'
rpcs3_remove_root=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"path\":\"$home/rpcs3-games\"}" "http://127.0.0.1:$port/sharp-library/rpcs3/remove-root")
printf '%s' "$rpcs3_remove_root" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["roots"] == [] and v["games"] == []'
[ -d "$home/rpcs3-games" ]
rpcs3_bad_update=$(curl --silent --fail --request POST "http://127.0.0.1:$port/sharp-library/rpcs3/update/install")
printf '%s' "$rpcs3_bad_update" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["running"]'
i=0
while [ "$i" -lt 50 ]; do
    rpcs3_bad_progress=$(curl --silent --fail "http://127.0.0.1:$port/sharp-library/rpcs3/update/progress")
    rpcs3_bad_status=$(printf '%s' "$rpcs3_bad_progress" | python3 -c 'import json, sys; print(json.load(sys.stdin)["status"])')
    [ "$rpcs3_bad_status" = "failed" ] && break
    i=$((i + 1))
    sleep 0.1
done
printf '%s' "$rpcs3_bad_progress" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["status"] == "failed" and "digest" in v["error"]'
[ ! -e "$rpcs3_env/current" ]
[ -z "$(find "$rpcs3_env/downloads" -name '*.part' -print -quit)" ]

mkdir -p "$home/d3d-game" "$home/bottles/steam_1234"
printf 'smoke' > "$home/d3d-game/game.exe"
printf '%s' '{"id":"steam_1234","name":"Smoke Game","bottle_type":"steam","steam_app_id":1234,"prefix_path":"'$home'/prefix-steam","arch":"wow64","runtime_profile":"d3dmetal","preferred_pipeline":"d3dmetal","installed_components":[],"health":"new"}' > "$home/bottles/steam_1234/bottle.json"
d3d_save=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"appid\":1234,\"gameDir\":\"$home/d3d-game\",\"name\":\"Smoke D3DMetal\"}" "http://127.0.0.1:$port/d3dmetal/bottles/steam_1234/save")
printf '%s' "$d3d_save" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["state"]["name"] == "Smoke D3DMetal"'
d3d_status=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"appid":1234}' "http://127.0.0.1:$port/d3dmetal/bottles/steam_1234/status")
printf '%s' "$d3d_status" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["state"]["appid"] == 1234'
game_route=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"appid":4321}' "http://127.0.0.1:$port/game/resolve-routing")
printf '%s' "$game_route" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pipeline"] == "vkd3d" and v["graphics_backend"] == "vkd3d-proton"'
mkdir -p "$home/games/4321/Smoke.app"
printf 'smoke' > "$home/games/4321/game.exe"
dual_info=$(curl --silent --fail "http://127.0.0.1:$port/game/dual-info?appid=4321")
printf '%s' "$dual_info" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["has_native_build"] and v["wine_dir"]'
mkdir -p "$home/games/777" "$home/assets/goldberg/x64"
printf 'goldberg-dll' > "$home/assets/goldberg/x64/steam_api64.dll"
goldberg_toggle=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"appid":777}' "http://127.0.0.1:$port/goldberg/toggle")
printf '%s' "$goldberg_toggle" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["goldberg_active"]'
goldberg_status=$(curl --silent --fail "http://127.0.0.1:$port/goldberg/status?appid=777")
printf '%s' "$goldberg_status" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["persisted_active"]'
dependency_validation=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"ids":["unknown-smoke-dependency"]}' "http://127.0.0.1:$port/setup/install-deps")
printf '%s' "$dependency_validation" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert not v["ok"] and v["results"][0]["error"] == "unknown dependency"'
redist_sources=$(curl --silent --fail "http://127.0.0.1:$port/bottles/redist-sources")
printf '%s' "$redist_sources" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and len(v["sources"]) == 11'
compatibility_matrix=$(curl --silent --fail "http://127.0.0.1:$port/bottles/compatibility-matrix")
printf '%s' "$compatibility_matrix" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and len(v["cases"]) == 8'
compatibility_record=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"id":"minecraft-installer","installerOpens":"smoke","notes":"smoke evidence"}' "http://127.0.0.1:$port/bottles/record-compatibility")
printf '%s' "$compatibility_record" | python3 -c 'import json, sys; v=json.load(sys.stdin); c=next(c for c in v["cases"] if c["id"] == "minecraft-installer"); assert v["ok"] and c["installer_opens"] == "smoke"'
bottle_sync=$(curl --silent --fail --request POST "http://127.0.0.1:$port/bottles/sync-steam")
printf '%s' "$bottle_sync" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["count"] == len(v["bottles"])'
mkdir -p "$home/bottles/doctor" "$home/doctor-prefix" "$home/doctor-game"
printf '{"id":"doctor","name":"Doctor","prefix_path":"%s/doctor-prefix","installed_components":[{"id":"x","state":"installed"}],"installed_app_detections":[{"name":"game"}],"game_install_path":"%s/doctor-game"}' "$home" "$home" > "$home/bottles/doctor/bottle.json"
bottle_doctor=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"id":"doctor"}' "http://127.0.0.1:$port/bottles/doctor")
printf '%s' "$bottle_doctor" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["report"]["ready"]'
directx_verify=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"id":"doctor"}' "http://127.0.0.1:$port/bottles/verify-directx")
printf '%s' "$directx_verify" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["missing_count"] > 0 and not v["complete"]'
mkdir -p "$home/runtime/wine/bin"
printf '#!/bin/sh\nexit 0\n' > "$home/runtime/wine/bin/metalsharp-wine"
chmod +x "$home/runtime/wine/bin/metalsharp-wine"
bottle_font=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"id":"doctor"}' "http://127.0.0.1:$port/bottles/apply-font-subs")
printf '%s' "$bottle_font" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pid"] > 0'
bottle_prepare=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"id":"doctor"}' "http://127.0.0.1:$port/bottles/prepare")
printf '%s' "$bottle_prepare" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and "report" in v'
bottle_repair=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"id":"doctor","component":"wine-mono","dryRun":true}' "http://127.0.0.1:$port/bottles/repair-component")
printf '%s' "$bottle_repair" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["repair"]["status"] == "available"'
agility=$(curl --silent --fail "http://127.0.0.1:$port/setup/agility-versions")
printf '%s' "$agility" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["default"] == "1.619.3" and len(v["retail"]) == 13 and len(v["preview"]) == 11'
dependencies=$(curl --silent --fail "http://127.0.0.1:$port/setup/dependencies")
printf '%s' "$dependencies" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert v["platform"] == "macos"
assert len(v["dependencies"]) == 10
assert {d["id"] for d in v["dependencies"]} == {"homebrew", "xcode_cli", "rosetta", "metalsharp_wine", "metalsharp_host_runtime", "dxmt_runtime", "dxmt_m12_runtime", "mono", "moltenvk", "steam"}
'
device=$(curl --silent --fail "http://127.0.0.1:$port/setup/device-name")
printf '%s' "$device" | python3 -c '
import json, re, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert re.match(r"^[A-Za-z]+-[A-Za-z]+$", v["name"])
'

logs=$(curl --silent --fail "http://127.0.0.1:$port/logs")
printf '%s' "$logs" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert len(v["logs"]) == 1
assert len(v["logs"][0]["lines"]) == 2
assert all("name" in entry and isinstance(entry["lines"], list) for entry in v["logs"])
'
stream=$(curl --silent --fail "http://127.0.0.1:$port/logs/stream?after=0")
printf '%s' "$stream" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True and v["total"] == 2 and len(v["lines"]) == 2
'
crashes=$(curl --silent --fail "http://127.0.0.1:$port/logs/crash-reports")
printf '%s' "$crashes" | python3 -c 'import json, sys; assert json.load(sys.stdin) == {"ok": True, "reports": []}'

scan=$(curl --silent --fail "http://127.0.0.1:$port/scan")
printf '%s' "$scan" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] is True and isinstance(v["data"]["games"], list) and "steam" in v["data"]'
pipelines=$(curl --silent --fail "http://127.0.0.1:$port/mtsp/pipelines?appid=620")
printf '%s' "$pipelines" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] is True and v["appid"] == 620 and len(v["pipelines"]) == 9 and v["recommended"] == "vkd3d"'
shape=$(curl --silent --fail "http://127.0.0.1:$port/mtsp/launch-shape?appid=620")
printf '%s' "$shape" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] is True and v["appid"] == 620 and v["pipeline"] == "vkd3d"'
rules=$(curl --silent --fail "http://127.0.0.1:$port/mtsp/default-rules")
printf '%s' "$rules" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] is True and isinstance(v["rules"], list)'

steam_status=$(curl --silent --fail "http://127.0.0.1:$port/steam/status")
printf '%s' "$steam_status" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert "installed" in v and "running" in v and "login_state" in v
'
steam_library=$(curl --silent --fail "http://127.0.0.1:$port/steam/library")
printf '%s' "$steam_library" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] is True and v["total"] == 0 and v["games"] == []'
api=$(curl --silent --fail "http://127.0.0.1:$port/steam/api-key")
printf '%s' "$api" | python3 -c 'import json, sys; assert json.load(sys.stdin)["key"] == ""'
curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"key":"test-key"}' "http://127.0.0.1:$port/steam/save-api-key" >/dev/null
api_after=$(curl --silent --fail "http://127.0.0.1:$port/steam/api-key")
printf '%s' "$api_after" | python3 -c 'import json, sys; assert json.load(sys.stdin)["key"] == "test-key"'

cache=$(curl --silent --fail "http://127.0.0.1:$port/cache/size")
printf '%s' "$cache" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert v["shader_cache"]["status"] == "empty"
assert v["pipeline_cache"]["status"] == "empty"
'
fx=$(curl --silent --fail "http://127.0.0.1:$port/metalfx/state")
printf '%s' "$fx" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert v["enabled"] is True
assert v["factor"] == 1.5
'
curl --silent --fail --request POST --header 'Content-Type: application/json' \
    --data '{"enabled":false,"factor":1.75}' \
    "http://127.0.0.1:$port/metalfx/toggle" >/dev/null
fx_after=$(curl --silent --fail "http://127.0.0.1:$port/metalfx/state")
printf '%s' "$fx_after" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["enabled"] is False
assert v["factor"] == 2.0
assert v["conf_factor"] == 2.0
'

handle_created=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"pid":4242,"object_type":"File","name":"\\\\Device\\\\test"}' "http://127.0.0.1:$port/kernel-translation/handle/create")
handle_raw=$(printf '%s' "$handle_created" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["entry"]["object_type"] == "File"; print(v["handle_raw"])')
handle_query=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"pid\":4242,\"handle\":$handle_raw}" "http://127.0.0.1:$port/kernel-translation/handle/query")
printf '%s' "$handle_query" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["TypeName"] == "File"'
handle_dup=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"pid\":4242,\"source_handle\":$handle_raw,\"access_mask\":2}" "http://127.0.0.1:$port/kernel-translation/handle/duplicate")
printf '%s' "$handle_dup" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["entry"]["access_mask"] == 2'
handle_seed=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"pid":99,"count":2}' "http://127.0.0.1:$port/kernel-translation/handle/seed-demo")
printf '%s' "$handle_seed" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["created"] == 2 and v["totalHandles"] == 2'
apc=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"thread_handle":256,"target_thread_id":300,"apc_routine":"0xDEAD"}' "http://127.0.0.1:$port/kernel-translation/apc/queue")
printf '%s' "$apc" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["entry"]["status"] == "Pending"'
apc_alert=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"thread_id":300}' "http://127.0.0.1:$port/kernel-translation/apc/test-alert")
printf '%s' "$apc_alert" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["pendingCount"] == 1 and v["remainingInQueue"] == 0'
apc_wait=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"thread_id":301}' "http://127.0.0.1:$port/kernel-translation/apc/wait-alertable")
printf '%s' "$apc_wait" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["status"] == "STATUS_WAIT_0"'
apc_context=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"thread_id":302,"pc":"0xDEADBEEF","sp":"0x12340000","x0":"0xAAAAAAAA","x1":"0xBBBBBBBB"}' "http://127.0.0.1:$port/kernel-translation/apc/set-thread-context")
printf '%s' "$apc_context" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["newContext"]["pc"] == "0xDEADBEEF"'
integrity=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"pid":123,"count":2}' "http://127.0.0.1:$port/kernel-translation/integrity/seed-demo")
printf '%s' "$integrity" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["created"] == 16'
drivers=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"name":"SmokeDriver"}' "http://127.0.0.1:$port/kernel-translation/driver/load")
printf '%s' "$drivers" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["driver"]["status"] == "Loaded"; print(v["driver_id"])' >/tmp/metalsharp-smoke-driver-id.$$
driver_id=$(cat /tmp/metalsharp-smoke-driver-id.$$); rm -f /tmp/metalsharp-smoke-driver-id.$$
device=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data "{\"driver_id\":$driver_id,\"device_name\":\"SmokeDevice\"}" "http://127.0.0.1:$port/kernel-translation/driver/create-device")
printf '%s' "$device" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["device"]["device_type"] == 34'
ob_registration=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"operation":"open_process"}' "http://127.0.0.1:$port/kernel-translation/ob/register-callback")
printf '%s' "$ob_registration" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["registration"]["active"] is True'
ob_protected=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"pid":9001,"name":"SmokeGame"}' "http://127.0.0.1:$port/kernel-translation/ob/protect-process")
printf '%s' "$ob_protected" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["protected"]["pid"] == 9001'
ob_operation=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"source_pid":1,"target_pid":9001,"operation":"open_process"}' "http://127.0.0.1:$port/kernel-translation/ob/simulate-operation")
printf '%s' "$ob_operation" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["operation_id"] > 0'
es_registration=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"callback_type":"process_notify"}' "http://127.0.0.1:$port/kernel-translation/es/register-callback")
printf '%s' "$es_registration" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["registration"]["active"] is True'
es_event=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"parent_pid":1,"child_pid":2,"action":"created","image_name":"smoke.exe"}' "http://127.0.0.1:$port/kernel-translation/es/fire-process-event")
printf '%s' "$es_event" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["event_id"] > 0'
es_ipc=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"direction":"outbound","message_type":"process"}' "http://127.0.0.1:$port/kernel-translation/es/create-ipc-channel")
printf '%s' "$es_ipc" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["channel"]["direction"] == "outbound"'
es_channels=$(curl --silent --fail --request POST "http://127.0.0.1:$port/kernel-translation/es/ipc-channels")
printf '%s' "$es_channels" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["count"] == 1'
thread_snapshot=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"pid":123}' "http://127.0.0.1:$port/kernel-translation/thread/snapshot")
printf '%s' "$thread_snapshot" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["thread_count"] == 1'
thread_watcher=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"pid":123}' "http://127.0.0.1:$port/kernel-translation/thread/create-watcher")
printf '%s' "$thread_watcher" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["watcher"]["active"] is True'
integration_install=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{}' "http://127.0.0.1:$port/kernel-translation/integration/extension-install")
printf '%s' "$integration_install" | python3 -c 'import json, sys; assert json.load(sys.stdin)["ok"] is True'
integration_log=$(curl --silent --fail --request POST --header 'Content-Type: application/json' --data '{"pid":123,"nt_syscall":"NtOpenProcess"}' "http://127.0.0.1:$port/kernel-translation/integration/log-translation")
printf '%s' "$integration_log" | python3 -c 'import json, sys; v=json.load(sys.stdin); assert v["ok"] and v["log"]["pid"] == 123'
abi=$(curl --silent --fail "http://127.0.0.1:$port/runtime/host-abi")
printf '%s' "$abi" | python3 -c '
import json, sys
v = json.load(sys.stdin)
assert v["ok"] is True
assert v["magic"] == "MSAB"
assert v["version"] == {"major": 1, "minor": 0}
assert v["steam_bridge"]["default_port"] == 18733
'

status=$(curl --silent --output /dev/null --write-out '%{http_code}' "http://127.0.0.1:$port/not-a-route")
[ "$status" = 404 ]
method_status=$(curl --silent --request PUT --output /dev/null --write-out '%{http_code}' "http://127.0.0.1:$port/status")
[ "$method_status" = 405 ]
