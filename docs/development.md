# Development
**Updated:** 2026-10-05

Build, run, test, and package MetalSharp from source. Requires Apple Silicon; native code targets macOS 14.0 (`CMAKE_OSX_DEPLOYMENT_TARGET` in `CMakeLists.txt`).

## Prerequisites

```bash
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install cmake zstd
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Packaging the `.app` also needs full Xcode (`actool` compiles the Liquid Glass icon).

## Clone

```bash
git clone --recurse-submodules https://github.com/metalsharp/MetalSharp.git
cd MetalSharp
```

## Build

```bash
# Native engine (C++ D3D/Metal layer), x86_64 for Rosetta 2 PE translation
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --parallel $(sysctl -n hw.ncpu)

# C backend -> app/src-c/build/metalsharp-backend
make -C app/src-c

# GPUI app (generate tilted dock artwork once first)
(cd app-gpui && swift generate-dock-art.swift)
cargo build --release --manifest-path app-gpui/Cargo.toml
```

Download the runtime bundles (Wine 11.17, D3DMetal, DXMT, Mono/FNA, Goldberg, Steam setup files) from the `bundles` release:

```bash
./tools/dmg/create-bundles.sh
```

Building the app does not compile Wine; see [Building MetalSharp Wine](building-wine.md).

## Run

```bash
cargo run --locked --manifest-path app-gpui/Cargo.toml                             # production: :9274, ~/.metalsharp
cargo run --locked --manifest-path app-gpui/Cargo.toml -- --connected-validation  # isolated home on :9276
cargo run --locked --manifest-path app-gpui/Cargo.toml -- --preview               # offline sample UI
```

Production mode behaves like the installed app: it terminates any stale `metalsharp-backend` on 9274, starts a fresh one against `~/.metalsharp`, and stops it on quit. Quit any other MetalSharp first.

Validation mode reads `METALSHARP_GPUI_PORT` (default 9276), `METALSHARP_GPUI_HOME` (absolute, dedicated directory) and optionally `METALSHARP_GPUI_BACKEND`, and protects its backend with a per-launch session token. It refuses the production port and home.

The backend alone honors `METALSHARP_PORT` (default `127.0.0.1:9274`) and `METALSHARP_HOME` (default `~/.metalsharp`).

## Test

```bash
make -C app/src-c test        # HTTP smoke, JSON, migration, SIGPIPE, emulator update transactions
make -C app/src-c asan-test   # same under sanitizers
cargo test --manifest-path app-gpui/Cargo.toml
```

A build-only browser harness is available with `--features browser-fixture -- --browser-fixture` (offline, fixed HTML) or `-- --browser-steam-test` (live Steam Store; only when explicitly needed).

## Package

Ad-hoc signed app (no Apple Developer account needed):

```bash
ACTOOL=/Applications/Xcode.app/Contents/Developer/usr/bin/actool \
CARGO_TARGET_DIR=/tmp/metalsharp-target \
  app-gpui/package-app.sh
open app-gpui/target/MetalSharp.app
```

Keep `CARGO_TARGET_DIR` on the internal disk; release builds can fail on an external volume. `package-app.sh` bundles the C backend, host runtime, tools, updater, configs, and the runtime archives from `app/bundles` under bundle id `com.metalsharp.app`.

DMG (`MetalSharp-<version>-arm64.dmg`):

```bash
tools/dmg/package-gpui-dmg.sh app-gpui/target/MetalSharp.app dist/gpui
```

`package-local-preview.sh` and `package-local-testing-app.sh` / `package-local-testing-dmg.sh` build the same app under separate bundle ids for side-by-side testing (they still use `~/.metalsharp` and port 9274).

## Release Signing

Releases must be Developer ID signed and notarized, or Gatekeeper blocks them. Set these GitHub Actions secrets:

- `MACOS_CERTIFICATE_P12` (base64 `.p12`) and `MACOS_CERTIFICATE_PASSWORD`
- One notarization set: `APPLE_ID`, `APPLE_APP_SPECIFIC_PASSWORD`, `APPLE_TEAM_ID` **or** `APPLE_API_KEY_P8_BASE64`, `APPLE_API_KEY_ID`, `APPLE_API_ISSUER`

The release job runs `tools/dmg/prepare-apple-signing.sh` → `package-app.sh` with `METALSHARP_GPUI_SIGN_IDENTITY` (hardened runtime, `tools/dmg/entitlements.mac.plist`) → `package-gpui-dmg.sh` → `sign-notarize-dmg.sh` → `verify-notarization.sh`. Without the secrets, CI ships an ad-hoc DMG with a `DMG-SIGNING.txt` marker.

The in-app updater only accepts a DMG named `MetalSharp-<version>-arm64.dmg` containing `com.metalsharp.app` at that version, passing `codesign --verify --deep --strict`, and signed by the **same Developer ID team** as the installed app. Ad-hoc releases cannot be installed through the updater.

## Pre-commit Hook

Opt in once per clone:

```bash
ln -s ../../.github/hooks/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
```

| Staged files | Check | If tool missing |
|---|---|---|
| `*.c`, `*.cpp`, `*.h`, `*.hpp`, `*.m`, `*.mm` | `clang-format --dry-run --Werror` | fail |
| `app-gpui/**/*.rs` | `cargo fmt --check` | fail |
| `configs/mtsp-rules.toml` | `python3 tools/ci/validate-rules-toml.py` | fail |
| `*.sh` | `shellcheck` | warn |
| `*.md` | `python3 tools/ci/check-doc-freshness.py` | warn |

Skip with `git commit --no-verify`; CI runs the same checks.

## Troubleshooting

- **`cmake` fails:** check `xcode-select -p` returns a path.
- **`can't find crate` in release builds:** move `CARGO_TARGET_DIR` to the internal disk.
- **Missing bundles:** rerun `./tools/dmg/create-bundles.sh`.
- **App won't open:** `xattr -cr /path/to/MetalSharp.app`.
