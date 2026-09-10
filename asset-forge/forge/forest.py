"""Temperate tree families: connected metric scaffolds, shoots and foliage fans.

Geometry is generated locally. No language/image model runs during seed production.
Profiles control architecture; a seed selects topology and allometry before raster.
"""
from __future__ import annotations
import hashlib
import math
import time
import numpy as np
from . import materials as M, oak
from .forest_profiles import PROFILES

VERSION = 'temperate-shoots-v2'
SIZES = oak.SIZES
FORMS = oak.FORMS

def validate_recipe(recipe, kind):
    # UNDERSTORY_DISPATCH_BEGIN
    if isinstance(recipe, dict) and recipe.get("generator") == "temperate-understory-v1":
        from . import understory
        return understory.validate_recipe(recipe, kind)
    # UNDERSTORY_DISPATCH_END
    if not isinstance(recipe,dict):raise ValueError('Tree recipe must be an object')
    if recipe.get('generator') == oak.VERSION:
        return oak.validate_recipe(recipe, kind)
    if kind != 'tree' or set(recipe)-{'generator','profile','size','form'}:
        raise ValueError('Invalid temperate tree recipe')
    if recipe.get('generator') != VERSION or recipe.get('profile') not in PROFILES:
        raise ValueError('Unsupported temperate tree profile')
    size, form = recipe.get('size','mixed'), recipe.get('form','mixed')
    if size not in (*SIZES,'mixed') or form not in (*FORMS,'mixed'):
        raise ValueError('Invalid temperate size/form')
    return dict(generator=VERSION, profile=recipe['profile'], size=size, form=form)

def architecture(profile, seed, height, size, form):
    p = profile
    salt = int.from_bytes(hashlib.sha256(p.taxon.encode()).digest()[:4], 'little')
    rng = np.random.default_rng([seed, salt])
    branches, leaves = [], []
    stage = SIZES.index(size)
    width = height*p.width*{'open':1.12,'woodland':.70,'edge':.97}[form]*rng.uniform(.88,1.12)
    clear = min(.68, p.clear*.45 if form=='open' and p.architecture not in ('conifer','hemlock','cedar','spruce','giant','column','fern','pine') else p.clear + ({'open':-.035,'woodland':.16,'edge':.045}[form]))
    radius = max(.075, height * (.026 if p.architecture=='giant' else .018))*rng.uniform(.87,1.12)
    if p.architecture=='fern':radius=max(.14,height*.035)
    if p.bark == 'pale': radius *= .70
    lean = rng.normal(0,.018,2)*height
    if form == 'edge': lean += np.array([height*.075,0])
    def axis(t): return np.array([lean[0]*t*t,lean[1]*t*t,height*t])
    def curve(a,b,r0,r1,bend=0.,droop=0.,steps=5):
        a,b = np.asarray(a),np.asarray(b)
        delta=b-a
        side=oak.unit(np.cross(delta,[0.,0.,1.]))
        wobble=side*rng.uniform(-bend,bend)
        ts=np.linspace(0,1,steps)
        ps=np.array([a+delta*t+math.sin(math.pi*t)*(wobble+np.array([0.,0.,-droop])) for t in ts])
        branches.append((ps,np.linspace(r0,r1,steps)))
        return ps
    def spray(a,b,scale=1.,compound=False):
        # Every thin fan begins on a woody/leaf-bearing shoot. At 100 mm,
        # these represent groups of leaves or needles, not giant single leaves.
        shoot=curve(a,b,.012,.003,steps=3)
        d=oak.unit(b-a);cross=oak.unit(np.cross(d,[0.,0.,1.]))
        if np.linalg.norm(cross)<.1:cross=np.array([1.,0.,0.])
        n=7 if compound else 5
        for j,t in enumerate(np.linspace(.12,1,n)):
            base=a+(b-a)*t
            signs=(-1,1) if compound or p.architecture=='opposite' else ((-1 if j%2 else 1),)
            for sign in signs:
                direction=oak.unit(d*.48+cross*sign+np.array([0,0,rng.uniform(-.10,.25)]))
                length=p.leaf*scale*rng.uniform(.78,1.12)
                half=length*(.19 if compound else .32)
                leaves.append((base,direction,length,half,.07,float(rng.uniform(0,math.tau))))
        return shoot
    # Dominant leader and branch attachments share exactly the same curve.
    is_conifer=p.architecture in ('conifer','hemlock','cedar','spruce','giant','column') or (p.architecture=='pine' and stage==0)
    dominant_broadleaf=p.taxon in ('alnus-glutinosa','liriodendron-tulipifera')
    leader_end = 1.0 if is_conifer or dominant_broadleaf or p.architecture in ('light','fern') else min(.78,clear+.22)
    trunk=np.array([axis(t) for t in np.linspace(0,leader_end,33)])
    if p.architecture=='hemlock':
        trunk[-3:,0]+=np.array([.0,.12,.35])*min(height/12,2)
        trunk[-1,2]-=.18*min(height/12,2)
    branches.append((trunk,np.linspace(radius,.018,len(trunk))))
    for i in range(5):
        az=i*math.tau/5+rng.uniform(-.2,.2)
        curve(axis(.04),[math.cos(az)*radius*2.4,math.sin(az)*radius*2.4,.025],radius*.48,.035)
    def anchor(t):
        idx=int(np.clip(round(t/leader_end*32),1,32));return trunk[idx]
    if p.architecture=='fern':
        # Fronds are a curved rachis with alternating pinnae, never tree boughs.
        for i in range(10+stage*3):
            az=i*2.39996+rng.uniform(-.16,.16);d=np.array([math.cos(az),math.sin(az),0.])
            a=trunk[-1];reach=width*.5*rng.uniform(.75,1.1)
            ps=np.array([a+d*reach*t+np.array([0,0,height*(.24*math.sin(math.pi*t)-.23*t*t)]) for t in np.linspace(0,1,12)])
            branches.append((ps,np.linspace(.018,.004,len(ps))))
            side=np.cross(d,[0,0,1.])
            for j in range(1,12):
                for sign in (-1,1):
                    b=ps[j]+side*sign*reach*.24*math.sin(math.pi*j/12)+d*.12
                    spray(ps[j],b,.70,True)
                    if j>1:spray(ps[j-1],ps[j],.60,True)
        return branches,leaves
    if is_conifer:
        # Whorls are staggered and incomplete; lateral branchlets furnish each
        # bough along its length, preserving wedges of air between boughs.
        levels=(16+stage*9) if p.architecture=='column' else min(44,max(12,int(height*.5)+8))
        for level,t in enumerate(np.linspace(clear,.965,levels)):
            n=int(rng.integers(4,7))
            phase=rng.uniform(0,math.tau)
            for j in range(n):
                if rng.random()<.10:continue
                az=phase+j*math.tau/n+rng.uniform(-.13,.13)
                radial=np.array([math.cos(az),math.sin(az),0.]);side=np.cross(radial,[0,0,1.])
                a=anchor(t+rng.uniform(-.009,.009))
                shape=(1-t)**.85 if p.architecture!='column' else .72*(1-t)**.65
                taper=min(1.,(1-t)*8)
                reach=max(.18,width*.65*shape*rng.uniform(.75,1.2))
                b=a+radial*reach+np.array([0,0,p.rise*height*(1-t)-p.droop*height*(1-t)])
                bough=curve(a,b,max(.035,radius*.24*(1-t)),.013,bend=reach*.07,droop=reach*.10)
                count=max(4,min(15,int(reach/.34)))
                for k,u in enumerate(np.linspace(.18,1,count)):
                    ix=min(3,int(u*4));v=u*4-ix;base=bough[ix]*(1-v)+bough[ix+1]*v
                    for sign in (-1,1):
                        end=base+side*sign*min(1.4,reach*.28)*(1-u*.60)+radial*.23*taper
                        end[2]-=p.droop*height*rng.uniform(.15,.45)
                        spray(base,end,.9*taper,True)
                        for direction in (-1,1):
                            fork=base+(end-base)*rng.uniform(.4,.75)
                            vertical=direction*min(1.4,height*.025)*(1-u*.4)*taper
                            spray(fork,end+radial*.35*taper+np.array([0,0,vertical]),.8*taper,True)
        # Furnish the terminal leader without rounding off its spire.
        for t in np.linspace(.91,.995,6):spray(anchor(t),axis(t)+np.array([.12,0,.18]),max(.15,.55*(1-t)/.09),True)
    else:
        # Crown space constrains twig destinations, never fills foliage balls.
        # Recursively divide those destinations into branch territories; each
        # daughter grows from its parent's tip. This yields a real fork hierarchy
        # instead of independently attaching every leafy arm to one pole.
        total=(420,900,1700)[stage]
        total=max(90,int(total*p.density*min(1.8,(height/20)**1.65)))
        bottom=clear+(.04 if p.architecture in ('layered','scrub','weeping') else .09)
        top=.96
        points=[]
        while len(points)<total:
            q=rng.uniform(-1,1,3);norm=np.dot(q,q)
            if norm>1 or norm<.18:continue
            az=math.atan2(q[1],q[0])
            rmod=1+.20*math.sin(az*3+seed)+.14*math.cos(az*5-seed)
            if dominant_broadleaf:rmod*=.80-.25*q[2]
            if p.architecture=='pine':rmod*=1+.18*math.sin(az*2+q[2]*4)
            if p.taxon in ('cornus-florida','acer-palmatum'):
                q[2]=.6*round(q[2]*2)/2+.4*q[2]+.15*math.sin(az*2+seed)
            z=bottom+(q[2]+1)*.5*(top-bottom)
            if p.architecture=='vase':rmod*=.77+.24*(q[2]+1)/2
            point=axis(z)+np.array([q[0]*width*.48*rmod,q[1]*width*.48*rmod,0])
            points.append(point)
        targets=np.array(points)
        def split(cloud,k):
            k=min(k,len(cloud));centers=[cloud[rng.integers(len(cloud))]]
            for _ in range(1,k):
                ds=np.min(np.sum((cloud[:,None]-np.array(centers)[None])**2,axis=2),axis=1)
                centers.append(cloud[np.argmax(ds)])
            centers=np.array(centers)
            for _ in range(6):
                labels=np.argmin(np.sum((cloud[:,None]-centers[None])**2,axis=2),axis=1)
                for j in range(k):
                    group=cloud[labels==j]
                    if len(group):centers[j]=group.mean(0)
            return [cloud[labels==j] for j in range(k) if np.any(labels==j)]
        def grow(base,cloud,depth,parent_radius):
            mean=cloud.mean(0)
            end=base+(mean-base)*(.68 if depth<2 else .88)
            rr=max(.009,radius*.85*math.sqrt(len(cloud)/total))
            curve(base,end,min(parent_radius,rr),rr*.65,np.linalg.norm(end-base)*.09)
            if depth<3 and len(cloud)>6:
                for group in split(cloud,4):grow(end,group,depth+1,rr*.65)
            else:
                for tip in cloud:
                    # Furnish the last metre(s) along a connected twig. Fine
                    # stems resolve to leaf/stem voxels at the game lattice.
                    tip=tip.copy()
                    if p.architecture=='weeping':tip[2]=max(.20,tip[2]-height*p.droop*rng.uniform(.35,.75))
                    twig=curve(end,tip,rr*.60,.003,np.linalg.norm(tip-end)*.07)
                    length=np.linalg.norm(tip-end)
                    steps=max(2,min(7,int(length/.38)))
                    direction=oak.unit(tip-end);side=oak.unit(np.cross(direction,[0,0,1.]))
                    for j,u in enumerate(np.linspace(.25,1,steps)):
                        ix=min(3,int(u*4));v=u*4-ix;a=twig[ix]*(1-v)+twig[ix+1]*v
                        shootdir=oak.unit(direction*.50+side*(1 if j%2 else -1)*.65+np.array([0,0,.14]))
                        dest=a+shootdir*rng.uniform(.30,.65)
                        dest[2]-=height*p.droop*rng.uniform(.06,.18)
                        dest[2]=max(.12,dest[2])
                        spray(a,dest,1.,p.architecture in ('compound','cedar-wide','pine'))
        for group in split(targets,5 if stage else 4):
            root_t=clear+rng.uniform(.01,.08)
            if dominant_broadleaf or p.architecture=='light':root_t=clear+(group[:,2].mean()/height-clear)*.72
            grow(anchor(root_t),group,0,radius*.75)
        return branches,leaves
    return branches,leaves

def build(spec,seed,pitch,connectivity=True):
    from .pipeline import Asset
    from .spec import spec_hash
    start=time.perf_counter()
    recipe=validate_recipe(spec['plant_recipe'],spec['kind'])
    p=PROFILES[recipe['profile']]
    size=recipe['size'] if recipe['size']!='mixed' else SIZES[((seed-1)//3)%3]
    form=recipe['form'] if recipe['form']!='mixed' else FORMS[(seed-1)%3]
    rng=np.random.default_rng([seed,937])
    height=float(spec['height_m'])*{'small':.32,'medium':.66,'large':1.}[size]*rng.uniform(.87,1.10)
    if p.architecture=='oak':
        branches,leaves=oak.master(seed,dict(size=size,form=form,height_m=height,width_m=height*p.width,
            trunk_radius_m=height*.040))
        leaves=[(a,d,l*p.leaf/.48,w*p.leaf/.48,t,phase) for a,d,l,w,t,phase in leaves]
    else: branches,leaves=architecture(p,seed,height,size,form)
    grow=time.perf_counter()
    grid,dropped=oak.raster(branches,leaves,pitch,wood_cutoff=.025)
    needle=p.architecture in ('conifer','hemlock','cedar','spruce','pine','cedar-wide','column','giant')
    if needle:grid.data[grid.data==M.MAT_LEAF_BROADLEAF]=M.MAT_LEAF_NEEDLE
    if p.bark=='pale':grid.data[grid.data==M.MAT_BARK]=M.MAT_BARK_PALE
    # Bark marks use existing engine materials; no new runtime palette required.
    if p.bark in ('pale','shaggy','fluted'):
        coords=np.argwhere(grid.data==(M.MAT_BARK_PALE if p.bark=='pale' else M.MAT_BARK))
        if len(coords):
            world=coords+grid.origin
            pattern=(world[:,2]%9==0)&((world[:,0]+world[:,1]*3)%5<2) if p.bark=='pale' else ((world[:,0]*3+world[:,1]*5)%11==0)&(world[:,2]%17<11)
            marks=coords[pattern];grid.data[tuple(marks.T)]=M.MAT_BARK if p.bark=='pale' else M.MAT_DEADWOOD
    if recipe['profile']=='cherry-blossom':
        cs=np.argwhere(grid.data==M.MAT_LEAF_BROADLEAF)
        # Coherent clusters on existing leafy shoots, not alternating pink cells.
        cell=np.floor((cs+grid.origin)*pitch/.28).astype(np.int64)
        hashed=(cell[:,0]*73856093)^(cell[:,1]*19349663)^(cell[:,2]*83492791)^seed
        keep=(hashed%17)<5
        grid.data[tuple(cs[keep].T)]=M.MAT_LEAF_BLOSSOM
    wood=grid.material_mask([M.MAT_BARK,M.MAT_BARK_PALE,M.MAT_HEARTWOOD,M.MAT_DEADWOOD])
    woodfrac=grid.component_fraction(wood,connectivity=1) if connectivity else 1.
    attached=grid.component_fraction(connectivity=3) if connectivity else 1.
    dims=np.array(grid.shape)*pitch
    stats=dict(seed=seed,spec_hash=spec_hash(spec),kind='tree',generator=VERSION,profile=recipe['profile'],
        architecture=p.architecture,habitat=p.habitat,size_class=size,growth_form=form,
        nodes=sum(len(ps) for ps,_ in branches),segments=sum(len(ps)-1 for ps,_ in branches),max_order=4,
        clumps=len(leaves),branch_curves=len(branches),voxel_cm=pitch*100,height_m=round(float(dims[2]),3),
        length_m=round(float(dims[0]),3),footprint_m=dims[:2].tolist(),extent_vox=list(grid.shape),
        voxels=grid.count(),by_material=grid.histogram(),wood_connected=woodfrac,
        wood_detached=round(int(wood.sum())*(1-woodfrac)),attached_frac=attached,
        detached=round(grid.count()*(1-attached)),ground_contact=int(np.count_nonzero(grid.data[:,:,0])),
        orphans_removed=dropped,grid_mb=round(grid.data.nbytes/1e6,2),
        ms_grow=round((grow-start)*1000,1),ms_total=round((time.perf_counter()-start)*1000,1))
    live=dict(spec,plant_recipe=dict(recipe,size=size,form=form))
    live['height_m']=height
    return Asset(grid=grid,skeleton=None,spec=spec,seed=seed,realized=live,stats=stats)
