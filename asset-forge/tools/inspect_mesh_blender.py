"""Render a source GLB from fixed oblique views and record its real object list.

blender --background --disable-autoexec --python tools/inspect_mesh_blender.py -- input.glb outdir
No model is silently selected, deleted or admitted into the game library.
"""
import bpy
import json
import math
import sys
from pathlib import Path
from mathutils import Vector

source,target=sys.argv[sys.argv.index('--')+1:]
source=Path(source).resolve();target=Path(target).resolve();target.mkdir(parents=True,exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=str(source))
scene=bpy.context.scene
meshes=[o for o in scene.objects if o.type=='MESH']
report=[]
for obj in meshes:
    bounds=[obj.matrix_world@Vector(c) for c in obj.bound_box]
    report.append(dict(name=obj.name,vertices=len(obj.data.vertices),
        bounds_min=[min(v[i] for v in bounds) for i in range(3)],
        bounds_max=[max(v[i] for v in bounds) for i in range(3)],
        materials=[m.name for m in obj.data.materials if m]))
(target/'source-objects.json').write_text(json.dumps(report,indent=2)+'\n')
all_bounds=[obj.matrix_world@Vector(c) for obj in meshes for c in obj.bound_box]
lo=Vector([min(v[i] for v in all_bounds) for i in range(3)])
hi=Vector([max(v[i] for v in all_bounds) for i in range(3)])
center=(lo+hi)/2;span=max(hi-lo)
bpy.ops.object.camera_add()
camera=bpy.context.object;camera.data.type='ORTHO';camera.data.ortho_scale=span*1.35
camera.data.clip_end=span*50;camera.data.clip_start=span*.0001;scene.camera=camera
for offset,energy,size in [((2,-3,4),1000,4),((-3,-1,2),600,3),((1,3,3),900,3)]:
    bpy.ops.object.light_add(type='AREA',location=center+Vector(offset)*span)
    lamp=bpy.context.object;lamp.data.energy=energy*span*span
    lamp.data.shape='DISK';lamp.data.size=size*span
    lamp.rotation_euler=(center-lamp.location).to_track_quat('-Z','Y').to_euler()
scene.world=bpy.data.worlds.new('Reference studio');scene.world.use_nodes=True
scene.world.node_tree.nodes['Background'].inputs[0].default_value=(.35,.35,.35,1)
scene.world.node_tree.nodes['Background'].inputs[1].default_value=.5
scene.render.engine='CYCLES';scene.cycles.device='CPU';scene.cycles.samples=24
scene.cycles.use_denoising=True;scene.render.threads_mode='FIXED';scene.render.threads=6
scene.render.resolution_x=800;scene.render.resolution_y=700;scene.render.resolution_percentage=100
scene.render.image_settings.file_format='PNG';scene.render.film_transparent=False
# Fixed exposure keeps this bright three-light studio from washing out coat
# regions. Orthographic RGB images remain the unlit color reference.
scene.view_settings.exposure=-1.5
for i,offset in enumerate(((3,-5,2),(-3,-5,2),(0,-5,1),(5,0,1))):
    camera.location=center+Vector(offset)*span
    camera.rotation_euler=(center-camera.location).to_track_quat('-Z','Y').to_euler()
    scene.render.filepath=str(target/f'view-{i}.png')
    bpy.ops.render.render(write_still=True)
print(json.dumps(dict(source=str(source),objects=len(meshes),bounds=[list(lo),list(hi)])))
