"""Reference-directed oak pilot: branch hierarchy and attached leafy shoots.

Run from asset-forge: python tools/oak_pilot.py --pitch 100 --seed 7
Works in metres until rasterization. Does not change production generators.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import time
from pathlib import Path

import _path  # noqa: F401
import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw
from forge import materials, render, vxa, vox
from forge.grid import VoxelGrid

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/oak-pilot'


def unit(v):
    return v / max(np.linalg.norm(v), 1e-9)


def master(seed):
    """Return connected wood curves and leaf-group blades, all in metres.

    A blade is a small group of leaves at this pitch, never a canopy volume.
    Every blade starts on a woody shoot. Randomness is independent of pitch.
    """
    rng = np.random.default_rng(seed)
    branches, blades = [], []

    def curve(start, end, r0, r1, bend=.12):
        delta = end - start
        side = unit(np.cross(delta, [0., 0., 1.]))
        wobble = side * rng.uniform(-bend, bend)
        ps = np.array([start + delta*t + math.sin(math.pi*t)*
                       (wobble + np.array([0, 0, -bend]))
                       for t in np.linspace(0, 1, 7)])
        branches.append((ps, np.linspace(r0, r1, len(ps))))
        return ps

    trunk = curve(np.zeros(3), np.array([.18, -.12, 3.7]), .64, .24, .1)
    for i in range(7):
        angle = i * math.tau/7 + rng.uniform(-.2,.2)
        curve(np.array([0.,0.,.45]),
              np.array([math.cos(angle)*rng.uniform(1.,1.6),
                        math.sin(angle)*rng.uniform(1.,1.6), .025]), .23, .025, .1)

    # Scaffold ends occupy an uneven dome. Successive branches emerge at
    # different heights and angles rather than a stack of radial whorls.
    for i in range(22):
        phase = i * 2.39996323 + rng.uniform(-.18,.18)
        radial = np.array([math.cos(phase), math.sin(phase), 0.])
        lateral = np.array([-radial[1], radial[0], 0.])
        level = min(i,17) / 17
        reach = 3.65 * math.sqrt(1 - .88 * level**2)
        end = radial * reach + np.array([.15, -.1, 3.6 + level*3.9])
        if i >= 18:
            end = radial * .25 + np.array([.15,-.1,7.9])
        end += rng.normal(0,.17,3)
        attach = trunk[min(6, 3 + i//5)]
        arm = curve(attach, end, .24*(1-.35*level), .085, .35)
        for j in range(8):
            angle = phase + rng.uniform(-1.25,1.25)
            outward = np.array([math.cos(angle),math.sin(angle),0.])
            anchor = arm[3 + j%4]
            tip = end + outward*rng.uniform(.65,1.6) + lateral*rng.uniform(-.65,.65)
            tip[2] += rng.uniform(-.15,1.1)
            secondary = curve(anchor, tip, .080, .025, .16)
            for k in range(7):
                az = angle + rng.uniform(-1.65,1.65)
                d = unit(np.array([math.cos(az), math.sin(az),rng.uniform(-.25,.55)]))
                base = secondary[3 + k%4]
                twig_end = base + d*rng.uniform(.65,1.25)
                twig = curve(base, twig_end, .025, .009, .09)
                for n in range(4):
                    base2 = twig[2+n]
                    # Alternate lateral shoots and leave the axial tip leafy.
                    side = unit(np.cross(d,[0.,0.,1.])) * (1 if n%2 else -1)
                    sd = unit(d*.7 + side*rng.uniform(.3,.85) + np.array([0,0,.3]))
                    shoot_end = base2 + sd*rng.uniform(.36,.65)
                    curve(base2,shoot_end,.009,.003, .025)
                    cross = unit(np.cross(sd,[0.,0.,1.]))
                    for q,t in enumerate(np.linspace(.12,1,7)):
                        p = base2 + (shoot_end-base2)*t
                        leaf_d = unit(sd*.4 + cross*(1 if q%2 else -1) +
                                      np.array([0,0,rng.uniform(-.15,.3)]))
                        length = rng.uniform(.30,.49) * (1.05-.22*t)
                        blades.append((p, leaf_d, length, rng.uniform(.13,.21),
                                       rng.uniform(.055,.10), float(rng.uniform(0,math.tau))))
    return branches, blades


def raster(branches, blades, pitch):
    points = np.concatenate([p for p,_ in branches])
    origin = np.floor((points.min(0)-.85)/pitch).astype(int)
    origin[2] = 0
    high = np.ceil((points.max(0)+.85)/pitch).astype(int)
    grid = VoxelGrid(tuple(high-origin+1), origin, pitch)
    # Fine shoots are narrower than either permitted cell. Their occupied
    # cells represent stem + leaves together, not 100 mm brown timber.
    for ps, rs in sorted(branches, key=lambda branch: branch[1][0] > .025):
        mat = materials.MAT_BARK if rs[0] > .025 else materials.MAT_LEAF_BROADLEAF
        for i in range(1,len(ps)):
            grid.capsule(ps[i-1]/pitch-origin,ps[i]/pitch-origin,
                         rs[i-1]/pitch,rs[i]/pitch,mat,
                         core_mat=materials.MAT_HEARTWOOD if mat==materials.MAT_BARK else None)
    wood = grid.material_mask([materials.MAT_BARK,materials.MAT_HEARTWOOD])
    for p,d,length,width,thick,phase in blades:
        cross = unit(np.cross(d,[0.,0.,1.]))
        normal = unit(np.cross(d,cross))
        mid = p + d*length*.5
        bound = length*.6 + width + thick
        lo = np.maximum(0,np.floor((mid-bound)/pitch-origin).astype(int))
        hi = np.minimum(grid.shape,np.ceil((mid+bound)/pitch-origin).astype(int)+1)
        coords = np.indices(tuple(hi-lo)).reshape(3,-1).T+lo
        rel = (coords+origin+.5)*pitch-p
        along = rel@d / length
        lateral = rel@cross
        depth = rel@normal
        # Tapered, lobed thin blade. Connected midrib is explicit at both pitches.
        profile = np.maximum(0,np.sin(np.pi*np.clip(along,0,1)))**.65
        lobes = .83+.17*np.cos(along*math.pi*6+phase)
        mask = ((along>=0)&(along<=1)&(np.abs(lateral)<=width*profile*lobes)&
                (np.abs(depth)<=max(thick*.5,pitch*.52)))
        cs = coords[mask]
        grid._write(*cs.T,materials.MAT_LEAF_BROADLEAF,True)
        grid.line(p/pitch-origin,(p+d*length*.92)/pitch-origin,
                  materials.MAT_LEAF_BROADLEAF,only_air=True)
    # Only discard voxelization slivers, with an explicit accounting gate.
    labels, count = ndimage.label(grid.data>0,structure=np.ones((3,3,3)))
    sizes = np.bincount(labels.ravel()); sizes[0]=0
    drop = (labels!=sizes.argmax())&(grid.data>0)
    dropped = int(drop.sum())
    assert not np.any(drop & (wood>0)), 'Disconnected wood'
    grid.data[drop]=0
    assert dropped < max(8,grid.count()*.005), 'Excess foliage cleanup'
    return grid.crop(),dropped


def tile(grid, wood=False, turn=0, pixels_per_m=55):
    g = VoxelGrid(grid.shape,grid.origin,grid.voxel_m)
    g.data = grid.data.copy()
    if wood:g.data[g.data==materials.MAT_LEAF_BROADLEAF]=0
    g = render.turned(g,turn)
    scale=3 if grid.voxel_m==.05 else 6
    im = render.view(g,'broad',scale=scale,tilt_deg=0,
                     background=(224,230,224,255))
    # True elevation: no depth contributes to height. Derive metres/pixel
    # from the renderer's voxel sprite, not from the silhouette bounds.
    pixels=np.asarray(im)[:,:,:3]
    yy,xx=np.nonzero(np.any(pixels!=[224,230,224],axis=2))
    im=im.crop((int(xx.min()),int(yy.min()),int(xx.max()+1),int(yy.max()+1)))
    factor=grid.voxel_m*pixels_per_m/(2*scale)
    im=im.resize((round(im.width*factor),round(im.height*factor)),Image.Resampling.LANCZOS)
    canvas=Image.new('RGB',(960,920),(224,230,224))
    canvas.paste(im,((960-im.width)//2,870-im.height))
    ImageDraw.Draw(canvas).text((25,20),f'Oak pilot | {grid.voxel_m*1000:g} mm | '+
                               ('branch structure' if wood else f'view {turn+1}'),fill=(25,40,27))
    player_reference(canvas,pixels_per_m)
    return canvas


def player_reference(canvas,pixels_per_m):
    """Front elevation of the standing game proxy, not part of tree geometry.

    VoxelMovementTuning.h: height 180 UU, width 60 UU; 1 UU = .01 m.
    VoxelProxyBody.cpp: head 30x28, torso 40x70, arms 14x70,
    legs 20x80 UU. Positions below are converted to height above feet.
    """
    d=ImageDraw.Draw(canvas)
    x,ground=480+3.6*pixels_per_m,870
    def box(cx,bottom,width,height,color):
        bounds=(round(x+(cx-width/2)*pixels_per_m),round(ground-(bottom+height)*pixels_per_m),
                round(x+(cx+width/2)*pixels_per_m)-1,round(ground-bottom*pixels_per_m)-1)
        d.rectangle(bounds,fill=color)
    d.line((400,ground,x+65,ground),fill=(140,156,144),width=1)
    limb=(196,149,108);head=(237,218,196);torso=(124,160,196)
    box(-.11,0,.20,.80,limb);box(.11,0,.20,.80,limb)
    box(0,.80,.40,.70,torso)
    box(-.23,.75,.14,.70,limb);box(.23,.75,.14,.70,limb)
    box(0,1.52,.30,.28,head)
    bx=round(x+.48*pixels_per_m);top=round(ground-1.80*pixels_per_m)
    d.line((bx,top,bx,ground),fill=(72,87,77),width=1)
    for y in [top,ground]:d.line((bx-4,y,bx+4,y),fill=(72,87,77),width=1)
    d.text((round(x)-36,ground+10),'Player 1.80 m',fill=(25,40,27))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pitch',type=int,choices=[50,100],default=100)
    p.add_argument('--seed',type=int,default=7)
    p.add_argument('--views',action='store_true')
    args=p.parse_args()
    out=OUT/f'seed-{args.seed}/{args.pitch}mm';out.mkdir(parents=True,exist_ok=True)
    start=time.perf_counter()
    branches,blades=master(args.seed)
    grid,dropped=raster(branches,blades,args.pitch/1000)
    wood=grid.material_mask([materials.MAT_BARK,materials.MAT_HEARTWOOD])
    assert ndimage.label(wood)[1]==1
    assert ndimage.label(grid.data>0,structure=np.ones((3,3,3)))[1]==1
    vxa.write(grid,out/'tree.vxa')
    restored=vxa.read(out/'tree.vxa')
    assert np.array_equal(restored.data,grid.data)
    assert np.array_equal(restored.origin,grid.origin) and restored.voxel_m==grid.voxel_m
    vox.write(grid,out/'tree.vox')
    for turn in range(4 if args.views else 1):
        tile(grid,turn=turn).save(out/f'view-{turn}.png')
    tile(grid,wood=True).save(out/'wood.png')
    stats=dict(seed=args.seed,voxel_mm=args.pitch,dimensions_m=(np.array(grid.shape)*grid.voxel_m).tolist(),
               voxels=grid.count(),wood_voxels=int(wood.sum()),branch_curves=len(branches),
               leaf_groups=len(blades),wood_components=1,components=1,orphan_voxels_removed=dropped,
               ground_contact=int(np.count_nonzero(grid.data[:,:,0])),
               sha256=hashlib.sha256((out/'tree.vxa').read_bytes()).hexdigest(),
               elapsed_seconds=round(time.perf_counter()-start,2),visual_approved=False,
               approach='connected curved scaffold, secondary limbs, twigs, alternate leafy shoots; tapered leaf-group blades')
    (out/'report.json').write_text(json.dumps(stats,indent=2)+'\n')
    print(json.dumps(stats),flush=True)


if __name__=='__main__':main()
