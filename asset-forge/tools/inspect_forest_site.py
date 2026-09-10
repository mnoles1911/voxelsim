"""Read a bounded fine-tile crop for site selection, not game ground-fit approval.

Uses the shipping block decoder and cubic reconstruction of elevation control
points. Does not generate tiles or change world identity. The game still owns
exact sampling, residency, collision and placement admission.
"""
import json
import mmap
from pathlib import Path
import numpy as np
from scipy.ndimage import map_coordinates
from terrain_service import tile_codec as tc

ROOT=Path(__file__).resolve().parents[2]
PATH=ROOT/'tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4-b5e821e98/000000000135276f/s16/-11_-6.vxtl'
X,Y=-160980.,-82020.
with PATH.open('rb') as stream, mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as data:
    magic,version,seed,tx,ty,scale,size=tc._HEADER.unpack_from(data)
    assert (magic,version,seed,tx,ty,scale)==(tc.MAGIC,tc.VERSION_V2,20260719,-11,-6,tc.FINE_SCALE)
    edge_log,predictor,quant,codec,bake,flags,base,parent,reserved,nsec=tc._V2_EXT.unpack_from(data,tc._HEADER.size)
    assert predictor==tc.PRED_MED and parent==0 and reserved==tc._ZERO_RESERVED3
    off=tc._HEADER.size+tc._V2_EXT.size
    sections={sid:(start,length) for sid,start,length in (tc._SECTION_ENTRY.unpack_from(data,off+i*tc._SECTION_ENTRY.size) for i in range(nsec))}
    assert all(start+length<=len(data) for start,length in sections.values())
    bs=1<<edge_log;nb=size//bs;px=(X-tx*15360)/1.875;py=(Y-ty*15360)/1.875
    bx0,by0=int(px)//bs-2,int(py)//bs-2
    def crop(ix,dx):
        index_start,_=sections[ix];data_start,data_length=sections[dx]
        payload=memoryview(data)[data_start:data_start+data_length]
        result=np.empty((5*bs,5*bs),dtype=np.int16)
        for oy in range(5):
            for ox in range(5):
                block=(by0+oy)*nb+bx0+ox
                begin=index_start+block*tc._BLOCK_ENTRY.size
                result[oy*bs:(oy+1)*bs,ox*bs:(ox+1)*bs]=tc._decode_plane(data[begin:begin+tc._BLOCK_ENTRY.size],payload,size=bs,block_log2=edge_log,codec=codec,elem_dtype='<i2',out_dtype=np.int16)
        payload.release()
        return result
    elev=crop(tc.SECTION_ELEV_INDEX,tc.SECTION_ELEV_DATA)
    water=crop(tc.SECTION_WATER_INDEX,tc.SECTION_WATER_DATA)
    # Larger than the 16-tree grid and bounded placement retries. The engine's
    # world/core axis transform still needs checking in the actual capture.
    dy,dx=np.mgrid[-25:101:1,-25:101:1]
    rows=py+dy/1.875-by0*bs;cols=px+dx/1.875-bx0*bs
    heights=(map_coordinates(elev.astype(float),[rows,cols],order=3,prefilter=False)*tc.QUANT_MM[quant]+base)/1000
    wet=water[np.rint(rows).astype(int),np.rint(cols).astype(int)]>=0
    report=dict(source=str(PATH),site=[X,Y],bake_version=bake,crop_origin=[bx0*bs,by0*bs],sample_extent_m=[-25,100],
                height_m=[float(heights.min()),float(heights.max())],height_span_m=float(np.ptp(heights)),wet_control_point_fraction=float(wet.mean()),
                limitation='Read-only approximate preflight; not proof of game biome, ground fitting or residency. No water interpolation or procedural amplifier detail evaluated.')
    target=ROOT/'asset-forge/out/tree-runtime-appearance-v1/fine-site-preflight.json'
    target.write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
