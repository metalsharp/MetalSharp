#!/bin/bash
set -euo pipefail

DMG_PATH=""
BACKEND_PID=""
TARGET_VERSION=""
STATUS_FILE=""
METALSHARP_HOME_ARG=""
APP_PID="0"
# The build being installed: --variant regular|fex, else from the DMG's name.
TARGET_VARIANT_ARG=""
TARGET_VARIANT=""
# --reinstall 1: the same version of the installed build may go back over it (the FEX build's --recover).
TARGET_REINSTALL="0"

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

# The first stop reports progress. Later sweeps catch anything that respawned
# while Steam/Wine shut down or the DMG verified; they stay silent so the
# progress the app polls never jumps back to an earlier step.
force_stop_old_runtime() {
    local report="${1:-report}"
    [ "$report" = "quiet" ] || write_status "stopping_old_runtime" 24 "Force-stopping the old MetalSharp app and backend..."
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

    [ "$report" = "quiet" ] || write_status "unmounting_old_runtime" 26 "Unmounting stale MetalSharp disk images..."
    unmount_stale_metalsharp_images
}

stop_steam_and_wine() {
    for pat in steam steam.exe steamwebhelper steamwebhelper.exe wine wine64 wineserver wineloader; do
        pkill -x "$pat" 2>/dev/null || true
    done
    for pat in Steam.exe steamwebhelper.exe wineserver wineloader; do
        pkill -f "$pat" 2>/dev/null || true
    done
    sleep 1
}

# Recovery reports 2-20% before it hands off here, so installation progress
# starts above it and only moves forward.
prepare_install() {
    write_status "starting" 22 "Starting update..."
    force_stop_old_runtime
    write_status "killing_steam" 28 "Stopping Steam and Wine processes..."
    stop_steam_and_wine
    force_stop_old_runtime quiet

    write_status "verifying_dmg" 30 "Verifying DMG..."
    if [ ! -f "$DMG_PATH" ]; then
        write_status "error" 30 "DMG not found: $DMG_PATH" "dmg_not_found"
        exit 1
    fi
    if ! hdiutil verify "$DMG_PATH" >/dev/null 2>&1; then
        write_status "error" 30 "DMG failed verification: $DMG_PATH" "dmg_verify_failed"
        exit 1
    fi
    force_stop_old_runtime quiet
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

# The build an app bundle is, from its Info.plist MetalSharpVariant: "regular"
# or "fex". Prints nothing when the key is missing (apps from before the FEX
# build, which are all the regular build) or holds anything else.
read_app_variant() {
    local value
    value="$(/usr/libexec/PlistBuddy -c "Print :MetalSharpVariant" "$1/Contents/Info.plist" 2>/dev/null || true)"
    value="$(printf '%s' "$value" | tr '[:upper:]' '[:lower:]')"
    case "$value" in
        regular|fex) echo "$value" ;;
        *) echo "" ;;
    esac
}

# The installed build: its MetalSharpVariant, else regular.
installed_app_variant() {
    local value
    value="$(read_app_variant "$1")"
    echo "${value:-regular}"
}

# The build a DMG holds, from its file name, the same rule the backend uses for
# release assets: a name with "FEX" in it (MetalSharp-1.0.0-arm64-FEX.dmg, the
# in-app download MetalSharp-1.0.0-FEX.dmg) is the FEX build; any other
# MetalSharp DMG (MetalSharp-1.0.0-arm64.dmg, MetalSharp-1.0.0.dmg) is regular.
dmg_name_variant() {
    case "$(basename "$1" | tr '[:upper:]' '[:lower:]')" in
        *fex*) echo "fex" ;;
        *) echo "regular" ;;
    esac
}

# Whether the target <version> <variant> may replace the installed <version>
# <variant> [reinstall]: a newer version always may; the same version as a
# switch between the regular and FEX builds, or, with reinstall=1, as a repair
# of the same build; an older version never. An installed app whose version
# can't be read doesn't block the install.
install_target_allowed() {
    local target_version="$1" target_variant="$2" current_version="$3" current_variant="$4" reinstall="${5:-0}"
    [ -n "$current_version" ] || return 0
    version_gt "$target_version" "$current_version" && return 0
    version_gt "$current_version" "$target_version" && return 1
    [ "$target_variant" != "$current_variant" ] || [ "$reinstall" = "1" ]
}

# The DMG --recover installs for the installed build <variant> at <version>:
# sets RECOVERY_VARIANT, RECOVERY_DMG_NAME (the release asset) and
# RECOVERY_CACHED_NAME (the in-app updater's download of it). The regular
# build's are MetalSharp-<ver>-arm64.dmg and MetalSharp-<ver>.dmg; the FEX
# build reinstalls the FEX build, MetalSharp-<ver>-arm64-FEX.dmg and
# MetalSharp-<ver>-FEX.dmg, which carries its MetalSharp Wine.
recovery_assets() {
    local variant="$1" version="$2"
    if [ "$variant" = "fex" ]; then
        RECOVERY_VARIANT="fex"
        RECOVERY_DMG_NAME="MetalSharp-$version-arm64-FEX.dmg"
        RECOVERY_CACHED_NAME="MetalSharp-$version-FEX.dmg"
    else
        RECOVERY_VARIANT="regular"
        RECOVERY_DMG_NAME="MetalSharp-$version-arm64.dmg"
        RECOVERY_CACHED_NAME="MetalSharp-$version.dmg"
    fi
}

# The Developer ID check --recover and FEX installs share: both apps pass a
# strict deep signature check and the update is signed by the installed app's
# team. Without an installed app only the update's own signature is checked.
# On failure prints the reason and returns 1.
verify_update_signature() {
    local target_app="$1" installed_app="$2" details target_team installed_team
    if ! codesign --verify --deep --strict "$target_app" >/dev/null 2>&1; then
        echo "update app signature is invalid"
        return 1
    fi
    if ! details="$(codesign -dv --verbose=4 "$target_app" 2>&1)"; then
        echo "could not read update signature"
        return 1
    fi
    target_team="$(printf '%s\n' "$details" | sed -n 's/^TeamIdentifier=//p' | head -n 1)"
    if [ -z "$target_team" ] || [ "$target_team" = "not set" ]; then
        echo "update app is not signed with a Developer ID team"
        return 1
    fi
    [ -n "$installed_app" ] && [ -d "$installed_app" ] || return 0
    if ! codesign --verify --deep --strict "$installed_app" >/dev/null 2>&1; then
        echo "installed app signature is invalid"
        return 1
    fi
    if ! details="$(codesign -dv --verbose=4 "$installed_app" 2>&1)"; then
        echo "could not read installed app signature"
        return 1
    fi
    installed_team="$(printf '%s\n' "$details" | sed -n 's/^TeamIdentifier=//p' | head -n 1)"
    if [ "$target_team" != "$installed_team" ]; then
        echo "update app is not signed by the installed app's Developer ID team"
        return 1
    fi
    return 0
}

# Sets TARGET_VARIANT from --variant, else from the DMG's file name.
resolve_target_variant() {
    case "$TARGET_VARIANT_ARG" in
        regular|fex) TARGET_VARIANT="$TARGET_VARIANT_ARG" ;;
        '') TARGET_VARIANT="$(dmg_name_variant "$DMG_PATH")" ;;
        *)
            write_status "error" 0 "Update variant must be regular or fex: $TARGET_VARIANT_ARG" "variant_invalid"
            return 1
            ;;
    esac
}

# Whether the mounted <app> may replace the installed <app> (TARGET_VERSION_CLEAN,
# TARGET_VARIANT): the app holds the build being installed, it is newer or the
# same version of the other build, and the FEX build (or a switch away from
# it) carries the bundle id and the installed app's Developer ID team, the
# checks --recover gives the regular build. Sets CURRENT_APP_VERSION and
# CURRENT_VARIANT; on refusal writes the error status and returns 1.
check_install_target() {
    local app_source="$1" app_path="$2" dmg_app_variant signature_error
    dmg_app_variant="$(read_app_variant "$app_source")"
    if [ -n "$dmg_app_variant" ] && [ "$dmg_app_variant" != "$TARGET_VARIANT" ]; then
        write_status "error" 50 "DMG holds the $dmg_app_variant build of MetalSharp, not the $TARGET_VARIANT build" "variant_mismatch"
        return 1
    fi

    CURRENT_APP_VERSION=""
    CURRENT_VARIANT="regular"
    if [ -d "$app_path" ]; then
        CURRENT_APP_VERSION="$(normalize_version "$(read_app_version "$app_path")")"
        CURRENT_VARIANT="$(installed_app_variant "$app_path")"
    fi
    if ! install_target_allowed "$TARGET_VERSION_CLEAN" "$TARGET_VARIANT" "$CURRENT_APP_VERSION" "$CURRENT_VARIANT" \
        "$TARGET_REINSTALL"; then
        if version_gt "$CURRENT_APP_VERSION" "$TARGET_VERSION_CLEAN"; then
            write_status "error" 50 "DMG version $TARGET_VERSION_CLEAN is older than installed MetalSharp $CURRENT_APP_VERSION" "target_not_newer"
        else
            write_status "error" 50 "DMG version $TARGET_VERSION_CLEAN is not newer than installed MetalSharp $CURRENT_APP_VERSION" "target_not_newer"
        fi
        return 1
    fi

    if [ "$TARGET_VARIANT" = "fex" ] || [ "$CURRENT_VARIANT" = "fex" ]; then
        write_status "verifying_signature" 48 "Verifying the update's Developer ID signature..."
        if ! /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$app_source/Contents/Info.plist" 2>/dev/null | grep -qx 'com.metalsharp.app'; then
            write_status "error" 50 "Update image is not the MetalSharp app" "app_bundle_invalid"
            return 1
        fi
        if ! signature_error="$(verify_update_signature "$app_source" "$app_path")"; then
            write_status "error" 50 "$signature_error" "signature_invalid"
            return 1
        fi
    fi
    return 0
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
    local target_variant=""
    local current_variant=""
    local reason=""
    local reinstall="0"
    local script_path=""
    local app_pid="0"
    local backend_pid="0"

    STATUS_FILE="$ms_dir/update_install_status.json"
    TARGET_VERSION="latest"
    DMG_PATH=""
    write_status "recovery_preparing" 2 "Preparing the verified update..."

    recovery_fail() {
        if [ -n "$mount_path" ] && [ -d "$mount_path" ]; then
            hdiutil detach "$mount_path" -quiet 2>/dev/null || true
            rmdir "$mount_path" 2>/dev/null || true
        fi
        write_status "error" 0 "$*" "recovery_failed"
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
    TARGET_VERSION="$recovery_version"
    write_status "recovery_verifying" 4 "Checking the latest official release..."
    # Recovery installs the installed build again: the regular build's asset is
    # exactly MetalSharp-<ver>-arm64.dmg (cached by the in-app updater as
    # MetalSharp-<ver>.dmg), the FEX build's MetalSharp-<ver>-arm64-FEX.dmg
    # (MetalSharp-<ver>-FEX.dmg), never one for the other, and the app inside is
    # checked for its build below. The FEX build may reinstall its own version.
    current_variant="$(installed_app_variant "$installed_app")"
    recovery_assets "$current_variant" "$recovery_version"
    reinstall="0"
    [ "$RECOVERY_VARIANT" != "fex" ] || reinstall="1"
    dmg_name="$RECOVERY_DMG_NAME"
    existing_dmg="$ms_dir/cache/updates/$RECOVERY_CACHED_NAME"
    recovery_dmg="$recovery_dir/$dmg_name"
    download_url="https://github.com/$repository/releases/download/$release_tag/$dmg_name"
    mkdir -p "$recovery_dir" || recovery_fail "could not create recovery cache"

    # Reuse the DMG left by the in-app updater when it is intact. Otherwise
    # fetch the latest official stable release. At the time this rescue script
    # is published that is 0.79.0; once 0.80.0 is released, the same script
    # will repair/install 0.80.0 directly.
    if [ -s "$existing_dmg" ] && hdiutil verify "$existing_dmg" >/dev/null 2>&1; then
        dmg_path="$existing_dmg"
    elif [ -s "$recovery_dmg" ] && hdiutil verify "$recovery_dmg" >/dev/null 2>&1; then
        dmg_path="$recovery_dmg"
    else
        partial_path="$recovery_dmg.part"
        write_status "recovery_downloading" 8 "Downloading the official MetalSharp $recovery_version update..."
        echo "Downloading the official MetalSharp $recovery_version update..."
        if [ -s "$partial_path" ]; then
            if ! curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 --continue-at - \
                --output "$partial_path" "$download_url"; then
                rm -f "$partial_path"
                curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 \
                    --output "$partial_path" "$download_url" || recovery_fail "could not download the official update"
            fi
        else
            curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 \
                --output "$partial_path" "$download_url" || recovery_fail "could not download the official update"
        fi
        mv "$partial_path" "$recovery_dmg"
        hdiutil verify "$recovery_dmg" >/dev/null || recovery_fail "downloaded update image failed verification"
        dmg_path="$recovery_dmg"
    fi

    DMG_PATH="$dmg_path"
    write_status "recovery_verifying" 15 "Verifying the downloaded update and app signature..."
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
    target_variant="$(read_app_variant "$app_source")"
    [ "${target_variant:-regular}" = "$RECOVERY_VARIANT" ] || \
        recovery_fail "update image holds the ${target_variant:-regular} build, not the $RECOVERY_VARIANT build"
    target_variant="$RECOVERY_VARIANT"
    reason="$(verify_update_signature "$app_source" "$installed_app")" || recovery_fail "$reason"
    current_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$installed_app/Contents/Info.plist" 2>/dev/null || true)"
    [ -n "$current_version" ] || recovery_fail "could not read installed app version"
    # The same version is allowed as a reinstall of the FEX build.
    if ! install_target_allowed "$(normalize_version "$recovery_version")" "$target_variant" \
        "$(normalize_version "$current_version")" "$current_variant" "$reinstall"; then
        if version_gt "$(normalize_version "$current_version")" "$(normalize_version "$recovery_version")"; then
            recovery_fail "v$recovery_version is older than the installed v$current_version"
        fi
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

    write_status "awaiting_confirmation" 20 "Waiting for confirmation to close MetalSharp and Steam/Wine processes..."
    local prompt="Install MetalSharp $recovery_version over v$current_version?"
    if [ "$current_variant" != "$target_variant" ]; then
        prompt="Switch from the FEX build of MetalSharp v$current_version to the regular build of MetalSharp $recovery_version?"
    elif [ "$target_variant" = "fex" ]; then
        prompt="Reinstall the Arm64 FEX build of MetalSharp $recovery_version, with its MetalSharp Wine, over v$current_version?"
    fi
    if ! /usr/bin/osascript -e "display dialog \"$prompt This will close MetalSharp and stop Steam/Wine processes. Save work in running games first.\" buttons {\"Cancel\", \"Install Update\"} default button \"Cancel\" cancel button \"Cancel\" with icon caution" >/dev/null; then
        recovery_fail "update cancelled"
    fi
    echo "Installing MetalSharp $recovery_version over v$current_version; the normal migration handoff will run after relaunch."
    exec /bin/bash "$script_path" \
        --dmg "$dmg_path" \
        --backend-pid "$backend_pid" \
        --target-version "$recovery_version" \
        --variant "$target_variant" \
        --reinstall "$reinstall" \
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
        --variant) shift; TARGET_VARIANT_ARG="${1:-}"; shift ;;
        --reinstall) shift; TARGET_REINSTALL="${1:-0}"; shift ;;
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
resolve_target_variant || exit 1

prepare_install

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

check_install_target "$APP_SOURCE" "$APP_PATH" || exit 1

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
printf '{"needed":true,"target_version":"%s","target_variant":"%s","previous_variant":"%s","timestamp":%s}\n' "$TARGET_VERSION" "$TARGET_VARIANT" "$CURRENT_VARIANT" "$(date +%s)" > "$MS_DIR/.post-update-migration" 2>/dev/null || true

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
    RAW=$(curl -sf --unix-socket "$MS_DIR/run/backend.sock" "http://localhost/status" 2>/dev/null) || true
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
