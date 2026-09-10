"""Dedicated masked material for the opt-in environment LOD prototype."""
import unreal
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from vegetation_material_common import add_vegetation
from voxel_surface_lighting_common import add_surface_lighting
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
appearance=mel.create_material_expression(m,unreal.MaterialExpressionScalarParameter)
appearance.set_editor_property('parameter_name','TreeAppearance');appearance.set_editor_property('default_value',0.)
decode=mel.create_material_expression(m,unreal.MaterialExpressionCustom)
decode.set_editor_property('code','float3 exact=float3(C.r<=.04045?C.r/12.92:pow((C.r+.055)/1.055,2.4),C.g<=.04045?C.g/12.92:pow((C.g+.055)/1.055,2.4),C.b<=.04045?C.b/12.92:pow((C.b+.055)/1.055,2.4)); return lerp(Legacy,exact,step(.5,Enabled));')
decode.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT3)
decode_inputs=[]
for name in ('C','Legacy','Enabled'):
    item=unreal.CustomInput();item.set_editor_property('input_name',name);decode_inputs.append(item)
decode.set_editor_property('inputs',decode_inputs)
for src,pin in ((color,'C'),(power,'Legacy'),(appearance,'Enabled')):assert mel.connect_material_expressions(src,'',decode,pin)
assert mel.connect_material_property(decode,"",unreal.MaterialProperty.MP_BASE_COLOR)
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
add_surface_lighting(m,power)
mel.recompile_material(m)
if not unreal.EditorAssetLibrary.save_loaded_asset(m):
    raise RuntimeError('Failed to save environment LOD material')
unreal.log('EnvironmentLOD dither material saved')
