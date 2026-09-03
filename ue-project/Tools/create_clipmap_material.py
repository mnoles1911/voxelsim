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
    # The GRAPH default here is 0.0, but the SHIPPED default is 1.0: the actor
    # pushes VertexAlbedoWeightOverride (default 1.0 since 2026-08-30) onto the
    # per-level MID every run. It is a SCALAR and not a static switch
    # deliberately: AVoxelClipmapActor already owns a per-level MID, so this
    # can be A/B'd from a command line without a second shader permutation.
    vertex_albedo = b.lerp(lerp_snow, "", vertex_color, "",
                           b.scalar("VertexAlbedoWeight", 0.0), "")

    # --- VIRTUAL RINGS: MATERIAL-SPACE VOXELIZATION (2026-09-02) ------------
    #
    # The far field's smoothness is not a colour problem (the palette already
    # matches) -- it is that a heightfield has no terraces, no axis-quantized
    # faces, and no unlit risers, while the voxel cascade holds ALL of those at
    # ~2-3 px apparent cell size at EVERY distance (the refuted normal-fade arm
    # proved blockiness never diminishes with range; VoxelMarchRenderer.cpp at
    # NormalFadeStartM). So the clipmap continues the cascade's own law in its
    # PIXEL shader: cell size 12.8 m at the 8,192 m seam (identical to R7
    # across the boundary), doubling per distance band -- terraced height
    # steps, normals snapped to cube axes so the sun lights whole cells the way
    # it lights real faces, per-cell flattened colour, and the marcher's
    # hemisphere-ambient formula verbatim for lighting parity (its constants
    # are restated here from voxel.March.AmbientIntensity's defaults --
    # materials cannot read cvars; if those defaults move, move these).
    #
    # VoxelizeWeight 1.0 is the DEFAULT (owner-accepted direction); 0.0 is the
    # control arm and must reproduce today's frame exactly -- every output
    # lerps back to its pre-voxelize input at W=0. AVoxelClipmapActor pushes
    # -VoxelClipmapVoxelize= onto the per-level MIDs for the A/B.
    #
    # The lattice is WORLD-ANCHORED (stable under TSR/camera translation) and
    # band-doubling (the cone rule), the two defences the marcher's own tint
    # fade uses against far-field boiling. Honest limit, recorded up front:
    # the SILHOUETTE stays smooth -- a pixel shader cannot cut the skyline.
    world_pos = mel.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -500, 800)
    cam_pos = mel.create_material_expression(material, unreal.MaterialExpressionCameraPositionWS, -500, 900)
    vert_nrm = mel.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -500, 1000)

    vox = mel.create_material_expression(material, unreal.MaterialExpressionCustom, -100, 800)
    vox.set_editor_property("description", "VirtualRingVoxelize")
    vox.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    vox.set_editor_property("code", """
// Virtual-ring voxelization. All distances in UU (cm).
float2 dxy = WP.xy - CamP.xy;
float d = length(dxy);
const float seam = 819200.0;                 // 8,192 m cascade edge
float band = clamp(floor(log2(max(d, seam) / seam)), 0.0, 3.0);
float cell = 1280.0 * exp2(band);            // 12.8 m at the seam, doubling per band
// Terracing: quantize height into cell steps; classify riser by slope share.
// V2 (owner: "even more blockiness"): stronger riser share, harder steps.
float zc = WP.z / cell;
float f = frac(zc);
float slope = saturate(1.0 - abs(VN.z));
float riserW = saturate(slope * 3.5);        // share of each step that reads as riser
float aa = fwidth(zc) * 1.5 + 1e-4;
float riser = 1.0 - smoothstep(riserW - aa, riserW + aa, f);
// Axis-quantized normal: up on treads, dominant horizontal axis on risers --
// near-pure horizontal so risers take the full unlit-face contrast.
float2 hd = (abs(VN.x) > abs(VN.y)) ? float2(VN.x >= 0.0 ? 1.0 : -1.0, 0.0)
                                    : float2(0.0, VN.y >= 0.0 ? 1.0 : -1.0);
float3 nq = normalize(lerp(float3(0, 0, 1), float3(hd, 0.05), riser));
// XY cell lattice: each world-anchored cell reads as its own block face.
float2 cid = floor(WP.xy / cell);
// Per-cell value jitter (world-anchored hash; TSR-stable): the near field's
// per-voxel variation, continued at cell scale.
float h = frac(sin(dot(cid + floor(WP.z / cell) * 0.618, float2(12.9898, 78.233))) * 43758.5453);
float jitter = 1.0 + (h - 0.5) * 0.14;
// Cell-edge darkening: the face-grid read of voxel terrain, AA'd by fwidth.
float2 fxy = abs(frac(WP.xy / cell) - 0.5);
float2 exy = fwidth(WP.xy / cell) * 1.2 + 1e-4;
float edge = max(smoothstep(0.5 - exy.x, 0.5, fxy.x),
                 smoothstep(0.5 - exy.y, 0.5, fxy.y));
float edgeDim = 1.0 - edge * 0.18;
// Per-cell flattened colour: posterized luminance (4 hard steps), per-cell
// jitter, edge darkening, risers to the palette side-face ratio.
float lum = max(dot(VC, float3(0.299, 0.587, 0.114)), 1e-4);
float lq = (floor(lum * 4.0) + 0.5) / 4.0;
float3 cq = VC * (lq / lum) * jitter * edgeDim;
cq = lerp(cq, cq * 0.58, riser);
// Marcher hemisphere ambient, verbatim: tint * intensity * ground/sky mix.
float3 amb = cq * float3(1.00, 1.04, 1.12) * 1.5
           * lerp(0.45, 1.0, saturate(nq.z * 0.5 + 0.5));
// W = 0 must be byte-identical to the pre-voxelize graph.
OutNrm = normalize(lerp(VN, nq, W));
OutAmb = amb * W;
return lerp(VC, cq, W);
""")
    vox_inputs = []
    for nm in ("WP", "CamP", "VN", "VC", "W"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", nm)
        vox_inputs.append(ci)
    vox.set_editor_property("inputs", vox_inputs)
    out_nrm = unreal.CustomOutput()
    out_nrm.set_editor_property("output_name", "OutNrm")
    out_nrm.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    out_amb = unreal.CustomOutput()
    out_amb.set_editor_property("output_name", "OutAmb")
    out_amb.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    vox.set_editor_property("additional_outputs", [out_nrm, out_amb])

    voxelize_w = b.scalar("VoxelizeWeight", 1.0)
    b.link(world_pos, "", vox, "WP")
    b.link(cam_pos, "", vox, "CamP")
    b.link(vert_nrm, "", vox, "VN")
    b.link(vertex_albedo, "", vox, "VC")
    b.link(voxelize_w, "", vox, "W")

    tint_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 150, 350)
    if not mel.connect_material_expressions(vox, "", tint_multiply, "A"):
        raise RuntimeError("connect voxelize -> tint_multiply.A failed")
    if not mel.connect_material_expressions(debug_tint, "", tint_multiply, "B"):
        raise RuntimeError("connect debug_tint -> tint_multiply.B failed")
    if not mel.connect_material_property(tint_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect tint_multiply -> BaseColor failed")

    # World-space quantized normal out; the material flips to world-space
    # normals for it (the vertex normal passthrough at W=0 is world-space too,
    # so the control arm is unchanged).
    if not mel.connect_material_property(vox, "OutNrm", unreal.MaterialProperty.MP_NORMAL):
        raise RuntimeError("connect voxelize -> Normal failed")
    material.set_editor_property("tangent_space_normal", False)

    # Lighting parity: the marcher's hemisphere ambient rides emissive there,
    # so it rides emissive here, gated entirely by VoxelizeWeight.
    if not mel.connect_material_property(vox, "OutAmb", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("connect voxelize ambient -> Emissive failed")

    # Same snow-smooths-roughness term as M_VoxelTerrain, same defaults, so the
    # snow cap on a distant peak and the snow underfoot shade alike.
    roughness = b.lerp(b.scalar("RoughnessBase", 0.90), "", b.scalar("RoughnessSnow", 0.55), "", snow_w)
    # Wet shores go glossier as well as darker -- the same Lagarde pairing
    # M_VoxelTerrain applies, with the same parameter name and default, because
    # the two materials meet along the seam this term is most visible on.
    if wet is not None:
        roughness = b.lerp(roughness, "", b.scalar("WetShoreRoughness", 0.22), "", wet)
    # Roughness parity with the marcher (0.86) under voxelization.
    roughness = b.lerp(roughness, "", b.scalar("VoxelizeRoughness", 0.86), "", voxelize_w)
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
