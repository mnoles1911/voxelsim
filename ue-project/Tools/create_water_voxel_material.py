"""Author M_WaterVoxel (W2 task spec item 4 "Rendering v0"): the active-water
voxel material UWaterChunkComponent/FWaterChunkSceneProxy render with
(VoxelWaterChunkComponent.h/.cpp), distinct from M_Ocean's implicit static
surface plane (AVoxelOceanActor -- that one keeps existing unchanged; active
water renders ON TOP of it per the task spec, overlap acceptable v0).

Same headless-Python pattern as create_voxel_material.py / create_ocean_material.py
(checked connections throughout -- see create_voxel_material.py's comment on
why a silently-failed pin connect is worth guarding against explicitly).

Translucent blue, vertex-color-driven AO (reusing the SAME vertex-color
convention as M_VoxelTerrain: R=material id [unused by this material, always
a fixed placeholder from the water mesher's sampler], G=AO 0/85/170/255,
B unused, A=255) so the water mesher's greedy-mesher AO looks shaded
consistently with terrain rather than flat-lit. Deliberately basic -- v0 does
not shade by fill fraction (surface cells with partial fill render as a full
cube per the task spec; per-fill-fraction shading/foam/caustics are W5
polish, same scope line create_ocean_material.py draws for the ocean plane).

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash
"""

import unreal

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_WaterVoxel"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME


def main():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
        unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material, factory)
    if material is None:
        raise RuntimeError("Failed to create material asset at " + FULL_PATH)

    # Translucent (task spec: "translucent blue material"); two-sided since a
    # meshed water brick's faces can be viewed from inside a flooding cavity
    # before the player has line of sight to its "outside" face, same
    # reasoning M_Ocean's two-sided flag documents.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("two_sided", True)

    mel = unreal.MaterialEditingLibrary

    base_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -500, -300)
    base_tint.set_editor_property("constant", unreal.LinearColor(0.05, 0.25, 0.55, 1.0))  # translucent blue

    vertex_color = mel.create_material_expression(material, unreal.MaterialExpressionVertexColor, -500, -100)

    ao_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -200, -200)
    if not mel.connect_material_expressions(base_tint, "", ao_multiply, "A"):
        raise RuntimeError("connect base_tint -> ao_multiply.A failed")
    if not mel.connect_material_expressions(vertex_color, "G", ao_multiply, "B"):
        raise RuntimeError("connect vertex_color.G -> ao_multiply.B failed")
    if not mel.connect_material_property(ao_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect ao_multiply -> BaseColor failed")

    opacity = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -200, -50)
    opacity.set_editor_property("r", 0.55)
    if not mel.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY):
        raise RuntimeError("connect opacity -> Opacity failed")

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -200, 50)
    roughness.set_editor_property("r", 0.1)
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness -> Roughness failed")

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_WaterVoxel created and saved at " + FULL_PATH)


main()
