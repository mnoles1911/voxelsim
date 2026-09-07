"""Validate and export the photo-inspired bamboo raft at its authored pitch.

python tools/bambooprobe.py
Writes VXA, VOX, a surface-only OBJ/MTL in metres, and review views.
"""
import json
from pathlib import Path

import numpy as np
from scipy import ndimage

import _path  # noqa: F401
from forge import palette, pipeline, render, spec, vox, vxa


def export_obj(grid, path):
    """Expose actual voxel faces; no smoothing, resampling or rounded mesh."""
    faces = [
        (0, 1, [(1,0,0),(1,1,0),(1,1,1),(1,0,1)]),
        (0,-1, [(0,0,0),(0,0,1),(0,1,1),(0,1,0)]),
        (1, 1, [(0,1,0),(0,1,1),(1,1,1),(1,1,0)]),
        (1,-1, [(0,0,0),(1,0,0),(1,0,1),(0,0,1)]),
        (2, 1, [(0,0,1),(1,0,1),(1,1,1),(0,1,1)]),
        (2,-1, [(0,0,0),(0,1,0),(1,1,0),(1,0,0)]),
    ]
    data = grid.data
    occ = data != 0
    centre = np.array([data.shape[0] / 2, data.shape[1] / 2, 0])
    groups = {}
    for axis, direction, corners in faces:
        neighbor = np.roll(occ, -direction, axis=axis)
        edge = [slice(None)] * 3
        edge[axis] = -1 if direction == 1 else 0
        neighbor[tuple(edge)] = False
        coords = np.argwhere(occ & ~neighbor)
        for xyz in coords:
            mat = int(data[tuple(xyz)])
            groups.setdefault(mat, []).append((xyz + np.array(corners) - centre) * grid.voxel_m)
    mtl = path.with_suffix('.mtl')
    with mtl.open('w', encoding='utf-8') as f:
        for mat in sorted(groups):
            rgb = np.array(palette.entry(mat)[1]) / 255.0
            f.write(f'newmtl voxel_{mat}\nKd {rgb[0]:.5f} {rgb[1]:.5f} {rgb[2]:.5f}\nKs 0 0 0\nd 1\nillum 1\n\n')
    count = 0
    with path.open('w', encoding='utf-8') as f:
        f.write(f'# {grid.voxel_m*1000:g} mm cubic voxel surface, metres, Z up\nmtllib {mtl.name}\ns off\n')
        for mat, quads in sorted(groups.items()):
            f.write(f'usemtl voxel_{mat}\n')
            for quad in quads:
                for x, y, z in quad:
                    f.write(f'v {x:.5f} {y:.5f} {z:.5f}\n')
                start = count * 4 + 1
                f.write(f'f {start} {start+1} {start+2} {start+3}\n')
                count += 1
    return count


def main():
    root = Path(__file__).resolve().parents[1]
    name = 'bushcraft-bamboo-raft'
    body, report = spec.load(root / 'specs' / f'{name}.json')
    assert not report.warnings, report.warnings
    out = root / 'out/artifact'
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for seed in (1, 2, 3, 4):
        asset = pipeline.build(body, seed)
        grid = asset.grid
        assert grid.voxel_m == 0.025
        assert not pipeline.health(asset), pipeline.health(asset)
        _, components = ndimage.label(grid.data != 0)
        assert components == 1, (seed, components)
        assert asset.stats['bridges_added'] == 0, 'bamboo required connectivity repair'
        bounds = np.array(grid.shape) * grid.voxel_m
        assert 3.8 < bounds[0] < 4.5 and 2.3 < bounds[1] < 2.6
        assert 1.4 < bounds[2] < 1.7, 'upright posts missing or clipped'
        step = asset.stats['steps'][0]
        assert step['deck_culms'] >= 14
        assert min(c['nodes'] for c in step['culms'][:step['deck_culms']]) >= 7
        # Within the central half of the deck, exclude outboard posts/crossbars.
        deck = grid.data[grid.shape[0]//3:2*grid.shape[0]//3, 12:-12]
        assert (deck != 0).any(axis=2).mean() > 0.98, 'open gaps across central deck'
        assert asset.stats['hollow_frac'] > 0.10, 'bamboo became solid logs'
        results.append({'seed': seed, 'bbox_m': bounds.tolist(),
                        'voxels': asset.stats['voxels'], 'pieces': components,
                        'hollow_fraction': asset.stats['hollow_frac']})
        if seed == 1:
            base = out / f'{name}-0001'
            vxa.write(grid, base.with_suffix('.vxa'))
            vox.write(grid, base.with_suffix('.vox'), name=name)
            loaded = vxa.read(base.with_suffix('.vxa'))
            assert loaded.voxel_m == grid.voxel_m and np.array_equal(loaded.data, grid.data)
            quads = export_obj(grid, base.with_suffix('.obj'))
            results[-1]['mesh_quads'] = quads
            spec.save(body, out / f'{name}-0001-spec.json')
            (out / f'{name}-0001.json').write_text(json.dumps(asset.stats, indent=2)+'\n', encoding='utf-8')
            # View from the open end, as in the reference photograph.
            render.view(render.turned(grid, 2), 'iso', target_px=1800).save(out / f'{name}-reference-view.png')
    (out / f'{name}-validation.json').write_text(json.dumps(results, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(results, indent=2))
    print('bambooprobe: PASS; VXA, VOX, OBJ/MTL and preview exported')


if __name__ == '__main__':
    main()
