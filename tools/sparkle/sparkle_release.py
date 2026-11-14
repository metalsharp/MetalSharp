#!/usr/bin/env python3
"""MetalSharp's Sparkle release: the update archive, its .delta patches, and the build's appcast.

Run by .github/workflows/release.yml after the app is packaged and signed, before its DMG is made:

  sparkle_release.py notarize --app MetalSharp.app
      notarizes the app itself and staples its ticket (Sparkle installs the app, not the DMG), with the notarytool
      credentials sign-notarize-dmg.sh takes.

  sparkle_release.py publish --app MetalSharp.app --variant regular|fex --version X.Y.Z --out dist/gpui
      MetalSharp-X.Y.Z-arm64[-FEX].zip: the app, as Sparkle's full update;
      MetalSharp-X.Y.Z-arm64[-FEX]-from<old>.delta: a patch from each of the last --previous releases that published
        this build's Sparkle archive. Each is applied to a copy of that release's app, which must come out identical
        to this one and pass codesign --verify --deep --strict, or the release stops;
      appcast.xml (regular) or appcast-fex.xml (fex): one item, this version, its archive and its deltas, every file
        signed with the EdDSA key in $SPARKLE_ED_PRIVATE_KEY (or --ed-key-file).

The app reads its build's appcast from the latest release (SUFeedURL, releases/latest/download/<appcast>), and Sparkle
picks the .delta made from the version it runs, else the full archive.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

ROOT = Path(__file__).resolve().parent.parent.parent
REPO = "metalsharp/MetalSharp"
SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
# The delta format the apps' Sparkle (2.9.6) reads; BinaryDelta's newest.
DELTA_FORMAT = "4"


def asset_stem(version: str, variant: str) -> str:
    """The release's name for this build: MetalSharp-1.3.0-arm64 or MetalSharp-1.3.0-arm64-FEX, as its DMG."""
    return f"MetalSharp-{version}-arm64" + ("-FEX" if variant == "fex" else "")


def archive_name(version: str, variant: str) -> str:
    return asset_stem(version, variant) + ".zip"


def delta_name(version: str, variant: str, old_version: str) -> str:
    return f"{asset_stem(version, variant)}-from{old_version}.delta"


def appcast_name(variant: str) -> str:
    return "appcast-fex.xml" if variant == "fex" else "appcast.xml"


def download_url(version: str, name: str, repo: str = REPO, base_url: str = "") -> str:
    """Where the release serves `name`; base_url replaces the release's own folder (a local feed, for testing)."""
    if base_url:
        return base_url.rstrip("/") + "/" + name
    return f"https://github.com/{repo}/releases/download/v{version}/{name}"


def version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r"\d+", version))


@dataclass
class Signed:
    name: str
    url: str
    length: int
    signature: str


@dataclass
class Delta(Signed):
    from_version: str = ""


@dataclass
class Item:
    version: str
    variant: str
    archive: Signed
    deltas: list[Delta] = field(default_factory=list)
    notes_url: str = ""
    minimum_system_version: str = "13.0"


def appcast_xml(item: Item) -> str:
    """The build's appcast: one item, sparkle:version and shortVersionString both the release version (MetalSharp's
    CFBundleVersion is its marketing version), the full archive and the deltas Sparkle chooses from by its version."""
    title = f"MetalSharp {item.version}" + (" (Arm64 with FEX)" if item.variant == "fex" else "")
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        f'<rss version="2.0" xmlns:sparkle={quoteattr(SPARKLE_NS)}>',
        "  <channel>",
        f"    <title>{escape(title)}</title>",
        "    <item>",
        f"      <title>{escape(title)}</title>",
        f"      <sparkle:version>{escape(item.version)}</sparkle:version>",
        f"      <sparkle:shortVersionString>{escape(item.version)}</sparkle:shortVersionString>",
        f"      <sparkle:minimumSystemVersion>{escape(item.minimum_system_version)}</sparkle:minimumSystemVersion>",
    ]
    if item.notes_url:
        lines.append(f"      <sparkle:fullReleaseNotesLink>{escape(item.notes_url)}</sparkle:fullReleaseNotesLink>")
    lines.append(
        f"      <enclosure url={quoteattr(item.archive.url)} length=\"{item.archive.length}\""
        f' type="application/octet-stream" sparkle:edSignature={quoteattr(item.archive.signature)}/>'
    )
    if item.deltas:
        lines.append("      <sparkle:deltas>")
        for delta in sorted(item.deltas, key=lambda d: version_key(d.from_version), reverse=True):
            lines.append(
                f"        <enclosure url={quoteattr(delta.url)} sparkle:deltaFrom={quoteattr(delta.from_version)}"
                f' length="{delta.length}" type="application/octet-stream"'
                f" sparkle:edSignature={quoteattr(delta.signature)}/>"
            )
        lines.append("      </sparkle:deltas>")
    lines += ["    </item>", "  </channel>", "</rss>", ""]
    return "\n".join(lines)


def previous_versions(releases: list[dict], version: str, variant: str, count: int) -> list[str]:
    """The last `count` published releases before `version` that carry this build's Sparkle archive: the versions
    users can be on with an app that reads this appcast (releases before Sparkle had none, and get the full archive
    through their own updater)."""
    found = []
    for release in releases:
        if release.get("draft") or release.get("prerelease"):
            continue
        tag = str(release.get("tag_name", "")).lstrip("v")
        if not tag or version_key(tag) >= version_key(version):
            continue
        names = {asset.get("name") for asset in release.get("assets", [])}
        if archive_name(tag, variant) in names:
            found.append(tag)
    found.sort(key=version_key, reverse=True)
    return found[:count]


def parse_signature(output: str) -> tuple[str, int]:
    """sign_update's `sparkle:edSignature="..." length="..."`."""
    signature = re.search(r'sparkle:edSignature="([^"]+)"', output)
    length = re.search(r'length="(\d+)"', output)
    if not signature or not length:
        raise ValueError(f"unexpected sign_update output: {output.strip()!r}")
    return signature.group(1), int(length.group(1))


# ───────────────────────────── tools ─────────────────────────────


def run(*args: str | Path, capture: bool = False, **kwargs) -> str:
    result = subprocess.run([str(a) for a in args], check=True, text=True,
                            stdout=subprocess.PIPE if capture else None, **kwargs)
    return result.stdout if capture else ""


def sparkle_tools() -> Path:
    directory = os.environ.get("SPARKLE_DIR") or run(ROOT / "tools/sparkle/fetch-sparkle.sh", capture=True).strip()
    return Path(directory) / "bin"


def sign(path: Path, key_file: Path, tools: Path) -> tuple[str, int]:
    signature, length = parse_signature(run(tools / "sign_update", "--ed-key-file", key_file, path, capture=True))
    if length != path.stat().st_size:
        raise ValueError(f"{path.name}: sign_update length {length} is not its size {path.stat().st_size}")
    return signature, length


def zip_app(app: Path, archive: Path) -> None:
    archive.unlink(missing_ok=True)
    run("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app, archive)


def unzip_app(archive: Path, into: Path) -> Path:
    run("ditto", "-x", "-k", archive, into)
    apps = list(into.glob("*.app"))
    if len(apps) != 1:
        raise ValueError(f"{archive.name} holds {len(apps)} apps, expected one")
    return apps[0]


def github_releases(repo: str) -> list[dict]:
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/releases?per_page=30",
        headers={"Accept": "application/vnd.github+json", "User-Agent": "MetalSharp-release",
                 **({"Authorization": f"Bearer {os.environ['GITHUB_TOKEN']}"} if os.environ.get("GITHUB_TOKEN") else {})},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def download(url: str, path: Path) -> None:
    with urllib.request.urlopen(url, timeout=600) as response, open(path, "wb") as out:
        shutil.copyfileobj(response, out)


def make_delta(old_app: Path, new_app: Path, delta: Path, tools: Path, scratch: Path) -> None:
    """BinaryDelta's patch from old_app to new_app, applied to a copy of old_app, which must then be new_app: the same
    files and a valid code signature, or the delta would leave users with a broken MetalSharp."""
    delta.unlink(missing_ok=True)
    run(tools / "BinaryDelta", "create", "--version", DELTA_FORMAT, old_app, new_app, delta)
    applied = scratch / "applied" / new_app.name
    shutil.rmtree(applied.parent, ignore_errors=True)
    applied.parent.mkdir(parents=True)
    run(tools / "BinaryDelta", "apply", old_app, applied, delta)
    run("diff", "-rq", "--no-dereference", new_app, applied)
    run("codesign", "--verify", "--deep", "--strict", applied)
    shutil.rmtree(applied.parent, ignore_errors=True)


def notarize(app: Path) -> None:
    """Notarizes the app and staples its ticket, so the app Sparkle installs (and the DMG made from it) carries it."""
    api = all(os.environ.get(k) for k in ("APPLE_API_KEY", "APPLE_API_KEY_ID", "APPLE_API_ISSUER"))
    password = all(os.environ.get(k) for k in ("APPLE_ID", "APPLE_APP_SPECIFIC_PASSWORD", "APPLE_TEAM_ID"))
    if api:
        auth = ["--key", os.environ["APPLE_API_KEY"], "--key-id", os.environ["APPLE_API_KEY_ID"],
                "--issuer", os.environ["APPLE_API_ISSUER"]]
    elif password:
        auth = ["--apple-id", os.environ["APPLE_ID"], "--password", os.environ["APPLE_APP_SPECIFIC_PASSWORD"],
                "--team-id", os.environ["APPLE_TEAM_ID"]]
    else:
        raise SystemExit("Missing complete Apple notarization credentials.")
    with tempfile.TemporaryDirectory() as scratch:
        archive = Path(scratch) / (app.stem + ".zip")
        zip_app(app, archive)
        output = run("xcrun", "notarytool", "submit", archive, "--wait", "--timeout", "2h",
                     "--output-format", "json", *auth, capture=True)
        print(output)
        result = json.loads(output or "{}")
        if result.get("status") != "Accepted":
            if result.get("id"):
                subprocess.run(["xcrun", "notarytool", "log", *auth, result["id"]])
            raise SystemExit(f"Apple notarization did not accept {app.name}: {result.get('status')}")
    run("xcrun", "stapler", "staple", app)
    run("xcrun", "stapler", "validate", app)


def publish(args: argparse.Namespace) -> None:
    app = Path(args.app).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    tools = sparkle_tools()
    run("codesign", "--verify", "--deep", "--strict", app)

    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = Path(scratch_name)
        key_file = Path(args.ed_key_file) if args.ed_key_file else scratch / "ed.key"
        if not args.ed_key_file:
            key = os.environ.get("SPARKLE_ED_PRIVATE_KEY", "").strip()
            if not key:
                raise SystemExit("SPARKLE_ED_PRIVATE_KEY is not set")
            key_file.write_text(key + "\n")
            key_file.chmod(0o600)

        archive = out / archive_name(args.version, args.variant)
        zip_app(app, archive)
        signature, length = sign(archive, key_file, tools)
        item = Item(
            version=args.version,
            variant=args.variant,
            archive=Signed(archive.name, download_url(args.version, archive.name, args.repo, args.base_url), length,
                           signature),
            notes_url=f"https://github.com/{args.repo}/releases/tag/v{args.version}",
        )

        olds = args.from_version or previous_versions(github_releases(args.repo), args.version, args.variant,
                                                       args.previous)
        for old in olds:
            old_dir = scratch / f"old-{old}"
            old_dir.mkdir()
            old_archive = Path(args.old_archive_dir) / archive_name(old, args.variant) if args.old_archive_dir \
                else old_dir / archive_name(old, args.variant)
            if not old_archive.is_file():
                download(download_url(old, archive_name(old, args.variant), args.repo), old_archive)
            old_app = unzip_app(old_archive, old_dir)
            delta = out / delta_name(args.version, args.variant, old)
            make_delta(old_app, app, delta, tools, scratch)
            signature, length = sign(delta, key_file, tools)
            item.deltas.append(Delta(delta.name, download_url(args.version, delta.name, args.repo, args.base_url), length,
                                     signature, from_version=old))
            print(f"{delta.name}: {length} bytes, from {old}")
            shutil.rmtree(old_dir, ignore_errors=True)

    appcast = out / appcast_name(args.variant)
    appcast.write_text(appcast_xml(item))
    print(f"{archive.name}: {item.archive.length} bytes; {appcast.name} with {len(item.deltas)} delta(s)")


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    notarize_parser = commands.add_parser("notarize", help="notarize and staple the app")
    notarize_parser.add_argument("--app", required=True)
    publish_parser = commands.add_parser("publish", help="the update archive, its deltas and the appcast")
    publish_parser.add_argument("--app", required=True)
    publish_parser.add_argument("--variant", choices=["regular", "fex"], required=True)
    publish_parser.add_argument("--version", required=True)
    publish_parser.add_argument("--out", required=True)
    publish_parser.add_argument("--repo", default=REPO)
    publish_parser.add_argument("--previous", type=int, default=2,
                                help="deltas from this many earlier releases with a Sparkle archive")
    publish_parser.add_argument("--from-version", action="append",
                                help="make a delta from this version instead of looking up releases (repeatable)")
    publish_parser.add_argument("--old-archive-dir",
                                help="where the old versions' archives are, instead of downloading them")
    publish_parser.add_argument("--ed-key-file", help="the EdDSA private key, instead of $SPARKLE_ED_PRIVATE_KEY")
    publish_parser.add_argument("--base-url", default="",
                                help="serve the archive and deltas from here instead of the release (a local test feed)")
    args = parser.parse_args(argv)
    if args.command == "notarize":
        notarize(Path(args.app).resolve())
    else:
        publish(args)


if __name__ == "__main__":
    main(sys.argv[1:])
