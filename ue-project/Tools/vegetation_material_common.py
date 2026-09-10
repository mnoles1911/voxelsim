"""Masked cubic foliage and bounded weather-driven vertex motion.

UV0: asset-local planar metres. UV1: normalized root height, asset height cm.
Vertex alpha: 1 inert, .75 wood, .5 foliage, .25 herbaceous/petals.
These are geometry metadata, never inferred from an RGB colour.
"""
import unreal
from tree_foliage_mask import SHAPED_MASK_BODY

# Shared with the renderer's 30 cm bounds expansion. This is visual bending,
# not a collision deformation or a load applied to Chaos falling bodies.
WIND_CODE = r"""
float leaf = 1-step(.08,abs(Class-.5));
float wood = 1-step(.08,abs(Class-.75));
float soft = 1-step(.08,abs(Class-.25));
float moving = saturate(leaf+wood+soft);
float speed = length(Wind.xy);
float2 direction = Wind.xy / max(speed,.001);
float strength = saturate((speed+Wind.z*.35)/10.0)*saturate(Valid)*saturate(Enabled);
float h = saturate(Data.x);
float height = max(Data.y,1);
// Continuous world-space phase travels WITH the air; no per-voxel noise.
float phase = dot(World.xy, direction) * .002;
float slow = .62 + .24*sin(Time*1.15-phase) + .14*sin(Time*.53-phase*.61);
float bend = h*h * min(24.0,height*.06) * slow;
// Leaves and soft stems flutter gently, wood participates in coherent bend.
float flutter = (leaf+soft)*h*min(3.0,height*.015)*sin(Time*3.7-phase*2.3);
float2 offset = direction*bend + float2(-direction.y,direction.x)*flutter;
return float3(offset,0)*strength*moving;
"""

LEGACY_MASK_CODE = r"""
float foliage = 1-step(.08,abs(Class-.5));
// Larger trees need holes that survive a whole-canopy view: a 1 cm mask was
// already filled by the distance filter at 21 m. Tree cells are 4 cm, grouped
// in 8 cm gaps. Small flowers keep appropriately smaller 1.25-4 cm cells.
// Asset height, rather than voxel pitch, keeps all LODs on the same pattern.
float cellM = lerp(.0125,.04,saturate((Data.y-40.0)/160.0));
float2 pixel = floor(UV/cellM);
float2 cluster = floor(pixel/2.0);
float n = frac(sin(dot(cluster,float2(127.1,311.7)))*43758.5453);
float fine = frac(sin(dot(pixel,float2(269.5,183.3)))*43758.5453);
float clustered = step(.36,n);
float footprint = max(length(ddx(UV)),length(ddy(UV)))/cellM;
// Lose the small speckle first; retain the larger gaps through useful viewing
// distances. Only close those when even their 8 cm extent is sub-pixel.
float detail = lerp(step(.10,fine),1,smoothstep(.65,1.3,footprint));
float coverage = clustered*detail;
coverage = lerp(coverage,1,smoothstep(2.0,5.0,footprint));
return lerp(1,coverage,foliage*saturate(Cutout));
"""

# Existing non-tree plants keep their reviewed material until they receive a
# matching appearance source. Only leaf geometry can acquire holes.
MASK_CODE = "if(TreeAppearance>.5){\n"+SHAPED_MASK_BODY+r"""
float foliage=1-step(.08,abs(Class-.5));
return lerp(1,step(.5,FoliageCoverage),foliage*saturate(Cutout));
}
"""+LEGACY_MASK_CODE


def add_vegetation(material, vertex_color, lod_mask=None):
    mel = unreal.MaterialEditingLibrary
    def node(cls):
        return mel.create_material_expression(material, cls)
    def link(src, output, dst, pin):
        if not mel.connect_material_expressions(src, output, dst, pin):
            raise RuntimeError('Vegetation material connection failed: '+pin)
    def scalar(name, value):
        n=node(unreal.MaterialExpressionScalarParameter)
        n.set_editor_property('parameter_name',name)
        n.set_editor_property('default_value',value)
        return n
    def custom(code, outputs, connections):
        n=node(unreal.MaterialExpressionCustom)
        n.set_editor_property('code',code)
        n.set_editor_property('output_type',outputs)
        inputs=[]
        for name in connections:
            i=unreal.CustomInput();i.set_editor_property('input_name',name);inputs.append(i)
        n.set_editor_property('inputs',inputs)
        for name,(src,out) in connections.items():link(src,out,n,name)
        return n
    collection=unreal.load_asset('/Game/Voxel/MPC_VoxelSky')
    if not collection:
        raise RuntimeError('Existing MPC_VoxelSky is required; do not recreate it.')
    names={str(p.get_editor_property('parameter_name')) for prop in ('scalar_parameters','vector_parameters') for p in collection.get_editor_property(prop)}
    def param(name):
        if name not in names:raise RuntimeError('Existing weather collection is missing '+name)
        n=node(unreal.MaterialExpressionCollectionParameter)
        n.set_editor_property('collection',collection)
        n.set_editor_property('parameter_name',name)
        return n
    uv=node(unreal.MaterialExpressionTextureCoordinate)
    data=node(unreal.MaterialExpressionTextureCoordinate);data.set_editor_property('coordinate_index',1)
    world=node(unreal.MaterialExpressionWorldPosition)
    # Excluding shader displacement avoids a feedback loop in the mask/phase.
    world.set_editor_property('world_position_shader_offset',unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    time=node(unreal.MaterialExpressionTime)
    offset=custom(WIND_CODE,unreal.CustomMaterialOutputType.CMOT_FLOAT3,{
        'World':(world,''),'Time':(time,''),'Data':(data,''),
        'Class':(vertex_color,'A'),'Wind':(param('WindVectorMS'),''),
        'Valid':(param('WindFieldValid'),''),'Enabled':(scalar('WindEnabled',1.),'')})
    assert mel.connect_material_property(offset,'',unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    mask=custom(MASK_CODE,unreal.CustomMaterialOutputType.CMOT_FLOAT1,{
        'UV':(uv,''),'Data':(data,''),'Class':(vertex_color,'A'),'Cutout':(scalar('FoliageCutout',1.),''),
        'TreeAppearance':(scalar('TreeAppearance',0.),''),'Needle':(scalar('TreeNeedle',0.),''),'Opening':(scalar('TreeOpening',.45),'')})
    if lod_mask:
        product=node(unreal.MaterialExpressionMultiply)
        link(mask,'',product,'A');link(lod_mask,'',product,'B');mask=product
    assert mel.connect_material_property(mask,'',unreal.MaterialProperty.MP_OPACITY_MASK)
    material.set_editor_property('blend_mode',unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property('two_sided',True)
    material.set_editor_property('opacity_mask_clip_value',.333)
