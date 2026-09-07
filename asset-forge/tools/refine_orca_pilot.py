"""Reduce baked texture mottling and restore the grey orca saddle patch."""
import _path
from pathlib import Path
import json,hashlib
import numpy as np
from scipy.spatial import cKDTree
from voxelize_reference_mesh import write_views
root=Path(__file__).resolve().parents[1]
source=root/'out/creature-reconstruction/orca-source-study'
out=root/'out/creature-reconstruction/orca-refined';out.mkdir(exist_ok=True)
with np.load(source/'surface-appearance.npz') as d:
    cells,surface,original=(d[k].copy() for k in ['occupied_cells','cells','rgb'])
value=original.astype(float).mean(1)
near=cKDTree(surface).query(surface,k=7)[1]
smooth=np.median(value[near],axis=1)
rgb=np.clip(np.array([24,28,31])+(smooth-20)[:,None]*.12,15,48)
light=value>110
rgb[light]=np.clip(np.array([217,219,212])+(smooth[light]-200)[:,None]*.15,190,228)
xyz=surface*.0125
saddle=(xyz[:,0]>2.0)&(xyz[:,0]<4.2)&(xyz[:,2]>1.5)&(value>55)
rgb[saddle]=np.clip(np.array([100,106,108])+(smooth[saddle]-150)[:,None]*.2,70,140)
rgb=rgb.astype(np.uint8)
np.savez_compressed(out/'surface-appearance.npz',occupied_cells=cells,cells=surface,rgb=rgb,voxel_m=.0125)
write_views(out,cells,surface,rgb,.0125)
(out/'report.json').write_text(json.dumps(dict(geometry_changed=False,occupied=len(cells),surface=len(surface),visual_approved=False),indent=2)+'\n')
print('orca coat study complete')
