"""Author a restrained red-stag coat on the inspected untextured deer study."""
import _path
from pathlib import Path
import json
import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree
from voxelize_reference_mesh import write_views
root=Path(__file__).resolve().parents[1]/'out/creature-reconstruction'
with np.load(root/'red-deer-solid-study/surface-appearance.npz') as d:
    cells=d['occupied_cells'].copy()
# Normalize this master's snout to the same left-facing convention as the set.
cells[:,0]=cells[:,0].max()-cells[:,0]
volume=np.zeros(cells.max(0)+3,bool);volume[tuple(cells.T)]=True
surface=np.argwhere(volume&~ndimage.binary_erosion(volume));cells=np.argwhere(volume)
x,y,z=(surface*.0125).T;mid_y=(cells[:,1].min()+cells[:,1].max())*.00625
noise=ndimage.gaussian_filter(np.random.default_rng(724).normal(size=volume.shape),1.0)
grain=noise[tuple(surface.T)]/max(noise.std(),1e-6)*3.5
rgb=np.tile([134,99,66],(len(surface),1)).astype(float)
dorsal=np.clip((z-1.20)/.30,0,1)*(x>.62)
rgb=rgb*(1-dorsal[:,None]*.5)+np.array([101,85,68])*dorsal[:,None]*.5
neck=np.exp(-((x-.44)/.28)**4)*np.clip((z-.98)/.30,0,1)*np.clip((1.91-z)/.18,0,1)
rgb=rgb*(1-neck[:,None]) + np.array([94,71,51])*neck[:,None]
belly=np.exp(-((x-1.11)/.49)**4-((z-.81)/.16)**2)
rgb=rgb*(1-belly[:,None]*.72)+np.array([169,147,112])*belly[:,None]*.72
legs=np.clip((.86-z)/.35,0,1)
rgb=rgb*(1-legs[:,None])+np.array([105,85,62])*legs[:,None]
rump_distance=((x-2.03)/.31)**2+((z-1.20)/.35)**2+((y-mid_y)/.34)**2
rump=np.clip((1.16-rump_distance)/.28,0,1)
rgb=rgb*(1-rump[:,None])+np.array([177,160,125])*rump[:,None]
antlers=(z>2.02);rgb[antlers]=[151,132,98]
rgb+=grain[:,None]*.65
hooves=z<.10;rgb[hooves]=[38,34,28]
muzzle=(x<.105)&(z>1.66)&(z<1.95);rgb[muzzle]=[36,33,29]
for eye in [[.25,mid_y-.11,1.80],[.25,mid_y+.11,1.80]]:
    idx=cKDTree(surface*.0125).query(eye,k=4)[1]
    rgb[idx]=[26,23,19]
out=root/'red-deer-refined';out.mkdir(exist_ok=True)
rgb=np.clip(rgb,0,255).astype(np.uint8)
np.savez_compressed(out/'surface-appearance.npz',occupied_cells=cells,cells=surface,rgb=rgb,voxel_m=.0125)
write_views(out,cells,surface,rgb,.0125)
report=dict(occupied=len(cells),surface=len(surface),dimensions_m=((cells.max(0)+1)*.0125).tolist(),visual_approved=False,geometry_changed='heading reversal only')
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
