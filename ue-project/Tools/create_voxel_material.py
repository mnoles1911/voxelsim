"""Author M_VoxelTerrain (docs/m1-plan.md stage 1 deliverable 4).

Stage-1 simplification (explicitly sanctioned by the task spec): no per-material
LUT / texture arrays yet (that is stage 3). Vertex color carries R=material id,
G=AO (0/85/170/255 for the 4 mesher AO levels), so stage 1 just needs lit,
AO-shaded terrain: BaseColor = a flat albedo tint * VertexColor.G, Roughness
constant 0.9. Two-sided is enabled defensively -- see the M1 stage 1 report for
why (quad winding could not be verified visually in this headless run).

docs/debug-tooling-plan.md P1 "Chunk-state tints": also adds a DebugTint
VectorParameter (default opaque white) multiplied into BaseColor after the AO
multiply, so UVoxelChunkComponent::SetDebugTint (voxel.Debug mode 2 +
voxel.Debug.ChunkStates) can recolor a chunk via a per-component MID without
touching the shared material when debug tinting is off (white is the
multiplicative identity).

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
    # Every connection is checked: a silently-failed pin connect produced an
    # invisible-terrain material once (2026-07-19) and cost a debug session.
    if not mel.connect_material_expressions(albedo_tint, "", ao_multiply, "A"):
        raise RuntimeError("connect albedo_tint -> multiply.A failed")
    if not mel.connect_material_expressions(vertex_color, "G", ao_multiply, "B"):
        raise RuntimeError("connect vertex_color.G -> multiply.B failed")

    # docs/debug-tooling-plan.md P1: DebugTint VectorParameter, default opaque
    # white (the multiplicative identity -- SetMaterial/SetVectorParameterValue
    # only ever touches a per-component MID, so the shared material's default
    # keeps every non-debug chunk visually unchanged).
    debug_tint = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -200, 150)
    debug_tint.set_editor_property("parameter_name", "DebugTint")
    debug_tint.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    tint_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 0, 50)
    if not mel.connect_material_expressions(ao_multiply, "", tint_multiply, "A"):
        raise RuntimeError("connect ao_multiply -> tint_multiply.A failed")
    if not mel.connect_material_expressions(debug_tint, "", tint_multiply, "B"):
        raise RuntimeError("connect debug_tint -> tint_multiply.B failed")
    if not mel.connect_material_property(tint_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect tint_multiply -> BaseColor failed")

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -200, 250)
    roughness.set_editor_property("r", 0.9)
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness failed")

    # One-sided: absolute quad winding was verified empirically on 5.8
    # (2026-07-19) — front faces render correctly without two-sided cost.

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_VoxelTerrain created and saved at " + FULL_PATH)


main()
