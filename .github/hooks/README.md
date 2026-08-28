# Pre-commit Hook

This directory contains shared pre-commit hook(s) for the MetalSharp repo.
Hooks are NOT installed by default — each developer opts in.

## Install (one-time per clone)

```bash
# From the repo root
ln -s ../../.github/hooks/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
```

To remove: `rm .git/hooks/pre-commit`.

## What it checks

The `pre-commit` hook runs these checks only when the relevant files are staged:

| Files staged | Check | Behavior if tool missing |
|--------------|-------|---------------------------|
| `*.c`, `*.cpp`, `*.h`, `*.hpp`, `*.mm`, `*.m` | `clang-format --dry-run --Werror` | **FAIL** — no silent skip |
| `app/**/*.tsx?` | `tsc --noEmit` | **FAIL** — no silent skip |
| `app/src/{renderer,main,shared}/**/*.{ts,tsx,js,jsx}` | `biome ci` | **FAIL** — no silent skip |
| `app/**/*.{ts,tsx,js,jsx,html,css,json}` | `prettier --check` | **FAIL** — no silent skip |
| `configs/mtsp-rules.toml` | `python3 tools/ci/validate-rules-toml.py` | **FAIL** — no silent skip |
| `*.sh` | `shellcheck` | warn only (optional CI check) |
| `*.md`, `README.md`, `AGENTS.md` | `python3 tools/ci/check-doc-freshness.py` | warn only (already a soft check) |

## Policy: fail hard when the toolchain is missing

This hook **fails the commit** if a check is required but the toolchain
needed to run it is missing. The whole point is to prevent the surprise of
formatting or type checks failing in CI after passing locally because a
required tool was absent:

```
[pre-commit] Pre-commit cannot run required checks because tools are missing:
  - clang-format (required to format C/C++/Obj-C files)
Install the missing tools, or bypass this hook with 'git commit --no-verify'.
Bypassing is acceptable but the same failure will surface in CI.
```

The only checks that skip silently are:

- `shellcheck` — listed in `tools/ci/shellcheck.sh`, easy to install
  (`brew install shellcheck`), but not blocking if absent.
- Doc freshness — explicitly a warn-only soft check.

## Skipping

```bash
git commit --no-verify
```

CI still runs the same checks, so skipping locally won't make a bad commit
go through — it just means the failure surfaces in CI instead of locally.

## Why not a project-wide setting?

Hooks live in `.git/hooks/` and aren't tracked by git. A symlink under
`.github/hooks/` lets the team share the script without forcing install —
some developers prefer to run checks via their own tooling (e.g. `lefthook`,
`pre-commit`, or a Makefile target) and the symlink-based approach makes the
shared script easy to swap out.
