"""Verify oak pilot, make matched review sheets, and optionally save its draft.

Historical verification only: python tools/oak_pilot_review.py --verify
Requires oak_pilot.py builds at 50 and 100 mm (seed 7).
"""
import argparse
import hashlib
import json
import shutil

import oak_pilot as oak
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from forge import materials, pipeline, render, spec, vxa


def sheet(images, labels, path, columns=3):
    width,height=480,460
    canvas=Image.new('RGB',(columns*width,((len(images)+columns-1)//columns)*height),(224,230,224))
    for i,(im,label) in enumerate(zip(images,labels)):
        im=im.resize((width,height),Image.Resampling.LANCZOS)
        canvas.paste(im,(i%columns*width,i//columns*height))
        d=ImageDraw.Draw(canvas)
        d.rectangle((i%columns*width,i//columns*height,i%columns*width+width,i//columns*height+30),fill=(224,230,224))
        d.text((i%columns*width+12,i//columns*height+10),label,fill=(25,40,27))
    canvas.save(path)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify',action='store_true')
    parser.add_argument('--install',action='store_true')
    args=parser.parse_args()
    if args.install:
        raise SystemExit("Retired pilot installer. Use Forge with canonical temperate-oak; tools/environment_inventory.py generates its variants.")
    root=oak.OUT
    coarse=vxa.read(root/'seed-7/100mm/tree.vxa')
    fine=vxa.read(root/'seed-7/50mm/tree.vxa')
    if not (root/'baseline/tree.vxa').exists():
        original,_=spec.load(oak.ROOT/'specs/temperate-oak.json')
        (root/'baseline').mkdir(exist_ok=True)
        vxa.write(pipeline.build(original,7).grid,root/'baseline/tree.vxa')
    baseline=vxa.read(root/'baseline/tree.vxa')
    sheet([oak.tile(g,pixels_per_m=38) for g in [baseline,coarse,fine]],
          ['Existing oak / 100 mm','Pilot / 100 mm','Pilot / 50 mm'],root/'comparison.png')
    sheet([oak.tile(g,turn=t) for g in [coarse,fine] for t in range(4)],
          [f'{mm} mm / azimuth {t*90} degrees' for mm in [100,50] for t in range(4)],
          root/'eight-views.png',4)
    sheet([oak.tile(coarse),oak.tile(coarse,wood=True),oak.tile(fine),oak.tile(fine,wood=True)],
          ['100 mm foliage','100 mm woody skeleton','50 mm foliage','50 mm woody skeleton'],
          root/'structure.png',2)
    for grid,mm in [(coarse,100),(fine,50)]:
        for turn in range(4):oak.tile(grid,turn=turn).save(root/f'seed-7/{mm}mm/view-{turn}.png')
        oak.tile(grid,wood=True).save(root/f'seed-7/{mm}mm/wood.png')
    oak.tile(fine).save(root/'beauty-50mm.png')
    saved_report=root/'validation.json'
    report=(json.loads(saved_report.read_text()) if saved_report.exists() else
            {'visual_approved':False,'installed':False,'checks':[]})
    if args.verify:
        report['checks']=[]
        for seed in [7,19]:
            branches,blades=oak.master(seed)
            for pitch in [.1,.05]:
                grid,dropped=oak.raster(branches,blades,pitch)
                wood=grid.material_mask([materials.MAT_BARK,materials.MAT_HEARTWOOD])
                assert ndimage.label(wood)[1]==1
                assert ndimage.label(grid.data>0,structure=np.ones((3,3,3)))[1]==1
                assert np.any(grid.data[:,:,0])
                if seed==7:
                    reference=coarse if pitch==.1 else fine
                    assert np.array_equal(grid.data,reference.data), 'Nondeterministic build'
                    assert np.array_equal(grid.origin,reference.origin)
                report['checks'].append(dict(seed=seed,pitch_mm=round(pitch*1000),
                    voxels=grid.count(),wood_components=1,components=1,dropped=dropped,
                    deterministic_repeat=seed==7))
                print(report['checks'][-1],flush=True)
        # Both grids are independent samplings of the exact same metric master.
        assert np.max(np.abs(np.array(coarse.shape)*.1-np.array(fine.shape)*.05))<=.101
        report['checks'].append('50/100 mm dimensions agree within one coarse cell')
    (root/'validation.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
