#!/usr/bin/env python3
"""Split release DMGs into thirds, scan each part, and format release notes."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

MAX_VIRUSTOTAL_BYTES = 650 * 1024 * 1024
PART_COUNT = 3
START_MARKER = "<!-- virustotal-results:start -->"
END_MARKER = "<!-- virustotal-results:end -->"


class ScanError(RuntimeError):
    """Raised when a VirusTotal scan cannot be completed safely."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def split_dmg(path: Path, output_dir: Path, max_bytes: int = MAX_VIRUSTOTAL_BYTES) -> list[dict[str, Any]]:
    size = path.stat().st_size
    if size < PART_COUNT:
        raise ScanError(f"DMG is too small to split into {PART_COUNT} parts: {path}")

    base_size, remainder = divmod(size, PART_COUNT)
    expected_sizes = [base_size + (1 if index < remainder else 0) for index in range(PART_COUNT)]
    if max(expected_sizes) > max_bytes:
        raise ScanError(
            f"{path.name} is {size} bytes; an equal third exceeds the "
            f"VirusTotal {max_bytes}-byte upload limit"
        )

    output_dir.mkdir(parents=True, exist_ok=True)
    part_paths = [
        output_dir / f"{path.name}.part-{index}-of-{PART_COUNT}"
        for index in range(1, PART_COUNT + 1)
    ]
    with path.open("rb") as source:
        for part, expected_size in zip(part_paths, expected_sizes, strict=True):
            remaining = expected_size
            with part.open("wb") as output:
                while remaining:
                    chunk = source.read(min(8 * 1024 * 1024, remaining))
                    if not chunk:
                        raise ScanError(f"unexpected end of file while splitting {path}")
                    output.write(chunk)
                    remaining -= len(chunk)
        if source.read(1):
            raise ScanError(f"split did not consume exactly {size} bytes from {path}")

    actual_sizes = [part.stat().st_size for part in part_paths]
    if actual_sizes != expected_sizes or sum(actual_sizes) != size:
        raise ScanError(f"split size verification failed for {path}")

    return [
        {
            "dmg": path.name,
            "part": index,
            "part_count": PART_COUNT,
            "path": str(part),
            "size": part.stat().st_size,
            "sha256": sha256_file(part),
        }
        for index, part in enumerate(part_paths, start=1)
    ]


class VirusTotalClient:
    def __init__(
        self,
        api_key: str,
        poll_seconds: int = 60,
        timeout_seconds: int = 90 * 60,
        api_interval_seconds: int = 16,
    ) -> None:
        if not api_key:
            raise ScanError("VIRUSTOTAL_API_KEY is not configured")
        self.api_key = api_key
        self.poll_seconds = poll_seconds
        self.timeout_seconds = timeout_seconds
        self.api_interval_seconds = api_interval_seconds
        self.last_api_request = 0.0

    def _rate_limit(self) -> None:
        elapsed = time.monotonic() - self.last_api_request
        if elapsed < self.api_interval_seconds:
            time.sleep(self.api_interval_seconds - elapsed)

    def _get_json(self, url: str) -> dict[str, Any]:
        self._rate_limit()
        request = urllib.request.Request(url, headers={"x-apikey": self.api_key})
        try:
            with urllib.request.urlopen(request, timeout=90) as response:
                payload = json.load(response)
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")[:1000]
            raise ScanError(f"VirusTotal API returned HTTP {error.code}: {detail}") from error
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            raise ScanError(f"VirusTotal API request failed: {type(error).__name__}") from error
        finally:
            self.last_api_request = time.monotonic()
        if not isinstance(payload, dict):
            raise ScanError("VirusTotal API returned a non-object response")
        return payload

    def _new_upload_url(self) -> str:
        payload = self._get_json("https://www.virustotal.com/api/v3/files/upload_url")
        upload_url = payload.get("data")
        if not isinstance(upload_url, str) or not upload_url.startswith(("https://", "http://")):
            raise ScanError("VirusTotal did not return a valid large-file upload URL")
        return upload_url

    def upload(self, path: Path) -> str:
        for attempt in range(1, 4):
            upload_url = self._new_upload_url()
            command = [
                "curl",
                "--silent",
                "--show-error",
                "--fail-with-body",
                "--request",
                "POST",
                "--form",
                f"file=@{path};filename={path.name}",
                upload_url,
            ]
            completed = subprocess.run(command, capture_output=True, text=True, check=False)
            if completed.returncode == 0:
                try:
                    payload = json.loads(completed.stdout)
                    analysis_id = payload["data"]["id"]
                except (json.JSONDecodeError, KeyError, TypeError) as error:
                    raise ScanError("VirusTotal upload returned an invalid response") from error
                if not isinstance(analysis_id, str) or not analysis_id:
                    raise ScanError("VirusTotal upload did not return an analysis ID")
                return analysis_id
            if attempt == 3:
                detail = completed.stderr.strip()[:1000]
                raise ScanError(f"VirusTotal upload failed after 3 attempts: {detail}")
            time.sleep(30 * attempt)
        raise ScanError("VirusTotal upload failed")

    def wait(self, analysis_id: str) -> dict[str, int]:
        deadline = time.monotonic() + self.timeout_seconds
        while time.monotonic() < deadline:
            payload = self._get_json(
                f"https://www.virustotal.com/api/v3/analyses/{analysis_id}"
            )
            try:
                attributes = payload["data"]["attributes"]
                status = attributes["status"]
            except (KeyError, TypeError) as error:
                raise ScanError("VirusTotal analysis returned an invalid response") from error
            if status == "completed":
                raw_stats = attributes.get("stats", {})
                if not isinstance(raw_stats, dict):
                    raise ScanError("VirusTotal analysis returned invalid statistics")
                return {
                    str(name): int(value)
                    for name, value in raw_stats.items()
                    if isinstance(value, int)
                }
            if status not in {"queued", "in-progress"}:
                raise ScanError(f"VirusTotal analysis entered unexpected status: {status}")
            time.sleep(self.poll_seconds)
        raise ScanError(f"VirusTotal analysis timed out after {self.timeout_seconds} seconds")


def markdown_report(tag: str, results: list[dict[str, Any]]) -> str:
    lines = [
        START_MARKER,
        "## VirusTotal partial-file scans",
        "",
        (
            "The release DMG exceeds VirusTotal's 650 MiB upload limit. Each report below "
            "covers one raw third of a DMG, not a complete mountable DMG, and must not be "
            "interpreted as a whole-installer verdict."
        ),
        "",
        "| DMG | Part | Size | Detections | VirusTotal report |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for result in results:
        stats = result["stats"]
        malicious = int(stats.get("malicious", 0))
        suspicious = int(stats.get("suspicious", 0))
        detected = malicious + suspicious
        total = sum(int(value) for value in stats.values())
        dmg = str(result["dmg"]).replace("|", "\\|")
        size_mib = int(result["size"]) / (1024 * 1024)
        report_url = f"https://www.virustotal.com/gui/file/{result['sha256']}"
        lines.append(
            f"| `{dmg}` | {result['part']}/{result.get('part_count', PART_COUNT)} | "
            f"{size_mib:.1f} MiB | "
            f"{detected}/{total} | [Report]({report_url}) |"
        )
    lines.extend(["", f"Scanned automatically for release `{tag}`.", END_MARKER, ""])
    return "\n".join(lines)


def replace_marked_section(body: str, section: str) -> str:
    section = section.strip() + "\n"
    start = body.find(START_MARKER)
    end = body.find(END_MARKER)
    if start == -1 and end == -1:
        return body.rstrip() + ("\n\n" if body.strip() else "") + section
    if start == -1 or end == -1 or end < start:
        raise ScanError("release notes contain malformed VirusTotal markers")
    end += len(END_MARKER)
    return body[:start].rstrip() + "\n\n" + section + body[end:].lstrip("\n")


def validate_assets_command(args: argparse.Namespace) -> int:
    artifact_dir = Path(args.artifact_dir).resolve()
    dmgs = sorted(artifact_dir.rglob("*.dmg"))
    if not dmgs:
        raise ScanError("release artifact contains no DMG files")
    payload = json.loads(Path(args.assets_json).read_text())
    raw_assets = payload.get("assets", [])
    if not isinstance(raw_assets, list):
        raise ScanError("GitHub release asset response is invalid")
    assets = {
        asset.get("name"): asset
        for asset in raw_assets
        if isinstance(asset, dict) and isinstance(asset.get("name"), str)
    }
    for dmg in dmgs:
        remote = assets.get(dmg.name)
        if not remote:
            raise ScanError(f"published release is missing {dmg.name}")
        expected_digest = f"sha256:{sha256_file(dmg)}"
        if remote.get("size") != dmg.stat().st_size or remote.get("digest") != expected_digest:
            raise ScanError(f"published release asset does not match exact workflow artifact: {dmg.name}")
        print(f"Validated published asset {dmg.name}: {expected_digest}")
    return 0


def scan_command(args: argparse.Namespace) -> int:
    artifact_dir = Path(args.artifact_dir).resolve()
    tag_files = list(artifact_dir.rglob("RELEASE-TAG.txt"))
    if len(tag_files) != 1:
        raise ScanError(f"expected exactly one RELEASE-TAG.txt, found {len(tag_files)}")
    artifact_tag = tag_files[0].read_text().strip()
    if artifact_tag != args.tag:
        raise ScanError(f"artifact tag {artifact_tag!r} does not match release tag {args.tag!r}")

    dmgs = sorted(artifact_dir.rglob("*.dmg"))
    if not dmgs:
        raise ScanError("release artifact contains no DMG files")

    parts: list[dict[str, Any]] = []
    part_dir = Path(args.part_dir).resolve()
    for dmg in dmgs:
        parts.extend(split_dmg(dmg, part_dir / dmg.stem))

    client = VirusTotalClient(
        os.environ.get("VIRUSTOTAL_API_KEY", ""),
        poll_seconds=args.poll_seconds,
        timeout_seconds=args.timeout_seconds,
        api_interval_seconds=args.api_interval_seconds,
    )
    results: list[dict[str, Any]] = []
    for part in parts:
        path = Path(part["path"])
        print(
            f"Uploading {part['dmg']} part {part['part']}/{part['part_count']} "
            f"({part['size']} bytes, sha256={part['sha256']})",
            flush=True,
        )
        analysis_id = client.upload(path)
        stats = client.wait(analysis_id)
        result = {**part, "analysis_id": analysis_id, "stats": stats}
        results.append(result)
        print(
            f"Completed {part['dmg']} part {part['part']}/{part['part_count']}: "
            f"malicious={stats.get('malicious', 0)} suspicious={stats.get('suspicious', 0)}",
            flush=True,
        )

    report = {
        "tag": args.tag,
        "partial_file_scans": True,
        "results": results,
    }
    Path(args.json_output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    Path(args.markdown_output).write_text(markdown_report(args.tag, results))
    return 0


def merge_command(args: argparse.Namespace) -> int:
    body = Path(args.body).read_text()
    section = Path(args.section).read_text()
    Path(args.output).write_text(replace_marked_section(body, section))
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser(
        "validate-assets", help="match workflow DMGs to published release assets"
    )
    validate.add_argument("--artifact-dir", required=True)
    validate.add_argument("--assets-json", required=True)
    validate.set_defaults(func=validate_assets_command)

    scan = subparsers.add_parser("scan", help="split and scan release DMGs")
    scan.add_argument("--artifact-dir", required=True)
    scan.add_argument("--tag", required=True)
    scan.add_argument("--part-dir", required=True)
    scan.add_argument("--json-output", required=True)
    scan.add_argument("--markdown-output", required=True)
    scan.add_argument("--poll-seconds", type=int, default=60)
    scan.add_argument("--timeout-seconds", type=int, default=90 * 60)
    scan.add_argument("--api-interval-seconds", type=int, default=16)
    scan.set_defaults(func=scan_command)

    merge = subparsers.add_parser("merge-body", help="merge scan results into release notes")
    merge.add_argument("--body", required=True)
    merge.add_argument("--section", required=True)
    merge.add_argument("--output", required=True)
    merge.set_defaults(func=merge_command)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return args.func(args)
    except ScanError as error:
        print(f"VirusTotal release scan failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
