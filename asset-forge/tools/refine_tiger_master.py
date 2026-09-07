"""Remove explicitly identified sub-voxel whisker strands from the tiger master.

At 12.5 mm they become a solid grey moustache. Preserve muzzle, eyes, teeth,
ears and paws; do not globally remove small anatomical components.
"""
from pathlib import Path
import json
import numpy as np
import trimesh

ROOT=Path(__file__).resolve().parents[1]
base=ROOT/'out/creature-reconstruction/bengal-tiger-master'
scene=trimesh.load(base/'master.glb',force='scene')
removed=[]
for mesh in scene.geometry.values():
    keep=np.ones(len(mesh.faces),bool)
    groups=trimesh.graph.connected_components(mesh.face_adjacency,
              nodes=np.arange(len(mesh.faces)),min_len=1)
    for faces in groups:
        vertices=mesh.vertices[mesh.faces[faces].ravel()]
        lo,hi=vertices.min(0),vertices.max(0)
        # glTF Y is up. These bounds were inspected on the normalized source.
        strand=(len(faces)<=68 and lo[0]>2.55 and lo[1]>1.0 and hi[1]<1.15
                and (hi[2]<-.318 or lo[2]>-.223))
        if strand:
            keep[faces]=False
            removed.append(dict(faces=len(faces),bounds=[lo.tolist(),hi.tolist()]))
    mesh.update_faces(keep)
    mesh.remove_unreferenced_vertices()
assert len(removed)==32, 'Source geometry changed: review whisker selection again'
scene.export(base/'clean-master.glb')
(base/'whisker-cleanup.json').write_text(json.dumps(dict(
    rationale='32 sub-voxel whisker strands otherwise become a thick voxel moustache',
    removed=removed,remaining_bounds=scene.bounds.tolist()),indent=2)+'\n')
print(json.dumps(dict(removed_strands=len(removed),bounds=scene.bounds.tolist())))
