"""Shared opt-in L1/L2 support, matching VoxelMarch.usf without adding direct sun.
The host gates Enabled until both GI and propagated-volume modifiers are off.
Existing BaseColor stays lit; this only contributes hemisphere + sun deficit.
"""
import unreal
CODE=r"""
float skyShare=saturate(Normal.z*.5+.5);
float hemi=lerp(Ambient.w,1.,skyShare);
float3 support=Ambient.rgb*hemi;
if(Sun.w>=0.)
{
    float ndl=dot(Normal,Sun.xyz);
    float sunTerm=max(max(Sun.w,.5+.5*ndl),Normal.z*Wrap.w);
    float deficit=max(sunTerm-saturate(ndl),0.);
    support+=Wrap.rgb*deficit;
}
// SkyScale=1 only in this explicitly gated L1/L2 diagnostic. Do not claim
// propagated-volume or directional-GI parity from this material expression.
// VoxelMarch writes this contribution directly into scene color. The ordinary
// material base pass multiplies emissive by View.PreExposure, so compensate
// here to match the marcher's existing scene-color units instead of glowing.
return Albedo*support*saturate(Enabled)*View.OneOverPreExposure;
"""

def validate_collection(collection, require_complete=True):
    # Unreal FName is case-insensitive; sets of raw strings miss collisions.
    if not isinstance(collection, unreal.MaterialParameterCollection):
        raise RuntimeError('Surface lighting path must contain a MaterialParameterCollection')
    scalars=list(collection.get_editor_property('scalar_parameters'))
    vectors=list(collection.get_editor_property('vector_parameters'))
    snames=[str(p.get_editor_property('parameter_name')).casefold() for p in scalars]
    vnames=[str(p.get_editor_property('parameter_name')).casefold() for p in vectors]
    all_names=snames+vnames
    if len(set(all_names)) != len(all_names):
        raise RuntimeError('Collection has duplicate or cross-type parameter names')
    required_vectors={'ambientskyandground','sundirandwrapfloor','wrapcolorandskyboost'}
    if 'enabled' in vnames or required_vectors.intersection(snames):
        raise RuntimeError('Surface lighting collection parameter type mismatch')
    if require_complete:
        if 'enabled' not in snames or not required_vectors.issubset(vnames):
            raise RuntimeError('Surface lighting collection schema incomplete')
        enabled=scalars[snames.index('enabled')]
        if enabled.get_editor_property('default_value') != 0.:
            raise RuntimeError('Diagnostic Enabled default must remain zero')
    return scalars,vectors

def add_surface_lighting(material,albedo):
    mel=unreal.MaterialEditingLibrary
    collection=unreal.load_asset('/Game/Voxel/MPC_VoxelSurfaceLighting')
    if not collection: raise RuntimeError('Run create_surface_lighting_collection.py first')
    validate_collection(collection)
    def param(name):
        n=mel.create_material_expression(material,unreal.MaterialExpressionCollectionParameter)
        n.set_editor_property('collection',collection);n.set_editor_property('parameter_name',name);return n
    normal=mel.create_material_expression(material,unreal.MaterialExpressionPixelNormalWS)
    inputs={'Albedo':albedo,'Normal':normal,'Ambient':param('AmbientSkyAndGround'),
            'Sun':param('SunDirAndWrapFloor'),'Wrap':param('WrapColorAndSkyBoost'),'Enabled':param('Enabled')}
    node=mel.create_material_expression(material,unreal.MaterialExpressionCustom)
    node.set_editor_property('code',CODE);node.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    pins=[]
    for name in inputs:
        p=unreal.CustomInput();p.set_editor_property('input_name',name);pins.append(p)
    node.set_editor_property('inputs',pins)
    for name,source in inputs.items():
        if not mel.connect_material_expressions(source,'',node,name): raise RuntimeError('Surface lighting connection failed: '+name)
    if not mel.connect_material_property(node,'',unreal.MaterialProperty.MP_EMISSIVE_COLOR): raise RuntimeError('Surface lighting emissive connection failed')
