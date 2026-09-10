"""Copy hash-pinned diagnostic cases into an isolated, bake-only publication.

No endorsements, placement rules or species manifest are produced. Full-library
coverage must still be tested after the small bake/verification gate passes.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--cases', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit('Use a fresh output directory')
    publication_bytes = (args.source / 'appearance/published.json').read_bytes()
    publication = json.loads(publication_bytes)
    if publication.get('preview_only') is not True:
        raise SystemExit('This diagnostic helper requires an explicitly private source')
    cases_bytes = args.cases.read_bytes()
    cases = json.loads(cases_bytes)['cases']
    rows = {row['id']: row for row in publication['models']}
    if len(rows) != len(publication['models']):
        raise SystemExit('Duplicate source identities')
    selected, payloads, seen = [], {}, set()
    for case in cases:
        row = rows[case['id']]
        species, identity = row['species'], row['id']
        if identity in seen or not all(re.fullmatch(r'[a-z0-9-]+', part) for part in (species, identity)):
            raise SystemExit('Duplicate or unsafe case identity')
        seen.add(identity)
        geometry_path = Path('banks') / species / (identity + '.vxa')
        data = (args.source / geometry_path).read_bytes()
        md5 = hashlib.md5(data).hexdigest()
        appearance_path = Path('appearance') / (md5 + '.vac')
        appearance = (args.source / appearance_path).read_bytes()
        if not (hashlib.sha256(data).hexdigest() == row['geometry_sha256'] == case['vxa_sha256']
                and md5 == row['geometry_md5'] == case['vxa_md5']
                and hashlib.sha256(appearance).hexdigest() == row['sha256'] == case['vac_sha256']):
            raise SystemExit('Source/case hash mismatch: ' + identity)
        selected.append(row)
        payloads[geometry_path] = data
        payloads[appearance_path] = appearance
    if not selected:
        raise SystemExit('No cases selected')
    marker = dict(preview_only=True, bake_only=True,
                  purpose='Isolated mesh persistence diagnostic; not a world inventory or endorsement',
                  source_publication_sha256=hashlib.sha256(publication_bytes).hexdigest(),
                  cases_sha256=hashlib.sha256(cases_bytes).hexdigest(), ids=sorted(seen))
    subset = dict(publication, models=selected, preview_only=True, bake_only=True)
    payloads[Path('appearance/published.json')] = json.dumps(subset, indent=2).encode()
    payloads[Path('PREVIEW_ONLY.json')] = json.dumps(marker, indent=2).encode()
    args.output.mkdir(parents=True)
    for relative, data in payloads.items():
        destination = args.output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    print(f'{len(selected)} hash-verified bake-only cases: {args.output.resolve()}')


if __name__ == '__main__':
    main()
