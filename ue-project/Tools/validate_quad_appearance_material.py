"""Compile the saved terrain material without changing or saving its graph."""
import json
import unreal
from pathlib import Path
material=unreal.load_asset('/Game/Voxel/M_VoxelTerrain')
assert material, 'Missing terrain material'
assert material.get_editor_property('blend_mode') == unreal.BlendMode.BLEND_MASKED, 'Quad cutouts require masked depth/shadow passes'
unreal.MaterialEditingLibrary.recompile_material(material)
stats=unreal.MaterialEditingLibrary.get_statistics(material)
count=stats.get_editor_property('num_pixel_shader_instructions')
assert count>0, 'Terrain material has no compiled pixel shader'
report={'material':material.get_path_name(),'blend_mode':str(material.get_editor_property('blend_mode')),
        'pixel_instructions':count,'scope':'Saved material mode and shader-map compilation; not raster/depth/shadow pixel parity'}
Path('D:/voxelsim/asset-forge/out/tree-runtime-appearance-v1/quad-material-validation').mkdir(parents=True,exist_ok=True)
Path('D:/voxelsim/asset-forge/out/tree-runtime-appearance-v1/quad-material-validation/report.json').write_text(json.dumps(report,indent=2))
unreal.log('QUAD_MATERIAL_VALIDATION_COMPLETE '+json.dumps(report))
