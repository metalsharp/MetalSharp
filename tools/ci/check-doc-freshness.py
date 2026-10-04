#!/usr/bin/env python3
"""Warn about stale docs and missing CHANGELOG entries.

Two checks:

  1. Doc freshness: a doc is considered stale if it has no
     `Updated:` / `Created:` / `Date:` line in its first 20 lines, or if
     the `Updated:` line is older than `STALE_DAYS`.

  2. CHANGELOG in sync: `CHANGELOG.md` must contain a section header for
     the current version declared in `CMakeLists.txt`. Catches the
     failure mode where a `chore(release): bump version to X.Y.Z` commit
     bumps the 5 version surfaces but forgets to update the changelog.

By default this is a warning-only check (exit 0) so it doesn't break CI
while docs are being refreshed. Pass `--strict` to make it an error.

Usage:
    python3 tools/ci/check-doc-freshness.py [--strict] [--stale-days=120]
                                             [--check-changelog] [--no-check-changelog]
"""

from __future__ import annotations

import argparse
import re
import sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs"
README = ROOT / "README.md"
AGENTS = ROOT / "AGENTS.md"
CHANGELOG = ROOT / "CHANGELOG.md"
CMAKE_LISTS = ROOT / "CMakeLists.txt"

DATE_RE = re.compile(
    r"(?i)(?:updated|created|last\s+updated|date|modified)[^0-9\n]{0,15}(\d{4}-\d{2}-\d{2})"
)

# Days at which a doc is considered stale (4 months)
DEFAULT_STALE_DAYS = 120

# Files that are exempt from this check
EXEMPT = {
    "docs/README.md",  # Repo map
    "docs/compatibility/GAMES-SUPPORTED.md",  # Updated frequently
}


def is_exempt(path: Path) -> bool:
    rel = str(path.relative_to(ROOT))
    if rel.startswith("docs/archive/"):
        return True
    return rel in EXEMPT


def doc_date(path: Path) -> str | None:
    """Return the most recent date stamp found in the first 20 lines."""
    head = "\n".join(path.read_text(errors="replace").splitlines()[:20])
    matches = DATE_RE.findall(head)
    if not matches:
        return None
    # Return the most recent date among matched stamps
    return max(matches)


def get_current_version() -> str | None:
    """Read the current app version from CMakeLists.txt's project() call."""
    if not CMAKE_LISTS.exists():
        return None
    match = re.search(r"project\(metalsharp VERSION ([0-9]+\.[0-9]+\.[0-9]+)", CMAKE_LISTS.read_text())
    return match.group(1) if match else None


def check_changelog() -> tuple[str | None, bool, str | None]:
    """Return (current_version, section_found, section_date).

    - current_version: the version declared in `CMakeLists.txt`, or None
      if the file is missing or unparseable.
    - section_found: True if a `## v<version>` header was located.
    - section_date: the date stamp embedded in the section header
      (or None if the section has no date).

    """
    version = get_current_version()
    if version is None:
        return None, False, None

    if not CHANGELOG.exists():
        return version, False, None

    # Match any header that starts with `## v<version>` (possibly followed by
    # a date, an em-dash, parentheses, etc.). We don't enforce the format
    # because changelogs are messy.
    pattern = re.compile(
        rf"^[ \t]*##[ \t]+v{re.escape(version)}\b[^\n]*$",
        re.MULTILINE,
    )
    match = pattern.search(CHANGELOG.read_text(errors="replace"))
    if match is None:
        return version, False, None

    # Try to extract a date from the section header.
    header = match.group(0)
    date_match = DATE_RE.search(header)
    return version, True, (date_match.group(1) if date_match else None)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit non-zero if any stale docs are found.",
    )
    parser.add_argument(
        "--stale-days",
        type=int,
        default=DEFAULT_STALE_DAYS,
        help=f"Days before a doc is considered stale (default: {DEFAULT_STALE_DAYS}).",
    )
    parser.add_argument(
        "--check-changelog",
        dest="check_changelog",
        action="store_true",
        default=True,
        help="Verify CHANGELOG.md has a section for the current version (default: on).",
    )
    parser.add_argument(
        "--no-check-changelog",
        dest="check_changelog",
        action="store_false",
        help="Skip the CHANGELOG sync check.",
    )
    args = parser.parse_args()

    today = date.today()
    cutoff = today.toordinal() - args.stale_days

    missing_date: list[Path] = []
    stale: list[tuple[Path, str, int]] = []  # (path, date, days_old)

    targets = list(DOCS.rglob("*.md"))
    for extra in (README, AGENTS):
        if extra.exists():
            targets.append(extra)

    for md in sorted(targets):
        if is_exempt(md):
            continue
        d = doc_date(md)
        if d is None:
            missing_date.append(md)
            continue
        try:
            doc_d = date.fromisoformat(d)
        except ValueError:
            missing_date.append(md)
            continue
        age = today.toordinal() - doc_d.toordinal()
        if doc_d.toordinal() < cutoff:
            stale.append((md, d, age))

    changelog_status: str | None = None
    changelog_section_date: str | None = None
    if args.check_changelog:
        version, section_found, section_date = check_changelog()
        if version is not None and not section_found:
            changelog_status = f"missing section for v{version}"
        else:
            changelog_section_date = section_date

    if missing_date:
        print(
            f"docs without `Updated:` header ({len(missing_date)}):",
            file=sys.stderr,
        )
        for p in missing_date[:20]:
            print(f"  {p.relative_to(ROOT)}", file=sys.stderr)
        if len(missing_date) > 20:
            print(
                f"  ... and {len(missing_date) - 20} more",
                file=sys.stderr,
            )

    if stale:
        print(
            f"docs older than {args.stale_days} days ({len(stale)}):",
            file=sys.stderr,
        )
        for p, d, age in stale[:20]:
            print(
                f"  {p.relative_to(ROOT)} (updated {d}, {age} days old)",
                file=sys.stderr,
            )
        if len(stale) > 20:
            print(
                f"  ... and {len(stale) - 20} more",
                file=sys.stderr,
            )

    if changelog_status is not None:
        print(
            f"CHANGELOG.md is missing a section for the current version: "
            f"{changelog_status}",
            file=sys.stderr,
        )

    problems = bool(missing_date or stale or changelog_status is not None)
    if not problems:
        parts = [
            f"doc freshness OK: all docs have `Updated:` headers and are within "
            f"{args.stale_days} days.",
        ]
        if args.check_changelog:
            if changelog_section_date:
                parts.append(
                    f"CHANGELOG.md has a section for the current version "
                    f"(dated {changelog_section_date})."
                )
            else:
                parts.append("CHANGELOG.md has a section for the current version.")
        print(" ".join(parts))
        return 0

    if args.strict and problems:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())