"""Top-down occupied-voxel projection of actual placed understory.

Uses a 25mm XY raster with anchors floored to that raster (<25mm shift per
axis). Includes every occupied voxel above/below the anchor, irrespective of
occlusion or leaf cutouts. This is footprint coverage, not rendered visibility,
ground shading, collision or terrain support. Intersecting external anchors
are retained to avoid clipping plants at the survey boundary.
"""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
from pathlib import Path
import numpy as np
import _path
from forge import vxa
from analyze_ecological_plots import captured_bank_names,runtime_variant
from analyze_ecological_clearance import stamp


def projection(data,origin,yaw):
    ox,oy,_=map(int,origin);nx,ny,_=data.shape
    offsets=((ox,oy),(-(oy+ny-1),ox),(-(ox+nx-1),-(oy+ny-1)),(oy,-(ox+nx-1)))
    return np.rot90(np.any(data!=0,axis=2),k=yaw),offsets[yaw]


def analyze(root):
    run=json.loads((root/'run-manifest.json').read_text(encoding='utf-8-sig'))
    fixture=Path(next(a.split('=',1)[1] for a in run['arguments'] if a.startswith('-VoxelAssetDir=')))
    config=(fixture/'placement.json').read_bytes()
    if hashlib.sha256(config).hexdigest().upper()!=run['configurationSha256'].upper():raise ValueError('Changed ecological configuration')
    names=captured_bank_names(fixture,run)
    profiles={p['species']:p for p in json.loads(config)['profiles']}
    slots={n:sorted(p['variants'],key=lambda v:v['bank_file']) for n,p in profiles.items()}
    with (root/'terrain-samples.csv').open(encoding='utf-8-sig') as f:terrain=list(csv.DictReader(f))
    xs=sorted({int(r['x_mm']) for r in terrain});ys=sorted({int(r['y_mm']) for r in terrain})
    if len(terrain)!=1024 or len(xs)!=32 or len(ys)!=32 or any(b-a!=8000 for axis in (xs,ys) for a,b in zip(axis,axis[1:])):
        raise ValueError('Expected 256m native survey')
    x0,y0=xs[0]-4000,ys[0]-4000
    grouped=defaultdict(list)
    with (root/'terrain-placement.csv').open(encoding='utf-8-sig') as f:
        for row in csv.DictReader(f):
            r={k:int(v) for k,v in row.items()}
            if r['terrain_lattice']:continue
            if not 0<=r['bank_id']<len(names):raise ValueError('Invalid bank')
            name=names[r['bank_id']];p=profiles[name]
            variant=runtime_variant(slots[name],r['seed_slot'])
            grouped[p['kind'],variant['id']].append((r,variant))
    union=np.zeros((10240,10240),dtype=bool)
    coverage=[]
    for kind in sorted({k for k,_ in grouped}):
        layer=np.zeros_like(union)
        for (asset_kind,_),instances in grouped.items():
            if asset_kind!=kind:continue
            variant=instances[0][1];blob=(fixture/variant['bank_file']).read_bytes()
            if hashlib.sha256(blob).hexdigest()!=variant['geometry_sha256']:raise ValueError('Changed geometry')
            grid=vxa.decode(blob)[0]
            if round(grid.voxel_m*1000000)!=25000:raise ValueError('Expected 25mm understory')
            rotations=[projection(grid.data,grid.origin,yaw) for yaw in range(4)]
            for r,_ in instances:
                mask,(ox,oy)=rotations[r['yaw']]
                stamp(layer,mask,(r['x_mm']-x0)//25+ox,(r['y_mm']-y0)//25+oy)
        coverage.append(dict(kind=kind,projected_coverage_fraction=float(layer.mean())))
        union|=layer
    result=dict(scope=__doc__,survey_area_m2=65536,voxel_pitch_mm=25,
        species_manifest_sha256=run['speciesManifestSha256'],by_kind=coverage,
        union_coverage_fraction=float(union.mean()))
    (root/'understory-coverage.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path)
    print(json.dumps(analyze(p.parse_args().capture),indent=2))
