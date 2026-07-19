"""Author M_VoxelTerrain (docs/m1-plan.md stage 1 deliverable 4).

Stage-1 simplification (explicitly sanctioned by the task spec): no per-material
LUT / texture arrays yet (that is stage 3). Vertex color carries R=material id,
G=AO (0/85/170/255 for the 4 mesher AO levels), so stage 1 just needs lit,
AO-shaded terrain: BaseColor = a flat albedo tint * VertexColor.G, Roughness
constant 0.9. Two-sided is enabled defensively -- see the M1 stage 1 report for
why (quad winding could not be verified visually in this headless run).

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash
"""

import unreal

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_VoxelTerrain"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME


def main():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
        unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material, factory)
    if material is None:
        raise RuntimeError("Failed to create material asset at " + FULL_PATH)

    mel = unreal.MaterialEditingLibrary

    vertex_color = mel.create_material_expression(material, unreal.MaterialExpressionVertexColor, -500, -50)

    albedo_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -500, 150)
    albedo_tint.set_editor_property("constant", unreal.LinearColor(0.5, 0.45, 0.4, 1.0))

    ao_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -200, 50)
    mel.connect_material_expressions(albedo_tint, "", ao_multiply, "A")
    mel.connect_material_expressions(vertex_color, "G", ao_multiply, "B")
    mel.connect_material_property(ao_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -200, 250)
    roughness.set_editor_property("r", 0.9)
    mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # Defensive: FVoxelChunkSceneProxy triangulates quads with correct
    # *relative* winding between +/- faces but the absolute UE front-face
    # winding convention could not be visually verified in this headless
    # session (no PIE allowed) -- two-sided removes any risk of culled faces
    # while normals (set explicitly per-vertex, not derived from winding)
    # stay correct either way.
    material.set_editor_property("two_sided", True)

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_VoxelTerrain created and saved at " + FULL_PATH)


main()
