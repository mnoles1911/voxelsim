"""MP040 support-separation study at the original 12.5 mm pitch."""
import _path
from pathlib import Path
import json
import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree
from voxelize_reference_mesh import write_views
from forge.grid import VoxelGrid

root=Path(__file__).resolve().parents[1]/'out/creature-reconstruction'
with np.load(root/'common-raven-museum-study/surface-appearance.npz') as d:
    cells,surface,rgb=(d[k].copy() for k in ['occupied_cells','cells','rgb'])
color=rgb[cKDTree(surface).query(cells)[1]].astype(float)
xyz=cells*.0125
# The support is warm wood below the feet. The black tail passes behind it.
wood=(color[:,0]-color[:,2]>12)&(color[:,0]-color[:,1]>4)
remove=(xyz[:,2]<.12)&(xyz[:,0]<.39)
remove |= wood&(xyz[:,2]<.29)&(xyz[:,0]<.39)
remove |= (xyz[:,2]<.29)&(xyz[:,0]<.38)
kept=cells[~remove]
volume=np.zeros(cells.max(0)+3,bool);volume[tuple(kept.T)]=True
# The scan's digits fuse into the perch. Rebuild two short tarsi and an
# anisodactyl foot on each side, with three forward digits and a hallux.
feet=VoxelGrid(volume.shape,voxel_m=.0125)
for y in [.075,.1625]:
    ankle=np.array([.285,y,.15])
    root_point=np.array([.315,y,.315])
    hock=np.array([.335,y,.235])
    nearest=kept[cKDTree(kept*.0125).query(root_point)[1]]*.0125
    for a,b in [(nearest,root_point),(root_point,hock),(hock,ankle)]:
        feet.capsule(a/.0125,b/.0125,.6,.55,1)
    for dx,dy in [(-.085,-.027),(-.10,0),(-.08,.028),(.055,0)]:
        mid=ankle+np.array([dx*.65,dy*.8,-.018])
        tip=ankle+np.array([dx,dy,-.045])
        feet.capsule(ankle/.0125,mid/.0125,.45,.45,1)
        feet.capsule(mid/.0125,tip/.0125,.35,.35,1)
volume |= feet.data>0
labels,count=ndimage.label(volume)
sizes=np.bincount(labels.ravel());sizes[0]=0
# Record disconnected remnants rather than quietly presenting them as anatomy.
largest=sizes.argmax(); volume=labels==largest
kept=np.argwhere(volume)
exposed=volume&~ndimage.binary_erosion(volume)
new_surface=np.argwhere(exposed)
old=rgb[cKDTree(surface).query(new_surface)[1]].astype(float)
# Live raven plumage is black; preserve modest source shading in blue-charcoal.
lum=old.mean(1)
new_rgb=np.clip(np.array([35,38,43])+(lum-70)[:,None]*.35,12,68).astype(np.uint8)
eyes=(old.mean(1)<28)&(new_surface[:,0]*.0125<.20)&(new_surface[:,2]*.0125>.42)
new_rgb[eyes]=[9,11,12]
for eye in [[.125,.0625,.5125],[.10,.1375,.5125]]:
    closest=cKDTree(new_surface*.0125).query(eye)[1]
    new_rgb[closest]=[7,9,10]
out=root/'common-raven-cleanup-study';out.mkdir(exist_ok=True)
np.savez_compressed(out/'surface-appearance.npz',occupied_cells=kept,cells=new_surface,rgb=new_rgb,voxel_m=.0125)
write_views(out,kept,new_surface,new_rgb,.0125)
report=dict(removed_support_cells=int(remove.sum()),components_before_cleanup=sorted(sizes[sizes>0].tolist(),reverse=True),occupied=len(kept),visual_approved=False)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(report)
