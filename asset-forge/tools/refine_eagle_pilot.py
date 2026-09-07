"""MP407 museum support removal and explicit raptor-foot reconstruction study."""
import _path
from pathlib import Path
import json
import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree
from forge.grid import VoxelGrid
from voxelize_reference_mesh import write_views
root=Path(__file__).resolve().parents[1]/'out/creature-reconstruction'
with np.load(root/'golden-eagle-source-study/surface-appearance.npz') as d:
    cells,surface,old=(d[k].copy() for k in ['occupied_cells','cells','rgb'])
xyz=cells*.0125
# The specimen branch crosses under the tarsi and extends past both wings.
remove=(xyz[:,2]<.30)&(xyz[:,0]<.58)
kept=cells[~remove]
grid=VoxelGrid(tuple(cells.max(0)+3),voxel_m=.0125)
grid.data[tuple(kept.T)]=1
feet=VoxelGrid(grid.shape,voxel_m=.0125)
for y in [.225,.3625]:
    root_point=np.array([.33,y,.36]); hock=np.array([.36,y,.24]);ankle=np.array([.32,y,.13])
    nearest=kept[cKDTree(kept*.0125).query(root_point)[1]]*.0125
    for a,b,r0,r1 in [(nearest,root_point,1.1,1.1),(root_point,hock,1.1,.8),(hock,ankle,.8,.65)]:
        feet.capsule(a/.0125,b/.0125,r0,r1,1)
    feet.capsule(root_point/.0125,hock/.0125,2.3,1.8,3)
    feet.capsule(hock/.0125,(ankle+np.array([0,0,.025]))/.0125,1.8,1.15,3)
    for dx,dy in [(-.09,-.035),(-.11,0),(-.09,.035),(.075,0)]:
        mid=ankle+np.array([dx*.7,dy,-.015]);tip=ankle+np.array([dx,dy,-.045])
        feet.capsule(ankle/.0125,mid/.0125,.65,.5,1)
        feet.capsule(mid/.0125,tip/.0125,.5,.2,2)
grid.data[feet.data>0]=feet.data[feet.data>0]+1
labels,n=ndimage.label(grid.data>0);sizes=np.bincount(labels.ravel());sizes[0]=0
grid.data[labels!=sizes.argmax()]=0
cells=np.argwhere(grid.data>0);surface_new=np.argwhere(grid.surface_mask())
rgb=old[cKDTree(surface).query(surface_new)[1]].copy()
foot_ids=grid.data[tuple(surface_new.T)]
rgb[foot_ids==2]=[171,139,61];rgb[foot_ids==3]=[41,37,29]
rgb[foot_ids==4]=[135,111,77]
feather=foot_ids==4
grain=((surface_new[:,0]*17+surface_new[:,1]*31+surface_new[:,2]*7)%17)-8
rgb[feather]=np.clip(np.array([128,105,73])+grain[feather,None],0,255)
# Taxidermy and source lighting exaggerate the pale head. Retain the nape's
# golden detail while bringing bright feather values back toward brown-gold.
head=(surface_new[:,0]*.0125<.27)&(surface_new[:,2]*.0125>.61)&(rgb.mean(1)>110)
rgb[head]=np.clip(rgb[head].astype(float)*.80,0,255).astype(np.uint8)
out=root/'golden-eagle-cleanup-study';out.mkdir(exist_ok=True)
np.savez_compressed(out/'surface-appearance.npz',occupied_cells=cells,cells=surface_new,rgb=rgb,voxel_m=.0125)
write_views(out,cells,surface_new,rgb,.0125)
report=dict(removed_support_cells=int(remove.sum()),components_before_cleanup=sorted(sizes[sizes>0].tolist(),reverse=True),occupied=len(cells),visual_approved=False)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
