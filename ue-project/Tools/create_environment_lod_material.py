"""Dedicated masked material for the opt-in environment LOD prototype."""
import unreal
from vegetation_material_common import add_vegetation
path='/Game/Voxel/M_VoxelEnvironmentLOD'
m=unreal.load_asset(path)
if not m:
    m=unreal.AssetToolsHelpers.get_asset_tools().create_asset('M_VoxelEnvironmentLOD','/Game/Voxel',unreal.Material,unreal.MaterialFactoryNew())
assert m
m.set_editor_property('blend_mode',unreal.BlendMode.BLEND_MASKED)
mel=unreal.MaterialEditingLibrary
mel.delete_all_material_expressions(m)
color=mel.create_material_expression(m,unreal.MaterialExpressionVertexColor)
power=mel.create_material_expression(m,unreal.MaterialExpressionPower)
power.set_editor_property("const_exponent",2.2)
assert mel.connect_material_expressions(color,"",power,"Base")
assert mel.connect_material_property(power,"",unreal.MaterialProperty.MP_BASE_COLOR)
rough=mel.create_material_expression(m,unreal.MaterialExpressionConstant)
rough.set_editor_property("r",.9)
assert mel.connect_material_property(rough,"",unreal.MaterialProperty.MP_ROUGHNESS)
fade=mel.create_material_expression(m,unreal.MaterialExpressionScalarParameter,-600,400)
fade.set_editor_property('parameter_name','Fade');fade.set_editor_property('default_value',1.)
reverse=mel.create_material_expression(m,unreal.MaterialExpressionScalarParameter,-600,500)
reverse.set_editor_property('parameter_name','Reverse');reverse.set_editor_property('default_value',0.)
pixel=mel.create_material_expression(m,unreal.MaterialExpressionScreenPosition,-600,600)
noise=mel.create_material_expression(m,unreal.MaterialExpressionCustom,-200,400)
noise.set_editor_property('code','float n=frac(sin(dot(floor(Pixel.xy),float2(12.9898,78.233)))*43758.5453); float a=step(n,Fade); return lerp(a,1-a,Reverse);')
noise.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT1)
inputs=[]
for name in ['Pixel','Fade','Reverse']:
    i=unreal.CustomInput();i.set_editor_property('input_name',name);inputs.append(i)
noise.set_editor_property('inputs',inputs)
assert mel.connect_material_expressions(pixel,'PixelPosition',noise,'Pixel')
assert mel.connect_material_expressions(fade,'',noise,'Fade')
assert mel.connect_material_expressions(reverse,'',noise,'Reverse')
add_vegetation(m,color,noise)
mel.recompile_material(m)
unreal.EditorAssetLibrary.save_loaded_asset(m)
unreal.log('EnvironmentLOD dither material saved')
