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

M2 "Transitions" upgrade (docs/voxel-earth-implementation-plan.md SS3.3:
"dithered cross-fade band (outer 15-20% of each ring; blue-noise threshold;
both LODs rendered in band; TSR resolves)"; docs/m2-plan.md's "hard boundary
v0" decision row is what this upgrades). Pure material change -- no subsystem/
streaming edits: adds four ScalarParameters (RingInnerFadeStart/End,
RingOuterFadeStart/End, all in UU/cm to match View.WorldCameraOrigin/Absolute
World Position) driving an opacity-masked dither fade:

  CameraDistance = Distance(AbsoluteWorldPosition, CameraPositionWS)
  InnerRamp = saturate((CameraDistance - InnerStart) / (InnerEnd - InnerStart))
  OuterRamp = saturate((OuterEnd - CameraDistance) / (OuterEnd - OuterStart))
  FadeFactor = InnerRamp * OuterRamp
  OpacityMask = DitherTemporalAA(FadeFactor)   -- Engine's
      /Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA,
      the standard blue-noise-esque screen-door dither function resolved by
      TAA/TSR over several frames (same function foliage/grass LOD crossfades
      use), matching the plan's "blue-noise threshold ... TSR resolves" line.

Defaults (INERT_LOW = -2/-1, INERT_HIGH = 1e7/2e7 UU) are chosen so both ramps
saturate to 1 for any real camera distance when a MID never overrides them --
"material params ... default = no fade, fully opaque" per the task spec, so
this asset change alone is inert until UVoxelChunkComponent::ApplyRingFadeParams
(VoxelChunkComponent.cpp) sets real per-level values on a MID. Blend mode
switches Opaque -> Masked (OpacityMaskClipValue stays the engine default,
0.3333, which is what DitherTemporalAA's output is calibrated against); the
existing vertex-color/AO/DebugTint BaseColor path and Roughness constant are
untouched -- Masked mode only adds the OpacityMask input, so with the inert
defaults every existing chunk renders bit-for-bit as before (Masked with
OpacityMask==1 draws every pixel, same as Opaque).

ADR-0008 material palette (asset-forge plan Phase 3.1): BaseColor becomes
lerp(biomeAlbedo, paletteRGB, isAsset), where the palette arrives at pixel rate
from VoxelQuadVertexFactory.ush through TexCoords[3]/[4]. isAsset is 1 only for
the ten materials the terrain generator cannot emit, so terrain is unchanged and
the change is inert until baked assets are in the world. **This graph has never
been run** -- see the block at the call site.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash
"""

import os
import sys

import unreal

# The shared biome graph lives next to this file. A -run=pythonscript commandlet
# does not put the script's own directory on sys.path, so add it explicitly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from terrain_material_common import build_terrain_base_color  # noqa: E402
from sky_star_graph import SkyGraphBuilder  # noqa: E402
from bathy_field_graph import sample_bathy_field  # noqa: E402

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_VoxelTerrain"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME

DITHER_FUNCTION_PATH = "/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA"

# Inert sentinel pairs -- MUST match VoxelChunkComponent.cpp's
# kInertLowStart/End and kInertHighStart/End exactly (both files document the
# other as the source of truth for this constraint).
INERT_LOW_START, INERT_LOW_END = -2.0, -1.0
INERT_HIGH_START, INERT_HIGH_END = 1.0e7, 2.0e7


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

    # --- BIOME + TEXTURE BASE COLOUR ---------------------------------------
    #
    # Replaces the stage-1 flat Constant3Vector(0.5, 0.45, 0.4). That constant
    # was, quite literally, the "everything is one flat beige" defect: this
    # material had VertexColor.R (the material id) plumbed all the way to the
    # GPU and then discarded it, so no upstream terrain or biome work could
    # ever have changed the colour of anything.
    #
    # The graph itself is shared with M_VoxelClipmap (terrain_material_common)
    # so the near field and the 50 km vista cannot diverge at their seam.
    # SkyGraphBuilder, not GraphBuilder. It IS a GraphBuilder (same algebra, same
    # checked connects) plus the one thing the Phase 3 wet-shore term needs: a
    # CollectionParameter binding that is checked by name against MPC_VoxelSky as
    # read back. An unresolved collection parameter compiles to a constant rather
    # than failing, so the check is the only thing between a stale MPC and a
    # terrain material that silently darkens nothing (or everything).
    sky_collection = unreal.load_object(None, "/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky")
    if sky_collection is None:
        raise RuntimeError(
            "MPC_VoxelSky not found -- M_VoxelTerrain now reads the bathymetry window's "
            "placement from it for wet shores. Run Tools/create_sky_material.py first.")
    b = SkyGraphBuilder(material, sky_collection)
    bathy = sample_bathy_field(b)

    # T_VoxelDetail UV: TextureCoordinate 0 is world-planar metres wrapped to a
    # 32 m period (VoxelChunkComponent's WrapWorldToUV -- the wrap is what keeps
    # LWC precision sane at 2000 km from the origin). Dividing by 8 repeats the
    # detail every 8 m; 8 divides 32 exactly, so vertices on either side of the
    # wrap still agree and no seam appears.
    tex_coord = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, 400)
    tex_coord.set_editor_property("coordinate_index", 0)
    detail_uv = b.div(tex_coord, b.scalar("DetailTileMeters", 8.0))

    base_color, snow_w, base_color_out, wet = build_terrain_base_color(
        b, vertex_color, detail_uv, "", bathy=bathy,
        # Voxel face normals are AXIS-ALIGNED, so every 10 cm step riser on a
        # gentle grassy hill is a "vertical" face. At full strength the slope
        # term would stripe the entire world grass/rock; at 0.35 it reads as
        # natural darkening of the riser. The clipmap, whose normals are real
        # terrain normals, passes 1.0.
        rock_slope_strength=0.35,
        detail_fine_strength=0.30,
        detail_coarse_strength=0.22,
    )

    # --- ADR-0008 MATERIAL PALETTE ------------------------------------------
    #
    # UNRUN. This function is a `-run=pythonscript` commandlet and one editor per
    # box is a hard rule here, so the graph below is written but has never been
    # executed and M_VoxelTerrain.uasset on disk does not contain it yet. The
    # shader half (VoxelQuadVertexFactory.ush + VoxelMaterialPalette.ush) is
    # compile-checked by tools/compile-shaders.ps1; this half is not checked by
    # anything until somebody runs this file.
    #
    # WHAT IT DOES. VoxelQuadVertexFactory.ush evaluates vxc::kMaterialPalette
    # per PIXEL and hands the result over as
    #
    #   TexCoords[3] = (R, G)      TexCoords[4] = (B, isAsset)
    #
    # and this reads them back as BaseColor = lerp(biome, palette, isAsset).
    #
    # THE LERP IS ONE-SIDED ON PURPOSE, and this is the part not to "simplify"
    # later. isAsset is 1 only for the ten materials terrain cannot produce
    # (bark, heartwood, deadwood, six leaf types, pale bark -- everything at or
    # above vxc::MAT_BARK). For every terrain voxel it is 0 and this node is the
    # identity, so the climate-driven biome path above is untouched: terrain's
    # colour is supposed to come from its climate, not from a material id, and
    # the biome path is the only appearance path that currently works. Replacing
    # it wholesale would be a change that cannot be verified in the same motion
    # as this one.
    #
    # WHY THE LERP GOES *BEFORE* THE AO MULTIPLY. ADR-0008 invariant 4: the
    # table carries no lighting and no ambient occlusion, because baking a
    # top-is-brighter bias into it would double-count the renderer's own terms
    # and go wrong the moment the sun moves. So an asset voxel has to arrive at
    # the same `* VertexColor.G` every terrain voxel goes through.
    #
    # AND IT IS WHAT MAKES THE SHADER SIDE LIVE AT ALL. The factory's palette
    # block is compiled out unless NUM_TEX_COORD_INTERPOLATORS > 4, which is
    # decided by whether a material reads UV4. Until this graph lands, the
    # renderer change costs literally nothing -- not an interpolant, not an
    # instruction. That is deliberate, and it is also why "the shader is in but
    # nothing changed" is the EXPECTED state right now rather than a bug.
    palette_uv_rg = mel.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -900, 700)
    palette_uv_rg.set_editor_property("coordinate_index", 3)
    palette_uv_b_flag = mel.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -900, 780)
    palette_uv_b_flag.set_editor_property("coordinate_index", 4)

    def mask(src, keep_x):
        """One component of a float2 TextureCoordinate."""
        n = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -750, 780)
        n.set_editor_property("r", bool(keep_x))
        n.set_editor_property("g", not keep_x)
        n.set_editor_property("b", False)
        n.set_editor_property("a", False)
        if not mel.connect_material_expressions(src, "", n, ""):
            raise RuntimeError("connect texcoord -> component mask failed")
        return n

    palette_rgb = b.append(palette_uv_rg, "", mask(palette_uv_b_flag, True), "")
    # saturate() is belt and braces. The factory writes exactly 0.0 or 1.0 and
    # the value is constant across a quad, so interpolation cannot move it; a
    # lerp alpha is the one place where being wrong outside [0,1] extrapolates
    # instead of clamping, and this material has been bitten by a channel that
    # did not carry what its name said before.
    is_asset = b.saturate(mask(palette_uv_b_flag, False))
    base_color = b.lerp(base_color, base_color_out, palette_rgb, "", is_asset)
    base_color_out = ""

    ao_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -200, 50)
    # Every connection is checked: a silently-failed pin connect produced an
    # invisible-terrain material once (2026-07-19) and cost a debug session.
    if not mel.connect_material_expressions(base_color, base_color_out, ao_multiply, "A"):
        raise RuntimeError("connect base_color -> multiply.A failed")
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

    # Roughness: snow is markedly smoother than soil or rock, and this is the
    # cheapest way to make a snow cap read as snow rather than as white paint.
    # Everything else keeps the original 0.9.
    roughness = b.lerp(b.scalar("RoughnessBase", 0.90), "", b.scalar("RoughnessSnow", 0.55), "", snow_w)
    # PHASE 3: the OTHER HALF of the wet-shore term, and it is not decoration.
    # Lagarde's rule is that wet ground goes darker AND glossier together,
    # because both come from the same water film -- it fills the microstructure,
    # which traps light (darker) and flattens the surface (glossier). Darkening
    # without this reads as a stain rather than as water, which is the standard
    # way this effect goes wrong. `wet` is already 0 wherever the baked field
    # says nothing, so this is a no-op on a run with no fine tier.
    if wet is not None:
        roughness = b.lerp(roughness, "", b.scalar("WetShoreRoughness", 0.22), "", wet)
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness failed")

    # One-sided: absolute quad winding was verified empirically on 5.8
    # (2026-07-19) — front faces render correctly without two-sided cost.

    # --- M2 ring cross-fade (Masked opacity, dithered) ---------------------
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)

    def make_fade_param(name, default_value, y):
        p = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, y)
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", default_value)
        return p

    inner_start = make_fade_param("RingInnerFadeStart", INERT_LOW_START, -300)
    inner_end = make_fade_param("RingInnerFadeEnd", INERT_LOW_END, -220)
    outer_start = make_fade_param("RingOuterFadeStart", INERT_HIGH_START, -140)
    outer_end = make_fade_param("RingOuterFadeEnd", INERT_HIGH_END, -60)

    # CameraDistance = Distance(AbsoluteWorldPosition, CameraPositionWS).
    # Both operands are LWC-typed engine expressions, so the compiler emits
    # the subtraction in emulated double precision before downcasting the
    # (small, camera-relative) result to float -- the standard LWC-safe
    # "distance from camera" idiom at planet scale (docs/voxel-earth-
    # implementation-plan.md SS3.3's LWC + origin-rebasing requirement).
    world_pos = mel.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -700, -400)
    camera_pos = mel.create_material_expression(material, unreal.MaterialExpressionCameraPositionWS, -700, -320)
    distance = mel.create_material_expression(material, unreal.MaterialExpressionDistance, -550, -400)
    if not mel.connect_material_expressions(world_pos, "", distance, "A"):
        raise RuntimeError("connect world_pos -> distance.A failed")
    if not mel.connect_material_expressions(camera_pos, "", distance, "B"):
        raise RuntimeError("connect camera_pos -> distance.B failed")

    def make_ramp(numerator_a, numerator_b, denom_a, denom_b, y):
        """saturate((numerator_a - numerator_b) / (denom_a - denom_b))"""
        num_sub = mel.create_material_expression(material, unreal.MaterialExpressionSubtract, -400, y)
        if not mel.connect_material_expressions(numerator_a, "", num_sub, "A"):
            raise RuntimeError("connect ramp numerator A failed")
        if not mel.connect_material_expressions(numerator_b, "", num_sub, "B"):
            raise RuntimeError("connect ramp numerator B failed")

        den_sub = mel.create_material_expression(material, unreal.MaterialExpressionSubtract, -400, y + 60)
        if not mel.connect_material_expressions(denom_a, "", den_sub, "A"):
            raise RuntimeError("connect ramp denominator A failed")
        if not mel.connect_material_expressions(denom_b, "", den_sub, "B"):
            raise RuntimeError("connect ramp denominator B failed")

        div = mel.create_material_expression(material, unreal.MaterialExpressionDivide, -280, y)
        if not mel.connect_material_expressions(num_sub, "", div, "A"):
            raise RuntimeError("connect ramp num_sub -> div.A failed")
        if not mel.connect_material_expressions(den_sub, "", div, "B"):
            raise RuntimeError("connect ramp den_sub -> div.B failed")

        sat = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -160, y)
        # "" targets the first (and only) input pin regardless of display
        # name -- same MaterialEditingLibrary convention create_ocean_material.py
        # relies on for its single-input ComponentMask/Sine nodes.
        if not mel.connect_material_expressions(div, "", sat, ""):
            raise RuntimeError("connect ramp div -> saturate.Input failed")
        return sat

    # InnerRamp: 0 at InnerStart -> 1 at InnerEnd (fades IN as distance grows).
    inner_ramp = make_ramp(distance, inner_start, inner_end, inner_start, -260)
    # OuterRamp: 1 at OuterStart -> 0 at OuterEnd (fades OUT as distance grows).
    outer_ramp = make_ramp(outer_end, distance, outer_end, outer_start, -100)

    fade_factor = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -20, -180)
    if not mel.connect_material_expressions(inner_ramp, "", fade_factor, "A"):
        raise RuntimeError("connect inner_ramp -> fade_factor.A failed")
    if not mel.connect_material_expressions(outer_ramp, "", fade_factor, "B"):
        raise RuntimeError("connect outer_ramp -> fade_factor.B failed")

    # unreal.EditorAssetLibrary.load_asset requires the Asset Registry to
    # already know about the target package; a `-run=pythonscript` commandlet
    # doesn't wait for Engine content's background scan to finish, so that
    # call fails here ("could not be found in the Asset Registry") even
    # though the .uasset is on disk. unreal.load_object bypasses the registry
    # and calls StaticLoadObject directly, which works regardless of scan
    # state -- needs the full object path (package.object), not just the
    # package path.
    dither_function = unreal.load_object(None, DITHER_FUNCTION_PATH + "." + "DitherTemporalAA")
    if dither_function is None:
        raise RuntimeError("failed to load DitherTemporalAA function at " + DITHER_FUNCTION_PATH)
    dither_call = mel.create_material_expression(
        material, unreal.MaterialExpressionMaterialFunctionCall, 140, -180
    )
    if not dither_call.set_material_function(dither_function):
        raise RuntimeError("set_material_function(DitherTemporalAA) failed")

    dither_inputs = mel.get_material_expression_input_names(dither_call)
    if len(dither_inputs) == 0:
        raise RuntimeError("DitherTemporalAA exposed zero input pins after set_material_function")
    if not mel.connect_material_expressions(fade_factor, "", dither_call, dither_inputs[0]):
        raise RuntimeError("connect fade_factor -> dither_call.%s failed" % dither_inputs[0])

    dither_outputs = mel.get_material_expression_output_names(dither_call)
    dither_output_name = dither_outputs[0] if len(dither_outputs) > 0 else ""
    if not mel.connect_material_property(dither_call, dither_output_name, unreal.MaterialProperty.MP_OPACITY_MASK):
        raise RuntimeError("connect dither_call -> OpacityMask failed")

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_VoxelTerrain created and saved at " + FULL_PATH)


main()
