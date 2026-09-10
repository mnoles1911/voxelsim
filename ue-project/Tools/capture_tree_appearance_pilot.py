"""Isolated editor comparison scene. Run with -ExecutePythonScript, not commandlet.

Captures diagnostic images only; editor timings are not game GPU benchmarks.
Requires the V2 importer report. Leaves production levels and catalogs untouched.
"""
import json
import time
import sys
import os
from pathlib import Path
import unreal

ROOT=Path(os.environ.get('VOXEL_REPO_ROOT',str(Path(unreal.Paths.project_dir()).resolve().parent)))
SOURCE=ROOT/'asset-forge/out/tree-appearance-pilot/game-v2'
OUT=SOURCE/'captures-linear'
OUT.mkdir(exist_ok=True)
DEST='/Game/Voxel/AppearancePilot/V2'
report=json.loads((SOURCE/'unreal-import-report.json').read_text())
models={r['id']:r for r in report['models']}
if os.environ.get('TREE_APPEARANCE_NON_NANITE')=='1':
    # Disable Nanite on the asset and rebuild its full source geometry. Merely
    # setting r.Nanite=0 would measure the simplified fallback instead.
    mesh_editor=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    for row in models.values():
        mesh=unreal.load_asset(row['meshes'][0])
        settings=mesh_editor.get_nanite_settings(mesh)
        settings.set_editor_property('enabled',False)
        mesh_editor.set_nanite_settings(mesh,settings,True)
        assert not mesh_editor.get_nanite_settings(mesh).get_editor_property('enabled')
        assert unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    unreal.log('TREE_APPEARANCE_FULL_SOURCE_NON_NANITE:'+str(len(models)))
level=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if unreal.EditorAssetLibrary.does_asset_exist(DEST+'/Comparison'):
    assert level.load_level(DEST+'/Comparison')
    actors.destroy_actors(actors.get_all_level_actors())
else:assert level.new_level(DEST+'/Comparison')
level.editor_set_viewport_realtime(True)
level.editor_set_game_view(True)
def spawn(cls,location=(0,0,0),rotation=(0,0,0)):
    return actors.spawn_actor_from_class(cls,unreal.Vector(*location),unreal.Rotator(pitch=rotation[0],yaw=rotation[1],roll=rotation[2]))

light=spawn(unreal.DirectionalLight,(0,0,3000),(-45,145,0))
light.light_component.set_editor_property('intensity',3.0)
light.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
unreal.log('PILOT_LIGHT_ROTATION:'+str(light.get_actor_rotation()))
fill=spawn(unreal.DirectionalLight,(0,0,3000),(-25,-35,0))
fill.light_component.set_editor_property('intensity',1.0)
fill.light_component.set_editor_property('cast_shadows',False)
fill.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
# A constant studio sky provides fixed ambient light and a readable background.
sky_material=unreal.load_asset(DEST+'/M_StudioSky')
if not sky_material:sky_material=unreal.AssetToolsHelpers.get_asset_tools().create_asset('M_StudioSky',DEST,unreal.Material,unreal.MaterialFactoryNew())
mel=unreal.MaterialEditingLibrary
mel.delete_all_material_expressions(sky_material)
sky_material.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_UNLIT)
sky_material.set_editor_property('two_sided',True)
sky_material.set_editor_property('is_sky',True)
sky_color=mel.create_material_expression(sky_material,unreal.MaterialExpressionConstant3Vector)
sky_color.set_editor_property('constant',unreal.LinearColor(.30,.38,.45,1))
mel.connect_material_property(sky_color,'',unreal.MaterialProperty.MP_EMISSIVE_COLOR)
mel.recompile_material(sky_material)
assert unreal.EditorAssetLibrary.save_loaded_asset(sky_material)
dome=spawn(unreal.StaticMeshActor)
dome.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Sphere'))
dome.static_mesh_component.set_material(0,sky_material)
dome.static_mesh_component.set_editor_property('cast_shadow',False)
dome.set_actor_scale3d(unreal.Vector(2000,2000,2000))
sky=spawn(unreal.SkyLight)
sky.light_component.set_editor_property('intensity',.7)
sky.light_component.set_editor_property('real_time_capture',True)
floor=spawn(unreal.StaticMeshActor,(0,0,-10))
floor.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube'))
floor.set_actor_scale3d(unreal.Vector(200,200,.1))
tree=spawn(unreal.StaticMeshActor)
# Standing pawn reference: same six-box, 1.8 m silhouette as Forge.
for minimum,size in [((-.20,-.20,.80),(.40,.40,.70)),((-.16,-.15,1.52),(.32,.30,.28)),((-.14,-.30,.75),(.28,.14,.70)),((-.14,.16,.75),(.28,.14,.70)),((-.14,-.21,0),(.28,.20,.80)),((-.14,.01,0),(.28,.20,.80))]:
    center=[(minimum[i]+size[i]/2)*100 for i in range(3)]
    center[0]+=300
    part=spawn(unreal.StaticMeshActor,center)
    part.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube'))
    part.set_actor_scale3d(unreal.Vector(*size))
camera=spawn(unreal.CameraActor)
camera.camera_component.set_editor_property('field_of_view',50)
# Exposure is locked for every comparison.
pp=spawn(unreal.PostProcessVolume)
pp.set_editor_property('unbound',True)
settings=pp.get_editor_property('settings')
for key,value in [('override_auto_exposure_min_brightness',True),('override_auto_exposure_max_brightness',True),('auto_exposure_min_brightness',1.0),('auto_exposure_max_brightness',1.0)]:
    settings.set_editor_property(key,value)
pp.set_editor_property('settings',settings)
sys.path.insert(0,str(ROOT/'ue-project/Tools'))
from import_tree_appearance_pilot import material
solid=material('M_Solid')
material('M_Leaf',False);material('M_Needle',True)
# Same two-sidedness for opaque/masked foliage comparisons.
opaque=material('M_FoliageOpaque')
opaque.set_editor_property('two_sided',True)
unreal.MaterialEditingLibrary.recompile_material(opaque)
unreal.EditorAssetLibrary.save_loaded_asset(opaque)
jobs=[(name,mode,distance) for name in models for mode in ('opaque','mask') for distance in (1,3)]
state={'index':0,'phase':'setup','time':0,'task':None}
world=unreal.EditorLevelLibrary.get_editor_world()
unreal.SystemLibrary.execute_console_command(world,'r.ProfileGPU.ShowUI 0')

def tick(delta):
    try:
        now=time.monotonic()
        if state['phase']=='setup':
            if state['index']==len(jobs):
                unreal.log('TREE_APPEARANCE_CAPTURE_COMPLETE:'+str(len(jobs)))
                unreal.unregister_slate_post_tick_callback(handle)
                unreal.EditorPythonScripting.set_keep_python_script_alive(False)
                return
            name,mode,distance=jobs[state['index']]
            mesh=unreal.load_asset(models[name]['meshes'][0])
            tree.static_mesh_component.set_static_mesh(mesh)
            for i,slot in enumerate(mesh.get_editor_property('static_materials')):
                mat=slot.get_editor_property('material_interface')
                if 'foliage' in str(slot.get_editor_property('material_slot_name')).lower() and mode=='opaque':mat=opaque
                tree.static_mesh_component.set_material(i,mat)
            center,extent=tree.get_actor_bounds(False)
            radius=max(extent.x,extent.y,extent.z,100)
            position=center+unreal.Vector(radius*3*distance,-radius*3*distance,radius*.5*distance)
            rotation=unreal.MathLibrary.find_look_at_rotation(position,center)
            camera.set_actor_location_and_rotation(position,rotation,False,False)
            unreal.EditorLevelLibrary.set_level_viewport_camera_info(position,rotation)
            state.update(phase='settle',time=now)
        elif state['phase']=='settle' and now-state['time']>12:
            state.update(phase='profile',time=now)
        elif state['phase']=='profile' and now-state['time']>4:
            name,mode,distance=jobs[state['index']]
            path=str(OUT/(name+'-'+mode+'-'+str(distance)+'.png'))
            # Screenshot scheduling can pump Slate recursively: enter the next
            # state before calling it, or the same job is captured repeatedly.
            state.update(phase='capture',time=now,shutter=time.time())
            unreal.log('TREE_APPEARANCE_GPU_CAPTURE_FRAME:'+str(jobs[state['index']]))
            unreal.SystemLibrary.execute_console_command(world,'ProfileGPU')
            state['task']=unreal.AutomationLibrary.take_high_res_screenshot(1280,720,path,camera)
        elif state['phase']=='capture' and now-state['time']>8:
            name,mode,distance=jobs[state['index']]
            image=OUT/(name+'-'+mode+'-'+str(distance)+'.png')
            if not image.exists() or image.stat().st_mtime<state['shutter']:raise RuntimeError('Missing fresh capture '+str(image))
            state['index']+=1;state['phase']='setup'
    except Exception:
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        raise

if os.environ.get('TREE_APPEARANCE_BUILD_MAPS')=='1':
    world.get_world_settings().set_editor_property('default_game_mode',unreal.GameModeBase)
    camera.set_editor_property('auto_activate_for_player',unreal.AutoReceiveInput.PLAYER0)
    for name in models:
        for mode in ('opaque','mask'):
            state.update(index=jobs.index((name,mode,1)),phase='setup')
            tick(0)
            path=DEST+'/Bench_'+name.replace('-','_')+'_'+mode
            assert unreal.EditorLoadingAndSavingUtils.save_map(world,path)
            unreal.log('TREE_APPEARANCE_BENCH_MAP:'+path)
    unreal.log('TREE_APPEARANCE_BENCH_MAPS_COMPLETE')
    mesh_editor=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    enabled=[bool(mesh_editor.get_nanite_settings(unreal.load_asset(r['meshes'][0])).get_editor_property('enabled')) for r in models.values()]
    assert len(set(enabled))==1
    (Path(unreal.Paths.project_dir())/'appearance-renderer.json').write_text(json.dumps(dict(nanite=enabled[0],models=list(models))))
else:
    unreal.EditorPythonScripting.set_keep_python_script_alive(True)
    handle=unreal.register_slate_post_tick_callback(tick)
