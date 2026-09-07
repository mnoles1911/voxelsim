import json
import subprocess
import sys
import types
from pathlib import Path

root = Path('D:/voxelsim')
sys.path.insert(0, str(root / 'asset-forge'))
from forge import pipeline, render, spec

def original(path):
    return subprocess.check_output(['git', 'show', 'HEAD:' + path], cwd=root).decode('utf-8')

old_module = types.ModuleType('forge.raft_before')
old_module.__package__ = 'forge'
exec(compile(original('asset-forge/forge/artifact.py'), '<original artifact>', 'exec'), old_module.__dict__)
old_spec = json.loads(original('asset-forge/specs/raft.json'))
new_spec, _ = spec.load(root / 'asset-forge/specs/raft.json')
after = pipeline.build(new_spec, 1)
current = pipeline.artifactlib
try:
    pipeline.artifactlib = old_module
    before = pipeline.build(old_spec, 1)
finally:
    pipeline.artifactlib = current
out = root / 'asset-forge/out/artifact'
for label, asset in [('before', before), ('after', after)]:
    render.view(asset.grid, 'iso', scale=2).save(out / f'raft-{label}-comparison.png')
    print(label, tuple(round(n * asset.grid.voxel_m, 3) for n in asset.grid.shape))
