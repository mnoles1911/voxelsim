"""A knapped, lashed stone axe on the authored cubic lattice."""
import numpy as np
from .grid import VoxelGrid
from .spec import get

def build(spec, rng, voxel_m, steps):
    dims=np.array([get(spec,'artifact.length_m'),get(spec,'artifact.beam_m'),get(spec,'artifact.depth_m')])
    shape=tuple(np.ceil(dims/voxel_m).astype(int)+4)
    grid=VoxelGrid(shape,(0,0,0),voxel_m)
    xyz=np.stack(np.meshgrid(*[(np.arange(n)-1.5)*voxel_m for n in shape],indexing='ij'),-1)
    x,y,z=np.moveaxis(xyz/dims,-1,0)
    def tube(points,radius,mat):
        for a,b in zip(points[:-1],points[1:]):
            a,b=np.array(a)*dims,np.array(b)*dims;ab=b-a
            t=np.clip(np.sum((xyz-a)*ab,-1)/np.dot(ab,ab),0,1)
            mask=np.sum((xyz-a-t[...,None]*ab)**2,-1)<=radius**2
            grid.data[mask]=mat
    tube([(.03,.55,.45),(.22,.47,.47),(.50,.46,.50),(.75,.53,.51),(.92,.55,.51)],.027,17)
    wood=grid.data!=0
    grid.data[wood & (y>.52+.025*np.sin(x*12))]=18
    grid.data[wood & (x<.11) & (z<.51)]=16
    # Broad cutting edge narrows toward a rounded poll; asymmetric flake scars
    # actually remove stone, rather than painting alternating metal-like bands.
    t=np.clip((y-.06)/.86,0,1);cx=.79+.012*np.sin(t*5)
    span=.15*(1-.57*t)*(.88+.12*np.sin(t*np.pi))
    thickness=(.035+.34*np.sin(t*np.pi*.85))*np.maximum(.25,1-.62*np.abs(x-cx)/span)
    stone=(y>=.06)&(y<=.92)&(np.abs(x-cx)<span)&(np.abs(z-.50)<thickness)
    for xx,yy,zz in [(.68,.25,.75),(.86,.30,.76),(.71,.62,.78),(.86,.67,.74),(.78,.13,.30)]:
        scar=((x-xx)/.06)**2+((y-yy)/.20)**2+((z-zz)/.18)**2<1
        stone &= ~scar
    grid.data[stone]=1
    grid.data[stone & (z>.60) & (x<cx) & (y>.2)]=2
    for i,xx in enumerate([.755,.785,.815]):
        tube([(xx,.36,.16),(xx,.63,.16),(xx+.006,.66,.81),
              (xx-.004,.35,.81),(xx,.36,.16)],max(.008,voxel_m*.8),39)
    tube([(.73,.35,.79),(.85,.64,.80),(.86,.67,.54),(.87,.71,.41)],max(.008,voxel_m*.8),39)
    tube([(.85,.63,.79),(.85,.70,.85),(.81,.71,.82),(.85,.63,.79)],max(.008,voxel_m*.8),39)
    finished=grid.data.copy();grid.data[:]=0
    steps.run('lashed stone axe',grid,lambda:grid.data.__setitem__(slice(None),finished),note=f'{voxel_m*1000:g} mm cubic stone, wood and cordage')
    return grid
