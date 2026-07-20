"""Author M_VoxelClipmap (docs/m2-plan.md Band 3 first slice, "Material" row).

Pattern: Tools/create_voxel_material.py (every connection checked -- a
silently-failed pin connect produced an invisible-terrain material once
(2026-07-19) and cost a debug session; same discipline applies here).

Slope+height tint, driven entirely by per-vertex data computed on the CPU in
AVoxelClipmapActor::RebuildLevel (VoxelClipmapActor.cpp) rather than in the
material graph -- VertexColor.R carries a 0..1 slope factor (0 = flat, 1 =
near-vertical), VertexColor.G carries a 0..1 snow factor (ramped over a band
around the amplifier's 2800m snowline, voxel-core/src/amplifier.cpp /
worldgen.hlsl MAT_SNOW threshold). Two lerps: flat-green -> steep-grey by R,
then that result -> white by G. Ocean is handled entirely by the existing
AVoxelOceanActor plane (the clipmap mesh is allowed to dip below z=0; water
covers it, per m2-plan.md's binding decision).

Also adds the same DebugTint VectorParameter (default opaque white) pattern
as M_VoxelTerrain, multiplied into BaseColor after the slope/snow lerps, so
AVoxelClipmapActor can cyan-tint every level under voxel.Debug.Rings (mode 2)
via a per-component MID, exactly like UVoxelChunkComponent::SetDebugTint does
for ring chunks (VoxelDebug::HeightmapBandTint, VoxelDebug.h/.cpp).

Two-sided (deviation from M_VoxelTerrain, which is one-sided once winding was
verified visually on 5.8): the clipmap's triangle winding was picked by hand
in VoxelClipmapActor.cpp and could not be verified interactively for this
headless task (same risk create_voxel_material.py's history flags) -- two-
sided guarantees the terrain renders right-side up from above regardless,
at a modest overdraw cost that's acceptable for 4 low-poly (65x65) sections.
Follow-up: flip to one-sided once a screenshot confirms correct winding.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash
"""

import unreal

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_VoxelClipmap"
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

    vertex_color = mel.create_material_expression(material, unreal.MaterialExpressionVertexColor, -700, -50)

    green_low_flat = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -700, 120)
    green_low_flat.set_editor_property("constant", unreal.LinearColor(0.15, 0.42, 0.13, 1.0))

    grey_steep = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -700, 260)
    grey_steep.set_editor_property("constant", unreal.LinearColor(0.45, 0.44, 0.42, 1.0))

    white_snow = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -700, 400)
    white_snow.set_editor_property("constant", unreal.LinearColor(0.95, 0.96, 0.98, 1.0))

    # lerp1 = Lerp(green_low_flat, grey_steep, VertexColor.R) -- flat/low ->
    # steep tint by slope.
    lerp_slope = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -380, 180)
    if not mel.connect_material_expressions(green_low_flat, "", lerp_slope, "A"):
        raise RuntimeError("connect green_low_flat -> lerp_slope.A failed")
    if not mel.connect_material_expressions(grey_steep, "", lerp_slope, "B"):
        raise RuntimeError("connect grey_steep -> lerp_slope.B failed")
    if not mel.connect_material_expressions(vertex_color, "R", lerp_slope, "Alpha"):
        raise RuntimeError("connect vertex_color.R -> lerp_slope.Alpha failed")

    # lerp2 = Lerp(lerp1, white_snow, VertexColor.G) -- slope-tinted result ->
    # white above the snowline band.
    lerp_snow = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -100, 300)
    if not mel.connect_material_expressions(lerp_slope, "", lerp_snow, "A"):
        raise RuntimeError("connect lerp_slope -> lerp_snow.A failed")
    if not mel.connect_material_expressions(white_snow, "", lerp_snow, "B"):
        raise RuntimeError("connect white_snow -> lerp_snow.B failed")
    if not mel.connect_material_expressions(vertex_color, "G", lerp_snow, "Alpha"):
        raise RuntimeError("connect vertex_color.G -> lerp_snow.Alpha failed")

    # docs/debug-tooling-plan.md P1 palette: "heightmap band cyan"
    # (VoxelDebug::HeightmapBandTint) -- default opaque white, the
    # multiplicative identity, so non-debug rendering is unchanged.
    debug_tint = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -100, 460)
    debug_tint.set_editor_property("parameter_name", "DebugTint")
    debug_tint.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    tint_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 150, 350)
    if not mel.connect_material_expressions(lerp_snow, "", tint_multiply, "A"):
        raise RuntimeError("connect lerp_snow -> tint_multiply.A failed")
    if not mel.connect_material_expressions(debug_tint, "", tint_multiply, "B"):
        raise RuntimeError("connect debug_tint -> tint_multiply.B failed")
    if not mel.connect_material_property(tint_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect tint_multiply -> BaseColor failed")

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -380, 500)
    roughness.set_editor_property("r", 0.9)
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness failed")

    # Two-sided -- see module docstring for why (winding not visually
    # verifiable in this headless task).
    material.set_editor_property("two_sided", True)

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_VoxelClipmap created and saved at " + FULL_PATH)


main()
