#!/bin/bash
# Copyright (c) 2026 MetalSharp. Commercial licensing: averyfelts@aol.com
#
# Replace MetalSharp's Wine runtime with the current VKMT runtime without
# copying the old bottle collection. The existing Steam prefix is used only
# as the source for Steam itself, its installed files, its drive mappings, and
# its Steam-local state. The final prefix remains prefix-steam because the
# MetalSharp launcher uses that stable path.
set -euo pipefail

IFS=$'\n\t'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
METALSHARP_HOME="${METALSHARP_HOME:-${HOME}/.metalsharp}"
VKMT_INSTALLER="${METALSHARP_VKMT_INSTALLER:-}"
VKMT_BUNDLE_DIR="${METALSHARP_VKMT_BUNDLE_DIR:-}"
VKMT_ARCHIVE="${METALSHARP_VKMT_ARCHIVE:-}"
VKMT_REPO="${METALSHARP_VKMT_REPO:-metalsharp/VKMT-Wine}"
VKMT_TAG="${METALSHARP_VKMT_TAG:-VKMT-1.0}"
VKMT_INSTALLER_SHA256="${METALSHARP_VKMT_INSTALLER_SHA256:-d55b10e388142d289f648ca349ee9ff91b627e341d655b1e070be38097344055}"
APP_RESOURCES_DIR="$(cd "$SCRIPT_DIR/../../.." 2>/dev/null && pwd || true)"
STEAM_BUNDLE="${METALSHARP_STEAM_BUNDLE:-}"
GOLDBERG_BUNDLE="${METALSHARP_GOLDBERG_BUNDLE:-}"
CURRENT_USER="${USER:-$(id -un)}"
PREFIX_NAME="prefix-steam"
APPLY=0
KEEP_OLD_PREFIX=0
LOCAL_ONLY=0
FRESH=0
INSTALL_PROGRESS_FILE="${METALSHARP_INSTALL_PROGRESS_FILE:-}"

STATE_STAGE=""
PREFIX_STAGE=""
OLD_PREFIX_HOLD=""
WINEBOOT_LOG=""

usage() {
  cat <<'USAGE'
Usage: migrate-to-vkmt-runtime.sh [options]

The default is a non-mutating plan. Pass --apply to perform the migration.

The migration:
  * installs/replaces ~/.metalsharp/runtime with the verified VKMT runtime;
  * creates a fresh ~/.metalsharp/prefix-steam with VKMT Wine and wineboots it;
  * restores the existing Steam installation, steamapps, userdata, settings,
    and drive mappings into that new prefix;
  * preserves MetalSharp caches and user settings, including the Steam API key
    cache and Chromium local storage containing the selected UI theme;
  * never copies ~/.metalsharp/bottles, games, or the old prefix wholesale.

Options:
  --apply                 Execute the migration (without this, print a plan).
  --metalsharp-home DIR   MetalSharp data root (default: ~/.metalsharp).
  --vkmt-installer PATH   VKMT install-metalsharp-wine-runtime.sh to run.
  --bundle-dir DIR        Local VKMT release bundle directory.
  --archive PATH          Reassembled VKMT runtime archive.
  --steam-bundle PATH     Existing metalsharp-steam.tar.zst asset.
  --goldberg-bundle PATH  Existing goldberg.tar.zst asset.
  --fresh                 Create a new installation when no Steam prefix exists.
  --local-only            Do not allow the VKMT installer to download assets.
  --keep-old-prefix       Keep the old prefix as a timestamped rollback copy.
  -h, --help              Show this help.

The VKMT installer itself keeps the replaced runtime as a timestamped runtime
rollback. The old Steam prefix is discarded after the new prefix is verified
unless --keep-old-prefix is explicitly supplied. The external baseline copy
made before migration remains independent of this script.
USAGE
}

info() {
  echo "migrate-to-vkmt-runtime: $*" >&2
}

json_escape() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\n'/ }"
  printf '%s' "$value"
}

write_install_progress() {
  local step="$1" status="$2" message="$3" error="${4:-}"
  local escaped_message escaped_error error_json tmp
  [ -n "$INSTALL_PROGRESS_FILE" ] || return 0

  escaped_message="$(json_escape "$message")"
  error_json="null"
  if [ -n "$error" ]; then
    escaped_error="$(json_escape "$error")"
    error_json="\"$escaped_error\""
  fi

  mkdir -p "$(dirname "$INSTALL_PROGRESS_FILE")"
  tmp="${INSTALL_PROGRESS_FILE}.tmp.$$"
  printf '{"step":%s,"total":100,"current":"MetalSharp Wine","status":"%s","log":"%s","error":%s}\n' \
    "$step" "$status" "$escaped_message" "$error_json" > "$tmp"
  mv -f -- "$tmp" "$INSTALL_PROGRESS_FILE"
}

die() {
  write_install_progress 95 error "MetalSharp Wine installation failed: $*" "$*"
  echo "migrate-to-vkmt-runtime: ERROR: $*" >&2
  exit 1
}

cleanup() {
  local status=$?
  if [ -n "$WINEBOOT_LOG" ] && [ -e "$WINEBOOT_LOG" ]; then
    if [ "$status" -ne 0 ]; then
      echo "wineboot output:" >&2
      cat "$WINEBOOT_LOG" >&2 || true
    fi
    rm -f -- "$WINEBOOT_LOG"
  fi
  if [ -n "$PREFIX_STAGE" ] && [ -e "$PREFIX_STAGE" ]; then
    rm -rf -- "$PREFIX_STAGE"
  fi
  if [ -n "$OLD_PREFIX_HOLD" ] && [ -e "$OLD_PREFIX_HOLD" ]; then
    rm -rf -- "$OLD_PREFIX_HOLD"
  fi
  if [ -n "$STATE_STAGE" ] && [ -e "$STATE_STAGE" ]; then
    rm -rf -- "$STATE_STAGE"
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM

copy_tree() {
  local source="$1" destination="$2"
  if [ ! -e "$source" ] && [ ! -L "$source" ]; then
    return 0
  fi
  mkdir -p "$(dirname "$destination")"
  /usr/bin/ditto --rsrc --extattr --acl "$source" "$destination"
}

copy_file() {
  local source="$1" destination="$2"
  if [ ! -e "$source" ]; then
    return 0
  fi
  mkdir -p "$(dirname "$destination")"
  /bin/cp -p "$source" "$destination"
}

path_mode() {
  /usr/bin/stat -f '%Lp:%u:%g' "$1"
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

find_bundle() {
  local explicit="$1" name="$2" candidate
  if [ -n "$explicit" ]; then
    [ -s "$explicit" ] || die "bundle is missing or empty: $explicit"
    echo "$explicit"
    return
  fi

  for candidate in \
    "$VKMT_BUNDLE_DIR/$name" \
    "$APP_RESOURCES_DIR/bundles/$name" \
    "$SCRIPT_DIR/../../app/bundles/$name" \
    "$METALSHARP_HOME/cache/bundles/$name" \
    "$HOME/Downloads/$name"; do
    if [ -s "$candidate" ]; then
      echo "$candidate"
      return
    fi
  done
  die "required bundled asset not found: $name"
}

resolve_installer() {
  if [ -n "$VKMT_INSTALLER" ]; then
    [ -x "$VKMT_INSTALLER" ] || die "VKMT installer is not executable: $VKMT_INSTALLER"
    return
  fi

  local cache_dir="$METALSHARP_HOME/cache/vkmt/$VKMT_TAG"
  local cached="$cache_dir/install-metalsharp-wine-runtime.sh"
  local url="https://github.com/$VKMT_REPO/releases/download/$VKMT_TAG/install-metalsharp-wine-runtime.sh"
  mkdir -p "$cache_dir"

  if [ ! -s "$cached" ] || [ "$(/usr/bin/shasum -a 256 "$cached" | awk '{print $1}')" != "$VKMT_INSTALLER_SHA256" ]; then
    [ "$LOCAL_ONLY" -eq 0 ] || die "VKMT installer is not cached and --local-only was supplied"
    local partial="$cached.partial"
    rm -f -- "$partial"
    write_install_progress 14 downloading "Downloading and verifying MetalSharp Wine..."
    info "Downloading MetalSharp Wine installer ($VKMT_TAG)"
    curl --fail --location --retry 4 --retry-delay 2 --output "$partial" "$url"
    write_install_progress 28 verifying "Verifying MetalSharp Wine installer..."
    [ "$(/usr/bin/shasum -a 256 "$partial" | awk '{print $1}')" = "$VKMT_INSTALLER_SHA256" ] \
      || die "downloaded VKMT installer failed SHA-256 verification"
    mv "$partial" "$cached"
  else
    write_install_progress 28 verifying "Verifying cached MetalSharp Wine installer..."
  fi
  chmod 0755 "$cached"
  VKMT_INSTALLER="$cached"
  write_install_progress 32 "done" "MetalSharp Wine installer verified."
}

find_wine_user() {
  local users_dir="$1" candidate
  if [ -d "$users_dir/$CURRENT_USER" ]; then
    echo "$CURRENT_USER"
    return
  fi
  candidate="$(find "$users_dir" -mindepth 1 -maxdepth 1 -type d \
    ! -name Default ! -name Public ! -name 'Default User' -print -quit 2>/dev/null || true)"
  [ -n "$candidate" ] || die "could not identify the existing Wine user under $users_dir"
  basename "$candidate"
}

assert_quiescent() {
  local name
  for name in MetalSharp metalsharp steam wineserver wine wine64 wine-preloader; do
    if /usr/bin/pgrep -x "$name" >/dev/null 2>&1; then
      die "${name} is running; quit MetalSharp, Steam, and all Wine processes first"
    fi
  done
}

capture_state() {
  local relative source destination
  STATE_STAGE="$(mktemp -d "$METALSHARP_HOME/.vkmt-migration-state.XXXXXX")"
  mkdir -p "$STATE_STAGE/metalsharp" "$STATE_STAGE/application-support"

  # Selected state/cache trees only. Do not add bottles or games here.
  for relative in cache pipeline-cache shader-cache configs steam-desktop; do
    source="$METALSHARP_HOME/$relative"
    destination="$STATE_STAGE/metalsharp/$relative"
    if [ -d "$source" ]; then
      copy_tree "$source" "$destination"
    fi
  done
  for relative in setup.json install_progress.json; do
    copy_file "$METALSHARP_HOME/$relative" "$STATE_STAGE/metalsharp/$relative"
  done

  if [ -d "$HOME/Library/Application Support/metalsharp" ]; then
    copy_tree "$HOME/Library/Application Support/metalsharp" \
      "$STATE_STAGE/application-support/metalsharp"
  fi
  if [ -d "$HOME/Library/Application Support/VKMT" ]; then
    copy_tree "$HOME/Library/Application Support/VKMT" \
      "$STATE_STAGE/application-support/VKMT"
  fi

  [ ! -e "$STATE_STAGE/metalsharp/bottles" ] || die "state capture unexpectedly included bottles"
  [ ! -e "$STATE_STAGE/metalsharp/games" ] || die "state capture unexpectedly included games"
}

restore_state() {
  local relative source destination
  for relative in cache pipeline-cache shader-cache configs steam-desktop; do
    source="$STATE_STAGE/metalsharp/$relative"
    destination="$METALSHARP_HOME/$relative"
    if [ -d "$source" ]; then
      copy_tree "$source" "$destination"
    fi
  done
  for relative in setup.json install_progress.json; do
    copy_file "$STATE_STAGE/metalsharp/$relative" "$METALSHARP_HOME/$relative"
  done

  if [ -d "$STATE_STAGE/application-support/metalsharp" ]; then
    copy_tree "$STATE_STAGE/application-support/metalsharp" \
      "$HOME/Library/Application Support/metalsharp"
  fi
  if [ -d "$STATE_STAGE/application-support/VKMT" ]; then
    copy_tree "$STATE_STAGE/application-support/VKMT" \
      "$HOME/Library/Application Support/VKMT"
  fi
}

validate_saved_state() {
  local api_key="$STATE_STAGE/metalsharp/cache/steam_config.json"
  if [ -f "$api_key" ]; then
    grep -q '"steam_api_key"[[:space:]]*:' "$api_key" \
      || die "saved Steam API-key cache is malformed"
  fi
  if [ -d "$STATE_STAGE/application-support/metalsharp" ]; then
    [ -d "$STATE_STAGE/application-support/metalsharp/Local Storage" ] \
      || die "saved MetalSharp UI storage is missing"
  fi
}

initialize_prefix() {
  local wine="$METALSHARP_HOME/runtime/wine/bin/metalsharp-wine"
  local wineserver="$METALSHARP_HOME/runtime/wine/bin/wineserver"
  WINEBOOT_LOG="$STATE_STAGE/wineboot.log"
  [ -x "$wine" ] || die "VKMT Wine launcher is missing: $wine"
  [ -x "$wineserver" ] || die "VKMT wineserver is missing: $wineserver"

  write_install_progress 92 installing "Creating the fresh VKMT Steam prefix..."
  info "Creating fresh VKMT Wine prefix"
  if ! (
    unset WINEARCH
    env \
      VKMT_RUNTIME_ROOT="$METALSHARP_HOME/runtime" \
      WINEBUILDDIR="$METALSHARP_HOME/runtime/wine/build-ec" \
      WINEPREFIX="$PREFIX_STAGE" \
      WINEDEBUG=-all \
      "$wine" wineboot --init
    env \
      VKMT_RUNTIME_ROOT="$METALSHARP_HOME/runtime" \
      WINEBUILDDIR="$METALSHARP_HOME/runtime/wine/build-ec" \
      WINEPREFIX="$PREFIX_STAGE" \
      WINEDEBUG=-all \
      "$wineserver" -w
  ) >"$WINEBOOT_LOG" 2>&1; then
    die "VKMT wineboot failed"
  fi
  rm -f -- "$WINEBOOT_LOG"
  WINEBOOT_LOG=""
}

stage_goldberg() {
  local bundle extract destination
  bundle="$(find_bundle "$GOLDBERG_BUNDLE" goldberg.tar.zst)"
  write_install_progress 88 installing "Staging Goldberg Steam support..."
  extract="$STATE_STAGE/goldberg-bundle"
  destination="$METALSHARP_HOME/runtime/goldberg"
  mkdir -p "$extract"
  tar --use-compress-program=unzstd -xf "$bundle" -C "$extract"
  [ -s "$extract/x86/steam_api.dll" ] \
    || die "goldberg bundle is missing x86/steam_api.dll"
  [ -s "$extract/x64/steam_api64.dll" ] \
    || die "goldberg bundle is missing x64/steam_api64.dll"
  mkdir -p "$destination"
  copy_tree "$extract" "$destination"
  [ -s "$destination/x86/steam_api.dll" ] \
    || die "Goldberg x86 runtime was not installed"
  [ -s "$destination/x64/steam_api64.dll" ] \
    || die "Goldberg x64 runtime was not installed"
}

install_fresh_steam() {
  local bundle extract installer wine wineserver steam_dir
  bundle="$(find_bundle "$STEAM_BUNDLE" metalsharp-steam.tar.zst)"
  extract="$STATE_STAGE/steam-bundle"
  installer="$extract/steam/SteamSetup.exe"
  wine="$METALSHARP_HOME/runtime/wine/bin/metalsharp-wine"
  wineserver="$METALSHARP_HOME/runtime/wine/bin/wineserver"
  mkdir -p "$extract"
  tar --use-compress-program=unzstd -xf "$bundle" -C "$extract"
  [ -s "$installer" ] || die "Steam bundle is missing steam/SteamSetup.exe"

  write_install_progress 96 installing "Installing Steam into the fresh VKMT Wine prefix..."
  info "Installing Steam into the fresh VKMT Wine prefix"
  if ! (
    unset WINEARCH
    env \
      VKMT_RUNTIME_ROOT="$METALSHARP_HOME/runtime" \
      WINEBUILDDIR="$METALSHARP_HOME/runtime/wine/build-ec" \
      WINEPREFIX="$PREFIX_STAGE" \
      WINEDEBUG=-all \
      WINEDEBUGGER=none \
      "$wine" "$installer" /S
    env \
      VKMT_RUNTIME_ROOT="$METALSHARP_HOME/runtime" \
      WINEBUILDDIR="$METALSHARP_HOME/runtime/wine/build-ec" \
      WINEPREFIX="$PREFIX_STAGE" \
      WINEDEBUG=-all \
      WINEDEBUGGER=none \
      "$wineserver" -w
  ) >"$STATE_STAGE/steam-install.log" 2>&1; then
    die "Steam installation failed"
  fi

  steam_dir="$PREFIX_STAGE/drive_c/Program Files (x86)/Steam"
  for _ in $(seq 1 180); do
    [ -s "$steam_dir/steam.exe" ] && break
    sleep 1
  done
  [ -s "$steam_dir/steam.exe" ] || die "Steam.exe was not created by SteamSetup.exe"

  if [ -s "$extract/steam/steamwebhelper.exe" ]; then
    copy_file "$extract/steam/steamwebhelper.exe" "$steam_dir/steamwebhelper.exe"
  fi
}

restore_steam() {
  local old_user="$1" old_steam="$2" new_steam relative
  new_steam="$PREFIX_STAGE/drive_c/Program Files (x86)/Steam"

  info "Restoring Steam installation and installed files"
  copy_tree "$old_steam" "$new_steam"

  # Keep external library mappings and all old drive-letter targets, but let
  # c: continue to resolve relative to the new prefix's drive_c.
  rm -rf -- "$PREFIX_STAGE/dosdevices"
  copy_tree "$METALSHARP_HOME/$PREFIX_NAME/dosdevices" "$PREFIX_STAGE/dosdevices"

  relative="drive_c/Program Files (x86)/Common Files/Steam"
  copy_tree "$METALSHARP_HOME/$PREFIX_NAME/$relative" "$PREFIX_STAGE/$relative"
  for relative in \
    "drive_c/users/$old_user/AppData/Local/Steam" \
    "drive_c/users/$old_user/AppData/Roaming/Steam"; do
    copy_tree "$METALSHARP_HOME/$PREFIX_NAME/$relative" "$PREFIX_STAGE/$relative"
  done

  [ -f "$new_steam/steam.exe" ] || die "Steam executable was not restored"
  [ -f "$new_steam/config/loginusers.vdf" ] \
    || die "Steam login state was not restored"
  [ -f "$new_steam/config/config.vdf" ] \
    || die "Steam configuration was not restored"
}

verify_permissions() {
  local old_steam="$1" new_steam="$2" old_file new_file relative
  for relative in steam.exe config/config.vdf config/loginusers.vdf; do
    old_file="$old_steam/$relative"
    new_file="$new_steam/$relative"
    [ "$(path_mode "$old_file")" = "$(path_mode "$new_file")" ] \
      || die "permissions/ownership changed for Steam/$relative"
  done
}

verify_runtime() {
  local verifier="$METALSHARP_HOME/runtime/scripts/verify-runtime.sh"
  [ -x "$verifier" ] || die "VKMT runtime verifier is missing: $verifier"
  VKMT_RUNTIME_ROOT="$METALSHARP_HOME/runtime" "$verifier" \
    --runtime-root "$METALSHARP_HOME/runtime" >/dev/null
}

activate_prefix() {
  local final_prefix="$METALSHARP_HOME/$PREFIX_NAME" rollback
  OLD_PREFIX_HOLD="$METALSHARP_HOME/.${PREFIX_NAME}.pre-vkmt.$$"

  if [ -e "$final_prefix" ]; then
    mv "$final_prefix" "$OLD_PREFIX_HOLD"
  fi
  if ! mv "$PREFIX_STAGE" "$final_prefix"; then
    if [ -e "$OLD_PREFIX_HOLD" ]; then
      mv "$OLD_PREFIX_HOLD" "$final_prefix"
    fi
    OLD_PREFIX_HOLD=""
    die "could not activate new VKMT Steam prefix"
  fi
  PREFIX_STAGE=""

  if [ "$KEEP_OLD_PREFIX" -eq 1 ] && [ -e "$OLD_PREFIX_HOLD" ]; then
    rollback="$METALSHARP_HOME/.${PREFIX_NAME}.pre-vkmt.$(date -u '+%Y%m%dT%H%M%SZ')"
    mv "$OLD_PREFIX_HOLD" "$rollback"
    OLD_PREFIX_HOLD=""
    info "Old prefix retained at $rollback"
  elif [ -e "$OLD_PREFIX_HOLD" ]; then
    # The full pre-migration installation was separately copied externally.
    # Do not retain the old prefix or any old bottles locally.
    rm -rf -- "$OLD_PREFIX_HOLD"
    OLD_PREFIX_HOLD=""
  fi
}

run_migration() {
  local old_steam old_user final_prefix
  local installer_args
  old_steam="$METALSHARP_HOME/$PREFIX_NAME/drive_c/Program Files (x86)/Steam"
  final_prefix="$METALSHARP_HOME/$PREFIX_NAME"

  if [ "$FRESH" -eq 0 ]; then
    old_user="$(find_wine_user "$METALSHARP_HOME/$PREFIX_NAME/drive_c/users")"
    [ -d "$old_steam" ] || die "existing Steam installation not found: $old_steam"
    [ -f "$old_steam/steam.exe" ] || die "existing Steam executable not found: $old_steam/steam.exe"
  fi

  capture_state
  validate_saved_state

  installer_args=(--target "$METALSHARP_HOME/runtime" --replace)
  if [ -n "$VKMT_BUNDLE_DIR" ]; then
    installer_args+=(--bundle-dir "$VKMT_BUNDLE_DIR")
  fi
  if [ -n "$VKMT_ARCHIVE" ]; then
    installer_args+=(--archive "$VKMT_ARCHIVE")
  fi
  if [ "$LOCAL_ONLY" -eq 1 ]; then
    installer_args+=(--local-only)
  fi

  write_install_progress 40 installing "Downloading and verifying MetalSharp Wine runtime parts..."
  info "Installing current VKMT runtime"

  # The VKMT installer downloads four verified runtime parts plus the native
  # GOG support archive. It predates this installer progress protocol, so
  # watch its log and advance the UI as each major download/verification stage
  # appears instead of leaving the setup bar at 0% for the whole job.
  local runtime_log="$STATE_STAGE/vkmt-runtime-install.log" runtime_pid runtime_status=0
  : > "$runtime_log"
  "$VKMT_INSTALLER" "${installer_args[@]}" >"$runtime_log" 2>&1 &
  runtime_pid=$!
  while kill -0 "$runtime_pid" >/dev/null 2>&1; do
    if grep -q "Downloading .*part01" "$runtime_log"; then
      write_install_progress 45 downloading "Downloading MetalSharp Wine (part 1 of 4)..."
    elif grep -q "Downloading .*part02" "$runtime_log"; then
      write_install_progress 54 downloading "Downloading MetalSharp Wine (part 2 of 4)..."
    elif grep -q "Downloading .*part03" "$runtime_log"; then
      write_install_progress 63 downloading "Downloading MetalSharp Wine (part 3 of 4)..."
    elif grep -q "Downloading .*part04" "$runtime_log"; then
      write_install_progress 72 downloading "Downloading MetalSharp Wine (part 4 of 4)..."
    elif grep -q "Downloading MetalSharp-GOG" "$runtime_log"; then
      write_install_progress 78 downloading "Downloading native support assets..."
    elif grep -q "Verified part" "$runtime_log"; then
      write_install_progress 82 verifying "Verifying MetalSharp Wine download parts..."
    elif grep -q "Reassembling" "$runtime_log"; then
      write_install_progress 85 verifying "Reassembling and verifying MetalSharp Wine..."
    elif grep -q "Extracting into transactional" "$runtime_log"; then
      write_install_progress 90 installing "Installing the verified MetalSharp Wine runtime..."
    elif grep -q "Verifying extracted payload" "$runtime_log"; then
      write_install_progress 91 verifying "Verifying the installed MetalSharp Wine runtime..."
    fi
    sleep 1
  done
  if wait "$runtime_pid"; then
    runtime_status=0
  else
    runtime_status=$?
  fi
  cat "$runtime_log" >&2
  [ "$runtime_status" -eq 0 ] || die "MetalSharp Wine runtime installer failed (status $runtime_status)"
  write_install_progress 91 "done" "MetalSharp Wine runtime verified and installed."
  verify_runtime
  restore_state
  stage_goldberg

  PREFIX_STAGE="$(mktemp -d "$METALSHARP_HOME/.${PREFIX_NAME}.vkmt-stage.XXXXXX")"
  initialize_prefix
  if [ "$FRESH" -eq 1 ]; then
    install_fresh_steam
  else
    restore_steam "$old_user" "$old_steam"
    verify_permissions "$old_steam" "$PREFIX_STAGE/drive_c/Program Files (x86)/Steam"
  fi

  if [ -f "$STATE_STAGE/metalsharp/cache/steam_config.json" ]; then
    [ -f "$METALSHARP_HOME/cache/steam_config.json" ] \
      || die "Steam API-key cache was not preserved"
  fi
  if [ -d "$STATE_STAGE/application-support/metalsharp/Local Storage" ]; then
    [ -d "$HOME/Library/Application Support/metalsharp/Local Storage" ] \
      || die "MetalSharp UI settings were not preserved"
  fi

  activate_prefix
  info "VKMT migration complete: $final_prefix"
  info "Current bottles were not copied or migrated"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --apply) APPLY=1; shift ;;
    --metalsharp-home)
      [ "$#" -ge 2 ] || die "--metalsharp-home requires a directory"
      METALSHARP_HOME="$2"; shift 2 ;;
    --vkmt-installer)
      [ "$#" -ge 2 ] || die "--vkmt-installer requires a path"
      VKMT_INSTALLER="$2"; shift 2 ;;
    --bundle-dir)
      [ "$#" -ge 2 ] || die "--bundle-dir requires a directory"
      VKMT_BUNDLE_DIR="$2"; shift 2 ;;
    --archive)
      [ "$#" -ge 2 ] || die "--archive requires a path"
      VKMT_ARCHIVE="$2"; shift 2 ;;
    --steam-bundle)
      [ "$#" -ge 2 ] || die "--steam-bundle requires a path"
      STEAM_BUNDLE="$2"; shift 2 ;;
    --goldberg-bundle)
      [ "$#" -ge 2 ] || die "--goldberg-bundle requires a path"
      GOLDBERG_BUNDLE="$2"; shift 2 ;;
    --fresh) FRESH=1; shift ;;
    --local-only) LOCAL_ONLY=1; shift ;;
    --keep-old-prefix) KEEP_OLD_PREFIX=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[ "$(uname -s)" = "Darwin" ] || die "this migration targets macOS"
[ "$(uname -m)" = "arm64" ] || die "this migration requires Apple Silicon"
require_command find
require_command pgrep
require_command mktemp
[ -d "$METALSHARP_HOME" ] || die "MetalSharp home does not exist: $METALSHARP_HOME"
if [ ! -d "$METALSHARP_HOME/$PREFIX_NAME" ]; then
  FRESH=1
fi
resolve_installer

if [ "$APPLY" -eq 0 ]; then
  cat <<PLAN
VKMT migration plan (dry run)
  MetalSharp home:       $METALSHARP_HOME
  VKMT installer:        $VKMT_INSTALLER
  VKMT runtime target:   $METALSHARP_HOME/runtime
  new prefix:            $METALSHARP_HOME/$PREFIX_NAME
  mode:                  $([ "$FRESH" -eq 1 ] && echo fresh || echo upgrade)
  Steam source:          $([ "$FRESH" -eq 1 ] && echo bundled SteamSetup.exe || echo "$METALSHARP_HOME/$PREFIX_NAME/drive_c/Program Files (x86)/Steam")
  state preserved:       cache, pipeline-cache, shader-cache, configs, UI storage
  state excluded:        bottles, games, old prefix wholesale
  old prefix:            discarded after successful activation

Pass --apply to execute. Use --keep-old-prefix for an additional local
rollback prefix; the external baseline remains the primary rollback copy.
PLAN
  exit 0
fi

assert_quiescent
run_migration
