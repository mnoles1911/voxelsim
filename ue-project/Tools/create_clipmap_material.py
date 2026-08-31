"""Author M_VoxelClipmap (docs/m2-plan.md Band 3 first slice, "Material" row).

Pattern: Tools/create_voxel_material.py (every connection checked -- a
silently-failed pin connect produced an invisible-terrain material once
(2026-07-19) and cost a debug session; same discipline applies here).

BIOME UPGRADE (2026-07-22). This used to be a two-lerp graph over a private
vertex-colour convention: VertexColor.R = slope, .G = snow, both precomputed in
AVoxelClipmapActor::RebuildLevel. That convention was completely different from
the voxel chunks' (R = material id, G = AO), which is a large part of why the
50 km vista rendered pale green while the near field rendered flat beige -- two
independently authored colour schemes with nothing forcing them to agree.

Both meshes now write ONE encoding (R = material id, G = shade, B = temperature,
A = precipitation; see VoxelClimateProbe.h) and this material decodes it with
the SAME shared graph M_VoxelTerrain uses (terrain_material_common.py). Slope
and snow did not go away -- slope is now derived per pixel from the interpolated
vertex normal (which RebuildLevel already computes and hands to the PMC) and
snow from temperature plus world Z, so the clipmap gets a smoother band than the
old 64-512 m/vertex grid could express, and the voxel near field gets the same
snowline for the first time.

Ocean is still handled entirely by the existing AVoxelOceanActor plane (the
clipmap mesh is allowed to dip below z=0; water covers it, per m2-plan.md's
binding decision) -- but sub-sea-level vertices now emit MAT_MUD so the sea bed
under shallow water reads as sediment rather than as drowned grassland.

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

import os
import sys

import unreal

# Shared biome graph, same directory. A -run=pythonscript commandlet does not
# put the script's own directory on sys.path, so add it explicitly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from terrain_material_common import build_terrain_base_color  # noqa: E402
from sky_star_graph import SkyGraphBuilder  # noqa: E402
from bathy_field_graph import sample_bathy_field  # noqa: E402

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

    # SkyGraphBuilder + the bathymetry sample, for the same reason M_VoxelTerrain
    # takes them: the wet-shore band must not stop at the seam between the near
    # voxel terrain and the clipmap. The 960 m field window is far smaller than a
    # clipmap level, so in practice this term only ever fires on the innermost
    # ring -- which is precisely where the seam is, and precisely where a
    # discontinuity would be visible.
    sky_collection = unreal.load_object(None, "/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky")
    if sky_collection is None:
        raise RuntimeError(
            "MPC_VoxelSky not found -- M_VoxelClipmap now reads the bathymetry window's "
            "placement from it for wet shores. Run Tools/create_sky_material.py first.")
    b = SkyGraphBuilder(material, sky_collection)
    bathy = sample_bathy_field(b)

    # T_VoxelDetail UV. The voxel material uses TexCoord0 (world-planar metres
    # wrapped to 32 m); the clipmap CANNOT -- its SharedUV0 is a plain [0,1]^2
    # grid over a level that spans up to 50 km, and AbsoluteWorldPosition out
    # there is exactly the LWC precision case the voxel path wraps to avoid.
    # Scaling the [0,1]^2 grid instead gives ~28 repeats across each level,
    # which at clipmap distances is large-scale mottling to break up the flat
    # green -- the only thing detail can usefully do at 64-512 m per vertex.
    tex_coord = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, 400)
    tex_coord.set_editor_property("coordinate_index", 0)
    detail_uv = b.mul(tex_coord, b.scalar("DetailTileRepeats", 28.0))

    base_color, snow_w, base_color_out, wet = build_terrain_base_color(
        b, vertex_color, detail_uv, "", bathy=bathy,
        # Full strength, unlike the voxel material's 0.35: a clipmap vertex
        # normal is a REAL terrain normal (central-difference heightmap
        # gradient, RebuildLevel pass 2), so a steep face here genuinely is
        # exposed rock rather than a 10 cm voxel step riser.
        rock_slope_strength=1.0,
        # Weaker than the voxel material's: at 64-512 m per vertex the fine
        # band is far below a pixel and would only add noise for TSR to chew on.
        detail_fine_strength=0.06,
        detail_coarse_strength=0.16,
    )

    # G is 255 on every clipmap vertex (a heightfield has no per-vertex AO), so
    # this multiply is the identity today. It is kept so the graph stays
    # structurally identical to M_VoxelTerrain's -- if the clipmap ever grows an
    # AO or shadow term, it has somewhere to go, and the two materials do not
    # silently diverge in the meantime.
    lerp_snow = b.mul(base_color, vertex_color, base_color_out, "G")

    # docs/debug-tooling-plan.md P1 palette: "heightmap band cyan"
    # (VoxelDebug::HeightmapBandTint) -- default opaque white, the
    # multiplicative identity, so non-debug rendering is unchanged.
    debug_tint = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -100, 460)
    debug_tint.set_editor_property("parameter_name", "DebugTint")
    debug_tint.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    # --- ONE COLOUR AUTHORITY (2026-08-30) ----------------------------------
    #
    # VertexAlbedoWeight 1.0 takes BaseColor straight from VertexColor.RGB --
    # the palette colour the CPU already computed for this vertex's REAL surface
    # material (AVoxelClipmapActor pass 2, via vxc::classifyBiome ->
    # vxc::biomeSurfaceMaterial -> vxc::kMaterialPalette) -- and bypasses the
    # biome LUT, the snow term and the slope-rock term entirely.
    #
    # WHY THE COLOUR AND NOT THE ID. The obvious design is to send the material
    # id and look the palette up here. It was tried and it does not survive the
    # vertex-colour path: an exposure-proof probe read VertexColor.R * 255 back
    # as ~6-8 where the CPU wrote 4, so every categorical threshold evaluated to
    # 0 across the whole frame (see this file's sibling note in
    # terrain_material_common.py). Only 0 and 255 are exact through that path.
    # A COLOUR does survive it, because that is what the channel is for.
    #
    # WHY THE SNOW AND SLOPE TERMS GO WITH IT. They are the clipmap's own
    # inventions -- the marcher has neither, and vxc::MAT_SNOW is legacy and
    # deliberately dead (core.h:487, superseded by PERMAFROST/ROCK). Keeping
    # them on top of a palette colour would re-introduce the very disagreement
    # this parameter exists to remove.
    #
    # DEFAULT 0.0 = the shipped graph, unchanged, so a control capture needs no
    # material instance. It is a SCALAR and not a static switch deliberately:
    # AVoxelClipmapActor already owns a per-level MID, so this can be A/B'd from
    # a command line without a second shader permutation.
    vertex_albedo = b.lerp(lerp_snow, "", vertex_color, "",
                           b.scalar("VertexAlbedoWeight", 0.0), "")

    tint_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 150, 350)
    if not mel.connect_material_expressions(vertex_albedo, "", tint_multiply, "A"):
        raise RuntimeError("connect vertex_albedo -> tint_multiply.A failed")
    if not mel.connect_material_expressions(debug_tint, "", tint_multiply, "B"):
        raise RuntimeError("connect debug_tint -> tint_multiply.B failed")
    if not mel.connect_material_property(tint_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect tint_multiply -> BaseColor failed")

    # Same snow-smooths-roughness term as M_VoxelTerrain, same defaults, so the
    # snow cap on a distant peak and the snow underfoot shade alike.
    roughness = b.lerp(b.scalar("RoughnessBase", 0.90), "", b.scalar("RoughnessSnow", 0.55), "", snow_w)
    # Wet shores go glossier as well as darker -- the same Lagarde pairing
    # M_VoxelTerrain applies, with the same parameter name and default, because
    # the two materials meet along the seam this term is most visible on.
    if wet is not None:
        roughness = b.lerp(roughness, "", b.scalar("WetShoreRoughness", 0.22), "", wet)
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
