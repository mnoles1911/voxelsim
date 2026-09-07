"""Export and validate the 12.5 mm stone axe for the UE felling prototype."""
import json
from pathlib import Path
import _path
import numpy as np
from scipy import ndimage
from forge import pipeline, render, spec, vox, vxa
from bambooprobe import export_obj

root = Path(__file__).resolve().parents[1]
out = root / 'out/tree-felling-prototype'
out.mkdir(parents=True, exist_ok=True)
body, report = spec.load(root / 'specs/bushcraft-stone-axe.json')
assert not report.warnings, report.warnings
asset = pipeline.build(body, 1)
grid = asset.grid
assert grid.voxel_m == .0125
assert not pipeline.health(asset), pipeline.health(asset)
assert ndimage.label(grid.data != 0)[1] == 1
assert np.array_equal(grid.data, pipeline.build(body, 1).grid.data)
vxa.write(grid, out / 'stone-axe-12_5mm.vxa')
vox.write(grid, out / 'stone-axe-12_5mm.vox', name='Lashed stone axe')
export_obj(grid, out / 'stone-axe-12_5mm.obj')
restored = vxa.read(out / 'stone-axe-12_5mm.vxa')
assert restored.voxel_m == .0125 and np.array_equal(restored.data, grid.data)
render.view(grid, 'iso', target_px=1000, background=(233, 229, 218, 255)).save(out / 'stone-axe.png')
result = dict(pitch_mm=12.5, voxels=int(np.count_nonzero(grid.data)),
              components=1, health=pipeline.health(asset), roundtrip=True)
(out / 'axe-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
