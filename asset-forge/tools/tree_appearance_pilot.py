"""Isolated appearance studies on exact saved voxels; never rewrite source assets."""
import json
import struct
import time
from pathlib import Path
if not __package__:
    import _path
import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree
from forge import inventory, materials, vxa, oak, forest
from forge.forest_profiles import PROFILES as TREE_PROFILES

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'web/public/tree-appearance-pilot'
PROFILES={
 'temperate-oak':('70634F','526F35','81954C','ridged'),
 'american-beech':('94968B','638345','96AC67','smooth'),
 'birch':('D0CDBB','74934B','A5B865','birch'),
 'sugar-maple':('797465','587B39','90A650','plates'),
 'scots-pine':('755740','486D51','7F965F','pine'),
 'western-red-cedar':('805C48','416447','728450','fibrous'),
 'weeping-willow':('797762','809351','A8B573','ridged'),
 'cherry-blossom':('69574F','668342','94A55C','cherry'),
}

def rgb(h):return np.array([int(h[i:i+2],16) for i in (0,2,4)],dtype=float)
def noise(c,seed):
    c=np.asarray(c,dtype=np.int64)
    h=(c[:,0]*73856093)^(c[:,1]*19349663)^(c[:,2]*83492791)^int(seed*193)
    h=(h^(h>>13))*1274126177
    return ((h^(h>>16))&65535)/65535

def skeleton(name,seed,body):
    recipe=body['plant_recipe'];size=recipe['size'];form=recipe['form']
    if size=='mixed':size=oak.SIZES[((seed-1)//3)%3]
    if form=='mixed':form=oak.FORMS[(seed-1)%3]
    if name=='temperate-oak':
        rng=np.random.default_rng([seed,81073])
        height={'small':4.6,'medium':9.5,'large':14.}[size]*rng.uniform(.85,1.15)*float(body.get('height_m',14))/14
        width=height*{'open':1.28,'woodland':.65,'edge':.94}[form]*rng.uniform(.88,1.12)
        radius={'small':.19,'medium':.48,'large':.73}[size]*rng.uniform(.86,1.14)
        return oak.master(seed,dict(size=size,form=form,height_m=height,width_m=width,trunk_radius_m=radius))
    rng=np.random.default_rng([seed,937])
    height=float(body['height_m'])*{'small':.32,'medium':.66,'large':1.}[size]*rng.uniform(.87,1.10)
    profile=TREE_PROFILES[name]
    if profile.architecture=='oak':
        return oak.master(seed,dict(size=size,form=form,height_m=height,width_m=height*profile.width,trunk_radius_m=height*.040))
    return forest.architecture(TREE_PROFILES[name],seed,height,size,form)

def branch_coordinates(branches,points):
    centres=[];directions=[];lengths=[];radii=[];ids=[]
    for bid,(ps,rs) in enumerate(branches):
        if max(rs)<.04:continue
        delta=np.diff(ps,axis=0);length=np.linalg.norm(delta,axis=1)
        along=0.
        for i,(d,n) in enumerate(zip(delta,length)):
            if n<1e-8:continue
            t=np.linspace(0,1,max(2,int(n/.07)+1))
            centres.extend(ps[i]+t[:,None]*d);directions.extend(np.tile(d/n,(len(t),1)))
            lengths.extend(along+t*n);radii.extend([max(.04,float(rs[0]))]*len(t));ids.extend([bid]*len(t));along+=n
    centres=np.asarray(centres);_,ix=cKDTree(centres).query(points,workers=2)
    direction=np.asarray(directions)[ix];radial=points-centres[ix]
    u=np.cross(direction,[0.,1.,0.]);bad=np.linalg.norm(u,axis=1)<.01;u[bad]=np.cross(direction[bad],[1.,0.,0.])
    u/=np.linalg.norm(u,axis=1,keepdims=True);v=np.cross(direction,u)
    angle=np.arctan2(np.sum(radial*v,axis=1),np.sum(radial*u,axis=1))
    return angle,np.asarray(lengths)[ix],np.asarray(radii)[ix],np.asarray(ids)[ix]

def build(name,seed,source=None,output=None,palette=None,include_interior=False):
    ident=f'{name}-{seed:04d}'
    if source is None:
        source=ROOT/'library'/name/ident
        if not source.exists():source=ROOT/'out/forge-candidates'/name/ident
    source=Path(source)
    before=inventory.digest(source/'tree.vxa')
    grid=vxa.read(source/'tree.vxa')
    branches,blades=skeleton(name,seed,json.loads((source/'spec.json').read_text()))
    meta=json.loads((source/'meta.json').read_text())
    assert len(branches)==meta['stats']['branch_curves'], 'Source skeleton revision mismatch'
    assert any(abs(grid.voxel_m-p)<1e-6 for p in (.05,.1))
    wood=np.isin(grid.data,[16,17,18,23])
    # Restore wood touching foliage for masked views, without changing occupancy.
    mask=(grid.data!=0) if include_interior else grid.surface_mask()|(wood&~ndimage.binary_erosion(wood))
    coords=np.argwhere(mask).astype(np.int16)
    mats=grid.data[tuple(coords.T)]
    positions=(coords.astype(float)+grid.origin)*grid.voxel_m
    base=np.array([materials.color(int(m)) for m in mats],dtype=np.uint8)
    bark,leaf,fresh,pattern=palette if palette is not None else PROFILES[name]
    colors=base.astype(float)
    iswood=np.isin(mats,[16,17,18,23]);isleaf=np.isin(mats,[19,20,21,22,24,25])
    field=noise(np.floor(positions/.65),seed)
    fine=noise(coords,seed+100)
    tree_gain=.97+.06*noise(np.array([[seed,1,9]]),seed)[0]
    colors[isleaf]=rgb(leaf)
    tips=isleaf&(field>.84)
    colors[tips]=rgb(fresh)
    colors[isleaf]*=((.96+.08*fine[isleaf])*tree_gain)[:,None]
    colors[mats==24]=rgb('E5C4C8')*(.96+.08*fine[mats==24,None])
    w=positions[iswood]
    if len(w):
        colors[iswood]=rgb(bark)
        angle,along,radius,bid=branch_coordinates(branches,w)
        patch=noise(np.floor(w/.5),seed+33)
        gain=.97+.06*fine[iswood]
        if pattern in ('ridged','fibrous','pine'):
            frequency=np.maximum(3,np.rint(2*np.pi*radius/(.16 if pattern=='fibrous' else .28)))
            stripes=np.sin(angle*frequency+np.sin(along*1.8)*.35)
            regions=np.column_stack([bid,np.floor(along/.9),np.zeros(len(w))])
            gain*=np.where((stripes>.58)&(noise(regions,seed)>.12),.85,1.)
        elif pattern=='plates':
            plate=noise(np.floor(w/np.array([.35,.35,.65])),seed+43)
            gain*=.86+.23*plate
        elif pattern=='smooth':gain*=.97+.06*patch
        elif pattern in ('birch','cherry'):
            marks=(np.mod(along,.65)<.105)&(noise(np.column_stack([bid,np.floor(angle*4),np.floor(along/.65)]),seed+7)>.61)
            gain*=np.where(marks,.38 if pattern=='birch' else 1.3,1.)
        colors[iswood]*=gain[:,None]*tree_gain
        if pattern=='pine':
            upper=np.clip((w[:,2]/max(positions[:,2])-.35)/.4,0,1)
            colors[iswood]=colors[iswood]*(1-upper[:,None]*.65)+rgb('AD7956')*upper[:,None]*.65
    colors=np.clip(np.rint(colors),0,255).astype(np.uint8)
    path=Path(output) if output is not None else OUT/ident;path.mkdir(parents=True,exist_ok=True)
    header=struct.pack('<IIII',*map(int,grid.shape),len(coords))
    # One occupancy payload, two RGB choices; same geometry for all A/B modes.
    (path/'voxels.bin').write_bytes(header+coords.tobytes()+mats.tobytes())
    (path/'baseline.rgb').write_bytes(base.tobytes())
    (path/'species.rgb').write_bytes(colors.tobytes())
    assert before==inventory.digest(source/'tree.vxa')
    return dict(id=ident,species=name,seed=seed,voxel_mm=round(grid.voxel_m*1000),voxels=len(coords),
        geometry_sha256=before,source=str(source.relative_to(ROOT)),
        needle=name in ('scots-pine','western-red-cedar'),
        files={p.name:inventory.digest(p) for p in path.iterdir()},
        color_count=len(np.unique(colors,axis=0)))

if __name__=='__main__':
    started=time.time();OUT.mkdir(parents=True,exist_ok=True)
    rows=[]
    for name in PROFILES:
        for seed in (1,4,7):
            rows.append(build(name,seed));print('PILOT',rows[-1]['id'],flush=True)
    inventory.write_json(OUT/'manifest.json',dict(status='appearance pilot, not endorsed',
        version=1,models=rows,elapsed_seconds=round(time.time()-started,2),
        limitations=['Nearest skeleton assignment can have seams at branch junctions','Fresh color patches not yet assigned by shoot identity',
            'Foliage is a shaped outer-shell mask; production terrain/shadow integration pending']))
