#!/bin/bash
set -euo pipefail

DMG_PATH=""
BACKEND_PID=""
TARGET_VERSION=""
STATUS_FILE=""
METALSHARP_HOME_ARG=""
APP_PID="0"

write_status() {
    local phase="$1" percent="$2" message="$3" error="${4:-}"
    local ver="$TARGET_VERSION"
    local safe_phase="${phase//\\/\\\\}"
    safe_phase="${safe_phase//\"/\\\"}"
    local safe_message="${message//\\/\\\\}"
    safe_message="${safe_message//\"/\\\"}"
    local safe_ver="${ver//\\/\\\\}"
    safe_ver="${safe_ver//\"/\\\"}"
    local error_json="null"
    if [ -n "$error" ]; then
        local safe_error="${error//\\/\\\\}"
        safe_error="${safe_error//\"/\\\"}"
        error_json="\"$safe_error\""
    fi
    mkdir -p "$(dirname "$STATUS_FILE")" 2>/dev/null || true
    local safe_dmg="${DMG_PATH//\\/\\\\}"
    safe_dmg="${safe_dmg//\"/\\\"}"
    printf '{"phase":"%s","percent":%d,"message":"%s","error":%s,"new_version":"%s","dmg_path":"%s","timestamp":%s}\n' \
        "$safe_phase" "$percent" "$safe_message" "$error_json" \
        "$safe_ver" "$safe_dmg" "$(date +%s)" > "$STATUS_FILE" 2>/dev/null || true
}

pid_alive() {
    local pid="$1"
    [ -n "$pid" ] && [ "$pid" -gt 0 ] 2>/dev/null && kill -0 "$pid" 2>/dev/null
}

kill_pid() {
    local pid="$1" timeout="${2:-10}"
    pid_alive "$pid" || return 0
    kill "$pid" 2>/dev/null || true
    local deadline=$((SECONDS + timeout))
    while [ $SECONDS -lt $deadline ]; do
        pid_alive "$pid" || return 0
        sleep 0.3
    done
    kill -9 "$pid" 2>/dev/null || true
    sleep 0.5
    pid_alive "$pid" && return 1 || return 0
}

force_kill_process_names() {
    local grace="${1:-3}"
    shift || true
    for pat in "$@"; do
        pkill -x "$pat" 2>/dev/null || true
    done
    sleep "$grace"
    for pat in "$@"; do
        pkill -9 -x "$pat" 2>/dev/null || true
    done
}

unmount_stale_metalsharp_images() {
    local mounted_paths
    mounted_paths="$(mount | awk '/MetalSharp|metalsharp/ { sub(/^.* on /, ""); sub(/ \([^)]*\).*$/, ""); print }')"
    [ -n "$mounted_paths" ] || return 0
    while IFS= read -r mp; do
        [ -n "$mp" ] || continue
        [ "$mp" = "$MOUNT_POINT" ] && continue
        case "$mp" in
            *MetalSharp*|*metalsharp*)
                hdiutil detach "$mp" -quiet 2>/dev/null || diskutil unmount force "$mp" >/dev/null 2>&1 || true
                ;;
        esac
    done <<< "$mounted_paths"
}

force_stop_old_runtime() {
    write_status "stopping_old_runtime" 5 "Force-stopping the old MetalSharp app and backend..."
    kill_pid "$BACKEND_PID" 5 || true
    pkill -x metalsharp-backend 2>/dev/null || true
    sleep 1
    pkill -9 -x metalsharp-backend 2>/dev/null || true

    # AppleScript launches a target application before sending `quit`. Never
    # invoke it unless the exact app process that requested this update is
    # still alive, or an update applied while MetalSharp is closed can open an
    # old registered bundle and accidentally enter its startup flow.
    if pid_alive "$APP_PID"; then
        local quit_pid timeout_seconds attempt
        timeout_seconds="${METALSHARP_APP_QUIT_TIMEOUT_SECONDS:-10}"
        case "$timeout_seconds" in
            ''|*[!0-9]*|0) timeout_seconds=10 ;;
        esac
        if [ "${#timeout_seconds}" -gt 2 ]; then
            timeout_seconds=30
        elif [ "$timeout_seconds" -gt 30 ]; then
            timeout_seconds=30
        fi
        # Bash treats leading-zero arithmetic operands as octal; normalize
        # validated environment values such as "08" before the loop below.
        timeout_seconds=$((10#$timeout_seconds))

        # Apple Events can hang indefinitely when macOS is waiting on user
        # approval or the app doesn't reply. Keep this installer independent
        # of the app it is replacing: bound the quit request, then continue to
        # the explicit TERM/KILL fallback below.
        osascript -e 'tell application id "com.metalsharp.app" to quit' >/dev/null 2>&1 &
        quit_pid=$!
        for ((attempt = 0; attempt < timeout_seconds * 5; attempt++)); do
            if ! pid_alive "$APP_PID" || ! pid_alive "$quit_pid"; then
                break
            fi
            sleep 0.2
        done
        if pid_alive "$quit_pid"; then
            kill "$quit_pid" 2>/dev/null || true
        fi
        wait "$quit_pid" 2>/dev/null || true
    fi
    kill_pid "$APP_PID" 5 || true
    force_kill_process_names 2 "MetalSharp" "MetalSharp Helper" "MetalSharp Helper (GPU)" "MetalSharp Helper (Renderer)" "MetalSharp Helper (Plugin)"

    write_status "unmounting_old_runtime" 28 "Unmounting stale MetalSharp disk images..."
    unmount_stale_metalsharp_images
}

# Let tests source the updater functions without running the installer.
if [ "${METALSHARP_UPDATE_TEST_SOURCE_ONLY:-0}" = "1" ]; then
    return 0 2>/dev/null || exit 0
fi

run_recovery_update() {
    local repository="metalsharp/MetalSharp"
    local latest_url=""
    local release_tag=""
    local recovery_version=""
    local dmg_name=""
    local installed_app="/Applications/MetalSharp.app"
    local ms_dir="${METALSHARP_HOME:-$HOME/.metalsharp}"
    local existing_dmg=""
    local recovery_dir="$ms_dir/cache/recovery"
    local recovery_dmg=""
    local dmg_path=""
    local partial_path=""
    local download_url=""
    local mount_path=""
    local app_source=""
    local target_version=""
    local current_version=""
    local target_team=""
    local installed_team=""
    local details=""
    local script_path=""
    local app_pid="0"
    local backend_pid="0"

    recovery_fail() {
        if [ -n "$mount_path" ] && [ -d "$mount_path" ]; then
            hdiutil detach "$mount_path" -quiet 2>/dev/null || true
            rmdir "$mount_path" 2>/dev/null || true
        fi
        echo "MetalSharp recovery: $*" >&2
        exit 1
    }

    if [ ! -d "$installed_app" ]; then
        recovery_fail "MetalSharp is not installed at $installed_app"
    fi
    latest_url="$(curl --fail --location --silent --show-error --output /dev/null --write-out '%{url_effective}' \
        "https://github.com/$repository/releases/latest")" || recovery_fail "could not discover the latest stable release"
    case "$latest_url" in
        "https://github.com/$repository/releases/tag/v"*) ;;
        *) recovery_fail "latest-release lookup returned an unexpected URL" ;;
    esac
    release_tag="${latest_url##*/}"
    recovery_version="${release_tag#v}"
    case "$recovery_version" in
        ''|*[!0-9.]*|.*|*.) recovery_fail "latest stable release has an invalid version: $release_tag" ;;
    esac
    dmg_name="MetalSharp-$recovery_version-arm64.dmg"
    existing_dmg="$ms_dir/cache/updates/MetalSharp-$recovery_version.dmg"
    recovery_dmg="$recovery_dir/$dmg_name"
    download_url="https://github.com/$repository/releases/download/$release_tag/$dmg_name"
    mkdir -p "$recovery_dir"

    # Reuse the DMG left by the in-app updater when it is intact. Otherwise
    # fetch the latest official stable release. At the time this rescue script
    # is published that is 0.73.0; once 0.74.0 is released, the same script
    # will repair/install 0.74.0 directly.
    if [ -s "$existing_dmg" ] && hdiutil verify "$existing_dmg" >/dev/null 2>&1; then
        dmg_path="$existing_dmg"
    elif [ -s "$recovery_dmg" ] && hdiutil verify "$recovery_dmg" >/dev/null 2>&1; then
        dmg_path="$recovery_dmg"
    else
        partial_path="$recovery_dmg.part"
        echo "Downloading the official MetalSharp $recovery_version update..."
        if [ -s "$partial_path" ]; then
            if ! curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 --continue-at - \
                --output "$partial_path" "$download_url"; then
                rm -f "$partial_path"
                curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 \
                    --output "$partial_path" "$download_url"
            fi
        else
            curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 \
                --output "$partial_path" "$download_url"
        fi
        mv "$partial_path" "$recovery_dmg"
        hdiutil verify "$recovery_dmg" >/dev/null || recovery_fail "downloaded update image failed verification"
        dmg_path="$recovery_dmg"
    fi

    echo "Verifying the official MetalSharp $recovery_version app signature..."
    mount_path="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-recovery.XXXXXX")"
    hdiutil attach -readonly -nobrowse -mountpoint "$mount_path" "$dmg_path" >/dev/null || \
        recovery_fail "could not mount the update image"
    app_source="$mount_path/MetalSharp.app"
    if [ ! -d "$app_source" ]; then
        for candidate in "$mount_path"/*/MetalSharp.app; do
            if [ -d "$candidate" ]; then app_source="$candidate"; break; fi
        done
    fi
    [ -d "$app_source" ] || recovery_fail "MetalSharp.app not found in the update image"
    target_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app_source/Contents/Info.plist" 2>/dev/null || true)"
    [ "$target_version" = "$recovery_version" ] || recovery_fail "expected v$recovery_version, found ${target_version:-unknown}"
    /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app_source/Contents/Info.plist" 2>/dev/null | grep -qx 'com.metalsharp.app' || \
        recovery_fail "update image is not the MetalSharp app"
    codesign --verify --deep --strict "$app_source" >/dev/null 2>&1 || recovery_fail "update app signature is invalid"
    codesign --verify --deep --strict "$installed_app" >/dev/null 2>&1 || recovery_fail "installed app signature is invalid"
    details="$(codesign -dv --verbose=4 "$app_source" 2>&1)" || recovery_fail "could not read update signature"
    target_team="$(printf '%s\n' "$details" | sed -n 's/^TeamIdentifier=//p' | head -n 1)"
    details="$(codesign -dv --verbose=4 "$installed_app" 2>&1)" || recovery_fail "could not read installed app signature"
    installed_team="$(printf '%s\n' "$details" | sed -n 's/^TeamIdentifier=//p' | head -n 1)"
    [ -n "$target_team" ] && [ "$target_team" = "$installed_team" ] || \
        recovery_fail "update app is not signed by the installed app's Developer ID team"
    current_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$installed_app/Contents/Info.plist" 2>/dev/null || true)"
    [ -n "$current_version" ] || recovery_fail "could not read installed app version"
    if ! awk -v left="$recovery_version" -v right="$current_version" 'BEGIN {
        nl = split(left, l, "."); nr = split(right, r, "."); n = nl > nr ? nl : nr;
        for (i = 1; i <= n; i++) {
            a = l[i] == "" ? 0 : l[i] + 0; b = r[i] == "" ? 0 : r[i] + 0;
            if (a > b) exit 0; if (a < b) exit 1;
        }
        exit 1;
    }'; then
        recovery_fail "v$recovery_version is not newer than the installed v$current_version"
    fi

    hdiutil detach "$mount_path" -quiet || recovery_fail "could not detach validation mount"
    rmdir "$mount_path" 2>/dev/null || true
    mount_path=""
    script_path="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    app_pid="$(pgrep -x MetalSharp | head -n 1 || true)"
    backend_pid="$(pgrep -x metalsharp-backend | head -n 1 || true)"
    app_pid="${app_pid:-0}"
    backend_pid="${backend_pid:-0}"

    /usr/bin/osascript -e "display dialog \"Install MetalSharp $recovery_version over v$current_version? This will close MetalSharp and stop Steam/Wine processes. Save work in running games first.\" buttons {\"Cancel\", \"Install Update\"} default button \"Cancel\" cancel button \"Cancel\" with icon caution" >/dev/null
    echo "Installing MetalSharp $recovery_version over v$current_version; the normal migration handoff will run after relaunch."
    exec /bin/bash "$script_path" \
        --dmg "$dmg_path" \
        --backend-pid "$backend_pid" \
        --target-version "$recovery_version" \
        --status-file "$ms_dir/update_install_status.json" \
        --metalsharp-home "$ms_dir" \
        --app-pid "$app_pid"
}

if [ "${1:-}" = "--recover" ]; then
    shift
    [ "$#" -eq 0 ] || { echo "usage: $0 --recover" >&2; exit 2; }
    run_recovery_update
    exit $?
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dmg) shift; DMG_PATH="${1:-}"; shift ;;
        --backend-pid) shift; BACKEND_PID="${1:-}"; shift ;;
        --target-version) shift; TARGET_VERSION="${1:-}"; shift ;;
        --status-file) shift; STATUS_FILE="${1:-}"; shift ;;
        --metalsharp-home) shift; METALSHARP_HOME_ARG="${1:-}"; shift ;;
        --app-pid) shift; APP_PID="${1:-0}"; shift ;;
        --) shift; break ;;
        *) shift ;;
    esac
done

MS_DIR="${METALSHARP_HOME_ARG:-${METALSHARP_HOME:-$HOME/.metalsharp}}"
STATUS_FILE="${STATUS_FILE:-$MS_DIR/update_install_status.json}"
DMG_PATH="${DMG_PATH:?--dmg required}"
BACKEND_PID="${BACKEND_PID:?--backend-pid required}"
TARGET_VERSION="${TARGET_VERSION:?--target-version required}"
APP_PATH="/Applications/MetalSharp.app"
TMP_APP_PATH="/Applications/.MetalSharp.app.update.$$"
BACKUP_APP_PATH="/Applications/.MetalSharp.app.previous.$$"
MOUNT_POINT=""

detach_mount() {
    if [ -n "$MOUNT_POINT" ] && [ -d "$MOUNT_POINT" ]; then
        hdiutil detach "$MOUNT_POINT" -quiet 2>/dev/null || true
        rmdir "$MOUNT_POINT" 2>/dev/null || true
    fi
}

cleanup() {
    detach_mount
    rm -rf "$TMP_APP_PATH" 2>/dev/null || true
    if [ -d "$BACKUP_APP_PATH" ] && [ -d "$APP_PATH" ]; then
        rm -rf "$BACKUP_APP_PATH" 2>/dev/null || true
    fi
}
trap cleanup EXIT

run_privileged() {
    local command="$1"
    osascript -e "do shell script \"${command//\"/\\\"}\" with administrator privileges" 2>/dev/null
}

cleanup_update_app_backups() {
    local candidate
    local name
    local quoted
    for candidate in /Applications/.MetalSharp.app.previous.* /Applications/.MetalSharp.app.update.*; do
        if [ ! -e "$candidate" ] && [ ! -L "$candidate" ]; then
            continue
        fi
        name="$(basename "$candidate")"
        if [[ ! "$name" =~ ^\.MetalSharp\.app\.(previous|update)\.[0-9]+$ ]]; then
            continue
        fi
        rm -rf "$candidate" 2>/dev/null && continue
        printf -v quoted '%q' "$candidate"
        run_privileged "rm -rf $quoted" || true
    done
}

verify_app_bundle() {
    local app_path="$1"
    for required in \
        "$app_path/Contents/Info.plist" \
        "$app_path/Contents/MacOS/MetalSharp" \
        "$app_path/Contents/Resources/runtime/metalsharp-backend" \
        "$app_path/Contents/Resources/scripts/tools/updater/update.sh"
    do
        if [ ! -s "$required" ]; then
            return 1
        fi
    done
    return 0
}

normalize_version() {
    local value="${1#v}"
    value="${value%%-*}"
    value="${value%%+*}"
    echo "$value" | sed -E 's/[^0-9.].*$//' | sed -E 's/^\.+|\.+$//g'
}

version_gt() {
    local left
    local right
    left="$(normalize_version "$1")"
    right="$(normalize_version "$2")"
    awk -v left="$left" -v right="$right" '
        BEGIN {
            left_len = split(left, l, ".");
            right_len = split(right, r, ".");
            max = (left_len > right_len) ? left_len : right_len;
            for (i = 1; i <= max; i++) {
                a = (l[i] == "") ? 0 : l[i] + 0;
                b = (r[i] == "") ? 0 : r[i] + 0;
                if (a > b) exit 0;
                if (a < b) exit 1;
            }
            exit 1;
        }
    '
}

read_app_version() {
    local app_path="$1"
    /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$app_path/Contents/Info.plist" 2>/dev/null || \
        defaults read "$app_path/Contents/Info" CFBundleShortVersionString 2>/dev/null || true
}

require_app_version() {
    local app_path="$1"
    local label="$2"
    local actual
    actual="$(normalize_version "$(read_app_version "$app_path")")"
    if [ -z "$actual" ]; then
        write_status "error" 50 "$label version could not be read" "app_version_missing"
        exit 1
    fi
    if [ "$actual" != "$TARGET_VERSION_CLEAN" ]; then
        write_status "error" 50 "$label version $actual does not match update target $TARGET_VERSION_CLEAN" "app_version_mismatch"
        exit 1
    fi
}

restore_backup() {
    if [ -d "$BACKUP_APP_PATH" ] && [ ! -d "$APP_PATH" ]; then
        mv "$BACKUP_APP_PATH" "$APP_PATH" 2>/dev/null || \
            run_privileged "mv '$BACKUP_APP_PATH' '$APP_PATH'" || true
    fi
}

TARGET_VERSION_CLEAN="$(normalize_version "$TARGET_VERSION")"
if [ -z "$TARGET_VERSION_CLEAN" ]; then
    write_status "error" 0 "Update target version is invalid: $TARGET_VERSION" "target_version_invalid"
    exit 1
fi

write_status "starting" 0 "Starting update..."

force_stop_old_runtime

write_status "killing_steam" 15 "Stopping Steam and Wine processes..."
for pat in steam steam.exe steamwebhelper steamwebhelper.exe wine wine64 wineserver wineloader; do
    pkill -x "$pat" 2>/dev/null || true
done
for pat in Steam.exe steamwebhelper.exe wineserver wineloader; do
    pkill -f "$pat" 2>/dev/null || true
done
sleep 1

force_stop_old_runtime

write_status "verifying_dmg" 30 "Verifying DMG..."
if [ ! -f "$DMG_PATH" ]; then
    write_status "error" 30 "DMG not found: $DMG_PATH" "dmg_not_found"
    exit 1
fi
if ! hdiutil verify "$DMG_PATH" >/dev/null 2>&1; then
    write_status "error" 30 "DMG failed verification: $DMG_PATH" "dmg_verify_failed"
    exit 1
fi

force_stop_old_runtime

write_status "mounting" 35 "Mounting update disk image..."
MOUNT_POINT="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-update-mount.XXXXXX")" || {
    write_status "error" 40 "Failed to create update mount point" "mount_failed"
    exit 1
}
if ! hdiutil attach -nobrowse -mountpoint "$MOUNT_POINT" "$DMG_PATH" >/dev/null 2>&1; then
    escaped_dmg="${DMG_PATH//\"/\\\"}"
    escaped_mount="${MOUNT_POINT//\"/\\\"}"
    osascript -e "do shell script \"hdiutil attach -nobrowse -mountpoint \\\"$escaped_mount\\\" \\\"$escaped_dmg\\\"\" with administrator privileges" >/dev/null 2>&1 || true
    sleep 2
fi

if [ -z "$MOUNT_POINT" ] || [ ! -d "$MOUNT_POINT" ] || [ -z "$(find "$MOUNT_POINT" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
    detach_mount
    write_status "error" 40 "Failed to mount DMG" "mount_failed"
    exit 1
fi

write_status "mounted" 45 "Mounted at $MOUNT_POINT"

APP_SOURCE=$(find "$MOUNT_POINT" -maxdepth 1 -name "*.app" -iname "*metalsharp*" 2>/dev/null | head -1)
if [ -z "$APP_SOURCE" ]; then
    write_status "error" 50 "MetalSharp.app not found in update" "app_not_found"
    exit 1
fi
if ! verify_app_bundle "$APP_SOURCE"; then
    write_status "error" 50 "Update app bundle is missing required MetalSharp files" "app_bundle_invalid"
    exit 1
fi
require_app_version "$APP_SOURCE" "DMG app"

if [ -d "$APP_PATH" ]; then
    CURRENT_APP_VERSION="$(normalize_version "$(read_app_version "$APP_PATH")")"
    if [ -n "$CURRENT_APP_VERSION" ] && ! version_gt "$TARGET_VERSION_CLEAN" "$CURRENT_APP_VERSION"; then
        write_status "error" 50 "DMG version $TARGET_VERSION_CLEAN is not newer than installed MetalSharp $CURRENT_APP_VERSION" "target_not_newer"
        exit 1
    fi
fi

write_status "installing" 50 "Staging new version..."
rm -rf "$TMP_APP_PATH" "$BACKUP_APP_PATH" 2>/dev/null || true
ditto "$APP_SOURCE" "$TMP_APP_PATH" 2>/dev/null || {
    run_privileged "ditto '$APP_SOURCE' '$TMP_APP_PATH'" || true
    sleep 1
}

if ! verify_app_bundle "$TMP_APP_PATH"; then
    write_status "error" 60 "Failed to stage a valid update app bundle" "stage_failed"
    exit 1
fi
require_app_version "$TMP_APP_PATH" "Staged app"

write_status "installing" 65 "Installing new version..."
if [ -d "$APP_PATH" ]; then
    mv "$APP_PATH" "$BACKUP_APP_PATH" 2>/dev/null || {
        run_privileged "mv '$APP_PATH' '$BACKUP_APP_PATH'" || true
        sleep 1
    }
    if [ -d "$APP_PATH" ]; then
        write_status "error" 68 "Failed to move the old app out of the way" "remove_failed"
        exit 1
    fi
fi

mv "$TMP_APP_PATH" "$APP_PATH" 2>/dev/null || {
    run_privileged "mv '$TMP_APP_PATH' '$APP_PATH'" || true
    sleep 1
}

if [ -d "$TMP_APP_PATH" ] || ! verify_app_bundle "$APP_PATH"; then
    rm -rf "$APP_PATH" 2>/dev/null || true
    restore_backup
    write_status "error" 70 "Failed to install a valid new version" "copy_failed"
    exit 1
fi
require_app_version "$APP_PATH" "Installed app"

rm -rf "$BACKUP_APP_PATH" 2>/dev/null || true
cleanup_update_app_backups
write_status "installed" 80 "New version installed"

mkdir -p "$MS_DIR"
printf '{"needed":true,"target_version":"%s","timestamp":%s}\n' "$TARGET_VERSION" "$(date +%s)" > "$MS_DIR/.post-update-migration" 2>/dev/null || true

write_status "unmounting" 82 "Unmounting update disk..."
detach_mount
MOUNT_POINT=""

write_status "relaunching" 85 "Launching MetalSharp v$TARGET_VERSION_CLEAN..."
sleep 1
open -n "$APP_PATH"

write_status "verifying" 90 "Verifying installation..."
sleep 5

BACKEND_VERSION=""
deadline=$((SECONDS + 45))
while [ $SECONDS -lt $deadline ]; do
    RAW=$(curl -sf "http://127.0.0.1:9274/status" 2>/dev/null) || true
    BACKEND_VERSION=$(echo "$RAW" | grep -o '"version":"[^"]*"' | head -1 | cut -d'"' -f4 || true)
    [ -n "$BACKEND_VERSION" ] && break
    sleep 1
done

if [ "$BACKEND_VERSION" != "$TARGET_VERSION_CLEAN" ] && [ -n "$BACKEND_VERSION" ]; then
    write_status "error" 92 "Launched backend reported v$BACKEND_VERSION instead of v$TARGET_VERSION_CLEAN" "backend_version_mismatch"
    pkill -x metalsharp-backend 2>/dev/null || true
    exit 1
fi

if [ "$BACKEND_VERSION" = "$TARGET_VERSION_CLEAN" ]; then
    write_status "complete" 100 "Update installed. Opening migration wizard..."
else
    write_status "error" 95 "Update installed, but backend reported v${BACKEND_VERSION:-?} instead of v${TARGET_VERSION_CLEAN}" "backend_version_mismatch"
fi
