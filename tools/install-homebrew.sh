#!/usr/bin/env bash
# Install Homebrew without terminal prompts. The administrator password, when
# needed, is collected by a native macOS dialog and passed through sudo's
# askpass interface; it is never stored in an argument or a file.
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf '%s\n' "Homebrew installation is only supported on macOS." >&2
  exit 1
fi

# This override is used only by hermetic tests. Normal installs use the
# architecture-specific Homebrew locations below.
brew_prefix_override="${METALSHARP_BREW_PREFIX:-}"

find_brew() {
  local candidate
  if [[ -n "$brew_prefix_override" ]]; then
    candidate="$brew_prefix_override/bin/brew"
    if [[ -x "$candidate" ]] && "$candidate" --version >/dev/null 2>&1; then
      printf '%s\n' "$candidate"
      return 0
    fi
    return 1
  fi
  for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
    if [[ -x "$candidate" ]] && "$candidate" --version >/dev/null 2>&1; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

if brew_path="$(find_brew)"; then
  printf '%s\n' "Homebrew is already installed at $brew_path"
  exit 0
fi

if ! command -v curl >/dev/null 2>&1; then
  printf '%s\n' "curl is required to install Homebrew." >&2
  exit 1
fi

installer_url="${HOMEBREW_INSTALLER_URL:-https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh}"
installer="$(mktemp "${TMPDIR:-/tmp}/metalsharp-homebrew.XXXXXX")"
osascript_bin="${METALSHARP_OSASCRIPT:-/usr/bin/osascript}"
askpass=""
cleanup() {
  rm -f "$installer"
  if [[ -n "$askpass" ]]; then
    rm -f "$askpass"
  fi
}
trap cleanup EXIT

curl --fail --silent --show-error --location "$installer_url" --output "$installer"
chmod 700 "$installer"

if [[ "$(id -u)" -ne 0 ]]; then
  if [[ ! -x "$osascript_bin" ]]; then
    printf '%s\n' "osascript is required for the administrator password dialog." >&2
    exit 1
  fi
  askpass="$(mktemp "${TMPDIR:-/tmp}/metalsharp-homebrew-askpass.XXXXXX")"
  cat >"$askpass" <<'ASKPASS'
#!/usr/bin/env bash
set -euo pipefail
osascript_path="${METALSHARP_OSASCRIPT:-/usr/bin/osascript}"
"$osascript_path" <<'APPLESCRIPT'
tell application "System Events"
  activate
  set resultRecord to display dialog "MetalSharp needs administrator access to install Homebrew." & return & return & "Enter your macOS login password:" default answer "" with hidden answer buttons {"Cancel", "Install"} default button "Install" cancel button "Cancel"
  return text returned of resultRecord
end tell
APPLESCRIPT
ASKPASS
  chmod 700 "$askpass"
  export SUDO_ASKPASS="$askpass"
fi

# Homebrew's official installer honors these variables and uses sudo -n when
# NONINTERACTIVE is set. The askpass helper above supplies the one GUI prompt
# required to authorize sudo without blocking on terminal input.
export NONINTERACTIVE=1
export CI=1
export HOMEBREW_NO_ANALYTICS=1
/bin/bash "$installer"

if ! brew_path="$(find_brew)"; then
  printf '%s\n' "Homebrew installer completed but brew was not found." >&2
  exit 1
fi

printf '%s\n' "Homebrew installed at $brew_path"
printf '%s\n' "PATH for this process: $(${brew_path} shellenv | tr '\n' ' ')"
