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
