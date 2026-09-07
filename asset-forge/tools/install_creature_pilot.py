"""Install a visually inspected RGB voxel study as a draft static pilot.

Usage: python tools/install_creature_pilot.py STUDY NAME KIND ATTRIBUTION
Requires six-views.png and oblique/view-0.png from that exact study.
Never approves runtime publication or overwrites an existing library entry.
"""
import _path
import argparse
import json
import shutil
from pathlib import Path

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree
from forge import materials, server, spec as specmod, vxa
from forge.grid import VoxelGrid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('study', type=Path)
    parser.add_argument('name')
    parser.add_argument('kind', choices=['quadruped', 'bird', 'fish'])
    parser.add_argument('attribution', type=Path)
    args = parser.parse_args()
    if not args.name.replace('_', '').isalnum():
        raise ValueError('Use a plain species identifier')
    dest = server.LIBRARY / args.name / (args.name + '-0001')
    if dest.exists() or (server.SPECS / (args.name + '.json')).exists():
        raise FileExistsError('Existing model or spec: inspect and update explicitly')
    provenance = json.loads(args.attribution.read_text(encoding='utf8'))
    required = ['source_url', 'author', 'license', 'modifications']
    if any(not provenance.get(k) for k in required):
        raise ValueError('Missing source attribution')
    for name in ['six-views.png', 'oblique/view-0.png', 'colored-voxels.glb']:
        if not (args.study / name).is_file():
            raise FileNotFoundError(name)
    with np.load(args.study / 'surface-appearance.npz', allow_pickle=False) as data:
        cells, surface, rgb = (data[k].copy() for k in ['occupied_cells', 'cells', 'rgb'])
        pitch = float(data['voxel_m'])
    if pitch != .0125 or rgb.dtype != np.uint8 or rgb.shape != surface.shape:
        raise ValueError('Expected 12.5 mm cubic study with uint8 surface RGB')
    lo = cells.min(0)
    cells -= lo
    surface -= lo
    grid = VoxelGrid(tuple(cells.max(0) + 1), voxel_m=pitch)
    ids = np.array([materials.resolve(n) for n in materials.CREATURE_NAMES], np.uint8)
    palette = np.array([materials.color(int(i)) for i in ids])
    mapped = ids[cKDTree(palette).query(rgb)[1]]
    grid.data[tuple(cells.T)] = mapped[cKDTree(surface).query(cells)[1]]
    if ndimage.label(grid.data > 0)[1] != 1:
        raise ValueError('Disconnected geometry requires explicit review before installation')
    if not np.array_equal(np.argwhere(grid.surface_mask()), surface):
        raise ValueError('Surface appearance does not match geometry')
    meta = server.import_asset(args.name, args.kind, grid, 'reviewed reference mesh; RGB voxel study')
    np.savez_compressed(dest / 'appearance.npz', cells=surface, rgb=rgb)
    for source, target in [('colored-voxels.glb', 'colored-voxels.glb'),
                           ('oblique/view-0.png', 'thumb.png')]:
        shutil.copy2(args.study / source, dest / target)
    (dest / 'attribution.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf8')
    meta.update(static_model_complete=False, review_status='pilot_candidate',
                visual_approved=False, runtime_ready=False, animation='not rigged',
                appearance='RGB surface sidecar', voxel_mm=12.5,
                dimensions_m=(np.array(grid.shape)*pitch).tolist())
    # Imported geometry supplies the dimensions; procedural defaults do not.
    for path in [dest / 'spec.json', server.SPECS / (args.name + '.json')]:
        spec = json.loads(path.read_text(encoding='utf8'))
        key = {'quadruped': 'quad', 'bird': 'bird', 'fish': 'fish'}[args.kind]
        spec[key]['length_m'] = float(grid.shape[0]*pitch)
        spec['curation'] = dict(status='draft', seeds=[1], notes='Pilot candidate awaiting set review')
        specmod.save(spec, path)
    meta['spec_hash'] = specmod.spec_hash(spec)
    restored = vxa.read(dest / 'tree.vxa')
    assert restored.voxel_m == pitch and np.array_equal(restored.data, grid.data)
    legacy = server.encode_voxels(restored)
    payload = server.encode_voxels(restored, dest / 'appearance.npz')
    assert payload == legacy + b'RGB1' + rgb.tobytes()
    meta['validation'] = ['one connected component', 'VXA roundtrip', 'RGB payload byte equality']
    (dest / 'meta.json').write_text(json.dumps(meta, indent=2) + '\n', encoding='utf8')
    print(json.dumps(meta))


if __name__ == '__main__':
    main()
