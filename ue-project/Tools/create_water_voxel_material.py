"""Author M_WaterVoxel (W2 task spec item 4 "Rendering v0"): the active-water
voxel material UWaterChunkComponent/FWaterChunkSceneProxy render with
(VoxelWaterChunkComponent.h/.cpp), distinct from M_Ocean's implicit static
surface plane (AVoxelOceanActor -- that one keeps existing unchanged; active
water renders ON TOP of it per the task spec, overlap acceptable v0).

Same headless-Python pattern as create_voxel_material.py / create_ocean_material.py
(checked connections throughout -- see create_voxel_material.py's comment on
why a silently-failed pin connect is worth guarding against explicitly).

Translucent blue, vertex-color-driven AO, plus a stepped fill-fraction surface.

Vertex-color convention, which now DIFFERS from M_VoxelTerrain's in R:
  R = CA fill fraction 0..255 remapped to 0..1 (was: an unused fixed material-id
      placeholder). Drives the World Position Offset that seats each surface
      cell's top boundary at its own fill height, so a waterline reads as
      discrete 10 cm steps instead of popping in and out at a >=128 threshold.
  G = AO 0/85/170/255, so greedy-mesher AO shades consistently with terrain.
  B = 1 if this vertex sits on the +Z boundary of its own voxel, else 0. The
      WPO moves only those, so a partial cell's side walls shorten with its
      surface instead of standing proud of it.
  A = 255, unused.

Terrain packs a binary sky-facing biome flag in R and per-chunk climate in B,
so this is water's OWN convention, not a shared one. The component path writes
it directly (VoxelWaterChunkComponent.cpp) and the pooled path reproduces it
under FVoxelQuadVertexFactoryParameters::WaterMode.

Still deliberately basic on COLOUR: constant tint, constant 0.55 opacity, no
refraction. That is load-bearing rather than lazy -- docs/gpu-water-pool-design.md
shows the water pool is safe as ONE primitive with ONE translucent sort key only
while those hold, since N surfaces then transmit (1-0.55)^N in any blend order.
Depth-tinted absorption, foam and caustics each break that and want the sort-key
work first; they are a separate, sequenced item.

W3 "Rendering v0" MATERIAL MOTION (this update): makes the surface look like it
is moving, material graph only, still without touching colour/opacity/refraction
-- the sort-key doc is explicit that vertex movement and lighting-only normal
variation are safe, and everything below is one of those two things:

  * Translucency lighting mode switched Volumetric NonDirectional (engine
    default, ignores the material normal entirely) -> Surface Per-Pixel
    Lighting, one clearly-flagged line, because normal-based motion cues are
    otherwise a silent no-op. Real GPU cost, easy to revert alone.
  * A two-scale panning procedural normal ripple (no texture asset exists in
    the project, so this is math nodes, same technique M_Ocean already uses)
    on the pooled vertex factory's existing world-planar UVs, masked to the
    top surface by VertexColor.B.
  * A small (<=1.5 UU) time-based WPO ripple keyed on ABSOLUTE world XY (never
    the wrapped UV -- a wrap boundary inside a brick would tear adjacent
    quads' shared vertices apart), ADDED to the existing fill-drop WPO and
    masked by VertexColor.B the same way.
  * Roughness/specular tightened slightly now that the normal actually
    reaches the lit result, so the moving normal reads as a moving glint.

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

    # --- Translucency lighting mode: SURFACE per-pixel, not the volumetric
    # default -----------------------------------------------------------
    # UE's default translucency lighting mode (TLM_VolumetricNonDirectional)
    # treats a translucent surface as a participating medium and explicitly
    # does not consider the material normal -- EngineTypes.h's own doc
    # comment on the enum says so directly: "the material normal is not
    # taken into account." That was fine for the constant (0,0,1) normal
    # this material shipped with, but it means the panning normal ripple
    # added below (W3 "make the water surface look like it is moving") would
    # compile and bind and have ZERO visible effect under the default mode --
    # a silent no-op indistinguishable from a broken graph.
    #
    # TLM_SurfacePerPixelLighting shades translucency like an opaque surface:
    # a full per-pixel normal, so the panning ripple actually changes the lit
    # result frame to frame. The cost is real -- this is a strictly more
    # expensive lighting path than the volumetric default, paid per
    # translucent pixel on every water surface in the pool -- which is why
    # it is this one line, called out on its own, rather than folded into
    # the blend_mode/two_sided pair above: reverting motion-shading cost
    # alone (keep the stepped surface and the WPO ripple, drop only the
    # per-pixel normal lighting) is a single-line revert back to the
    # engine's TLM_VolumetricNonDirectional default.
    material.set_editor_property(
        "translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING
    )

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

    # Roughness tightened slightly (0.1 -> 0.08) and Specular made explicit
    # (0.5 -- the engine's own unconnected-pin default, stated rather than
    # left implicit so the two are visibly tuned as a pair) now that the
    # panning normal ripple below actually reaches the lit result (see the
    # translucency-lighting-mode comment above). A tighter specular lobe
    # turns a moving normal into a moving GLINT rather than a moving blur,
    # which is the cue that reads as "surface in motion" at a glance. Both
    # are still flat Constants -- spatially uniform across the whole pool --
    # so this is a lighting-RESPONSE tweak, not a per-pixel input, and does
    # not touch the sort-key constraint (docs/gpu-water-pool-design.md) at
    # all: it changes how a fragment reacts to light, not what colour or
    # opacity it composites with its neighbours in.
    roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -200, 50)
    roughness.set_editor_property("r", 0.08)
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness -> Roughness failed")

    specular = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -200, 110)
    specular.set_editor_property("r", 0.5)
    if not mel.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR):
        raise RuntimeError("connect specular -> Specular failed")

    # --- Stepped fill-fraction surface (World Position Offset) --------------
    #
    # VertexColor.R carries the CA fill fraction 0..255, remapped to 0..1, for
    # the cell this face belongs to -- see UVoxelWaterSubsystem.cpp's meshing
    # sampler for how it gets there, and FVoxelQuadVertexFactoryParameters::
    # WaterMode for the pooled path's half of it. A full cell is 1.0 and an
    # empty one is 0.0.
    #
    # The quad packing cannot express a fractional height: VoxelQuadDecode.ush
    # computes FaceCoordVox = Slice + (Positive ? 1 : 0) in integers, so every
    # face lands on a voxel boundary. WPO is therefore the mechanism -- it is
    # material-only, costs no geometry, and is trivially reversible.
    #
    # ONLY TOP-BOUNDARY VERTICES MOVE, and the mesh says which those are:
    # VertexColor.B is 1 on a vertex sitting on the +Z boundary of its own
    # voxel and 0 otherwise (FVoxelQuadVertex::TopBoundary in
    # VoxelQuadDecode.ush; bTopCorner in VoxelWaterChunkComponent.cpp).
    #
    # This is deliberately NOT a face-normal test. Gating on the normal would
    # lower only +Z faces and leave a partially-filled cell's SIDE walls at
    # full height, ringing every pool with a one-voxel bathtub rim standing
    # proud of its own surface. A side face has two top vertices and two
    # bottom ones, so moving only the top pair turns it into a trapezoid whose
    # upper edge meets the lowered surface -- and bottom faces never move at
    # all, so nothing opens a gap to the floor.
    #
    # A cell at fill f drops its top boundary by (1 - f) * one voxel: a full
    # cell does not move, an almost-empty one sits almost on the floor.
    # 10.0 is VoxelCoords::VoxelSizeUU -- 10 unreal units per 10 cm voxel, the
    # same constant VoxelQuadDecode.ush quotes as VOXEL_SIZE_UU.
    one_minus_fill = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -500, 200)
    if not mel.connect_material_expressions(vertex_color, "R", one_minus_fill, ""):
        raise RuntimeError("connect vertex_color.R -> one_minus_fill failed")

    drop_amount = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -200, 250)
    if not mel.connect_material_expressions(one_minus_fill, "", drop_amount, "A"):
        raise RuntimeError("connect one_minus_fill -> drop_amount.A failed")
    if not mel.connect_material_expressions(vertex_color, "B", drop_amount, "B"):
        raise RuntimeError("connect vertex_color.B -> drop_amount.B failed")

    # (0, 0, -VoxelSizeUU): straight down, one voxel at full drop. Multiplying
    # a float3 by the scalar above broadcasts, so this stays one node.
    down_one_voxel = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -200, 380)
    down_one_voxel.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, -10.0, 1.0))

    world_position_offset = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -50, 300)
    if not mel.connect_material_expressions(down_one_voxel, "", world_position_offset, "A"):
        raise RuntimeError("connect down_one_voxel -> world_position_offset.A failed")
    if not mel.connect_material_expressions(drop_amount, "", world_position_offset, "B"):
        raise RuntimeError("connect drop_amount -> world_position_offset.B failed")
    # NOTE: world_position_offset (the fill-drop term above) is NOT connected
    # to MP_WORLD_POSITION_OFFSET here anymore -- it is summed with the WPO
    # ripple below first, at `total_wpo`, and THAT is what gets connected.
    # See "ADD to the existing fill-drop WPO, do not replace it" further down.

    # --- Panning normal-detail: two-scale procedural ripple (Normal) --------
    #
    # W3 "make the water surface look like it is moving", material graph only
    # -- no C++/shader change, no new texture. No normal-map asset exists in
    # the project (ue-project/Content/Voxel has T_VoxelPalette/T_VoxelBiomeLUT/
    # T_VoxelDetail and nothing water- or normal-named), so this is generated
    # from math nodes, the same technique M_Ocean already uses for its own
    # tangent-space normal perturbation -- make_wave_channel below is a direct
    # copy of that file's identically-named helper (same node types, same pin
    # names, same connection order), not a new pattern.
    #
    # UV SOURCE: TextureCoordinate(0), NOT a new WorldPosition node. The
    # pooled vertex factory already writes world-planar UVs into TEXCOORD0 --
    # Position/100 (metres), wrapped to a 32 m period on each face's two
    # in-plane axes (VoxelQuadVertexFactory.ush: "World-planar UVs on the
    # face's two in-plane axes ... matching the CPU path's position/100,
    # wrapped to 32 m"). For a +Z (top) face that pair is exactly world X and
    # Y. Reusing it is free -- already computed and interpolated per vertex --
    # and it wraps at a period the water pool's own detail texturing would
    # use too, so it introduces no NEW seam frequency the mesh doesn't
    # already tile at.
    #
    # This wrapped UV is deliberately NOT reused for the WPO ripple section
    # below -- see that section for why a wrap is safe for a pixel-shader-only
    # read but not for a vertex position.
    uv = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, 700)
    uv.set_editor_property("coordinate_index", 0)

    ripple_time = mel.create_material_expression(material, unreal.MaterialExpressionTime, -900, 950)

    def make_wave_channel(mask_r, mask_g, freq, speed, strength, y):
        """One panning sine channel: mask an axis of `uv` -> scale by freq ->
        add panned time -> Sine -> scale by strength. Copied from
        create_ocean_material.py's make_wave_channel (same expression
        classes, same pin names, same wiring order) with `uv` standing in for
        that file's `world_pos` and `strength` broken out as a parameter so
        the coarse/fine callers below can share one implementation.
        """
        mask = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -700, y)
        mask.set_editor_property("r", mask_r)
        mask.set_editor_property("g", mask_g)
        mask.set_editor_property("b", False)
        mask.set_editor_property("a", False)
        if not mel.connect_material_expressions(uv, "", mask, ""):
            raise RuntimeError("connect uv -> mask.Input failed")

        freq_const = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -700, y + 90)
        freq_const.set_editor_property("r", freq)
        scaled = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -550, y)
        if not mel.connect_material_expressions(mask, "", scaled, "A"):
            raise RuntimeError("connect mask -> scaled.A failed")
        if not mel.connect_material_expressions(freq_const, "", scaled, "B"):
            raise RuntimeError("connect freq_const -> scaled.B failed")

        speed_const = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -550, y + 170)
        speed_const.set_editor_property("r", speed)
        panned_time = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -400, y + 170)
        if not mel.connect_material_expressions(ripple_time, "", panned_time, "A"):
            raise RuntimeError("connect ripple_time -> panned_time.A failed")
        if not mel.connect_material_expressions(speed_const, "", panned_time, "B"):
            raise RuntimeError("connect speed_const -> panned_time.B failed")

        panned = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -250, y)
        if not mel.connect_material_expressions(scaled, "", panned, "A"):
            raise RuntimeError("connect scaled -> panned.A failed")
        if not mel.connect_material_expressions(panned_time, "", panned, "B"):
            raise RuntimeError("connect panned_time -> panned.B failed")

        sine = mel.create_material_expression(material, unreal.MaterialExpressionSine, -100, y)
        if not mel.connect_material_expressions(panned, "", sine, ""):
            raise RuntimeError("connect panned -> sine.Input failed")

        strength_const = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -100, y + 90)
        strength_const.set_editor_property("r", strength)
        scaled_sine = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 60, y)
        if not mel.connect_material_expressions(sine, "", scaled_sine, "A"):
            raise RuntimeError("connect sine -> scaled_sine.A failed")
        if not mel.connect_material_expressions(strength_const, "", scaled_sine, "B"):
            raise RuntimeError("connect strength_const -> scaled_sine.B failed")

        return scaled_sine

    # Two scales, each cross-coupled the way M_Ocean's channels are (the X
    # output driven by the V/G axis, the Y output driven by the U/R axis) so
    # the result doesn't visibly ripple in straight lines along one axis:
    #   COARSE: roughly 3.5-4 m wavelength (2*pi/freq), slow pan -- the big
    #           rolling shimmer.
    #   FINE:   roughly 0.7 m wavelength, faster pan, opposite-signed speeds
    #           to the coarse layer -- breaks the coarse layer up so it
    #           doesn't read as one uniform wave train.
    # uv is in METRES (see the comment above), unlike M_Ocean's world_pos
    # which is in UU/cm -- these freq/speed constants are roughly 100x
    # M_Ocean's for that reason, not because the water pool is meant to look
    # different in scale.
    coarse_x = make_wave_channel(False, True, 1.8, 0.30, 0.05, 700)
    coarse_y = make_wave_channel(True, False, 1.6, -0.24, 0.05, 1000)
    fine_x = make_wave_channel(False, True, 9.0, -0.90, 0.02, 1300)
    fine_y = make_wave_channel(True, False, 8.3, 0.75, 0.02, 1600)

    wave_x = mel.create_material_expression(material, unreal.MaterialExpressionAdd, 220, 850)
    if not mel.connect_material_expressions(coarse_x, "", wave_x, "A"):
        raise RuntimeError("connect coarse_x -> wave_x.A failed")
    if not mel.connect_material_expressions(fine_x, "", wave_x, "B"):
        raise RuntimeError("connect fine_x -> wave_x.B failed")

    wave_y = mel.create_material_expression(material, unreal.MaterialExpressionAdd, 220, 1150)
    if not mel.connect_material_expressions(coarse_y, "", wave_y, "A"):
        raise RuntimeError("connect coarse_y -> wave_y.A failed")
    if not mel.connect_material_expressions(fine_y, "", wave_y, "B"):
        raise RuntimeError("connect fine_y -> wave_y.B failed")

    # Mask by VertexColor.B (top-boundary flag) so only the top surface
    # shimmers and side walls stay flat -- same flag, same reasoning as the
    # WPO section below and as the file header's vertex-colour convention: B
    # is 1 only on a vertex sitting on its own voxel's +Z boundary. A
    # side-wall quad has two top vertices (B=1) and two bottom (B=0); here
    # that is a per-vertex multiplier on a pixel-shader-only Normal input, so
    # it interpolates smoothly across the quad like any other vertex-varying
    # value -- a side wall's shimmer fades out over its height rather than
    # snapping off, which is fine since this is a lighting-only cue and no
    # side wall is meant to look like a shimmering top surface anyway.
    wave_x_masked = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 380, 850)
    if not mel.connect_material_expressions(wave_x, "", wave_x_masked, "A"):
        raise RuntimeError("connect wave_x -> wave_x_masked.A failed")
    if not mel.connect_material_expressions(vertex_color, "B", wave_x_masked, "B"):
        raise RuntimeError("connect vertex_color.B -> wave_x_masked.B failed")

    wave_y_masked = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 380, 1150)
    if not mel.connect_material_expressions(wave_y, "", wave_y_masked, "A"):
        raise RuntimeError("connect wave_y -> wave_y_masked.A failed")
    if not mel.connect_material_expressions(vertex_color, "B", wave_y_masked, "B"):
        raise RuntimeError("connect vertex_color.B -> wave_y_masked.B failed")

    # Tangent-space normal (0,0,1) perturbed by (wave_x, wave_y): the same
    # append-X,-then-Y,-then-Z-then-connect-to-Normal idiom M_Ocean uses,
    # including that file's choice not to Normalize afterwards -- the
    # perturbation is small relative to Z=1.0 so the slight off-unit length
    # is imperceptible, and skipping Normalize is one fewer node evaluated
    # per pixel on every water fragment in the pool. A top face's tangent
    # basis is world-aligned (tangent=X, bitangent=Y, normal=Z --
    # VoxelQuadVertexFactory.ush's per-axis RotateLocalToWorld for an Axis==2
    # face), the same as M_Ocean's flat follow-plane, so the X/Y perturbation
    # lands in the same directions it was computed in.
    normal_xy = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, 520, 950)
    if not mel.connect_material_expressions(wave_x_masked, "", normal_xy, "A"):
        raise RuntimeError("connect wave_x_masked -> normal_xy.A failed")
    if not mel.connect_material_expressions(wave_y_masked, "", normal_xy, "B"):
        raise RuntimeError("connect wave_y_masked -> normal_xy.B failed")

    normal_z = mel.create_material_expression(material, unreal.MaterialExpressionConstant, 520, 1050)
    normal_z.set_editor_property("r", 1.0)

    normal_xyz = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, 680, 1000)
    if not mel.connect_material_expressions(normal_xy, "", normal_xyz, "A"):
        raise RuntimeError("connect normal_xy -> normal_xyz.A failed")
    if not mel.connect_material_expressions(normal_z, "", normal_xyz, "B"):
        raise RuntimeError("connect normal_z -> normal_xyz.B failed")

    if not mel.connect_material_property(normal_xyz, "", unreal.MaterialProperty.MP_NORMAL):
        raise RuntimeError("connect normal_xyz -> Normal failed")

    # --- Small WPO ripple: absolute-world-XY-keyed, added to the fill-drop
    # WPO ------------------------------------------------------------------
    #
    # The fill-drop WPO above is a STEP function of fill fraction -- it says
    # nothing about time, so a static pool looks perfectly still even with
    # the panning normal above doing real per-pixel work from some angles.
    # This adds a small continuous vertical bob so the geometry itself moves,
    # which is the cue that reads as liquid rather than tinted glass from a
    # grazing angle where the normal-only cue is weak.
    #
    # KEYED ON ABSOLUTE WORLD XY, NOT UV OR LOCAL SPACE. Unlike Normal above,
    # this is a POSITION output, and it MUST agree bit-for-bit between two
    # adjacent bricks at a vertex they share, or the bricks tear apart into a
    # visible crack every time the ripple phase moves. The UV used for the
    # normal detail above is wrapped to a 32 m period specifically because a
    # pixel-shader-only Normal read is never shared across a seam -- but that
    # same wrap is exactly what would break a vertex POSITION: a brick
    # straddling the mod-32 boundary would compute two different ripple
    # values for the same physical point depending on which side of the wrap
    # each of its quads' vertices sampled. Absolute world position has no
    # such boundary within the range this pool operates at (single-digit km
    # around the player -- docs/gpu-water-pool-design.md), so it is the only
    # safe key for a WPO term here.
    #
    # world_position_shader_offset is set to WPT_ExcludeAllShaderOffsets, NOT
    # left at its WPT_Default ("Absolute World Position, INCLUDING material
    # shader offsets"). This term is itself an input to World Position
    # Offset, so reading a position that already includes this material's
    # own WPO would feed the ripple's output back into its own input --
    # standard practice for anything that drives WPO from a position read
    # (wind sway, waves) is to read the PRE-offset position instead. It
    # happens to be numerically identical here either way, since the
    # fill-drop WPO above only ever moves Z and this reads X/Y -- but
    # excluding shader offsets is what keeps that true if a future change
    # ever makes the fill-drop term touch X/Y too.
    world_pos_abs = mel.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -900, 1900)
    world_pos_abs.set_editor_property(
        "world_position_shader_offset", unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS
    )

    mask_wx = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -700, 1850)
    mask_wx.set_editor_property("r", True)
    mask_wx.set_editor_property("g", False)
    mask_wx.set_editor_property("b", False)
    mask_wx.set_editor_property("a", False)
    if not mel.connect_material_expressions(world_pos_abs, "", mask_wx, ""):
        raise RuntimeError("connect world_pos_abs -> mask_wx.Input failed")

    mask_wy = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -700, 1950)
    mask_wy.set_editor_property("r", False)
    mask_wy.set_editor_property("g", True)
    mask_wy.set_editor_property("b", False)
    mask_wy.set_editor_property("a", False)
    if not mel.connect_material_expressions(world_pos_abs, "", mask_wy, ""):
        raise RuntimeError("connect world_pos_abs -> mask_wy.Input failed")

    # Two crossing sine waves (world X and world Y, different frequency/
    # speed/sign) summed rather than one -- a single sine ripples in perfectly
    # straight lines perpendicular to its axis, which reads as obviously
    # synthetic for a pool viewed from above. Frequencies are in 1/UU (world
    # position is in UU, the same units create_voxel_material.py's ring-fade
    # distance math uses): 0.006 UU^-1 is a ~1047 UU (~10.5 m) wavelength,
    # 0.0068 UU^-1 a ~924 UU (~9.2 m) one -- deliberately much longer than the
    # ~3.5-4 m / ~0.7 m normal-detail wavelengths above. This term moves
    # actual GEOMETRY, so staying low-frequency avoids the vertex grid
    # (10 cm voxels) visibly faceting a fast-changing ripple; the normal
    # detail is a pixel-shader effect and has no such grid to alias against.
    freq_wx = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -550, 1850)
    freq_wx.set_editor_property("r", 0.006)
    phase_wx_pos = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -400, 1850)
    if not mel.connect_material_expressions(mask_wx, "", phase_wx_pos, "A"):
        raise RuntimeError("connect mask_wx -> phase_wx_pos.A failed")
    if not mel.connect_material_expressions(freq_wx, "", phase_wx_pos, "B"):
        raise RuntimeError("connect freq_wx -> phase_wx_pos.B failed")

    speed_wx = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -400, 1780)
    speed_wx.set_editor_property("r", 0.6)
    phase_wx_time = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -250, 1780)
    if not mel.connect_material_expressions(ripple_time, "", phase_wx_time, "A"):
        raise RuntimeError("connect ripple_time -> phase_wx_time.A failed")
    if not mel.connect_material_expressions(speed_wx, "", phase_wx_time, "B"):
        raise RuntimeError("connect speed_wx -> phase_wx_time.B failed")

    phase_wx = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -100, 1815)
    if not mel.connect_material_expressions(phase_wx_pos, "", phase_wx, "A"):
        raise RuntimeError("connect phase_wx_pos -> phase_wx.A failed")
    if not mel.connect_material_expressions(phase_wx_time, "", phase_wx, "B"):
        raise RuntimeError("connect phase_wx_time -> phase_wx.B failed")

    sine_wx = mel.create_material_expression(material, unreal.MaterialExpressionSine, 40, 1815)
    if not mel.connect_material_expressions(phase_wx, "", sine_wx, ""):
        raise RuntimeError("connect phase_wx -> sine_wx.Input failed")

    freq_wy = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -550, 1950)
    freq_wy.set_editor_property("r", 0.0068)
    phase_wy_pos = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -400, 1950)
    if not mel.connect_material_expressions(mask_wy, "", phase_wy_pos, "A"):
        raise RuntimeError("connect mask_wy -> phase_wy_pos.A failed")
    if not mel.connect_material_expressions(freq_wy, "", phase_wy_pos, "B"):
        raise RuntimeError("connect freq_wy -> phase_wy_pos.B failed")

    speed_wy = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -400, 2020)
    speed_wy.set_editor_property("r", -0.51)
    phase_wy_time = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -250, 2020)
    if not mel.connect_material_expressions(ripple_time, "", phase_wy_time, "A"):
        raise RuntimeError("connect ripple_time -> phase_wy_time.A failed")
    if not mel.connect_material_expressions(speed_wy, "", phase_wy_time, "B"):
        raise RuntimeError("connect speed_wy -> phase_wy_time.B failed")

    phase_wy = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -100, 1985)
    if not mel.connect_material_expressions(phase_wy_pos, "", phase_wy, "A"):
        raise RuntimeError("connect phase_wy_pos -> phase_wy.A failed")
    if not mel.connect_material_expressions(phase_wy_time, "", phase_wy, "B"):
        raise RuntimeError("connect phase_wy_time -> phase_wy.B failed")

    sine_wy = mel.create_material_expression(material, unreal.MaterialExpressionSine, 40, 1985)
    if not mel.connect_material_expressions(phase_wy, "", sine_wy, ""):
        raise RuntimeError("connect phase_wy -> sine_wy.Input failed")

    sine_sum = mel.create_material_expression(material, unreal.MaterialExpressionAdd, 200, 1900)
    if not mel.connect_material_expressions(sine_wx, "", sine_sum, "A"):
        raise RuntimeError("connect sine_wx -> sine_sum.A failed")
    if not mel.connect_material_expressions(sine_wy, "", sine_sum, "B"):
        raise RuntimeError("connect sine_wy -> sine_sum.B failed")

    # sine_sum ranges [-2, 2] (two independent unit sines); 0.75 bounds the
    # final ripple to +/-1.5 UU, i.e. +/-1.5 cm of vertical bob -- inside the
    # "no more than 1-2 UU" budget, and that +/-1.5 UU extreme is only
    # reached at the isolated points where both independently-phased waves
    # peak together; the typical instantaneous magnitude is well under it.
    ripple_amplitude = mel.create_material_expression(material, unreal.MaterialExpressionConstant, 200, 1970)
    ripple_amplitude.set_editor_property("r", 0.75)
    ripple_height = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 350, 1900)
    if not mel.connect_material_expressions(sine_sum, "", ripple_height, "A"):
        raise RuntimeError("connect sine_sum -> ripple_height.A failed")
    if not mel.connect_material_expressions(ripple_amplitude, "", ripple_height, "B"):
        raise RuntimeError("connect ripple_amplitude -> ripple_height.B failed")

    # Same B mask as the fill-drop WPO above and the normal detail above, and
    # for the same reason: only vertices ON the top boundary may move, or a
    # side wall's bottom edge lifts off the floor it's supposed to be sealed
    # against.
    ripple_masked = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 500, 1900)
    if not mel.connect_material_expressions(ripple_height, "", ripple_masked, "A"):
        raise RuntimeError("connect ripple_height -> ripple_masked.A failed")
    if not mel.connect_material_expressions(vertex_color, "B", ripple_masked, "B"):
        raise RuntimeError("connect vertex_color.B -> ripple_masked.B failed")

    # (0,0,1) * scalar broadcast -> (0,0,ripple): the same broadcast idiom
    # down_one_voxel/drop_amount already use above for the fill-drop term.
    up_axis = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, 500, 2000)
    up_axis.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
    ripple_wpo = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 650, 1950)
    if not mel.connect_material_expressions(up_axis, "", ripple_wpo, "A"):
        raise RuntimeError("connect up_axis -> ripple_wpo.A failed")
    if not mel.connect_material_expressions(ripple_masked, "", ripple_wpo, "B"):
        raise RuntimeError("connect ripple_masked -> ripple_wpo.B failed")

    # ADD to the existing fill-drop WPO, do not replace it: the stepped
    # surface from the section above and the continuous ripple here are two
    # independent reasons a vertex moves, and both have to apply at once.
    total_wpo = mel.create_material_expression(material, unreal.MaterialExpressionAdd, 800, 300)
    if not mel.connect_material_expressions(world_position_offset, "", total_wpo, "A"):
        raise RuntimeError("connect world_position_offset -> total_wpo.A failed")
    if not mel.connect_material_expressions(ripple_wpo, "", total_wpo, "B"):
        raise RuntimeError("connect ripple_wpo -> total_wpo.B failed")

    if not mel.connect_material_property(total_wpo, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
        raise RuntimeError("connect total_wpo -> WorldPositionOffset failed")

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_WaterVoxel created and saved at " + FULL_PATH)


main()
