"""Isolated environment LOD pilot. Never writes production specs or banks."""
import json
import time
import sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from forge import spec, pipeline, vxa, lod, render
from forge.grid import VoxelGrid

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/environment-lod-prototype'
CASES=[('temperate-oak',5),('granite-boulder',5),('bramble-thicket',2.5),('meadow-daisy',2.5)]

def aligned_reduce(grid):
    # Align blocks to the object's coordinate origin, not its cropped minimum.
    # This keeps all LODs on the same physical lattice, including negative axes.
    low=np.mod(grid.origin,2)
    padded=np.pad(grid.data,[(int(v),0) for v in low])
    data=lod.reduce_grid(padded,2)
    result=VoxelGrid(data.shape,(grid.origin-low)//2,grid.voxel_m*2)
    result.data=data
    return result

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    report=[]
    for name,cm in CASES:
        body,_=spec.load(ROOT/'specs'/f'{name}.json')
        t=time.perf_counter()
        asset=pipeline.build(body,1,resolution_cm=cm,environment_lod_prototype=True)
        grid=asset.grid
        row={'name':name,'seed':1,'source_build_seconds':time.perf_counter()-t,
             'source_spec_hash':spec.spec_hash(body),'source_stats':asset.stats,'levels':[]}
        for level in range(3):
            mm=round(grid.voxel_m*1000)
            path=OUT/f'{name}-{mm}mm.vxa'
            vxa.write(grid,path)
            roundtrip=vxa.read(path)
            assert np.array_equal(roundtrip.data,grid.data) and roundtrip.voxel_m==grid.voxel_m
            occ=grid.data!=0
            padded=np.pad(occ,1)
            faces=sum(int(np.count_nonzero(occ & ~padded[tuple(slice(1+d,1+d+s) if a==axis else slice(1,1+s) for a,s in enumerate(occ.shape))])) for axis in range(3) for d in (-1,1))
            row['levels'].append({'pitch_mm':mm,'shape':list(grid.shape),'origin':grid.origin.tolist(),
                'solid_voxels':int(occ.sum()),'exposed_faces':faces,'dense_bytes':grid.data.nbytes,'vxa_bytes':path.stat().st_size})
            render.view(grid,'iso',target_px=900).save(OUT/f'{name}-{mm}mm.png')
            if mm==100:break
            grid=aligned_reduce(grid)
        report.append(row)
        print(name,[(x['pitch_mm'],x['solid_voxels'],x['exposed_faces']) for x in row['levels']],flush=True)
    (OUT/'report.json').write_text(json.dumps(report,indent=2,default=lambda v:v.item() if hasattr(v,'item') else str(v)))

if __name__=='__main__':main()
