import json
import sys
import zipfile
from pathlib import Path
import numpy as np

root = Path('D:/voxelsim/asset-forge')
sys.path.insert(0, str(root))
from forge import spec

name = 'bushcraft-bamboo-raft'
out = root / 'out/artifact'
base = out / f'{name}-0001'
vertices = []
faces = []
for line in base.with_suffix('.obj').read_text().splitlines():
    if line.startswith('v '):
        vertices.append([float(x) for x in line.split()[1:]])
    elif line.startswith('f '):
        faces.append([int(x) for x in line.split()[1:]])
points = np.asarray(vertices)
quads = np.asarray(faces)
assert len(quads) == 79216
assert quads.min() == 1 and quads.max() == len(points)
assert np.allclose(np.ptp(points, axis=0), [4.225, 2.45, 1.5])
assert np.allclose(np.linalg.norm(points[quads[:, 1]-1] - points[quads[:, 0]-1], axis=1), 0.025)
assert np.allclose(np.linalg.norm(points[quads[:, 2]-1] - points[quads[:, 1]-1], axis=1), 0.025)
hash_path = root / 'tools/spec_hashes.json'
hashes = json.loads(hash_path.read_text())
body, report = spec.load(root / 'specs' / f'{name}.json')
assert not report.warnings
hashes[name] = [spec.spec_hash(body), spec.seed_hash(body)]
hash_path.write_text(json.dumps(hashes, indent=0, sort_keys=True)+'\n', encoding='utf-8')
files = [base.with_suffix(ext) for ext in ('.vxa','.vox','.obj','.mtl')]
files += [out / f'{name}-0001-spec.json', out / f'{name}-reference-view.png',
          out / f'{name}-validation.json']
package = out / f'{name}-25mm.zip'
with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED) as bundle:
    for file in files:
        assert file.is_file()
        bundle.write(file, file.name)
    bundle.write(root / 'docs/bushcraft-bamboo-raft.md', 'README.md')
with zipfile.ZipFile(package) as bundle:
    assert bundle.testzip() is None
    assert len(bundle.namelist()) == 8
print(f'OBJ verified: {len(quads):,} cubic faces at 25 mm; correct metre-scale bounds.')
print(f'Package verified: {package} ({package.stat().st_size:,} bytes)')
