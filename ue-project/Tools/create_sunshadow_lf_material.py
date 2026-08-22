"""Author M_VoxelSunShadowLF -- the light-function that injects the marched
sun-shadow mask into the sun's screen shadow mask.

S2 of docs/shadow-march-design-2026-08-20.md. Pattern: Tools/create_voxel_material.py
(every connection checked -- a silently-failed pin connect produced an
invisible-terrain material once (2026-07-19); the same discipline applies
here, and doubly so for a LIGHT FUNCTION, whose failure mode is not a broken
picture but a sun that quietly ignores the marched shadows).

WHAT THIS MATERIAL IS. One texture parameter and one Custom node. The engine
evaluates light-function materials as a screen pass into the sun's
ScreenShadowMaskTexture (LightRendering.cpp:2379), multiplicatively composed
with whatever conventional shadows (props' CSM) already wrote there. The
Custom node samples the marched visibility mask -- written per frame by
FVoxelShadowMarchExtension into the render target the runtime binds to the
texture parameter -- at the pixel's buffer UV, reconstructed from
Parameters.ScreenPosition. That ScreenPosition is computed by the light
function pass from the SAME depth buffer the mask was marched from
(LightFunctionCommon.ush:74: mul(world-from-LookupDeviceZ, TranslatedWorldToClip)),
so the mapping is exact by construction, not by resolution luck.

THREE LOAD-BEARING CHOICES, do not "simplify" any of them away:

1. THE WORLDPOSITION NODE IS THE ATLAS ANCHOR. UE 5.8 routes light functions
   through a 2D light-function ATLAS -- evaluated in LIGHT-space tile UVs,
   where a screen-UV sample reads garbage -- whenever the material is judged
   "atlas compatible". Compatibility is inferred by the material TRANSLATOR
   (HLSLMaterialTranslator.cpp:1769: no vertex position, no scene depth, no
   scene textures, no texcoord manipulation), and a Custom node's HLSL is
   OPAQUE to that analysis -- a graph of only TextureObject + Custom would be
   judged compatible and silently mis-routed. Compiling a WorldPosition
   expression sets bUsesVertexPosition (:6276), which forces the classic
   screen-space path (LightRendering.cpp:1853 / CanLightUsesAtlasForUnbatchedLight
   :1372-1390). The WP input's contribution to the output is exactly zero;
   its COMPILATION is the entire point.

2. THE DEFAULT TEXTURE IS WHITE -- FAIL-LIT. If the runtime never binds the
   render target (mode != 2, or the subsystem declined), the parameter falls
   back to its default and the sun is UNCHANGED. A black or unset default
   would darken the whole world in a way that reads as a lighting bug two
   subsystems away.

3. THE PARAMETER NAME IS A CONTRACT. "VoxelSunShadowMask" is mirrored in
   VoxelShadowMarch.cpp (kSunShadowMaskParamName). Renaming one side does not
   fail -- SetTextureParameterValue on a missing name is a silent no-op and
   the material samples its white default forever, i.e. shadows simply never
   appear. One grep re-checks the pair: VoxelSunShadowMask.

Runtime knobs this material relies on (all set by UVoxelShadowMarchSubsystem
when it wires the sun): LightFunctionDisabledBrightness = 1 (fail-lit when the
engine fades the function out), fade distance pushed far. And
r.LightFunctionQuality=1 is pinned in DefaultEngine.ini [ConsoleVariables] --
it lives in the same [ShadowQuality@0] scalability block that silently zeroed
r.ShadowQuality for the project's whole measurement history.

Run via (the owner's box -- editor-bound, one-time, and the produced asset at
Content/Voxel/M_VoxelSunShadowLF.uasset MUST be checked in):

  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash
"""

import unreal

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_VoxelSunShadowLF"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME

MASK_PARAM_NAME = "VoxelSunShadowMask"  # mirrored: VoxelShadowMarch.cpp kSunShadowMaskParamName
WHITE_TEXTURE_PATH = "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"

CUSTOM_CODE = """// Screen-UV sample of the marched sun-shadow visibility mask (1 = lit).
// Parameters.ScreenPosition is the pre-divide clip position the light-function
// pass computed from the SAME depth the mask was marched from
// (LightFunctionCommon.ush:74), so this mapping is exact by construction.
// WP anchors atlas-incompatibility (see the generator script header); its
// arithmetic contribution is exactly zero.
float4 SP = Parameters.ScreenPosition;
float2 UV = (SP.xy / SP.w) * ResolvedView.ScreenPositionScaleBias.xy
          + ResolvedView.ScreenPositionScaleBias.wz;
float Vis = Texture2DSampleLevel(MaskTex, MaskTexSampler, UV, 0).r;
return Vis + 0.0f * saturate(abs(WP.x) * 1e-30f);
"""


def main():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    mel = unreal.MaterialEditingLibrary

    if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
        # Regeneration is deliberate and idempotent: delete and rebuild, the
        # family convention. The asset is small and carries no hand edits.
        unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material, factory)
    if material is None:
        raise RuntimeError("create_asset failed for " + FULL_PATH)

    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)

    # --- the mask parameter, defaulting WHITE (fail-lit) --------------------
    white = unreal.load_object(None, WHITE_TEXTURE_PATH)
    if white is None:
        raise RuntimeError("engine white texture not found at " + WHITE_TEXTURE_PATH)
    mask_param = mel.create_material_expression(
        material, unreal.MaterialExpressionTextureObjectParameter, -600, -100)
    mask_param.set_editor_property("parameter_name", MASK_PARAM_NAME)
    mask_param.set_editor_property("texture", white)
    # The mask is a linear R8 visibility factor; an sRGB decode would bend the
    # 0..1 multiplier and read as wrong shadow density, not as an error.
    mask_param.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

    # --- the atlas anchor ---------------------------------------------------
    world_pos = mel.create_material_expression(
        material, unreal.MaterialExpressionWorldPosition, -600, 120)

    # --- the sampler --------------------------------------------------------
    custom = mel.create_material_expression(material, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property("description", "SampleSunShadowMask")
    custom.set_editor_property("code", CUSTOM_CODE)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    inputs = []
    for name in ("MaskTex", "WP"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)

    # --- checked wiring -----------------------------------------------------
    if not mel.connect_material_expressions(mask_param, "", custom, "MaskTex"):
        raise RuntimeError("connect %s -> Custom.MaskTex failed" % MASK_PARAM_NAME)
    if not mel.connect_material_expressions(world_pos, "", custom, "WP"):
        raise RuntimeError("connect WorldPosition -> Custom.WP failed -- WITHOUT THIS "
                           "the material is judged light-function-atlas COMPATIBLE and "
                           "is evaluated in light space, where the screen-UV sample "
                           "reads garbage. See the script header.")
    if not mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("connect Custom -> EmissiveColor failed")

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    # Read-back verification, not trust: the domain is what routes this
    # material into RenderLightFunction at all.
    domain = material.get_editor_property("material_domain")
    if domain != unreal.MaterialDomain.MD_LIGHT_FUNCTION:
        raise RuntimeError("material domain read back as %s, not LightFunction" % str(domain))

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_VoxelSunShadowLF created and saved at " + FULL_PATH +
               " (param '" + MASK_PARAM_NAME + "', white fail-lit default, WP atlas anchor)")


main()
