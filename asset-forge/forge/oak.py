"""Versioned branch-and-shoot oak generator. No services or external models."""
from __future__ import annotations
import math
import time
import numpy as np
from scipy import ndimage
from . import materials
from .grid import VoxelGrid

VERSION = "oak-shoots-v1"
SIZES = ("small", "medium", "large")
FORMS = ("open", "woodland", "edge")


def validate_recipe(recipe, kind):
    if kind != "tree" or not isinstance(recipe,dict):
        raise ValueError("plant_recipe requires a tree and an object")
    if set(recipe)-{"generator","size","form"}:
        raise ValueError("Unknown plant_recipe field")
    if recipe.get("generator") != VERSION:
        raise ValueError("Unsupported plant_recipe generator")
    size,form=recipe.get("size","mixed"),recipe.get("form","mixed")
    if size not in (*SIZES,"mixed") or form not in (*FORMS,"mixed"):
        raise ValueError("Invalid oak size/form")
    return dict(generator=VERSION,size=size,form=form)


def build(spec,seed,pitch,connectivity=True):
    from .pipeline import Asset
    from .spec import spec_hash
    start=time.perf_counter()
    recipe=validate_recipe(spec["plant_recipe"],spec.get("kind"))
    size=recipe["size"] if recipe["size"]!="mixed" else SIZES[((seed-1)//3)%3]
    form=recipe["form"] if recipe["form"]!="mixed" else FORMS[(seed-1)%3]
    rng=np.random.default_rng([seed,81073])
    base={"small":4.6,"medium":9.5,"large":14.}[size]
    height=base*rng.uniform(.85,1.15)*float(spec.get("height_m",14))/14
    width=height*{"open":1.28,"woodland":.65,"edge":.94}[form]*rng.uniform(.88,1.12)
    radius={"small":.19,"medium":.48,"large":.73}[size]*rng.uniform(.86,1.14)
    settings=dict(size=size,form=form,height_m=height,width_m=width,trunk_radius_m=radius)
    branches,blades=master(seed,settings)
    grow=time.perf_counter()
    grid,dropped=raster(branches,blades,pitch,wood_cutoff=.025*radius/.64+1e-10)
    wood=grid.material_mask([materials.MAT_BARK,materials.MAT_HEARTWOOD])
    wood_fraction=grid.component_fraction(wood,connectivity=1) if connectivity else 1.
    attached=grid.component_fraction(connectivity=3) if connectivity else 1.
    dims=np.array(grid.shape)*pitch
    stats=dict(seed=seed,spec_hash=spec_hash(spec),kind="tree",generator=VERSION,
        size_class=size,growth_form=form,nodes=sum(len(p) for p,_ in branches),
        segments=sum(len(p)-1 for p,_ in branches),max_order=4,clumps=len(blades),
        branch_curves=len(branches),voxel_cm=pitch*100,height_m=round(float(dims[2]),3),
        length_m=round(float(dims[0]),3),footprint_m=dims[:2].tolist(),extent_vox=list(grid.shape),
        voxels=grid.count(),by_material=grid.histogram(),wood_connected=wood_fraction,
        wood_detached=round(int(wood.sum())*(1-wood_fraction)),attached_frac=attached,
        detached=round(grid.count()*(1-attached)),ground_contact=int(np.count_nonzero(grid.data[:,:,0])),
        orphans_removed=dropped,grid_mb=round(grid.data.nbytes/1e6,2),
        ms_grow=round((grow-start)*1000,1),ms_total=round((time.perf_counter()-start)*1000,1))
    live=dict(spec,plant_recipe=dict(generator=VERSION,size=size,form=form))
    live["height_m"]=height
    return Asset(grid=grid,skeleton=None,spec=spec,seed=seed,realized=live,stats=stats)

def unit(v):
    return v / max(np.linalg.norm(v), 1e-9)


def master(seed, settings=None):
    """Return connected wood curves and leaf-group blades, all in metres.

    A blade is a small group of leaves at this pitch, never a canopy volume.
    Every blade starts on a woody shoot. Randomness is independent of pitch.
    """
    rng = np.random.default_rng(seed)
    leaf_rng = np.random.default_rng([seed, 29173])
    settings = settings or {}
    stage = settings.get("size", "medium")
    counts = {"small": (10,4,4,3), "medium": (18,6,6,4), "large": (22,8,7,4)}[stage]
    arms, secondaries, twigs, shoots = counts
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
    for i in range(arms):
        phase = i * 2.39996323 + rng.uniform(-.18,.18)
        radial = np.array([math.cos(phase), math.sin(phase), 0.])
        lateral = np.array([-radial[1], radial[0], 0.])
        level = min(i,arms-5) / (arms-5)
        reach = 3.65 * math.sqrt(1 - .88 * level**2)
        end = radial * reach + np.array([.15, -.1, 3.6 + level*3.9])
        if i >= arms-4:
            end = radial * .25 + np.array([.15,-.1,7.9])
        end += rng.normal(0,.17,3)
        attach = trunk[min(6, 3 + i*3//max(arms-1,1))]
        arm = curve(attach, end, .24*(1-.35*level), .085, .35)
        for j in range(secondaries):
            angle = phase + rng.uniform(-1.25,1.25)
            outward = np.array([math.cos(angle),math.sin(angle),0.])
            anchor = arm[3 + j%4]
            tip = end + outward*rng.uniform(.65,1.6) + lateral*rng.uniform(-.65,.65)
            tip[2] += rng.uniform(-.15,1.1)
            secondary = curve(anchor, tip, .080, .025, .16)
            for k in range(twigs):
                az = angle + rng.uniform(-1.65,1.65)
                d = unit(np.array([math.cos(az), math.sin(az),rng.uniform(-.25,.55)]))
                base = secondary[3 + k%4]
                twig_end = base + d*rng.uniform(.65,1.25)
                twig = curve(base, twig_end, .025, .009, .09)
                for n in range(shoots):
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
                                      np.array([0,0,leaf_rng.uniform(-.15,.3)]))
                        length = leaf_rng.uniform(.30,.49) * (1.05-.22*t)
                        blades.append((p, leaf_d, length, leaf_rng.uniform(.13,.21),
                                       leaf_rng.uniform(.055,.10), float(leaf_rng.uniform(0,math.tau))))
    # Allometry is applied to the metric skeleton, never to baked voxels.
    # Leaves retain physical dimensions while branch count changes by stage.
    height = settings.get("height_m", 9.9)
    width = settings.get("width_m", 12.)
    form = settings.get("form", "open")
    raw = np.concatenate([ps for ps,_ in branches])
    zscale = (height-.4)/raw[:,2].max()
    xy = (width-.8)/max(np.ptp(raw[:,0]),np.ptp(raw[:,1]))
    clear = {"open": 1., "woodland": 1.65, "edge":1.25}[form]
    def transform(ps):
        result=ps.copy()
        result[:,:2] *= xy
        z=ps[:,2]
        # Raised bole compresses crown depth without altering overall height.
        result[:,2]=z*zscale + (clear-1)*zscale*np.minimum(z,2.2)*np.maximum(0,1-z/10)
        if form=="edge": result[:,0] += .18*result[:,2]
        return result
    radial = settings.get("trunk_radius_m",.48)/.64
    branches=[(transform(ps),rs*radial) for ps,rs in branches]
    rebuilt=[]
    for p,d,length,width,thick,phase in blades:
        endpoints=transform(np.array([p,p+d*.1]))
        rebuilt.append((endpoints[0],unit(endpoints[1]-endpoints[0]),length,width,thick,phase))
    return branches, rebuilt


def raster(branches, blades, pitch, wood_cutoff=.025):
    points = np.concatenate([p for p,_ in branches])
    origin = np.floor((points.min(0)-.85)/pitch).astype(int)
    origin[2] = 0
    high = np.ceil((points.max(0)+.85)/pitch).astype(int)
    grid = VoxelGrid(tuple(high-origin+1), origin, pitch)
    # Fine shoots are narrower than either permitted cell. Their occupied
    # cells represent stem + leaves together, not 100 mm brown timber.
    for ps, rs in sorted(branches, key=lambda branch: branch[1][0] > wood_cutoff):
        mat = materials.MAT_BARK if rs[0] > wood_cutoff else materials.MAT_LEAF_BROADLEAF
        for i in range(1,len(ps)):
            grid.capsule(ps[i-1]/pitch-origin,ps[i]/pitch-origin,
                         rs[i-1]/pitch,rs[i]/pitch,mat,
                         core_mat=materials.MAT_HEARTWOOD if mat==materials.MAT_BARK else None)
    wood = grid.material_mask([materials.MAT_BARK,materials.MAT_HEARTWOOD])
    for p,d,length,width,thick,phase in blades:
        cross = unit(np.cross(d,[0.,0.,1.]))
        normal = unit(np.cross(d,cross))
        mid = p + d*length*.5
        bound = np.abs(d)*length*.52 + np.abs(cross)*width + np.abs(normal)*max(thick*.5,pitch*.52) + pitch
        lo = np.maximum(0,np.floor((mid-bound)/pitch-origin).astype(int))
        hi = np.minimum(grid.shape,np.ceil((mid+bound)/pitch-origin).astype(int)+1)
        if np.any(hi<=lo):
            continue  # A hanging blade can lie entirely below the ground plane.
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

