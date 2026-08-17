"""Author M_VoxelDetailAsset (TASK #7, detail-lattice ground-cover rendering).

The mesh side (VoxelDetailAssetSubsystem.cpp) bakes each voxel face's FINAL
colour -- vxc::kMaterialPalette face colour, face-classed top/side/bottom,
plus the palette's per-voxel lightness jitter -- into the static mesh's vertex
colours. So this material is deliberately trivial: the one non-obvious step is
the sRGB decode, and it is load-bearing:

  * The C++ side hands BuildFromMeshDescriptions LINEAR floats, and the
    engine's BuildFromMeshDescription encodes vertex colours into the 8-bit
    colour buffer with FLinearColor::ToFColor(true) -- i.e. the buffer holds
    sRGB bytes (verified in the 5.8 engine source, StaticMesh.cpp).
  * The GPU reads that buffer as raw UNORM (VET_Color has no sRGB decode), so
    the VertexColor node delivers sRGB-ENCODED values.
  * BaseColor expects linear. Power(2.2) is the decode. Skipping it renders
    every flower washed-out and pale; double-applying it renders everything
    nearly black. One Power node, exactly once, here.

Storing sRGB in the 8 bits and decoding in the shader is also the right
precision trade: sRGB spends its code points perceptually, so dark foliage
greens survive quantisation.

Everything else follows M_VoxelTerrain's conventions where they transfer:
Roughness constant 0.9, one-sided (absolute quad winding is the same proven
convention FVoxelChunkSceneProxy uses -- the subsystem copies it), Opaque.
No DebugTint, no ring fades, no biome graph: a 5 cm tuft is drawn only inside
a ~112 m ring, entirely within R0, so there is no LOD seam to dither across.

If this asset does not exist at runtime the subsystem falls back to the
engine default material -- grey cover with correct shapes and a warning
naming this script -- so running this file is a visual upgrade, never a
prerequisite for the code path to work.

Run via (one editor per box -- do not run while another editor is up):
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash
"""

import unreal

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_VoxelDetailAsset"
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

    vertex_color = mel.create_material_expression(
        material, unreal.MaterialExpressionVertexColor, -500, -50)

    # The sRGB decode (see module docstring). ConstExponent rather than a
    # wired exponent pin: it is a constant of the encoding, not a tunable.
    power = mel.create_material_expression(
        material, unreal.MaterialExpressionPower, -300, -50)
    power.set_editor_property("const_exponent", 2.2)
    # Every connection is checked: a silently-failed pin connect produced an
    # invisible-terrain material once (2026-07-19) and cost a debug session.
    if not mel.connect_material_expressions(vertex_color, "", power, "Base"):
        raise RuntimeError("connect vertex_color -> power.Base failed")
    if not mel.connect_material_property(power, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect power -> BaseColor failed")

    roughness = mel.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -300, 150)
    roughness.set_editor_property("parameter_name", "Roughness")
    roughness.set_editor_property("default_value", 0.9)
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness failed")

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_VoxelDetailAsset created and saved at " + FULL_PATH)


main()
