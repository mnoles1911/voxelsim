"""Check all saved creature exports and selected deterministic rebuilds."""
import json
from pathlib import Path
import numpy as np
import _path  # noqa: F401
from forge import materials, pipeline, spec, vox, vxa
from forge.creature_detail import side_disk

ROOT = Path(__file__).resolve().parents[1]


def main():
    # Curved-surface regression: a pupil must cover more than the tangent cell,
    # stay symmetric and never add/remove geometry or paint an internal voxel.
    x, y, z = np.ogrid[:17, :17, :17]
    region = (x-8)**2+(y-8)**2+(z-8)**2 <= 7**2
    mat = region.astype(np.uint8)
    side_disk(mat, region, 8, 9, 3, 26)
    assert np.array_equal(mat != 0, region)
    assert np.array_equal(mat, mat[:, ::-1, :])
    assert np.count_nonzero(mat == 26) > 20
    for xx, yy, zz in np.argwhere(mat == 26):
        col = np.flatnonzero(region[xx, :, zz])
        assert yy in (col[0], col[-1])

    checks = []
    samples = {'common-raven', 'golden-eagle', 'wood-thrush', 'yellowhammer',
               'grey-wolf', 'fennec-fox', 'bengal-tiger', 'plains-zebra',
               'rainbow-trout', 'great-white-shark', 'giant-manta-ray', 'orca'}
    for path in sorted((ROOT / 'library').glob('*/*/meta.json')):
        meta = json.loads(path.read_text())
        if meta.get('category') != 'creature':
            continue
        body, validation = spec.load(path.parent / 'spec.json')
        assert not validation.warnings, (meta['id'], validation.warnings)
        grid, tags, joints = vxa.read_full(path.parent / 'tree.vxa')
        assert grid.voxel_m == float(body['resolution_cm'])/100, meta['id']
        assert grid.count() == meta['stats']['voxels'], meta['id']
        assert np.array_equal(tags != 0, grid.data != 0), meta['id']
        assert grid.data.max() < materials.ENGINE_MATERIAL_COUNT, meta['id']
        ids = set(map(int, np.unique(tags))) - {0}
        assert ids - {1} == {j['part'] for j in joints}, meta['id']
        parents = {j['part']: j['parent'] for j in joints}
        for pid in ids - {1}:
            seen = set()
            while pid != 1:
                assert pid not in seen and pid in parents, meta['id']
                seen.add(pid)
                pid = parents[pid]
        info = vox.inspect(path.parent / 'tree.vox')
        assert info['voxels'] == grid.count() and not info['oversized'], meta['id']
        assert not meta['problems'], (meta['id'], meta['problems'])
        if meta['species'] in samples or meta['species'].startswith('goblin-'):
            rebuilt = pipeline.build(body, meta['seed'])
            assert np.array_equal(rebuilt.grid.data, grid.data), meta['id']
            assert np.array_equal(rebuilt.parts, tags), meta['id']
        checks.append(meta['id'])
    assert len(checks) == 387, len(checks)
    result = dict(exports=len(checks), deterministic_rebuilds=17,
                  curved_surface_regression=True, failures=[])
    target = ROOT / 'out' / 'creature-refresh' / 'verification.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
