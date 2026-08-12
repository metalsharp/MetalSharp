#!/usr/bin/env bash
set -euo pipefail

SDK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="subnautica2"
APPID="1962700"
LAUNCH_METHOD="dxmt_metal12"
SECONDS_TO_RUN="20"
RESULTS_DIR="$SDK_DIR/results"
BACKEND_URL="${METALSHARP_BACKEND_URL:-http://127.0.0.1:9274}"
BACKEND_AUTH_ARGS=()
if [[ -n "${METALSHARP_API_TOKEN:-}" ]]; then
  BACKEND_AUTH_ARGS=(-H "Authorization: Bearer ${METALSHARP_API_TOKEN}")
fi
BACKEND_TOKEN_FILE="${METALSHARP_BACKEND_TOKEN_FILE:-${METALSHARP_HOME:-$HOME/.metalsharp}/.backend-token}"
GAME_DIR="/Volumes/AverySSD/SteamLibrary/steamapps/common/Subnautica2/Subnautica2/Binaries/Win64"
CORPUS_DIR="/Volumes/AverySSD/SteamLibrary/steamapps/common/Subnautica2/.metalsharp-cache/shader-cache/m12/1962700"
START_STEAM=0

backend_curl() {
  local token
  token="$(cat "$BACKEND_TOKEN_FILE" 2>/dev/null || true)"
  if [[ "$token" =~ ^[0-9a-f]{64}$ ]]; then
    curl -fsS -H "X-MetalSharp-Token: $token" "$@"
  elif ((${#BACKEND_AUTH_ARGS[@]} > 0)); then
    curl -fsS "${BACKEND_AUTH_ARGS[@]}" "$@"
  else
    echo "MetalSharp backend token not found at $BACKEND_TOKEN_FILE" >&2
    return 1
  fi
}

usage() {
  cat <<'USAGE'
Usage:
  capture-game-shader-corpus.sh [options]

Options:
  --profile NAME          Result profile name. Defaults to subnautica2.
  --appid ID              Steam appid. Defaults to 1962700.
  --launch-method NAME    MetalSharp launch method. Defaults to dxmt_metal12.
  --seconds N             Bounded capture window. Defaults to 20.
  --backend-url URL       MetalSharp backend URL. Defaults to http://127.0.0.1:9274.
  --game-dir PATH         Win64 game directory for runtime preflight.
  --corpus-dir PATH       Expected shader corpus directory.
  --results-dir PATH      Result output directory.
  --start-steam           Ask backend to launch Steam before capture.
  -h, --help              Show this help.

This is not a blind game run. It:
  1. Requires runtime-layout preflight to pass.
  2. Records the corpus state before launch.
  3. Launches the target through the backend for a fixed short window.
  4. Kills the target by appid and process pattern.
  5. Emits JSON with newly captured .dxbc files.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      PROFILE="$2"
      shift 2
      ;;
    --appid)
      APPID="$2"
      shift 2
      ;;
    --launch-method)
      LAUNCH_METHOD="$2"
      shift 2
      ;;
    --seconds)
      SECONDS_TO_RUN="$2"
      shift 2
      ;;
    --backend-url)
      BACKEND_URL="$2"
      shift 2
      ;;
    --game-dir)
      GAME_DIR="$2"
      shift 2
      ;;
    --corpus-dir)
      CORPUS_DIR="$2"
      shift 2
      ;;
    --results-dir)
      RESULTS_DIR="$2"
      shift 2
      ;;
    --start-steam)
      START_STEAM=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

mkdir -p "$RESULTS_DIR"

python3 "$SDK_DIR/scripts/preflight-runtime-layout.py" \
  --profile "$PROFILE" \
  --game-dir "$GAME_DIR" \
  --results-dir "$RESULTS_DIR"

before_file="$(mktemp)"
after_file="$(mktemp)"
launch_file="$(mktemp)"
cleanup() {
  rm -f "$before_file" "$after_file" "$launch_file"
}
trap cleanup EXIT

find "$CORPUS_DIR" -type f \( -name '*.dxbc' -o -name 'pso-*.json' \) 2>/dev/null | sort > "$before_file" || true

backend_curl -X POST "$BACKEND_URL/kill" \
  -H 'Content-Type: application/json' \
  -d "{\"appid\":$APPID,\"pid\":0}" >/dev/null || true
pkill -9 -f '[S]ubnautica2-Win64-Shipping.exe|[S]ubnautica2|[C]rashReportClient.exe|[c]rashpad_handler.exe' || true

if [[ "$START_STEAM" == "1" ]]; then
  backend_curl -X POST "$BACKEND_URL/steam/launch" \
    -H 'Content-Type: application/json' \
    -d '{}' >/dev/null
  sleep 15
fi

backend_curl -X POST "$BACKEND_URL/steam/launch-game" \
  -H 'Content-Type: application/json' \
  -d "{\"appid\":$APPID,\"launchMethod\":\"$LAUNCH_METHOD\"}" > "$launch_file"

sleep "$SECONDS_TO_RUN"

backend_curl -X POST "$BACKEND_URL/kill" \
  -H 'Content-Type: application/json' \
  -d "{\"appid\":$APPID,\"pid\":0}" >/dev/null || true
pkill -9 -f '[S]ubnautica2-Win64-Shipping.exe|[S]ubnautica2|[C]rashReportClient.exe|[c]rashpad_handler.exe' || true

find "$CORPUS_DIR" -type f \( -name '*.dxbc' -o -name 'pso-*.json' \) 2>/dev/null | sort > "$after_file" || true

RESULTS_DIR="$RESULTS_DIR" \
PROFILE="$PROFILE" \
APPID="$APPID" \
LAUNCH_METHOD="$LAUNCH_METHOD" \
SECONDS_TO_RUN="$SECONDS_TO_RUN" \
CORPUS_DIR="$CORPUS_DIR" \
BEFORE_FILE="$before_file" \
AFTER_FILE="$after_file" \
LAUNCH_FILE="$launch_file" \
python3 - <<'PY'
import json
import os
from pathlib import Path

before = set(Path(os.environ["BEFORE_FILE"]).read_text().splitlines())
after = set(Path(os.environ["AFTER_FILE"]).read_text().splitlines())
new_files = sorted(after - before)
new_dxbc = [path for path in new_files if path.endswith(".dxbc")]
new_pso_manifests = [path for path in new_files if Path(path).name.startswith("pso-") and path.endswith(".json")]
launch_text = Path(os.environ["LAUNCH_FILE"]).read_text()
try:
    launch = json.loads(launch_text)
except json.JSONDecodeError:
    launch = {"raw": launch_text}

result = {
    "schema": "metalsharp.d3d12-metal.shader-corpus-capture.v1",
    "profile": os.environ["PROFILE"],
    "appid": int(os.environ["APPID"]),
    "launch_method": os.environ["LAUNCH_METHOD"],
    "seconds": int(os.environ["SECONDS_TO_RUN"]),
    "corpus_dir": os.environ["CORPUS_DIR"],
    "before_count": len(before),
    "after_count": len(after),
    "new_count": len(new_files),
    "new_dxbc_count": len(new_dxbc),
    "new_pso_manifest_count": len(new_pso_manifests),
    "new_files": new_files,
    "new_dxbc": new_dxbc,
    "new_pso_manifests": new_pso_manifests,
    "launch": launch,
    "ok": len(new_files) > 0,
}
out = Path(os.environ["RESULTS_DIR"]) / f"shader-corpus-capture-{os.environ['PROFILE']}.json"
out.write_text(json.dumps(result, indent=2) + "\n")
print(out)
raise SystemExit(0 if result["ok"] else 1)
PY
