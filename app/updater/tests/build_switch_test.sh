#!/bin/bash
# update.sh: which target builds may replace the installed app (newer, or the
# same version of the other build), how a DMG's build is told, and the
# Developer ID check the FEX build gets.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/update.sh" >&2
    exit 2
fi

export METALSHARP_UPDATE_TEST_SOURCE_ONLY=1
# shellcheck source=../update.sh
source "$1"
unset METALSHARP_UPDATE_TEST_SOURCE_ONLY

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STATUS_LOG="$WORK/status.log"
write_status() { printf '%s|%s|%s\n' "$1" "$2" "${4:-}" >> "$STATUS_LOG"; }
last_error() { tail -n 1 "$STATUS_LOG" | cut -d'|' -f3; }

fail() { echo "build switch test failed: $*" >&2; exit 1; }
allowed() { install_target_allowed "$@" || fail "expected allowed: $*"; }
refused() { ! install_target_allowed "$@" || fail "expected refused: $*"; }

# A fake MetalSharp.app: <dir> <version> [variant] [bundle id]
make_app() {
    local app="$1" version="$2" variant="${3:-}" bundle="${4:-com.metalsharp.app}" variant_xml=""
    mkdir -p "$app/Contents"
    [ -z "$variant" ] || variant_xml="<key>MetalSharpVariant</key><string>$variant</string>"
    cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>$bundle</string>
<key>CFBundleShortVersionString</key><string>$version</string>
$variant_xml
</dict></plist>
PLIST
}

# codesign stub: apps under $WORK/<team>/ are signed by that team; "unsigned"
# fails verification and "noteam" verifies without a TeamIdentifier.
codesign() {
    local app="${!#}" team
    team="$(basename "$(dirname "$app")")"
    case "$1" in
        --verify) [ "$team" != "unsigned" ] ;;
        -dv)
            echo "Identifier=com.metalsharp.app" >&2
            [ "$team" = "noteam" ] || echo "TeamIdentifier=$team" >&2
            ;;
    esac
}

# --- the version rule ---------------------------------------------------------
allowed 1.0.1 regular 1.0.0 regular     # newer, same build
allowed 1.0.1 fex 1.0.0 fex
allowed 1.0.1 fex 1.0.0 regular         # newer and a switch
allowed 1.0.0 fex 1.0.0 regular         # same version: a switch is allowed
allowed 1.0.0 regular 1.0.0 fex
allowed 1.0.0 fex "" regular            # unreadable installed version
refused 1.0.0 regular 1.0.0 regular     # same version, same build
refused 1.0.0 fex 1.0.0 fex
refused 0.80.0 regular 1.0.0 regular    # downgrades, any build
refused 0.80.0 fex 1.0.0 regular
refused 0.80.0 regular 1.0.0 fex
refused 1.0 fex 1.0.0 fex               # 1.0 == 1.0.0

# --- telling the builds apart -------------------------------------------------
for name in MetalSharp-1.0.0-arm64.dmg MetalSharp-1.0.0.dmg /cache/updates/MetalSharp-0.80.0.dmg; do
    [ "$(dmg_name_variant "$name")" = "regular" ] || fail "$name is the regular build"
done
for name in MetalSharp-1.0.0-arm64-FEX.dmg MetalSharp-1.0.0-FEX.dmg /cache/updates/MetalSharp-1.0.0-fex.dmg; do
    [ "$(dmg_name_variant "$name")" = "fex" ] || fail "$name is the FEX build"
done
make_app "$WORK/plain/MetalSharp.app" 0.80.0
make_app "$WORK/fexkey/MetalSharp.app" 1.0.0 FEX
make_app "$WORK/oddkey/MetalSharp.app" 1.0.0 preview
[ -z "$(read_app_variant "$WORK/plain/MetalSharp.app")" ] || fail "no MetalSharpVariant reads as unknown"
[ "$(installed_app_variant "$WORK/plain/MetalSharp.app")" = "regular" ] || fail "an app without the key is regular"
[ "$(read_app_variant "$WORK/fexkey/MetalSharp.app")" = "fex" ] || fail "MetalSharpVariant FEX reads as fex"
[ "$(installed_app_variant "$WORK/oddkey/MetalSharp.app")" = "regular" ] || fail "an unknown key is regular"

TARGET_VARIANT_ARG="" DMG_PATH="$WORK/MetalSharp-1.0.0-FEX.dmg"
resolve_target_variant && [ "$TARGET_VARIANT" = "fex" ] || fail "an old invocation with a FEX DMG installs fex"
TARGET_VARIANT_ARG="" DMG_PATH="$WORK/MetalSharp-1.0.0.dmg"
resolve_target_variant && [ "$TARGET_VARIANT" = "regular" ] || fail "an old invocation with a regular DMG installs regular"
TARGET_VARIANT_ARG="fex"
resolve_target_variant && [ "$TARGET_VARIANT" = "fex" ] || fail "--variant fex wins over the name"
TARGET_VARIANT_ARG="preview"
! resolve_target_variant || fail "--variant preview is refused"
[ "$(last_error)" = "variant_invalid" ] || fail "--variant preview reports variant_invalid"
TARGET_VARIANT_ARG=""

# --- the Developer ID check ---------------------------------------------------
make_app "$WORK/TEAM1/MetalSharp.app" 1.0.0 regular
make_app "$WORK/TEAM2/MetalSharp.app" 1.0.0 fex
make_app "$WORK/unsigned/MetalSharp.app" 1.0.0 fex
make_app "$WORK/noteam/MetalSharp.app" 1.0.0 fex
verify_update_signature "$WORK/TEAM1/MetalSharp.app" "$WORK/TEAM1/MetalSharp.app" >/dev/null || fail "same team passes"
reason="$(verify_update_signature "$WORK/TEAM2/MetalSharp.app" "$WORK/TEAM1/MetalSharp.app")" && fail "another team fails"
[ "$reason" = "update app is not signed by the installed app's Developer ID team" ] || fail "team reason: $reason"
reason="$(verify_update_signature "$WORK/unsigned/MetalSharp.app" "$WORK/TEAM1/MetalSharp.app")" && fail "a bad signature fails"
[ "$reason" = "update app signature is invalid" ] || fail "signature reason: $reason"
reason="$(verify_update_signature "$WORK/noteam/MetalSharp.app" "$WORK/TEAM1/MetalSharp.app")" && fail "no team fails"
reason="$(verify_update_signature "$WORK/TEAM1/MetalSharp.app" "$WORK/unsigned/MetalSharp.app")" && fail "a bad installed signature fails"
[ "$reason" = "installed app signature is invalid" ] || fail "installed reason: $reason"
verify_update_signature "$WORK/TEAM2/MetalSharp.app" "$WORK/missing/MetalSharp.app" >/dev/null || fail "no installed app checks the update only"

# --- the install decision, as the installer runs it ---------------------------
# check_install_target <dmg app> <installed app> <target version> <target variant>
check() {
    TARGET_VERSION_CLEAN="$3" TARGET_VARIANT="$4"
    : > "$STATUS_LOG"
    check_install_target "$1" "$2"
}
mkdir -p "$WORK/case"
n=0
# setup <installed version> <installed variant|-> <target version> <target variant|-> [target team]
setup() {
    n=$((n + 1))
    local dir="$WORK/case/$n" installed_variant="$2" target_variant="$4" team="${5:-TEAMA}"
    [ "$installed_variant" != "-" ] || installed_variant=""
    [ "$target_variant" != "-" ] || target_variant=""
    make_app "$dir/TEAMA/MetalSharp.app" "$1" "$installed_variant"
    make_app "$dir/$team/target/MetalSharp.app" "$3" "$target_variant"
    INSTALLED_APP="$dir/TEAMA/MetalSharp.app"
    TARGET_APP="$dir/$team/target/MetalSharp.app"
}
# the stub reads the team from the app's parent directory
codesign() {
    local app="${!#}" team
    case "$app" in
        */target/MetalSharp.app) team="$(basename "$(dirname "$(dirname "$app")")")" ;;
        *) team="$(basename "$(dirname "$app")")" ;;
    esac
    case "$1" in
        --verify) [ "$team" != "unsigned" ] ;;
        -dv) [ "$team" = "noteam" ] || echo "TeamIdentifier=$team" >&2 ;;
    esac
}

# regular is unchanged: newer installs, same version and older are target_not_newer
setup 0.80.0 - 1.0.0 -
check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 regular || fail "regular newer installs"
! grep -q verifying_signature "$STATUS_LOG" || fail "regular to regular skips the signature step as before"
setup 1.0.0 - 1.0.0 -
! check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 regular || fail "regular same version is refused"
[ "$(last_error)" = "target_not_newer" ] || fail "regular same version reports target_not_newer"
setup 1.0.0 regular 0.80.0 regular
! check "$TARGET_APP" "$INSTALLED_APP" 0.80.0 regular || fail "regular downgrade is refused"
[ "$(last_error)" = "target_not_newer" ] || fail "regular downgrade reports target_not_newer"

# same version switches, both ways, with the team check
setup 1.0.0 - 1.0.0 fex
check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 fex || fail "regular to FEX at the same version installs"
grep -q verifying_signature "$STATUS_LOG" || fail "the FEX install verifies the signature"
[ "$CURRENT_VARIANT" = "regular" ] || fail "an installed app without the key is regular"
setup 1.0.0 fex 1.0.0 regular
check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 regular || fail "FEX to regular at the same version installs"
grep -q verifying_signature "$STATUS_LOG" || fail "a switch away from FEX verifies the signature"
setup 1.0.0 fex 1.0.0 -
check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 regular || fail "a regular DMG without the key switches from FEX"
setup 1.0.0 fex 1.1.0 regular
check "$TARGET_APP" "$INSTALLED_APP" 1.1.0 regular || fail "FEX to a newer regular build installs the regular DMG"
grep -q verifying_signature "$STATUS_LOG" || fail "a newer regular build over FEX verifies the signature"

# same version, same build, and downgrades stay refused on FEX
setup 1.0.0 fex 1.0.0 fex
! check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 fex || fail "FEX same version is refused"
[ "$(last_error)" = "target_not_newer" ] || fail "FEX same version reports target_not_newer"
setup 1.0.1 regular 1.0.0 fex
! check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 fex || fail "a downgrade that switches builds is refused"
[ "$(last_error)" = "target_not_newer" ] || fail "a switching downgrade reports target_not_newer"
setup 1.0.0 fex 1.0.1 fex
check "$TARGET_APP" "$INSTALLED_APP" 1.0.1 fex || fail "FEX newer installs"

# the FEX build must hold the FEX app and carry the installed app's team
setup 1.0.0 regular 1.0.0 regular
! check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 fex || fail "a FEX DMG holding the regular app is refused"
[ "$(last_error)" = "variant_mismatch" ] || fail "a mislabeled DMG reports variant_mismatch"
setup 1.0.0 regular 1.0.0 fex
! check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 regular || fail "a regular DMG holding the FEX app is refused"
[ "$(last_error)" = "variant_mismatch" ] || fail "a mislabeled regular DMG reports variant_mismatch"
setup 0.80.0 - 1.0.0 fex TEAMB
! check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 fex || fail "a FEX app from another team is refused"
[ "$(last_error)" = "signature_invalid" ] || fail "another team reports signature_invalid"
setup 0.80.0 - 1.0.0 fex unsigned
! check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 fex || fail "an unsigned FEX app is refused"
[ "$(last_error)" = "signature_invalid" ] || fail "an unsigned FEX app reports signature_invalid"
n=$((n + 1))
make_app "$WORK/case/$n/TEAMA/MetalSharp.app" 0.80.0
make_app "$WORK/case/$n/TEAMA/target/MetalSharp.app" 1.0.0 fex com.example.other
! check "$WORK/case/$n/TEAMA/target/MetalSharp.app" "$WORK/case/$n/TEAMA/MetalSharp.app" 1.0.0 fex || \
    fail "a FEX app with another bundle id is refused"
[ "$(last_error)" = "app_bundle_invalid" ] || fail "another bundle id reports app_bundle_invalid"

# --recover reinstalls the installed build: the FEX build gets the FEX DMG (which carries its
# MetalSharp Wine) and may reinstall its own version; the regular build keeps its assets and rule
recovery_assets fex 1.0.0
[ "$RECOVERY_VARIANT" = fex ] || fail "FEX recovery reinstalls the FEX build"
[ "$RECOVERY_DMG_NAME" = MetalSharp-1.0.0-arm64-FEX.dmg ] || fail "FEX recovery downloads the FEX DMG"
[ "$RECOVERY_CACHED_NAME" = MetalSharp-1.0.0-FEX.dmg ] || fail "FEX recovery reuses the updater's FEX download"
[ "$(dmg_name_variant "$RECOVERY_DMG_NAME")" = fex ] || fail "the FEX recovery DMG reads as the FEX build"
recovery_assets regular 1.0.0
[ "$RECOVERY_VARIANT" = regular ] || fail "regular recovery installs the regular build"
[ "$RECOVERY_DMG_NAME" = MetalSharp-1.0.0-arm64.dmg ] || fail "regular recovery downloads the regular DMG"
[ "$RECOVERY_CACHED_NAME" = MetalSharp-1.0.0.dmg ] || fail "regular recovery reuses the updater's regular download"
allowed 1.0.0 fex 1.0.0 fex 1
allowed 1.0.1 fex 1.0.0 fex 1
refused 1.0.0 fex 1.0.1 fex 1
refused 1.0.0 regular 1.0.0 regular
refused 1.0.0 fex 1.0.0 fex 0
setup 1.0.0 fex 1.0.0 fex
TARGET_REINSTALL=1
check "$TARGET_APP" "$INSTALLED_APP" 1.0.0 fex || fail "FEX recovery reinstalls the same version"
TARGET_REINSTALL=0

echo "updater build switch tests passed"
