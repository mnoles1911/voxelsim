"""Verify goblin anatomy/rig/export; --write refreshes the five library models.

Does not record a human Keep gesture or change any curation verdict.
"""
import argparse
import json
from pathlib import Path
import tempfile
import numpy as np
from scipy import ndimage
import _path  # noqa: F401
from forge import categories, parts, pipeline, render, spec, vox, vxa

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    results=[]
    for path in sorted((ROOT/'specs').glob('goblin-*.json')):
        s,report=spec.load(path)
        assert not report.warnings, report.warnings
        a=pipeline.build(s,1)
        b=pipeline.build(s,1)
        assert np.array_equal(a.grid.data,b.grid.data), path
        assert np.array_equal(a.parts,b.parts), path
        assert a.grid.voxel_m==.0125
        assert not pipeline.health(a), (path,pipeline.health(a))
        assert ndimage.label(a.grid.data!=0)[1]==1, f'{path}: not face-connected'
        assert np.array_equal(a.parts!=0,a.grid.data!=0), f'{path}: missing part tags'
        joints=parts.joints(a.parts)
        assert all(j['origin'] is not None for j in joints), (path,joints)
        expected={parts.P_ARM,parts.P_FOREARM,parts.P_HAND,parts.P_LEG,parts.P_SHIN,parts.P_FOOT}
        assert expected | {pid+parts.SIDE_STRIDE for pid in expected} <= set(map(int,np.unique(a.parts)))
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'roundtrip.vxa'
            vxa.write(a.grid,p,a.parts,joints)
            g,t,j=vxa.read_full(p)
            assert g.voxel_m==a.grid.voxel_m
            assert np.array_equal(g.data,a.grid.data)
            assert np.array_equal(t,a.parts)
            assert len(j)==len(joints)
        row={'name':path.stem,'voxel_cm':1.25,'voxels':a.grid.count(),'joints':len(joints),'face_components':1,'problems':[]}
        results.append(row)
        if args.write:
            entry=f'{path.stem}-0001';d=ROOT/'library'/path.stem/entry
            d.mkdir(parents=True,exist_ok=True)
            spec.save(s,d/'spec.json');spec.save(a.realized,d/'realized.json')
            models=vox.write(a.grid,d/'tree.vox',name=entry)
            vxa.write(a.grid,d/'tree.vxa',a.parts,joints)
            render.view(a.grid,render.camera_for(s),target_px=850).save(d/'thumb.png')
            meta={'id':entry,'species':path.stem,'kind':s['kind'],'category':categories.of(s),'seed':1,'spec_hash':spec.spec_hash(s),'stats':a.stats,'problems':[],'vox_models':models}
            (d/'meta.json').write_text(json.dumps(meta,indent=2)+'\n')
        print(json.dumps(row))
    assert len(results)==5
    if args.write:
        (ROOT/'out'/'goblin-review').mkdir(parents=True,exist_ok=True)
        (ROOT/'out'/'goblin-review'/'verification.json').write_text(json.dumps(results,indent=2)+'\n')
    return 0


if __name__=='__main__':
    raise SystemExit(main())
