"""Small bushcraft items authored on the asset's cubic lattice.

Dimensions reuse artifact length/beam/depth. Material IDs are appearance only.
Thin lashings intentionally occupy at least one voxel. Physical handle radii
are independent of pitch so finer exports do not shrink the tools.
"""
import numpy as np
from .grid import VoxelGrid
from .spec import get
from . import materials

FORMS = ('flake_blade', 'stone_knife', 'fiber_bundle', 'cordage_coil',
         'hammerstone', 'wooden_haft', 'stone_axe_head', 'stone_axe')

def build(spec, rng, voxel_m, steps):
    form = get(spec, 'artifact.form')
    if form=='stone_axe':
        from .stone_axe import build as build_axe
        return build_axe(spec,rng,voxel_m,steps)
    dims = np.array([get(spec, 'artifact.length_m'), get(spec, 'artifact.beam_m'),
                     get(spec, 'artifact.depth_m')], float)
    shape = tuple(np.ceil(dims / voxel_m).astype(int) + 4)
    grid = VoxelGrid(shape, (0, 0, 0), voxel_m)
    xyz = np.stack(np.meshgrid(*[(np.arange(n)-1.5)*voxel_m for n in shape], indexing='ij'), -1)
    x,y,z = np.moveaxis(xyz / dims, -1, 0)
    phase = rng.uniform(-0.3,0.3)
    tones = {38: materials.resolve(get(spec,'materials.hull')),
             17: materials.resolve(get(spec,'materials.frame')),
             39: materials.resolve(get(spec,'materials.trim')),
             18: materials.resolve(get(spec,'materials.strake'))}

    def paint(mask, mat):
        grid.data[mask] = tones.get(mat,mat)

    def tube(points, radius, mat):
        for a,b in zip(points[:-1],points[1:]):
            a,b = np.array(a)*dims, np.array(b)*dims
            ab=b-a
            t=np.clip(np.sum((xyz-a)*ab,axis=-1)/max(np.dot(ab,ab),1e-12),0,1)
            paint(np.sum((xyz-a-t[...,None]*ab)**2,axis=-1)<=radius**2,mat)

    def blade(start, end, center_y, width, center_z, thick):
        t=(x-start)/(end-start)
        edge=width*(0.06+0.94*np.sin(np.pi*np.clip(t,0,1))**0.85)
        center=center_y+0.035*np.sin(6*t+phase)
        # Wedge, faceted ridge, asymmetric flaking. No independent voxel noise.
        depth=thick*np.maximum(0.12,1-np.abs(y-center)/np.maximum(edge,.001)*.88)
        # Overlapping shallow flake scars shape the surface, not just its color.
        depth-=.075*np.maximum(0,np.sin(t*30+(y-center)*15+phase))*(np.abs(y-center)/np.maximum(edge,.001))
        mask=(t>=0)&(t<=1)&(np.abs(y-center)<=edge)&(np.abs(z-center_z)<=depth)
        paint(mask,38)
        paint(mask & (np.sin(t*19+(y-center)*10+phase)>.50) & (z>center_z),37)
        paint(mask & (np.abs(y-center)>edge*.72),46)

    def haft(end=.94):
        tube([(0.03,.52,.48),(.27,.47,.50),(.60,.50,.52),(end,.53,.50)],
             .0255,17)
        occupied=grid.data!=0
        paint(occupied & (y>.52+0.035*np.sin(x*13+phase)),18)
        paint(occupied & (x<.10),16)

    def axe_head(standalone=False):
        # Head crosses the haft. Poll on +Y, broad bit on -Y.
        hx=.48 if standalone else .79
        width=.50 if standalone else .155
        t=np.clip((y-.04)/.9,0,1)
        span=width*(1-.48*t)*(0.83+0.17*np.sin(np.pi*t))
        thick=(.055+.30*np.sin(np.pi*t*.75))*np.maximum(.30,1-.65*np.abs(x-hx)/np.maximum(span,.001))
        mask=(y>=.04)&(y<=.94)&(np.abs(x-hx)<=span)&(np.abs(z-.51)<=thick)
        paint(mask,38)
        paint(mask & ((x-hx)>.04) & (z>.53),37)
        paint(mask & (y<.20),46)
        paint(mask & (y>.8) & (z>.6),2)

    def bindings(axe=False):
        if axe:
            # Wrap around head and split haft, with diagonal securing turn.
            for xx in (.735,.765,.805,.835):
                tube([(xx,.36,.27),(xx,.62,.27),(xx,.65,.70),
                      (xx,.34,.70),(xx,.36,.27)],max(.008,voxel_m*.90),39)
            tube([(.71,.35,.68),(.85,.65,.70),(.85,.72,.45)],max(.008,voxel_m*.9),17)
        else:
            for xx in (.12,.18,.24,.30,.36):
                tube([(xx,.30,.50),(xx,.50,.82),(xx,.70,.50),
                      (xx,.5,.18),(xx,.30,.50)],max(.008,voxel_m*.9),39)

    def draw():
        if form in ('flake_blade','stone_knife'):
            if form=='stone_knife':
                tube([(.06,.5,.5),(.51,.5,.5)],.023,17)
                blade(.30,.98,.5,.43,.5,.40)
                bindings()
            else:
                blade(.02,.98,.5,.43,.5,.40)
        elif form in ('wooden_haft','stone_axe','stone_axe_head'):
            if form!='stone_axe_head': haft()
            if form!='wooden_haft': axe_head(form=='stone_axe_head')
            if form=='stone_axe': bindings(True)
        elif form=='hammerstone':
            mask=((x-.5)/.48)**2+((y-.5)/.46)**2+((z-.5)/.44)**2<=1
            paint(mask,2)
            paint(mask & (z>.62) & (x+y>.75),37)
            paint(mask & (x<.20),46)
        elif form=='fiber_bundle':
            for i in range(9):
                yy=.22+i*.065
                tube([(.03+(i%3)*.035,yy-.1,.38+(i%2)*.16),
                      (.48,yy,.50),(.96-(i%4)*.025,yy+.1,.42+(i%2)*.14)],
                     max(.007,voxel_m*.9),39 if i%3 else 18)
            tube([(.48,.13,.48),(.48,.48,.79),(.48,.86,.48),(.48,.48,.23),(.48,.13,.48)],max(.009,voxel_m*.9),17)
        elif form=='cordage_coil':
            # Three connected oval turns, open center, a securing hitch and tail.
            angles=np.linspace(0,6*np.pi,260)
            pts=[(.48+.36*np.cos(a),.46+.31*np.sin(a),.25+.40*i/(len(angles)-1)) for i,a in enumerate(angles)]
            tube(pts,max(.012,voxel_m*.90),39)
            tube([pts[-1],(.87,.56,.56),(.94,.78,.3),(.76,.91,.27)],max(.012,voxel_m*.90),39)
            tube([(.78,.34,.17),(.90,.34,.51),(.79,.34,.86),(.65,.34,.50),(.78,.34,.17)],max(.012,voxel_m*.90),17)
    steps.run(form.replace('_',' '),grid,draw,note=f'{voxel_m*1000:g} mm cubic bushcraft geometry')
    return grid
