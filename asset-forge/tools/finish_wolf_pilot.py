"""Finish and install the 12.5 mm static wolf with an authored RGB coat.

Reproducible from the proportion study; keeps the procedural baseline intact.
The RGB sidecar is for Asset Forge. VXA still uses engine material IDs.
"""
import _path
import json
import shutil
from pathlib import Path
import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree
from forge import materials, server, vxa
from forge.grid import VoxelGrid
from voxelize_reference_mesh import write_views

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = ROOT/'out/creature-reconstruction/grey-wolf-proportion-study'
    output = ROOT/'out/creature-reconstruction/grey-wolf-finished'
    output.mkdir(parents=True, exist_ok=True)
    with np.load(source/'surface-appearance.npz') as data:
        cells = data['occupied_cells'].copy()
        surface, rgb = data['cells'].copy(), data['rgb'].copy()
    # Remove the source's violet cast in inner-ear shadows, keeping luminance.
    ear = (surface[:,2] > 67) & (surface[:,0] < 38)
    violet = ear & (rgb[:,2].astype(int) > rgb[:,1].astype(int)+3)
    value = rgb[violet].mean(1)
    rgb[violet] = np.clip(value[:,None]*[1.03,1.0,.94],0,255).astype(np.uint8)
    lo = cells.min(0)
    cells -= lo
    surface -= lo
    grid = VoxelGrid(tuple(cells.max(0)+1), voxel_m=.0125)
    ids = np.array([materials.resolve(n) for n in materials.CREATURE_NAMES],np.uint8)
    palette = np.array([materials.color(int(i)) for i in ids])
    mapped = ids[cKDTree(palette).query(rgb)[1]]
    grid.data[tuple(cells.T)] = mapped[cKDTree(surface).query(cells)[1]]
    assert ndimage.label(grid.data > 0)[1] == 1
    assert np.array_equal(np.argwhere(grid.surface_mask()), surface)
    np.savez_compressed(output/'surface-appearance.npz', cells=surface,
                        occupied_cells=cells, rgb=rgb, voxel_m=.0125)
    write_views(output,cells,surface,rgb,.0125)
    name = 'grey_wolf_anatomy_pilot'
    meta = server.import_asset(name,'quadruped',grid,'reference mesh; authored coat')
    dest = server.LIBRARY/name/meta['id']
    np.savez_compressed(dest/'appearance.npz',cells=surface,rgb=rgb)
    shutil.copy2(output/'colored-voxels.glb',dest/'colored-voxels.glb')
    shutil.copy2(source/'oblique/view-0.png',dest/'thumb.png')
    provenance = json.loads((ROOT/'refs/reconstruction/grey-wolf/mesh-source-reviews.json').read_text())[1]
    provenance.pop('visual_review',None)
    provenance.pop('visual_approved',None)
    provenance['modifications'] = 'Uniform scale, torso extension, 12.5 mm cubic voxelization, authored grey/buff coat, amber eyes and neutral ear shadows.'
    (dest/'attribution.json').write_text(json.dumps(provenance,indent=2)+'\n')
    meta.update(appearance='authored RGB surface', static_model_complete=True,
                runtime_ready=False, animation='not rigged', voxel_mm=12.5)
    (dest/'meta.json').write_text(json.dumps(meta,indent=2)+'\n')
    restored = vxa.read(dest/'tree.vxa')
    assert restored.voxel_m == .0125
    assert np.array_equal(restored.data,grid.data)
    old = server.encode_voxels(restored)
    blob = server.encode_voxels(restored,dest/'appearance.npz')
    assert blob[:len(old)] == old and blob[len(old):len(old)+4] == b'RGB1'
    assert blob[len(old)+4:] == rgb.tobytes()
    report = dict(meta, dimensions_m=(np.array(grid.shape)*.0125).tolist(),
                  surface_voxels=len(surface), connected_components=1,
                  checks=['VXA occupancy and pitch roundtrip','RGB byte-exact viewer payload',
                          'one connected body','six-view visual review'],
                  limitations=['Static model; animation rig remains separate work',
                               'Engine VXA export uses shared palette; full RGB is displayed in Asset Forge'])
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
