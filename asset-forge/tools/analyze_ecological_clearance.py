"""Actual tree-voxel clearance on the flat native placement fixture.

Reference body: 0.6m diameter, 1.8m height. Reference building:5x5x3m.
This is conservative horizontal clearance, not the game's movement controller,
slope evaluation, visibility, resource access or real-terrain building approval.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import numpy as np
from scipy import ndimage
import _path
from forge import vxa


def rotated_projection(data,origin,yaw,height_cells,anchor_delta=0):
    ox,oy,oz=map(int,origin);nx,ny,nz=data.shape
    lo=max(0,-oz-anchor_delta);hi=min(nz,height_cells-oz-anchor_delta)
    mask=np.any(data[:,:,lo:hi]!=0,axis=2) if hi>lo else np.zeros((nx,ny),dtype=bool)
    origins=((ox,oy),(-(oy+ny-1),ox),(-(ox+nx-1),-(oy+ny-1)),(oy,-(ox+nx-1)))
    return np.rot90(mask,k=yaw),origins[yaw]


def stamp(destination,mask,x,y):
    x0=max(0,x);y0=max(0,y);x1=min(destination.shape[0],x+mask.shape[0]);y1=min(destination.shape[1],y+mask.shape[1])
    if x1>x0 and y1>y0:destination[x0:x1,y0:y1]|=mask[x0-x:x1-x,y0-y:y1-y]


def analyze(root):
    marker=json.loads((root/'PREVIEW_ONLY.json').read_text());assert marker['preview_only'] is True
    profiles={p['species']:p for p in json.loads((root/'placement.json').read_text())['profiles']}
    names=sorted(marker['species']);slots={n:sorted(profiles[n]['variants'],key=lambda v:v['bank_file']) for n in names}
    samples={}
    with (root/'placement-samples.csv').open(encoding='utf-8-sig',newline='') as stream:
        for row in csv.DictReader(stream):
            row={k:int(v) for k,v in row.items()};name=names[row['species']]
            if profiles[name]['kind']=='tree':samples.setdefault(row['world_seed'],[]).append((row,slots[name][row['bank_slot']]))
    result={'scope':__doc__,'sample_pitch_mm':100,'results':[]};cache={}
    for seed,rows in sorted(samples.items()):
        body=np.zeros((2560,2560),dtype=bool);building=body.copy()
        for row,variant in rows:
            assert row['z_mm']==100000,'This diagnostic requires the flat native fixture'
            key=(variant['id'],row['yaw_quarter'])
            if key not in cache:
                path=root/variant['bank_file'];blob=path.read_bytes()
                assert hashlib.sha256(blob).hexdigest()==variant['geometry_sha256']
                grid=vxa.decode(blob)[0];assert round(grid.voxel_m*1000)==100
                cache[key]=tuple(rotated_projection(grid.data,grid.origin,row['yaw_quarter'],h) for h in (18,30))
            for dest,(mask,(ox,oy)) in zip((body,building),cache[key]):
                stamp(dest,mask,row['x_mm']//100+1280+ox,row['y_mm']//100+1280+oy)
        # Include half a voxel diagonal in the radius to conservatively cover
        # solid voxel boxes, rather than treating their centers as point obstacles.
        walk=ndimage.distance_transform_edt(~body)>(3+2**.5/2)
        walk[:4,:]=False;walk[-4:,:]=False;walk[:,:4]=False;walk[:,-4:]=False
        labels,n=ndimage.label(walk)
        counts=np.bincount(labels.ravel());counts[0]=0
        usable=int(walk.sum())
        pad=~ndimage.maximum_filter(building,size=51,mode='constant',cval=1)
        # Map coordinates are reference-body/footprint centers, not cleared ground.
        result['results'].append({'world_seed':seed,'walkable_center_fraction':usable/walk.size,
            'largest_connected_fraction_of_walkable':float(counts.max()/usable) if usable else 0,
            'walkable_components':int(n),'building_center_fraction':float(pad.mean()),
            'crosses_x':bool((set(labels[4,:])- {0}) & set(labels[-5,:])),
            'crosses_y':bool((set(labels[:,4])- {0}) & set(labels[:,-5]))})
    (root/'clearance-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    print(json.dumps(analyze(parser.parse_args().directory),indent=2))
