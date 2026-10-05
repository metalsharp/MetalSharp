#!/usr/bin/env python3
"""Deterministic migration inventory, not a claim of exhaustive route parity.

Exact method/path branches are source-derived. Prefix/dynamic branches and
unmatched path comparisons remain explicit manual-review items. Literal UI
references are candidates, not proof of an API call or request/response contract.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'app/src-c/runtime/backend.c'
DESTINATION = ROOT / 'app-gpui/backend-route-inventory.json'
EXACT = re.compile(r'strcmp\s*\(\s*request->method\s*,\s*"(GET|POST|PUT|DELETE)"\s*\)\s*==\s*0\s*&&\s*strcmp\s*\(\s*request->path\s*,\s*"([^"\n]+)"\s*\)\s*==\s*0')
PATH = re.compile(r'(strcmp|strncmp)\s*\(\s*request->path\s*,\s*"([^"\n]+)"')


def line_number(text, offset):
    return text.count('\n', 0, offset) + 1


def inventory():
    source = SOURCE.read_text()
    candidates = []
    for directory, extensions in [('app-gpui/src', {'.rs'})]:
        for path in sorted((ROOT / directory).rglob('*')):
            if path.suffix in extensions:
                candidates.append((str(path.relative_to(ROOT)), path.read_text()))
    rows = {}
    covered_offsets = set()
    for match in EXACT.finditer(source):
        method, route = match.groups()
        references = []
        literal = re.compile(r'[\"\'`]' + re.escape(route) + r'[\"\'`]')
        for path, text in candidates:
            for reference in literal.finditer(text):
                references.append({'file': path, 'line': line_number(text, reference.start()), 'kind': 'literal-candidate'})
        row = rows.setdefault((method, route), {'method': method, 'path': route, 'router_lines': [], 'literal_references': references, 'request_contract': 'manual-review', 'response_contract': 'manual-review', 'acceptance': 'not-verified'})
        row['router_lines'].append(line_number(source, match.start()))
        for comparison in PATH.finditer(source, match.start(), match.end()):
            covered_offsets.add(comparison.start())
    manual = []
    for match in PATH.finditer(source):
        if match.start() not in covered_offsets:
            manual.append({'path_or_prefix': match.group(2), 'comparison': match.group(1), 'router_line': line_number(source, match.start()), 'status': 'manual-review'})
    return {
        'schema_version': 1,
        'generator': 'app-gpui/inventory-backend-routes.py',
        'router': 'app/src-c/runtime/backend.c',
        'router_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'limitations': [
            'This is a migration discovery inventory, not a full C/Rust parser or a parity report.',
            'Dynamic/prefix routes and unmatched comparisons require manual expansion.',
            'Literal references may be comments or non-API code; computed routes require manual reconciliation.',
            'Request/response schemas and acceptance IDs must be reviewed against current callers and C handlers.',
            'No backend, provider, install, launch or credential operation is executed by the generator.'
        ],
        'exact_routes': sorted(rows.values(), key=lambda row: (row['path'], row['method'])),
        'manual_review': manual,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    data = inventory()
    encoded = json.dumps(data, indent=2, ensure_ascii=False) + '\n'
    if args.check:
        if not DESTINATION.exists() or DESTINATION.read_text() != encoded:
            raise SystemExit('Backend-route inventory stale; run python3 app-gpui/inventory-backend-routes.py')
    else:
        DESTINATION.write_text(encoded)
    print(f"{len(data['exact_routes'])} exact routes; {len(data['manual_review'])} comparisons/prefixes need manual review")


if __name__ == '__main__':
    main()
