"""Read-only distance-field continuity audit; flags do not identify their cause.

Adjacent 7.5 m placement cells measuring the same water set should differ by
at most four 2 m quantization steps. Unknown/saturated values remain ambiguous.
This does not establish lake identity, water depth, or shoreline correctness.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from find_ecological_shore_sites import read_distance_plane


def compare(a,b):
    a=np.asarray(a,dtype=np.int16);b=np.asarray(b,dtype=np.int16)
    if a.shape!=b.shape:raise ValueError('Mismatched edges')
    known=(a!=255)&(b!=255)
    delta=np.abs(a-b)
    return dict(samples=int(a.size),known_pairs=int(known.sum()),
        discontinuities=int(((delta>4)&known).sum()),
        maximum_known_difference_mm=int(delta[known].max())*2000 if known.any() else None,
        ambiguous_pairs=int((~known).sum()),
        unknown_next_to_near_water=int((((a==255)&(b<251))|((b==255)&(a<251))).sum()))


def audit(paths):
    edges={}
    for path in paths:
        h,p=read_distance_plane(path)
        key=(h[3],h[4])
        if key in edges:raise ValueError('Duplicate tile coordinates')
        edges[key]=(h[2],h[-1],p[0,:].copy(),p[-1,:].copy(),p[:,0].copy(),p[:,-1].copy())
    results=[]
    for (x,y),a in sorted(edges.items()):
        for neighbor,side,ai,bi in [((x+1,y),'east',5,4),((x,y+1),'north',3,2)]:
            if neighbor not in edges:continue
            b=edges[neighbor]
            if a[:2]!=b[:2]:raise ValueError('Incompatible seed or size')
            results.append(dict(tile=[x,y],neighbor=list(neighbor),edge=side,**compare(a[ai],b[bi])))
    return dict(scope=__doc__,edges=results,
        discontinuities=sum(r['discontinuities'] for r in results),
        ambiguous_pairs=sum(r['ambiguous_pairs'] for r in results))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('tiles',nargs='+',type=Path)
    p.add_argument('--output',required=True,type=Path);a=p.parse_args()
    result=audit(a.tiles);a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
