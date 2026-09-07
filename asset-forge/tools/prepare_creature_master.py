"""Blender: bake the inspected static pose and normalize horizontal heading.

-- input.glb output.glb [length_m]
Exports evaluated geometry, preserving UV/materials and recording the transform.
PCA is a heading proposal which must be checked in the resulting six views.
"""
import bpy
import sys
import json
import math
from pathlib import Path
import numpy as np
from mathutils import Matrix, Vector

source,target,*length=sys.argv[sys.argv.index('--')+1:]
source=Path(source).resolve();target=Path(target).resolve()
target.parent.mkdir(parents=True,exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=str(source))
bpy.context.view_layer.update()
deps=bpy.context.evaluated_depsgraph_get()
objects=[]
for obj in list(bpy.context.scene.objects):
    if obj.type!='MESH':continue
    evaluated=obj.evaluated_get(deps)
    mesh=bpy.data.meshes.new_from_object(evaluated,preserve_all_data_layers=True,depsgraph=deps)
    mesh.transform(obj.matrix_world)
    baked=bpy.data.objects.new('master-'+obj.name,mesh)
    objects.append(baked)
for obj in list(bpy.context.scene.objects):
    bpy.data.objects.remove(obj,do_unlink=True)
for obj in objects:bpy.context.collection.objects.link(obj)
points=np.array([v.co[:] for obj in objects for v in obj.data.vertices])
center=points.mean(0)
values,vectors=np.linalg.eigh(np.cov(points[:,:2].T))
direction=vectors[:,-1]
angle=-math.atan2(direction[1],direction[0])
rotation=Matrix.Rotation(angle,4,'Z')
for obj in objects:obj.data.transform(rotation)
points=np.array([v.co[:] for obj in objects for v in obj.data.vertices])
lower=points.min(0);upper=points.max(0)
scale=float(length[0])/(upper[0]-lower[0]) if length else 1.
shift=Matrix.Translation(Vector((-lower).tolist()))
transform=Matrix.Scale(scale,4)@shift
for obj in objects:obj.data.transform(transform)
bpy.ops.export_scene.gltf(filepath=str(target),export_format='GLB',export_animations=False)
report=dict(source=str(source),heading_radians=angle,uniform_scale=scale,
            dimensions_m=((upper-lower)*scale).tolist(),meshes=[o.name for o in objects],
            limitations=['Static evaluated pose; no animation retained','PCA heading requires visual verification'])
target.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
