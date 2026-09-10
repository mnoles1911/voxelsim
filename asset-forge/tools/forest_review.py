"""Reference-family pilot and inventory review sheets (including game pawn)."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
import _path
from forge import forest, pipeline, spec, vxa, render, materials, inventory
from forge.forest_profiles import PROFILES

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/temperate-review'

def elevation(grid, title, *, wood=False, turn=0, width=440, height=440):
    g=render.turned(grid,turn)
    # Orthographic front elevation: one square per cubic voxel, projected
    # with no vertical perspective distortion. Pawn and tree share metres/pixel.
    data=g.data.copy()
    if wood:data[~np.isin(data,[16,17,18,23])]=0
    occ=data>0
    visible=occ.any(axis=1)
    depth=occ.shape[1]-1-np.argmax(occ[:,::-1,:],axis=1)
    ids=data[np.arange(data.shape[0])[:,None],depth,np.arange(data.shape[2])[None,:]]
    colors={0:(226,232,226),16:(100,76,53),17:(128,95,62),18:(92,79,62),19:(72,113,46),20:(42,88,65),21:(56,112,48),23:(209,207,187),24:(217,159,169)}
    palette=np.array([colors.get(i,(83,118,52)) for i in range(256)],dtype=np.uint8)
    ids[~visible]=0
    raw=Image.fromarray(palette[ids].transpose(1,0,2)[::-1])
    scale=min((width-72)/(g.shape[0]*g.voxel_m+1.4),(height-70)/max(g.shape[2]*g.voxel_m,2.0))
    raw=raw.resize((max(1,round(g.shape[0]*g.voxel_m*scale)),max(1,round(g.shape[2]*g.voxel_m*scale))),Image.Resampling.NEAREST)
    canvas=Image.new('RGB',(width,height),(226,232,226));ground=height-36
    canvas.paste(raw,(18,ground-raw.height));d=ImageDraw.Draw(canvas)
    x=24+raw.width+.55*scale
    def box(cx,z,w,h,c):d.rectangle((round(x+(cx-w/2)*scale),round(ground-(z+h)*scale),max(round(x+(cx-w/2)*scale),round(x+(cx+w/2)*scale)),max(round(ground-(z+h)*scale),round(ground-z*scale))),fill=c)
    for cx in [-.11,.11]:box(cx,0,.20,.80,(149,96,55))
    box(0,.80,.40,.70,(65,107,154));box(0,1.52,.30,.28,(220,186,151))
    for cx in [-.23,.23]:box(cx,.75,.14,.70,(149,96,55))
    d.line((12,ground,width-12,ground),fill=(149,164,147))
    d.text((12,10),title,fill=(28,43,31));d.text((12,height-22),f'{g.shape[2]*g.voxel_m:.1f} m | {g.voxel_m*1000:g} mm voxels | pawn 1.80 m',fill=(28,43,31))
    return canvas

def pilot(names, seed):
    OUT.mkdir(parents=True,exist_ok=True)
    images=[]
    for name in names:
        from temperate_collection import body_for
        body=body_for(name)
        a=pipeline.build(body,seed)
        problems=pipeline.health(a)
        dest=OUT/name;dest.mkdir(exist_ok=True)
        vxa.write(a.grid,dest/f'pilot-{seed}.vxa')
        (dest/f'pilot-{seed}.json').write_text(json.dumps(dict(stats=a.stats,problems=problems,generator_digest=inventory.generator_digest(),artifact_hash=inventory.digest(dest/f'pilot-{seed}.vxa')),indent=2))
        im=elevation(a.grid,f'{name} | seed {seed}')
        im.save(dest/f'pilot-{seed}.png');images.append(im)
        elevation(a.grid,f'{name} | structure',wood=True).save(dest/f'wood-{seed}.png')
        render.view(a.grid,'side',target_px=660).save(dest/f'shaded-{seed}.png')
        print(name,a.stats['height_m'],a.stats['voxels'],problems,flush=True)
    if len(images)==1:return
    sheet=Image.new('RGB',(440*4,440*((len(images)+3)//4)),(226,232,226))
    for i,im in enumerate(images):sheet.paste(im,((i%4)*440,(i//4)*440))
    sheet.save(OUT/f'pilots-{seed}.png')

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('names',nargs='*');ap.add_argument('--seed',type=int,default=7);ap.add_argument('--workers',type=int,default=1)
    args=ap.parse_args();names=args.names or list(PROFILES)
    if args.workers==1:pilot(names,args.seed)
    else:
        import concurrent.futures,os
        for key in ['OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS']:os.environ[key]='1'
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            futures={pool.submit(pilot,[name],args.seed):name for name in names}
            for f in concurrent.futures.as_completed(futures):
                try:f.result()
                except Exception as exc:print('FAILED',futures[f],repr(exc),flush=True)
        for start in range(0,len(names),12):
            subset=names[start:start+12];sheet=Image.new('RGB',(440*4,440*((len(subset)+3)//4)),(226,232,226))
            for i,name in enumerate(subset):
                path=OUT/name/f'pilot-{args.seed}.png'
                if path.exists():sheet.paste(Image.open(path),((i%4)*440,(i//4)*440))
            sheet.save(OUT/f'portfolio-{start//12+1}.png')
