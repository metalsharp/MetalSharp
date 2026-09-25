#!/bin/bash
set -euo pipefail

APP_PATH="/Applications/MetalSharp.app"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="$SCRIPT_DIR/recovery-config.plist"
CHECK_ONLY=0
DMG_PATH=""
AUTO_SELECT_DMG=1
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check) CHECK_ONLY=1; shift ;;
        --dmg) shift; DMG_PATH="${1:-}"; AUTO_SELECT_DMG=0; shift ;;
        *) echo "usage: $0 [--check] [--dmg /path/to/MetalSharp-update.dmg]" >&2; exit 2 ;;
    esac
done

[ -s "$CONFIG_FILE" ] || { echo "Recovery release configuration is missing." >&2; exit 1; }
RECOVERY_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :RecoveryVersion' "$CONFIG_FILE")"
REPOSITORY="$(/usr/libexec/PlistBuddy -c 'Print :Repository' "$CONFIG_FILE")"
DMG_NAME="$(/usr/libexec/PlistBuddy -c 'Print :DmgName' "$CONFIG_FILE")"
case "$RECOVERY_VERSION" in ''|*[!0-9.]*|.*|*.) echo "Invalid recovery release version." >&2; exit 1 ;; esac
case "$REPOSITORY" in
    */*) REPOSITORY_OWNER="${REPOSITORY%%/*}"; REPOSITORY_NAME="${REPOSITORY#*/}" ;;
    *) echo "Invalid repository in recovery configuration." >&2; exit 1 ;;
esac
case "$REPOSITORY" in */*/*|/*|*/|"") echo "Invalid repository in recovery configuration." >&2; exit 1 ;; esac
case "$REPOSITORY_OWNER$REPOSITORY_NAME" in *[!A-Za-z0-9_.-]*|"") echo "Invalid repository in recovery configuration." >&2; exit 1 ;; esac
case "$DMG_NAME" in ''|*/*|*[!A-Za-z0-9._-]*) echo "Invalid disk image name in recovery configuration." >&2; exit 1 ;; esac

if [ ! -d "$APP_PATH" ]; then
    echo "MetalSharp is not installed at $APP_PATH; use the regular installer." >&2
    exit 1
fi

MS_DIR="${METALSHARP_HOME:-$HOME/.metalsharp}"
if [ -z "$DMG_PATH" ]; then
    EXISTING_DMG="$MS_DIR/cache/updates/MetalSharp-$RECOVERY_VERSION.dmg"
    CACHE_DIR="$MS_DIR/cache/recovery"
    DMG_PATH="$CACHE_DIR/$DMG_NAME"
    if [ -s "$EXISTING_DMG" ]; then
        DMG_PATH="$EXISTING_DMG"
    elif [ ! -s "$DMG_PATH" ]; then
        mkdir -p "$CACHE_DIR"
        partial_path="$DMG_PATH.part"
        download_url="https://github.com/$REPOSITORY/releases/download/v$RECOVERY_VERSION/$DMG_NAME"
        echo "Downloading the signed MetalSharp $RECOVERY_VERSION update..."
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
        mv "$partial_path" "$DMG_PATH"
    fi
fi
[ -s "$DMG_PATH" ] || { echo "Update DMG is missing or empty: $DMG_PATH" >&2; exit 1; }

MOUNT_POINT=""
cleanup() {
    if [ -n "$MOUNT_POINT" ] && [ -d "$MOUNT_POINT" ]; then
        hdiutil detach "$MOUNT_POINT" -quiet 2>/dev/null || true
        rmdir "$MOUNT_POINT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

fail() {
    echo "MetalSharp recovery: $*" >&2
    exit 1
}

read_bundle_value() {
    /usr/libexec/PlistBuddy -c "Print :$2" "$1/Contents/Info.plist" 2>/dev/null || true
}

team_identifier() {
    local details
    details="$(codesign -dv --verbose=4 "$1" 2>&1)" || return 1
    printf '%s\n' "$details" | sed -n 's/^TeamIdentifier=//p' | head -n 1
}

version_gt() {
    awk -v left="$1" -v right="$2" '
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

echo "Verifying update image..."
if ! hdiutil verify "$DMG_PATH" >/dev/null 2>&1; then
    [ "$AUTO_SELECT_DMG" -eq 1 ] || fail "the update disk image failed verification"
    echo "Cached update image is invalid; downloading a fresh official copy..." >&2
    CACHE_DIR="$MS_DIR/cache/recovery"
    mkdir -p "$CACHE_DIR"
    DMG_PATH="$CACHE_DIR/$DMG_NAME"
    partial_path="$DMG_PATH.part"
    download_url="https://github.com/$REPOSITORY/releases/download/v$RECOVERY_VERSION/$DMG_NAME"
    rm -f "$DMG_PATH" "$partial_path"
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 \
        --output "$partial_path" "$download_url"
    mv "$partial_path" "$DMG_PATH"
    hdiutil verify "$DMG_PATH" >/dev/null || fail "the freshly downloaded update disk image failed verification"
fi
MOUNT_POINT="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-recovery.XXXXXX")"
hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT_POINT" "$DMG_PATH" >/dev/null || fail "could not mount the update disk image"

APP_SOURCE="$MOUNT_POINT/MetalSharp.app"
if [ ! -d "$APP_SOURCE" ]; then
    for candidate in "$MOUNT_POINT"/*/MetalSharp.app; do
        if [ -d "$candidate" ]; then
            APP_SOURCE="$candidate"
            break
        fi
    done
fi
[ -d "$APP_SOURCE" ] || fail "MetalSharp.app was not found in the update image"

BUNDLE_ID="$(read_bundle_value "$APP_SOURCE" CFBundleIdentifier)"
[ "$BUNDLE_ID" = "com.metalsharp.app" ] || fail "the update image does not contain the expected MetalSharp app"
TARGET_VERSION="$(read_bundle_value "$APP_SOURCE" CFBundleShortVersionString)"
[ "$TARGET_VERSION" = "$RECOVERY_VERSION" ] || fail "expected MetalSharp $RECOVERY_VERSION, found ${TARGET_VERSION:-unknown}"
codesign --verify --deep --strict "$APP_SOURCE" >/dev/null 2>&1 || fail "the update app's code signature is invalid"
codesign --verify --deep --strict "$APP_PATH" >/dev/null 2>&1 || fail "the installed app's code signature is invalid"
TARGET_TEAM="$(team_identifier "$APP_SOURCE")"
INSTALLED_TEAM="$(team_identifier "$APP_PATH")"
[ -n "$TARGET_TEAM" ] || fail "the update app is not Developer ID signed"
[ "$TARGET_TEAM" = "$INSTALLED_TEAM" ] || fail "the update app is signed by a different Developer ID team"

CURRENT_VERSION="$(read_bundle_value "$APP_PATH" CFBundleShortVersionString)"
[ -n "$CURRENT_VERSION" ] || fail "could not read the installed app version"
if [ "$CHECK_ONLY" -eq 0 ] && ! version_gt "$TARGET_VERSION" "$CURRENT_VERSION"; then
    fail "update version $TARGET_VERSION is not newer than installed version $CURRENT_VERSION"
fi

UPDATER_SCRIPT="$SCRIPT_DIR/update.sh"
[ -s "$UPDATER_SCRIPT" ] || fail "the signed recovery assistant does not contain its installer"

hdiutil detach "$MOUNT_POINT" -quiet || fail "could not detach the validation mount"
rmdir "$MOUNT_POINT" 2>/dev/null || true
MOUNT_POINT=""

echo "Validated signed MetalSharp $TARGET_VERSION update for installed version $CURRENT_VERSION (Developer ID team $TARGET_TEAM)."
if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "Validation only; no applications or user data were changed."
    exit 0
fi

/usr/bin/osascript -e "display dialog \"Install MetalSharp $TARGET_VERSION? This will close MetalSharp and stop Steam/Wine processes. Save work in running games first.\" buttons {\"Cancel\", \"Install Update\"} default button \"Cancel\" cancel button \"Cancel\" with icon caution" >/dev/null

APP_PID="$(pgrep -x MetalSharp | head -n 1 || true)"
APP_PID="${APP_PID:-0}"
BACKEND_PID="$(pgrep -x metalsharp-backend | head -n 1 || true)"
BACKEND_PID="${BACKEND_PID:-0}"
STATUS_FILE="$MS_DIR/update_install_status.json"

/bin/bash "$UPDATER_SCRIPT" \
    --dmg "$DMG_PATH" \
    --backend-pid "$BACKEND_PID" \
    --target-version "$TARGET_VERSION" \
    --status-file "$STATUS_FILE" \
    --metalsharp-home "$MS_DIR" \
    --app-pid "$APP_PID"

echo "MetalSharp $TARGET_VERSION recovery installation completed."
