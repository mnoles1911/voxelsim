"""Build and inspect the first survival crafting set; never auto-approve it."""
import json
from pathlib import Path
import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw, ImageFont
import _path
from forge import pipeline, render, spec, vxa, vox
from bambooprobe import export_obj

ROOT = Path(__file__).resolve().parents[1]
ITEMS = [
    ('hammerstone', (.175,.15,.125), 'Hammerstone'),
    ('flake_blade', (.225,.125,.075), 'Flake blade'),
    ('stone_knife', (.325,.125,.075), 'Wrapped stone knife'),
    ('fiber_bundle', (.35,.15,.10), 'Prepared plant fibers'),
    ('cordage_coil', (.35,.30,.125), 'Cordage coil'),
    ('wooden_haft', (.725,.30,.125), 'Wooden axe haft'),
    ('stone_axe_head', (.225,.30,.125), 'Stone axe head'),
    ('stone_axe', (.725,.30,.125), 'Lashed stone axe'),
]

def main():
    out=ROOT/'out/survival-starter-12_5mm'
    out.mkdir(parents=True,exist_ok=True)
    results=[]
    sheet=Image.new('RGB',(1800,1140),'#e9e5da')
    d=ImageDraw.Draw(sheet)
    font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',23)
    small=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',17)
    d.text((36,20),'SURVIVAL CRAFTING  /  12.5 mm cubic voxels',font=font,fill='#28322e')
    d.text((36,57),'Draft models and components  •  Individual views enlarged for inspection',font=small,fill='#59605b')
    for index,(form,dimensions,label) in enumerate(ITEMS):
        name='bushcraft-'+form.replace('_','-')
        p=ROOT/'specs'/f'{name}.json'
        if not p.exists():
            body,report=spec.validate(dict(name=name,kind='artifact',category='craftable',
                subcategory=('materials' if form in ('fiber_bundle','cordage_coil') else 'components' if form in ('wooden_haft','stone_axe_head') else 'tools'),
                resolution_cm='1.25',artifact=dict(form=form,length_m=dimensions[0],beam_m=dimensions[1],depth_m=dimensions[2]),
                materials=dict(hull='plume_slate',frame='heartwood',trim='plume_buff',strake='deadwood'),
                curation=dict(status='draft',seeds=[1],notes='First survival crafting set; awaiting visual review.')))
            spec.save(body,p)
        body,report=spec.load(p)
        assert not report.warnings,report.warnings
        asset=pipeline.build(body,1)
        grid=asset.grid
        assert grid.voxel_m==.0125
        assert not pipeline.health(asset),pipeline.health(asset)
        components=ndimage.label(grid.data!=0)[1]
        assert components==1,(form,components)
        assert asset.stats['bridges_added']==0,(form,'unexpected repair')
        repeat=pipeline.build(body,1)
        assert np.array_equal(repeat.grid.data,grid.data)
        base=out/name
        vxa.write(grid,base.with_suffix('.vxa'))
        vox.write(grid,base.with_suffix('.vox'),name=name)
        quads=export_obj(grid,base.with_suffix('.obj'))
        restored=vxa.read(base.with_suffix('.vxa'))
        assert restored.voxel_m==.0125 and np.array_equal(restored.data,grid.data)
        spec.save(body,out/f'{name}-spec.json')
        preview=render.view(grid,'iso',target_px=680,background=(233,229,218,255))
        preview.save(out/f'{name}.png')
        preview.thumbnail((410,380))
        cx=30+(index%4)*445
        cy=110+(index//4)*500
        sheet.paste(preview,(cx+(410-preview.width)//2,cy+(380-preview.height)//2))
        bounds=np.array(grid.data.shape)*grid.voxel_m
        d.text((cx,cy+390),label,font=font,fill='#28322e')
        d.text((cx,cy+426),' × '.join(f'{v:.3f}' for v in bounds)+' m',font=small,fill='#59605b')
        results.append(dict(name=name,voxel_m=grid.voxel_m,bounds_m=bounds.tolist(),
            voxels=int(np.count_nonzero(grid.data)),components=components,quads=quads,health=pipeline.health(asset)))
    sheet.save(out/'survival-starter-contact-sheet.png')
    (out/'validation.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))

if __name__=='__main__': main()
