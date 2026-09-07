"""Regression checks for authoring eligibility and exact binary pitch exports."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forge import spec, resolution, pipeline, server, vxa, manifest, lod

ROOT = Path(__file__).resolve().parents[1]

def main():
    cases = [('bird', None, 4), ('artifact', None, 4),
             ('bush', None, 3), ('tree', None, 1), ('tree', 'craftable', 4)]
    for kind, category, count in cases:
        raw = {'kind': kind}
        if category:
            raw['category'] = category
        assert len(resolution.allowed(raw)) == count
        for cm in resolution.TIERS_CM:
            body, _ = spec.validate(dict(raw, resolution_cm=cm))
            assert spec.get(body, 'resolution_cm') == resolution.normalize(raw, cm)
            assert server.preview_resolution(body) in map(float, resolution.allowed(body))
            try:
                pipeline.resolution_m(body, override=float(cm))
            except ValueError:
                assert cm not in resolution.allowed(raw)
            else:
                assert cm in resolution.allowed(raw)
    for path in (ROOT/'specs').glob('*.json'):
        body, report = spec.load(path)
        resolution.require(body, spec.get(body, 'resolution_cm'))
        assert not any('resolution' in str(w) for w in report.warnings), path
    body, _ = spec.load(ROOT/'specs/common-raven.json')
    data = manifest.encode([('common-raven', body)], seeds_baked={'common-raven': 1})
    assert manifest.decode(data)['species'][0]['voxel_size_mm'] == 12.5
    for path in (ROOT/'library').glob('*/*/*.vxa'):
        grid = vxa.read(path)
        assert grid.voxel_m * 1000 in (100, 50, 25, 12.5), path
    assert lod.ladder(12.5) == [(2,25), (4,50), (8,100), (16,200)]
    print('pitchprobe: PASS (eligibility, preview, overrides, specs, kept assets, VXM, LOD)')

if __name__ == '__main__':
    main()
