"""Read-only bank-site search from baked distance and exact coarse biome probes.

Results remain candidates: fine terrain, actual depth, and lake/river identity
must be checked at runtime. Positive distance avoids sampling water interiors.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import numpy as np

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'terrain-service'))
from terrain_service import tile_codec as tc


def read_distance_plane(tile):
    with tile.open('rb') as stream:
        h=tc._HEADER.unpack(stream.read(tc._HEADER.size))
        e=tc._V2_EXT.unpack(stream.read(tc._V2_EXT.size))
        if h[0]!=tc.MAGIC or h[1]!=tc.VERSION_V2 or h[5]!=tc.FINE_SCALE:
            raise ValueError('Expected fine V2 tile')
        entries=[tc._SECTION_ENTRY.unpack(stream.read(tc._SECTION_ENTRY.size)) for _ in range(e[-1])]
        if len({s for s,_,_ in entries})!=len(entries):raise ValueError('Duplicate section')
        sections={s:(o,n) for s,o,n in entries}
        header_end=stream.tell()
        blocks=[]
        for sid in (tc.SECTION_PLACE_DIST_WATER_INDEX,tc.SECTION_PLACE_DIST_WATER_DATA):
            offset,length=sections[sid]
            if offset<header_end or offset+length>tile.stat().st_size:raise ValueError('Invalid section extent')
            stream.seek(offset);blocks.append(stream.read(length))
    size=h[-1]//tc.PLACEMENT_SUBSAMPLE
    plane=tc._decode_plane(*blocks,size=size,block_log2=tc.placement_block_log2(h[-1],e[0]),
        codec=e[3],elem_dtype='u1',out_dtype=np.uint8,decompressor=None)
    return h,plane


def search(tile,coarse,center,count):
    h,plane=read_distance_plane(tile)
    yy,xx=np.nonzero((plane>0)&(plane<=8))
    x=(h[3]*h[-1]+xx*tc.PLACEMENT_SUBSAMPLE+2)*1.875
    y=(h[4]*h[-1]+yy*tc.PLACEMENT_SUBSAMPLE+2)*1.875
    order=np.argsort((x-center[0])**2+(y-center[1])**2)
    chosen=[]
    for i in order:
        if any((x[i]-p['x_m'])**2+(y[i]-p['y_m'])**2<250**2 for p in chosen):continue
        px,py=int(x[i]),int(y[i])
        command=[str(ROOT/'build/voxel-core-msvc/bench/Release/vxc_climateprobe.exe'),str(coarse),str(h[2]),'2',str(px),str(py)]
        result=subprocess.run(command,capture_output=True,text=True,check=True)
        site=[line for line in result.stdout.splitlines() if line.startswith('SITE ')]
        if len(site)!=1:raise ValueError('Probe does not support exact point mode')
        chosen.append(dict(x_m=px,y_m=py,distance_water_mm=int(plane[yy[i],xx[i]])*2000,coarse_probe=site[0]))
        if len(chosen)>=count:break
    return dict(scope=__doc__,tile=str(tile.resolve()),candidates=chosen)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('tile',type=Path);p.add_argument('coarse',type=Path)
    p.add_argument('--center',type=int,nargs=2,required=True);p.add_argument('--count',type=int,default=12)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    if not 1<=a.count<=100:raise ValueError('Count must be 1..100')
    report=search(a.tile,a.coarse,a.center,a.count)
    a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
