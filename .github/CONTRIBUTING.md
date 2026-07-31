# Contributing to MetalSharp

Thanks for helping improve MetalSharp. Contributions can include bug reports,
compatibility findings, documentation, tests, and code.

By participating, you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
Report suspected vulnerabilities privately as described in the
[Security Policy](SECURITY.md).

## Before you start

- Search existing [issues](https://github.com/metalsharp/MetalSharp/issues) and
  pull requests before opening a duplicate.
- Use [Discussions](https://github.com/metalsharp/MetalSharp/discussions) for
  questions, usage help, and early design conversations.
- Open an issue before a large or architectural change so maintainers can align
  on scope and approach.
- Keep pull requests focused. Unrelated fixes should use separate branches and
  pull requests.

## Development setup

MetalSharp development requires an Apple Silicon Mac running macOS 14 or later.
Clone the repository with its submodules:

```sh
git clone --recurse-submodules https://github.com/metalsharp/MetalSharp.git
cd MetalSharp
```

Install the prerequisites and follow the complete build instructions in
[Build from Source](../docs/guides/install-from-source.md).

Create a topic branch from the latest `main`:

```sh
git switch main
git pull --ff-only
git switch -c type/short-description
```

Use a descriptive prefix such as `fix/`, `feat/`, `docs/`, or `test/`. Write
small, focused commits with imperative messages that explain the change.

## Making changes

- Match the style and structure of nearby code.
- Do not commit secrets, credentials, generated build output, machine-specific
  paths, or absolute `/Users/...` paths.
- Add or update tests for behavior changes and bug fixes.
- Update relevant documentation and the compatibility matrix for user-visible
  changes.
- Keep new files in the appropriate existing directory. Root-level additions
  require maintainer agreement.
- Preserve third-party license notices and add required attribution when
  introducing a dependency or vendored component.

## Validation

Run the checks relevant to the files you changed. The pull request template
contains the authoritative checklist. Common checks include:

```sh
# Native code
cmake -B build-native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build-native --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir build-native

# Rust backend
cd app/src-rust
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
cargo test
cd ../..

# TypeScript and renderer formatting
cd app
npx tsc --noEmit
npx @biomejs/biome ci src/
npx prettier --check 'src/**/*.{ts,js,html,css,json}'
cd ..
```

Documentation-only changes should at minimum pass `git diff --check` and have
their links reviewed. If a check is not applicable, leave it unchecked and
explain why in the pull request rather than claiming it ran.

## Pull requests

Complete the repository's pull request template, including:

- a concise summary and list of changes;
- the mandatory PR readiness section;
- the local checks actually run and any intentionally skipped checks;
- test notes, including the game and launch route when runtime behavior changes;
  and
- risk and rollback details.

The `checklist-exception` label is reserved for readiness items that are
intentionally not applicable. Explain the exception in the pull request body;
a maintainer will decide whether to apply or retain the label.

Pull requests must pass CI and review before merging. Reviewers may request a
smaller scope, additional tests, documentation, or changes needed to protect
compatibility and release quality.

## Reporting bugs and compatibility results

Use the appropriate issue form and provide reproducible details. Include the
MetalSharp version, macOS version, Mac model, game or application version,
launch route, relevant configuration, and sanitized logs. Never post account
credentials, license keys, tokens, or other private information.
