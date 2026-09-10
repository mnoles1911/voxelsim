"""Screen actual-terrain 5x5x3m plot candidates against placed tree voxels.

Uses conservative terrain relief bounds and a foundation above their upper
bound. Dryness requires verified live hydrology channels at every footprint
voxel column. Older exports lack this provenance and cannot certify dryness.
This is not building approval:
caves, ground support, player edits and resource access remain separate checks.
"""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
from pathlib import Path
import numpy as np
import _path
from forge import vxa, manifest


def box_hits(data,origin,low,high):
    start=np.maximum(0,np.asarray(low,dtype=np.int64)-origin)
    stop=np.minimum(data.shape,np.asarray(high,dtype=np.int64)-origin)
    if np.any(stop<=start):return False
    return bool(np.any(data[tuple(slice(int(a),int(b)) for a,b in zip(start,stop))]))


def rotated_grid(data,origin,yaw):
    ox,oy,oz=map(int,origin);nx,ny,_=data.shape
    origins=((ox,oy,oz),(-(oy+ny-1),ox,oz),(-(ox+nx-1),-(oy+ny-1),oz),(oy,-(ox+nx-1),oz))
    return np.rot90(data,k=yaw,axes=(0,1)),np.array(origins[yaw],dtype=np.int64)


def runtime_variant(variants,seed_index):
    # AssetBankLibrary::bankGrid uses modulo for legacy layer seed draws.
    # Ecological exact-slot selections already fall within this same range.
    if not variants or seed_index<0:raise ValueError('Invalid runtime bank draw')
    return variants[seed_index%len(variants)]


def captured_bank_names(fixture,run):
    blob=(fixture/'species.vxm').read_bytes()
    expected=run.get('speciesManifestSha256')
    if not expected or hashlib.sha256(blob).hexdigest().upper()!=expected.upper():
        raise ValueError('Missing or changed captured species manifest')
    # bank_id is the wire row index, not a guessed ordering of a sidecar list.
    return [row['name'] for row in manifest.decode(blob)['species']]


def footprint_dry(plot):
    return (plot.get('plot_water_channels_verified')==1
            and plot.get('plot_tested_cells')==2500 and plot.get('plot_water_max_mm')==0)


def analyze(root):
    run=json.loads((root/'run-manifest.json').read_text(encoding='utf-8-sig'))
    config_path=Path(next(a.split('=',1)[1] for a in run['arguments'] if a.startswith('-VoxelEcologyConfig=')))
    if hashlib.sha256(config_path.read_bytes()).hexdigest().upper()!=run['configurationSha256'].upper():
        raise ValueError('Changed placement configuration')
    fixture=config_path.parent
    marker=json.loads((fixture/'PREVIEW_ONLY.json').read_text())
    if marker.get('preview_only') is not True:raise ValueError('Expected isolated test inventory')
    profiles={p['species']:p for p in json.loads(config_path.read_text())['profiles']}
    names=captured_bank_names(fixture,run)
    if len(names)!=len(set(names)) or set(names)!=set(marker['species']) or set(names)!=set(profiles):
        raise ValueError('Captured bank inventory differs from geometry profiles')
    slots={n:sorted(profiles[n]['variants'],key=lambda v:v['bank_file']) for n in names}
    with (root/'terrain-samples.csv').open(encoding='utf-8-sig',newline='') as stream:
        plots=[{k:int(v) for k,v in r.items()} for r in csv.DictReader(stream)]
    if len(plots)!=1024 or len({(r['x_mm'],r['y_mm']) for r in plots})!=1024:raise ValueError('Incomplete plot survey')
    candidates=[]
    for p in plots:
        lo,hi=p['plot_lower_mm'],p['plot_upper_mm']
        if lo==-9223372036854775808 or hi==9223372036854775807:continue
        if p['active']!=1 or hi<lo or hi-lo>500 or p['water_mm']>0:continue
        if not footprint_dry(p):continue
        x,y=p['x_mm']//100,p['y_mm']//100;floor=-(-hi//100)
        candidates.append((p,np.array([x-25,y-25,floor]),np.array([x+25,y+25,floor+30])))
    grouped=defaultdict(list)
    with (root/'terrain-placement.csv').open(encoding='utf-8-sig',newline='') as stream:
        for record in csv.DictReader(stream):
            row={k:int(v) for k,v in record.items()}
            if not row['terrain_lattice']:continue
            if not 0<=row['bank_id']<len(names):raise ValueError('Invalid captured bank id')
            name=names[row['bank_id']]
            if profiles[name]['kind']!='tree':raise ValueError('Unexpected terrain asset kind')
            variant=runtime_variant(slots[name],row['seed_slot'])
            grouped[variant['id']].append((row,variant))
    blocked=np.zeros(len(candidates),dtype=bool)
    for instances in grouped.values():
        variant=instances[0][1];blob=(fixture/variant['bank_file']).read_bytes()
        if hashlib.sha256(blob).hexdigest()!=variant['geometry_sha256']:raise ValueError('Changed tree geometry')
        grid=vxa.decode(blob)[0]
        if round(grid.voxel_m*1000)!=100:raise ValueError('Tree pitch must be 100mm')
        # One source grid at a time; rotations are views, not duplicate grids.
        for row,_ in instances:
            data,offset=rotated_grid(grid.data,grid.origin,row['yaw'])
            origin=offset+np.array([row['x_mm']//100,row['y_mm']//100,row['z_mm']//100])
            for i,(_,lo,hi) in enumerate(candidates):
                if not blocked[i] and box_hits(data,origin,lo,hi):blocked[i]=True
    clear=[dict(x_mm=p['x_mm'],y_mm=p['y_mm'],foundation_base_mm=int(lo[2])*100,
                footprint_water_checked=footprint_dry(p))
        for i,(p,lo,_) in enumerate(candidates) if not blocked[i]]
    report={'scope':__doc__,'survey_points':len(plots),
        'unverified_hydrology_footprints':sum(p.get('plot_water_channels_verified')!=1 for p in plots),
        'terrain_candidates':len(candidates),
        'tree_blocked_candidates':int(blocked.sum()),'tree_clear_candidates':clear,
        'approved_build_sites':False}
    (root/'plot-analysis.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path)
    result=analyze(p.parse_args().directory)
    print(json.dumps({k:v for k,v in result.items() if k!='tree_clear_candidates'},indent=2))
