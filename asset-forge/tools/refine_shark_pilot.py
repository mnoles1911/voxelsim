"""Author restrained countershading on the inspected white-shark geometry."""
import _path
from pathlib import Path
import numpy as np
from scipy.spatial import cKDTree
from voxelize_reference_mesh import write_views

ROOT=Path(__file__).resolve().parents[1]
source=ROOT/'out/creature-reconstruction/great-white-shark-study'
output=ROOT/'out/creature-reconstruction/great-white-shark-refined'
output.mkdir(parents=True,exist_ok=True)
with np.load(source/'surface-appearance.npz',allow_pickle=False) as data:
    cells,surface,rgb=(data[k].copy() for k in ['occupied_cells','cells','rgb'])
original=rgb.astype(float)
neighbors=cKDTree(surface).query(surface,k=9)[1]
value=np.median(original.mean(1)[neighbors],axis=1)
# The inspected texture encodes underside as neutral grey and back as blue-grey.
# Preserve its authored boundary, rather than imposing a horizontal plane on fins.
pale=(original[:,2]-original[:,0]<4)&(original.mean(1)>45)
noise=np.clip((value-70)*.25,-7,7)
rgb[:]=np.clip(np.array([53,65,70])+noise[:,None],0,255).astype(np.uint8)
rgb[pale]=np.clip(np.array([195,197,190])+np.clip((value[pale]-104)*.3,-12,12)[:,None],0,255).astype(np.uint8)
mouth=(original[:,0]>original[:,1]*1.15)&(original[:,0]>45)
rgb[mouth]=np.clip(original[mouth]*.8,0,255).astype(np.uint8)
eyes=original.mean(1)<25
rgb[eyes]=original[eyes].astype(np.uint8)
# Neutral highlights inside the source gill slits are not white skin. Keep
# their local detail dark instead of amplifying it with the belly remap.
xyz=surface*.0125
gills=(xyz[:,0]>.85)&(xyz[:,0]<1.28)&(xyz[:,2]>.68)&(xyz[:,2]<1.1)
rgb[gills]=np.clip(original[gills]*.45+np.array([27,32,34]),0,255).astype(np.uint8)
np.savez_compressed(output/'surface-appearance.npz',occupied_cells=cells,cells=surface,rgb=rgb,voxel_m=.0125)
write_views(output,cells,surface,rgb,.0125)
print(dict(occupied=len(cells),surface=len(surface),pale=int(pale.sum()),geometry_changed=False))
