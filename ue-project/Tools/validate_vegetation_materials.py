"""Read fresh saved material shader maps, independently of graph construction."""
import unreal
import time

for name in ('M_VoxelDetailAsset', 'M_VoxelEnvironmentLOD'):
    material = unreal.load_asset('/Game/Voxel/' + name)
    assert material and material.get_editor_property('blend_mode') == unreal.BlendMode.BLEND_MASKED
    assert material.get_editor_property('two_sided')
    unreal.MaterialEditingLibrary.recompile_material(material)
    for attempt in range(12):
        stats = unreal.MaterialEditingLibrary.get_statistics(material)
        if stats.get_editor_property('num_pixel_shader_instructions') > 0:
            break
        time.sleep(2)
    assert stats.get_editor_property('num_pixel_shader_instructions') > 0, name + ' has no compiled shader map'
    unreal.log('VEGETATION_VALID ' + name + ' ' + str(stats))
unreal.log('VEGETATION_VALIDATION_COMPLETE')
