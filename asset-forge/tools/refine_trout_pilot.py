"""Restore live rainbow-trout color regions on the MTSUZ-34 scan study."""
import _path
from pathlib import Path
import json
import numpy as np
from scipy.spatial import cKDTree
from voxelize_reference_mesh import write_views
root=Path(__file__).resolve().parents[1]/'out/creature-reconstruction'
with np.load(root/'rainbow-trout-large-study/surface-appearance.npz') as d:
    cells,surface,old=(d[k].copy() for k in ['occupied_cells','cells','rgb'])
xyz=surface*.0125;x,y,z=xyz.T
center_y=np.median(cells[:,1])*.0125
level=np.clip((z-.045)/.10,0,1)
rgb=np.array([197,202,187])*(1-level[:,None])+np.array([87,106,79])*level[:,None]
band=np.exp(-((z-.102)/.017)**2)*np.clip(abs(y-center_y)/.035,0,1)
band*=(x>.07)&(x<.57)
rgb=rgb*(1-band[:,None]*.68)+np.array([177,114,131])*band[:,None]*.68
near=cKDTree(surface).query(surface,k=7)[1]
lum=old.mean(1);detail=np.clip(lum-np.median(lum[near],axis=1),-20,20)
rgb+=detail[:,None]*.35
fins=(x>.55)|((z<.04)&(x>.10))
rgb[fins]=np.array([133,147,119])+detail[fins,None]*.3
# Spots below a voxel cannot retain their literal size. Sparse dark cells
# convey the diagnostic back and tail spotting without inventing red dots.
code=(surface[:,0]*73856093 ^ surface[:,1]*19349663 ^ surface[:,2]*83492791)%101
spots=(code<7)&(z>.105)&(x>.09)
spots|=(code<15)&(x>.55)
rgb[spots]=[42,49,38]
eyes=(lum<38)&(x<.12)&(z>.055)&(z<.15)
rgb[eyes]=[13,20,18]
for eye in [[.05,.025,.10],[.05,.10,.10]]:
    _,idx=cKDTree(xyz).query(eye,k=3)
    rgb[idx]=[33,42,35]
    rgb[idx[0]]=[9,14,12]
rgb=np.clip(rgb,0,255).astype(np.uint8)
out=root/'rainbow-trout-refined';out.mkdir(exist_ok=True)
np.savez_compressed(out/'surface-appearance.npz',occupied_cells=cells,cells=surface,rgb=rgb,voxel_m=.0125)
write_views(out,cells,surface,rgb,.0125)
(out/'report.json').write_text(json.dumps(dict(geometry_changed=False,occupied=len(cells),surface=len(surface),length_m=.65,scale_assumption='large trout game specimen, not the measured museum specimen',visual_approved=False),indent=2)+'\n')
