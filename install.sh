#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

METALSHARP_DIR="$(cd "$(dirname "$0")" && pwd)"

info()  { echo -e "${CYAN}[metalsharp]${NC} $*"; }
ok()    { echo -e "${GREEN}[ok]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC} $*"; }
fail()  { echo -e "${RED}[fail]${NC} $*"; exit 1; }

cmd_exists() { command -v "$1" &>/dev/null; }

step() {
    echo ""
    echo -e "${BOLD}${CYAN}── $1 ──${NC}"
}

echo -e "${BOLD}${CYAN}"
echo "  __  __  ___  ___  _       ___ __  __  ___  ___  ___ _    ___ "
echo " |  \\/  |/ __||_ _|/ \\     / __|  \\/  |/ __|| __)| _ \\ |  | __|"
echo " | |\\/| |\\__ \\ | |/ _ \\   | (__| |\\/| |\\__ \\| _| |   / |__| _| "
echo " |_|  |_||___/___/_/ \\_\\   \\___|_|  |_||___/|___||_|_\\\\____|___|"
echo -e "${NC}"
echo -e "  ${DIM}D3D → Metal translation layer. Native engine build.${NC}"
echo ""

if [[ "$(uname)" != "Darwin" ]]; then
    fail "MetalSharp requires macOS."
fi

ARCH=$(uname -m)
info "Detected: macOS $(sw_vers -productVersion) on $ARCH"

# ── Homebrew ──
step "Checking Homebrew"
"$METALSHARP_DIR/tools/install-homebrew.sh"
if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
fi

# ── Xcode CLI Tools ──
step "Checking Xcode Command Line Tools"
if xcode-select -p &>/dev/null; then
    ok "Xcode CLI tools installed"
else
    info "Installing Xcode CLI tools..."
    xcode-select --install 2>/dev/null || true
    info "After installation completes, re-run this script."
    exit 0
fi

# ── CMake ──
step "Checking CMake"
if cmd_exists cmake; then
    ok "CMake $(cmake --version | head -1 | awk '{print $3}')"
else
    info "Installing CMake..."
    brew install cmake
fi

# ── Build ──
step "Building MetalSharp native engine"
cd "$METALSHARP_DIR"

mkdir -p build
info "Configuring..."
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON 2>&1 | tail -3

info "Building..."
cmake --build build 2>&1 | tail -5
ok "Build complete"

# ── Tests ──
step "Running Tests"
TOTAL_PASS=0
TOTAL_FAIL=0
cd build
for t in test_*; do
    if [[ -x "$t" && -f "$t" ]]; then
        result=$(./"$t" 2>&1)
        if echo "$result" | grep -q "Results:"; then
            count=$(echo "$result" | grep -oE '[0-9]+ passed' | head -1 | grep -oE '^[0-9]+')
            fails=$(echo "$result" | grep -oE '[0-9]+ failed' | head -1 | grep -oE '^[0-9]+')
            TOTAL_PASS=$((TOTAL_PASS + ${count:-0}))
            TOTAL_FAIL=$((TOTAL_FAIL + ${fails:-0}))
        elif echo "$result" | grep -q "PASS\|PASSED"; then
            TOTAL_PASS=$((TOTAL_PASS + 1))
        fi
    fi
done
cd ..

if [[ $TOTAL_FAIL -eq 0 ]]; then
    ok "All tests passed ($TOTAL_PASS checks)"
else
    warn "Some tests failed ($TOTAL_PASS passed, $TOTAL_FAIL failed)"
fi

# ── Summary ──
echo ""
echo -e "${BOLD}${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}  MetalSharp native engine built.${NC}"
echo ""
echo "  Built targets in build/"
echo "  $TOTAL_PASS test checks passed"
echo ""
echo "  To build the Electron app instead:"
echo "    cd app && npm install && npm run build:all"
echo ""
echo "  To build a DMG:"
echo "    ./tools/dmg/create-bundles.sh"
echo "    cd app && npx electron-builder --mac dmg --arm64"
echo -e "${BOLD}${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
