## Summary

<!-- Brief description of what this PR does and why -->

## Changes

<!-- List the key changes -->

-

## PR Readiness (MANDATORY)

<!-- Process/review gates enforced by CI
     (.github/workflows/pr-readiness-check.yml). Checked items must use [x].
     To bypass intentionally, apply the `checklist-exception` label. -->

- [ ] Compatibility verified with at least one real game (game + launch method noted below)
- [ ] No hardcoded paths, secrets, or absolute `/Users/...` paths introduced
- [ ] Config/rules TOML validated if `configs/mtsp-rules.toml` or DLL maps changed
- [ ] Version triple (`CMakeLists.txt`, `Cargo.toml`, `package.json`, `package-lock.json`) in sync if version bumped
- [ ] Bottle/runtime migration and launch behavior preserved (rollback plan noted if changed)
- [ ] Docs / compatibility matrix updated for user-facing changes
- [ ] Regression test added for each bug fix

## Local toolchain (run before push)

> **Install the pre-commit hook once per clone** (it catches the same things
> as this checklist, and the same things as CI, locally in ~3s):
>
> ```bash
> ln -s ../../.github/hooks/pre-commit .git/hooks/pre-commit
> chmod +x .git/hooks/pre-commit
> ```
>
> The hook **fails the commit** if any required toolchain is missing — it will
> not silently skip `cargo fmt`, `clippy`, `tsc`, `prettier`, `biome`, etc.
> See [`.github/hooks/README.md`](../.github/hooks/README.md) for details.

> Run locally before pushing. All of these also run automatically in CI.

- [ ] `cargo fmt --all -- --check` passes (Rust)
- [ ] `cargo clippy --all-targets -- -D warnings` passes (Rust)
- [ ] `cargo build --release` passes (Rust backend)
- [ ] `cargo test` passes (Rust — 628+ tests)
- [ ] C++ compiles if changed: `cmake -B build-native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON && cmake --build build-native --parallel $(sysctl -n hw.ncpu)`
- [ ] `clang-format --dry-run --Werror` passes on any changed C/C++/Obj-C file
- [ ] `ctest --test-dir build-native` passes if tests changed
- [ ] TypeScript compiles if changed: `cd app && npx tsc --noEmit`
- [ ] Biome + Prettier pass if TS/JS changed: `cd app && npx @biomejs/biome ci src/ && npx prettier --check 'src/**/*.{ts,js,html,css,json}'`
- [ ] Shell scripts lint if changed: `tools/ci/shellcheck.sh`
- [ ] `tools/ci/validate-rules-toml.py` passes if `configs/mtsp-rules.toml` changed
- [ ] `python3 tools/ci/verify-dmg-workflow.py` passes if release/bundle tooling changed
- [ ] Tested with at least one game (which one? which launch method? which drive?)
- [ ] No hardcoded paths, secrets, or absolute `/Users/...` paths
- [ ] No new files should be added to the repo root; place them under `app/`, `tools/`, `tests/`, etc.

## Test notes

<!-- What did you test? Which game, which launch method, which drive? -->

## Risk

<!-- If this PR ships broken defaults or changes launch behavior, what's the rollback plan? -->
