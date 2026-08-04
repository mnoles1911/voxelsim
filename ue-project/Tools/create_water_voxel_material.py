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
  A = per-brick FOAM ACTIVITY 0..1 (W5; was a fixed 255, unused). 1 while
      vxc::WaterCA still calls this brick active, 0 once it settles and 0 for
      every implicit (worldgen) brick. The pooled path reaches it via
      ChunkParams.y, which the vertex factory already copies into colour A in
      every mode -- no new uniform-buffer member, and therefore no exposure to
      the loose-FShaderParameter trap that silently no-ops in this factory.

Terrain packs a binary sky-facing biome flag in R and per-chunk climate in B,
so this is water's OWN convention, not a shared one. The component path writes
it directly (VoxelWaterChunkComponent.cpp) and the pooled path reproduces it
under FVoxelQuadVertexFactoryParameters::WaterMode.

W5 COLOUR (this update): the constant tint and constant 0.55 opacity are GONE,
replaced by depth-tinted absorption and foam. Read this before touching either.

That constancy was load-bearing rather than lazy: docs/gpu-water-pool-design.md
shows the water pool was safe as ONE primitive with ONE translucent sort key
only while it held, since N identical surfaces transmit (1-0.55)^N in any blend
order. Both terms below make two water fragments differ in COLOUR and OPACITY,
not merely in lighting, so `over` composition stops being order-independent and
the single sort key stops being sound.

THE SORT WORK LANDED FIRST, IN THE SAME WAVE AND DELIBERATELY BEFORE THIS: the
water pool is now several primitives bucketed at 64 bricks (51.2 m) of world
space -- see GetOrCreateWaterPoolBucket in VoxelWaterSubsystem.cpp for why that
size and what it does and does not fix. The ordering was the point: a tint
applied over a broken sort makes a sorting artefact and a shading artefact
indistinguishable, and there is no way back from that except reverting one.

WHAT IS STILL NOT DONE, and is still a sort-key hazard if added: REFRACTION or
any scene-COLOUR read. Reading scene DEPTH (below) is not the same thing --
depth is written by the opaque pass and is invariant to translucent draw order,
so each fragment computes its own thickness identically however the stack is
composed. A scene-colour read is not, because the value it reads IS the
partially composed stack.

W6 STILL WATER (this update): gives the surface a Fresnel-weighted sky
reflection and an analytic sun glint, both independent of the foam channel, and
folds the same Fresnel into opacity. See "W6: THE STILL-WATER SURFACE" below for
the measurement that motivated it and for the correction it carries -- in short,
a settled basin was never invisible, it was a flat tinted film with no
view-dependent term, and the claim that vertex colour A was "what makes water
visible" does not survive reading this graph. Foam remains a lerp layered on
top and is unchanged; the only authored constant moved is shallow opacity,
0.55 -> 0.35.

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

    vertex_color = mel.create_material_expression(material, unreal.MaterialExpressionVertexColor, -500, -100)

    # --- W5 (1/3): DEPTH-TINTED ABSORPTION ---------------------------------
    #
    # "Colour by depth through the surface, so shallow water reads pale and
    # deep water reads deep." The quantity that means is the length of the
    # VIEW RAY inside the water, and the renderer already knows it: for a
    # translucent fragment, SceneDepth is the depth of the nearest OPAQUE
    # surface behind it (translucency does not write depth), and PixelDepth is
    # this fragment's own. Their difference is the along-ray thickness in
    # unreal units.
    #
    # WHY NOT VertexColor.R, which is right here and already carries fill.
    # Because it is the fill fraction of ONE CELL, not a depth: a brim-full
    # cell is 1.0 whether it is the top of a 10 m lake or a 10 cm puddle, so
    # tinting by it would colour by "how full is this voxel" and leave a
    # puddle and an ocean identical. R drives geometry (the WPO below); it
    # cannot drive absorption.
    #
    # WHY NOT A PER-BRICK COLUMN DEPTH pushed through ChunkParams. It would be
    # order-independent and cheap, but a water brick is 80 cm on a side, so the
    # tint would step in 80 cm blocks across a shoreline -- exactly the banding
    # the per-corner heights were introduced to remove from the geometry. The
    # view-ray thickness is continuous and, at a shallow grazing angle, is what
    # actually makes a beach read as a beach.
    #
    # THE ORDER-DEPENDENCE THIS INTRODUCES IS REAL AND IS THE REASON THE SORT
    # BUCKETS LANDED FIRST -- but note what it is NOT. Reading scene DEPTH does
    # not make one water fragment's colour depend on another's: depth comes from
    # the opaque pass and is fixed before any translucency draws. What changes
    # is that two water fragments no longer have the SAME colour and opacity, so
    # compositing them in the wrong order now shows. See the module docstring.
    #
    # KNOWN LIMITATION, worth stating rather than discovering: where nothing
    # opaque is behind the water (a surface pool seen against the sky at a
    # grazing angle), SceneDepth is the far plane and the water reads at maximum
    # depth tint. Underground -- where all of this project's water currently is
    # -- there is always a cavern wall or floor behind, so it does not arise;
    # a sky-facing pour is the case to watch for.
    scene_depth = mel.create_material_expression(material, unreal.MaterialExpressionSceneDepth, -1300, -700)
    pixel_depth = mel.create_material_expression(material, unreal.MaterialExpressionPixelDepth, -1300, -600)

    thickness_raw = mel.create_material_expression(material, unreal.MaterialExpressionSubtract, -1120, -650)
    if not mel.connect_material_expressions(scene_depth, "", thickness_raw, "A"):
        raise RuntimeError("connect scene_depth -> thickness_raw.A failed")
    if not mel.connect_material_expressions(pixel_depth, "", thickness_raw, "B"):
        raise RuntimeError("connect pixel_depth -> thickness_raw.B failed")

    # Clamped at zero. The difference is negative wherever the water fragment
    # is BEHIND the opaque depth it is being compared against, which happens on
    # the frame a chunk streams in and, transiently, wherever the WPO ripple
    # pushes a vertex past a wall. Feeding a negative into the exponential below
    # would produce a transmittance ABOVE 1 and light the water up rather than
    # darkening it -- a bright flash, which is far more visible than the
    # sub-pixel geometry error that caused it.
    thickness_zero = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -1120, -560)
    thickness_zero.set_editor_property("r", 0.0)
    thickness = mel.create_material_expression(material, unreal.MaterialExpressionMax, -960, -650)
    if not mel.connect_material_expressions(thickness_raw, "", thickness, "A"):
        raise RuntimeError("connect thickness_raw -> thickness.A failed")
    if not mel.connect_material_expressions(thickness_zero, "", thickness, "B"):
        raise RuntimeError("connect thickness_zero -> thickness.B failed")

    # Beer-Lambert, one channel: depth01 = 1 - exp(-thickness / D).
    #
    # D = 250 UU (2.5 m) -- so 2.5 m of water is ~63% of the way to the deep
    # colour, 5 m is ~86%, and 10 m is ~98%. Chosen against the SCENE this
    # renders, not against real water: cavern lakes here are metres deep, not
    # tens, and a real optical depth for clear water (order 10 m per channel)
    # would leave every body in the game reading as the pale shallow colour and
    # make the whole term invisible. A single scalar rather than a per-channel
    # extinction vector for the same reason the AO multiply is one node: the
    # per-channel version is a strictly better model that costs three
    # exponentials, and the two-colour lerp below already puts the red loss in
    # the right place by construction.
    absorb_rate = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -960, -560)
    #
    # W7: D = 250 -> 160 UU (2.5 m -> 1.6 m). The owner's phrasing is about
    # voxels STACKING: "as water voxels stack on top of one another there should
    # be a depth effect". A voxel is 10 cm, so the interesting range is the first
    # few tens of centimetres to a couple of metres, and at D = 250 that whole
    # band sat in the flattest part of the curve -- 30 cm of water reached only
    # depth01 0.11, so a pool several voxels deep looked the same as one voxel.
    # At D = 160: 30 cm -> 0.17, 1 m -> 0.46, 2 m -> 0.71, 5 m -> 0.96. The cue
    # now moves where the player actually sees it accumulate, and deep basins
    # still saturate (10 m -> 0.998).
    absorb_rate.set_editor_property("r", -1.0 / 160.0)
    absorb_exponent = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -800, -650)
    if not mel.connect_material_expressions(thickness, "", absorb_exponent, "A"):
        raise RuntimeError("connect thickness -> absorb_exponent.A failed")
    if not mel.connect_material_expressions(absorb_rate, "", absorb_exponent, "B"):
        raise RuntimeError("connect absorb_rate -> absorb_exponent.B failed")

    transmittance = mel.create_material_expression(material, unreal.MaterialExpressionExponential, -650, -650)
    if not mel.connect_material_expressions(absorb_exponent, "", transmittance, ""):
        raise RuntimeError("connect absorb_exponent -> transmittance failed")

    depth01 = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -500, -650)
    if not mel.connect_material_expressions(transmittance, "", depth01, ""):
        raise RuntimeError("connect transmittance -> depth01 failed")

    # Shallow: pale green-cyan, the colour of a few centimetres of water over
    # rock. Deep: near-black blue. Neither is the old constant (0.05,0.25,0.55);
    # that value sat between them and is what a ~2 m column now lands on, which
    # is roughly where the previous look was calibrated.
    shallow_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -500, -480)
    # NEUTRALISED PENDING CALIBRATION (2026-07-29). The authored value was
    # (0.38, 0.66, 0.68) -- a pale cyan -- and in engine the pool washed out to
    # near-WHITE, brighter than the terrain around it. Isolated by setting the
    # foam activity gain to 0 and re-capturing: still white, so the depth tint
    # is the cause and foam is exonerated.
    #
    # The graph, the buckets and the depth term all work; only these four
    # constants are wrong, and they were shipped explicitly "reasoned but
    # uncalibrated". Calibrating a water colour from screenshots at 3am is how
    # you get a second wrong value, so both ends are pinned to the pre-W5 look
    # -- LinearColor(0.05, 0.25, 0.55) at 0.55 opacity -- which renders exactly
    # as the shipped water did. Re-enabling the effect is these four numbers,
    # with a human looking at it.
    # CONSERVATIVE PASS (Matt, 2026-07-29). The authored (0.38, 0.66, 0.68)
    # washed the pool to near-WHITE. Rather than guess a second pale value,
    # SHALLOW IS PINNED TO THE SHIPPED BLUE and only the deep end moves.
    #
    # That makes the previous failure structurally impossible rather than
    # merely less likely: thin water -- which is most of a spread pour, and
    # every frame that looked wrong -- now renders EXACTLY as it did before
    # the tint existed, because at depth01 ~ 0 both the colour and the opacity
    # lerps resolve to their shallow ends, which are the old constants. Depth
    # can only ever ADD darkness from there. There is no input for which this
    # is brighter than the shipped water.
    # W7: "more blue colour". Blue is lifted and red pulled down at BOTH ends,
    # which raises saturation without raising luminance -- the shallow end is
    # no brighter than the shipped colour, it is bluer.
    shallow_tint.set_editor_property("constant", unreal.LinearColor(0.035, 0.26, 0.68, 1.0))
    deep_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -500, -400)
    # Deep end: the same hue, darkened and desaturated toward the absorption
    # the real effect is modelling. Deliberately NOT the authored
    # (0.012, 0.055, 0.16), which is nearly black -- a 2.5 m pond hitting that
    # would read as a hole in the ground. This is roughly a third of the
    # shallow value, so a deep body darkens visibly without going to ink.
    # W7: the owner asked for DARK BLUE at depth, not near-black. Red and green
    # come down, blue holds -- so the deep end reads unmistakably blue while
    # getting darker, instead of desaturating toward ink as it deepens.
    deep_tint.set_editor_property("constant", unreal.LinearColor(0.008, 0.055, 0.24, 1.0))

    water_tint = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -320, -450)
    if not mel.connect_material_expressions(shallow_tint, "", water_tint, "A"):
        raise RuntimeError("connect shallow_tint -> water_tint.A failed")
    if not mel.connect_material_expressions(deep_tint, "", water_tint, "B"):
        raise RuntimeError("connect deep_tint -> water_tint.B failed")
    if not mel.connect_material_expressions(depth01, "", water_tint, "Alpha"):
        raise RuntimeError("connect depth01 -> water_tint.Alpha failed")

    # Opacity varies with depth too, and that is the half that makes shallow
    # water read as shallow rather than merely pale: a 5 cm film over a cavern
    # floor should show the floor almost undimmed, which a fixed 0.55 cannot do
    # at any tint. 0.18 -> 0.86 brackets the old 0.55 so a mid-depth body sits
    # close to where this material has always sat.
    shallow_opacity = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -500, -330)
    # W6: shallow opacity drops 0.55 -> 0.35. This is the ONE authored constant
    # this change moves, and it is moved deliberately rather than as a re-tune.
    #
    # The owner's brief for still water is "shallow shoreline reads lighter than
    # the middle". The 0.55 -> 0.72 bracket does express that, but only over a
    # 0.17 range, and W6's measurement of a real lake found depth01 ~= 0.55 over
    # most of a basin -- so nearly the whole surface sat within a few hundredths
    # of one opacity and the shoreline gradient was, in practice, not there.
    # 0.35 -> 0.72 doubles the range the term has to work with.
    #
    # THIS DOES NOT REPEAT THE "washed to near-WHITE" INCIDENT recorded above.
    # That failure was the surface getting BRIGHTER; this makes thin water more
    # TRANSPARENT, so a shoreline reads lighter only because more of the lit bed
    # shows through it. Deep water is untouched at 0.72, which is where the
    # near-white regression was actually judged.
    #
    # W7, FROM THE OWNER JUDGING W6 IN GAME: "make the water voxels less
    # transparent, more blue colour, and as water voxels stack on top of one
    # another there should be a depth effect -- shallow pools more easily seen
    # through, deeper water dark blue and non-transparent."
    #
    # So the RANGE W6 opened up is kept and its ends are pushed apart further,
    # rather than sliding the whole thing opaque: shallow stays see-through on
    # purpose (0.35 -> 0.42, still the most transparent this material has been
    # below the old 0.55), and the deep end goes to genuinely opaque.
    shallow_opacity.set_editor_property("r", 0.42)
    deep_opacity = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -500, -270)
    # W7: 0.72 -> 0.98. The owner asked for deep water to be NON-TRANSPARENT.
    # Not 1.0: a hair of transmission keeps the deep tint from flattening into
    # a painted decal at grazing angles, and the Fresnel term below still adds
    # to this, so 0.98 already composites as opaque looking down into a basin.
    #
    # This is the opposite direction from the near-WHITE regression, which was
    # the surface getting brighter and lower-contrast. Deep water here gets
    # darker, bluer and more opaque; shallow water is the only thing that stays
    # readable through, which is exactly the depth cue being asked for.
    deep_opacity.set_editor_property("r", 0.98)
    depth_opacity = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -320, -300)
    if not mel.connect_material_expressions(shallow_opacity, "", depth_opacity, "A"):
        raise RuntimeError("connect shallow_opacity -> depth_opacity.A failed")
    if not mel.connect_material_expressions(deep_opacity, "", depth_opacity, "B"):
        raise RuntimeError("connect deep_opacity -> depth_opacity.B failed")
    if not mel.connect_material_expressions(depth01, "", depth_opacity, "Alpha"):
        raise RuntimeError("connect depth01 -> depth_opacity.Alpha failed")

    # --- W5 (2/3): FOAM ----------------------------------------------------
    #
    # Two signals, both of which already exist -- no new geometry, no new
    # texture, and (crucially) no new vertex-factory uniform member.
    #
    # SIGNAL 1, SLOPE. The water surface is a bilinear patch over four per-cell
    # corner heights, and the vertex factory builds its shading normal from that
    # patch (Decoded.ShadingNormal; the CPU path computes the identical
    # -dH/du,-dH/dv,1 in VoxelWaterChunkComponent.cpp). So VertexNormalWS.z is
    # already a slope reading, for free, and it is large wherever fill changes
    # fast across a cell: a spilling front, a step between fill levels, water
    # running down a slope. A settled pool has every corner equal, normal
    # straight up, and no foam anywhere -- which is the behaviour that matters,
    # because "foam everywhere all the time" is the standard way this effect
    # goes wrong.
    #
    # SIGNAL 2, ACTIVITY. VertexColor.A, 1 while vxc::WaterCA still calls the
    # brick active. This catches the case slope cannot: a brick churning
    # violently but momentarily flat.
    #
    # SIDE WALLS ARE EXCLUDED BY THE NORMAL, NOT BY VertexColor.B. B is the
    # top-BOUNDARY flag and is 1 on a side wall's upper pair of vertices, so
    # masking with it would ring every water body with a foam stripe along the
    # top of its side walls. A side face's shading normal has z == 0 exactly
    # (only the top face's normal is replaced by the corner-height gradient), so
    # saturate(N.z * 4) is 0 there and 1 on any top face that is not nearly
    # vertical -- and it also excludes bottom faces, whose z is -1.
    vertex_normal = mel.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -1300, -200)
    surface_normal_z = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1120, -200)
    surface_normal_z.set_editor_property("r", False)
    surface_normal_z.set_editor_property("g", False)
    surface_normal_z.set_editor_property("b", True)
    surface_normal_z.set_editor_property("a", False)
    if not mel.connect_material_expressions(vertex_normal, "", surface_normal_z, ""):
        raise RuntimeError("connect vertex_normal -> surface_normal_z failed")

    top_face_gain = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -1120, -120)
    top_face_gain.set_editor_property("r", 4.0)
    top_face_scaled = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -960, -180)
    if not mel.connect_material_expressions(surface_normal_z, "", top_face_scaled, "A"):
        raise RuntimeError("connect surface_normal_z -> top_face_scaled.A failed")
    if not mel.connect_material_expressions(top_face_gain, "", top_face_scaled, "B"):
        raise RuntimeError("connect top_face_gain -> top_face_scaled.B failed")
    top_face_mask = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -800, -180)
    if not mel.connect_material_expressions(top_face_scaled, "", top_face_mask, ""):
        raise RuntimeError("connect top_face_scaled -> top_face_mask failed")

    # SmoothStep(min, max, N.z) is 1 on flat water and 0 once the surface is
    # steep, so the foam term is its complement. The window is quoted in normal
    # z rather than in degrees on purpose, since that is what the node compares:
    #   0.985 -> ~10 degrees of tilt. Below this is ordinary bilinear wobble on
    #           a settled surface and must NOT foam, or every lake whitens.
    #   0.870 -> ~30 degrees. A full 10 cm fill step across one 10 cm voxel is
    #           45 degrees (z = 0.707), well past saturation, so a genuine
    #           spilling edge foams fully.
    slope_min = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -800, -100)
    slope_min.set_editor_property("r", 0.870)
    slope_max = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -800, -40)
    slope_max.set_editor_property("r", 0.985)
    slope_flat = mel.create_material_expression(material, unreal.MaterialExpressionSmoothStep, -650, -100)
    if not mel.connect_material_expressions(slope_min, "", slope_flat, "Min"):
        raise RuntimeError("connect slope_min -> slope_flat.Min failed")
    if not mel.connect_material_expressions(slope_max, "", slope_flat, "Max"):
        raise RuntimeError("connect slope_max -> slope_flat.Max failed")
    if not mel.connect_material_expressions(surface_normal_z, "", slope_flat, "Value"):
        raise RuntimeError("connect surface_normal_z -> slope_flat.Value failed")
    slope_foam = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -500, -100)
    if not mel.connect_material_expressions(slope_flat, "", slope_foam, ""):
        raise RuntimeError("connect slope_flat -> slope_foam failed")

    # Activity contributes at 0.6, not 1.0: a brick can be "active" for a single
    # CA step because one cell gained a few fill units, and full whitewater for
    # that is a flicker. Slope alone can still reach 1.0, so a real breaking
    # front is not capped by this.
    activity_gain = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -650, 20)
    # Foam restored to the authored gain. It was zeroed only as a DIAGNOSTIC to
    # isolate tint-versus-foam, and that test exonerated it: with the gain at 0
    # the water was still white, so the fault was never here. The slope mask is
    # correctly complemented (flat water gets zero slope-foam) and activity is
    # binary on the CA active set, so a settled body carries none.
    activity_gain.set_editor_property("r", 0.6)
    activity_foam = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -500, -20)
    if not mel.connect_material_expressions(vertex_color, "A", activity_foam, "A"):
        raise RuntimeError("connect vertex_color.A -> activity_foam.A failed")
    if not mel.connect_material_expressions(activity_gain, "", activity_foam, "B"):
        raise RuntimeError("connect activity_gain -> activity_foam.B failed")

    # MAX, not add: the two signals describe the same physical thing from two
    # directions, and a steep, active front should be fully foamed rather than
    # 1.6x foamed and clipped -- clipping would flatten the distinction between
    # "quite foamy" and "extremely foamy" over most of the interesting range.
    foam_raw = mel.create_material_expression(material, unreal.MaterialExpressionMax, -340, -60)
    if not mel.connect_material_expressions(slope_foam, "", foam_raw, "A"):
        raise RuntimeError("connect slope_foam -> foam_raw.A failed")
    if not mel.connect_material_expressions(activity_foam, "", foam_raw, "B"):
        raise RuntimeError("connect activity_foam -> foam_raw.B failed")

    foam = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -190, -60)
    if not mel.connect_material_expressions(foam_raw, "", foam, "A"):
        raise RuntimeError("connect foam_raw -> foam.A failed")
    if not mel.connect_material_expressions(top_face_mask, "", foam, "B"):
        raise RuntimeError("connect top_face_mask -> foam.B failed")

    # --- W5 (3/3): composite -----------------------------------------------
    foam_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -190, -420)
    foam_tint.set_editor_property("constant", unreal.LinearColor(0.82, 0.90, 0.94, 1.0))
    tinted = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -40, -450)
    if not mel.connect_material_expressions(water_tint, "", tinted, "A"):
        raise RuntimeError("connect water_tint -> tinted.A failed")
    if not mel.connect_material_expressions(foam_tint, "", tinted, "B"):
        raise RuntimeError("connect foam_tint -> tinted.B failed")
    if not mel.connect_material_expressions(foam, "", tinted, "Alpha"):
        raise RuntimeError("connect foam -> tinted.Alpha failed")

    # AO stays the LAST multiply on base colour, exactly where it was. It is a
    # geometric occlusion term from the greedy mesher and applies to whatever
    # colour the surface ended up being -- folding it in before the depth lerp
    # would make an occluded corner read as SHALLOWER rather than darker.
    ao_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 120, -450)
    if not mel.connect_material_expressions(tinted, "", ao_multiply, "A"):
        raise RuntimeError("connect tinted -> ao_multiply.A failed")
    if not mel.connect_material_expressions(vertex_color, "G", ao_multiply, "B"):
        raise RuntimeError("connect vertex_color.G -> ao_multiply.B failed")
    if not mel.connect_material_property(ao_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect ao_multiply -> BaseColor failed")

    # --- W6: THE STILL-WATER SURFACE ---------------------------------------
    #
    # WHAT THIS FIXES, stated against a measurement rather than an impression.
    # The W5 material gives still water a colour and an opacity but no SURFACE:
    # nothing in the graph above depends on where the viewer is standing, so a
    # settled basin composites as a flat tinted film and reads as haze over the
    # ground rather than as water. Measured at the tile (-12,-5) lake, camera
    # 2.4 m over the 365.1 m datum, against a no-water control frame at the same
    # pose: the shipped still surface moves the frame from mean RGB
    # (215.6, 207.0, 196.2) to (155.6, 152.5, 149.6), 99.99% of pixels differing
    # by more than 25. It was never invisible. It was a grey wash.
    #
    # THE CORRECTION THIS CARRIES. An earlier reading of the same lake concluded
    # that vertex colour A -- the foam activity -- was "what makes water visible"
    # and that a still surface therefore drew nothing. That is not what the graph
    # does: foam is a LERP on top of an already-complete colour and opacity (see
    # `tinted` and `opacity`), and with foam at 0 the opacity output is still
    # 0.55..0.72. The A/B that produced the claim compared Activity 0 against
    # Activity 1 and never took a no-water control, so "different from the foamed
    # frame" was read as "absent". Against the control it is the ACTIVITY-0 frame
    # that differs more (mean 60.0 vs 21.9). Nothing here rides the foam channel,
    # and nothing needed to.
    #
    # WHAT A SURFACE NEEDS THAT A FILM DOES NOT HAVE: a view-dependent term.
    # Fresnel is the whole of it -- water is nearly transparent looking straight
    # down and nearly a mirror at a grazing angle, and that single fact is what
    # the eye reads as "liquid surface" before any wave or glint registers.
    #
    # SORT-KEY POSITION, because the module docstring makes this the standing
    # question for any addition here. This reads NO scene colour, so the ban that
    # rules out refraction is not engaged. It does add a view-dependent term to
    # colour and opacity, which the depth tint already did -- but note it is
    # strictly weaker than the depth tint on this axis: two overlapping water
    # fragments at the same pixel share a view ray and a normal, so Fresnel gives
    # them the SAME value, where SceneDepth deliberately gives them different
    # ones. It introduces no ordering sensitivity the buckets do not already
    # carry.
    sky_collection = unreal.load_object(None, "/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky")
    if sky_collection is None:
        raise RuntimeError("MPC_VoxelSky not found -- the sun glint needs SunDirection")

    # SunDirection is written every frame by VoxelSkySubsystem (ApplySkyMaterial
    # parameters). Reusing it rather than a constant is what keeps the glint on
    # the actual sun: the water tracks sunrise and sunset for free, and a frozen
    # -TimeScale 0 capture gets the sun the rest of the frame was lit by.
    sun_dir_param = mel.create_material_expression(
        material, unreal.MaterialExpressionCollectionParameter, -1300, 1300)
    sun_dir_param.set_editor_property("collection", sky_collection)
    sun_dir_param.set_editor_property("parameter_name", "SunDirection")

    sun_dir3 = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1120, 1300)
    sun_dir3.set_editor_property("r", True)
    sun_dir3.set_editor_property("g", True)
    sun_dir3.set_editor_property("b", True)
    sun_dir3.set_editor_property("a", False)
    if not mel.connect_material_expressions(sun_dir_param, "", sun_dir3, ""):
        raise RuntimeError("connect sun_dir_param -> sun_dir3 failed")

    # SUN GLINT, computed analytically instead of left to the engine's specular.
    # The material already sets Specular 0.5 / Roughness 0.08 and that survives
    # unchanged, but a translucent surface's specular response is the part of
    # translucent lighting that is least reliable to author against -- the W3
    # note above records the same lesson from the other side, where the
    # volumetric lighting mode ignored the material normal outright. reflect(V)
    # dot SunDirection is not subject to any of that: it is the mirror direction
    # against this material's own rippled normal, so the glint lands exactly
    # where the wave that produced it is.
    refl_vec = mel.create_material_expression(material, unreal.MaterialExpressionReflectionVectorWS, -1120, 1450)

    glint_dot = mel.create_material_expression(material, unreal.MaterialExpressionDotProduct, -950, 1380)
    if not mel.connect_material_expressions(refl_vec, "", glint_dot, "A"):
        raise RuntimeError("connect refl_vec -> glint_dot.A failed")
    if not mel.connect_material_expressions(sun_dir3, "", glint_dot, "B"):
        raise RuntimeError("connect sun_dir3 -> glint_dot.B failed")

    glint_sat = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -800, 1380)
    if not mel.connect_material_expressions(glint_dot, "", glint_sat, ""):
        raise RuntimeError("connect glint_dot -> glint_sat failed")

    # Exponent 900: a TIGHT lobe on purpose. The sun subtends about half a degree
    # and a calm lake returns it as a small hard highlight; a loose lobe here is
    # the classic way this reads as wet plastic over the whole basin instead of
    # as one glint. This is also the term most able to re-create the near-white
    # failure the depth-tint comment records, and keeping it narrow is what
    # bounds the area it can affect at all.
    # Carried on the node's own ConstExponent rather than a wired Constant: the
    # Exponent pin is named "Exp", and a Constant wired into it would be one more
    # node for a value that never varies. Not a style preference -- the checked
    # connect above this is what caught the wrong pin name, and the fix that
    # removes the pin removes the class of mistake with it.
    glint_pow = mel.create_material_expression(material, unreal.MaterialExpressionPower, -650, 1400)
    glint_pow.set_editor_property("const_exponent", 900.0)
    if not mel.connect_material_expressions(glint_sat, "", glint_pow, "Base"):
        raise RuntimeError("connect glint_sat -> glint_pow.Base failed")

    # Slightly over 1 and slightly warm: a specular sun return is brighter than
    # the sky it sits in, and clamping it to 1 is what makes a highlight read as
    # a painted white dot rather than as light.
    glint_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -650, 1520)
    glint_tint.set_editor_property("constant", unreal.LinearColor(2.6, 2.45, 2.15, 1.0))
    glint = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -500, 1440)
    if not mel.connect_material_expressions(glint_pow, "", glint, "A"):
        raise RuntimeError("connect glint_pow -> glint.A failed")
    if not mel.connect_material_expressions(glint_tint, "", glint, "B"):
        raise RuntimeError("connect glint_tint -> glint.B failed")

    # SKY REFLECTION. A constant sky colour gated by sun altitude, NOT a scene
    # capture and NOT a reflection probe: the ban in the module docstring is on
    # reading scene COLOUR, and a probe read is the same hazard wearing a
    # different name. SunDirection.z is the sine of the sun's altitude, so
    # saturate() of it is 0 from dusk to dawn and the lake stops reflecting a
    # blue sky it cannot see -- the one piece of time-of-day behaviour this term
    # genuinely needs, and it costs one node.
    sun_alt = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1120, 1600)
    sun_alt.set_editor_property("r", False)
    sun_alt.set_editor_property("g", False)
    sun_alt.set_editor_property("b", True)
    sun_alt.set_editor_property("a", False)
    if not mel.connect_material_expressions(sun_dir_param, "", sun_alt, ""):
        raise RuntimeError("connect sun_dir_param -> sun_alt failed")

    day_gate = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -950, 1600)
    if not mel.connect_material_expressions(sun_alt, "", day_gate, ""):
        raise RuntimeError("connect sun_alt -> day_gate failed")

    # KNOWN LIMITATION, stated rather than left to be discovered, and the exact
    # counterpart of the SceneDepth-against-sky note in the depth section above.
    # saturate(SunDirection.z) is "is the sun up", which is not the same question
    # as "can THIS surface see the sky". A static cavern pool a hundred metres
    # underground at noon therefore still gets a sky-blue reflection at grazing
    # angles, from a sky it has no line of sight to. It is Fresnel-weighted, so
    # looking down into the pool -- how a cavern pool is normally met -- it is
    # near zero, and it is the same magnitude a surface lake gets. Fixing it
    # properly needs a sky-visibility signal that does not exist on this vertex
    # format: VertexColor.G is the greedy mesher's local AO, which is ~1 in the
    # middle of any chamber large enough to hold a pool and so cannot express it.
    # NOT VERIFIED IN A CAPTURE: -VoxelFloodTest found no flooded cavern at the
    # lake site, and the default cavern site's fine tiles are absent from this
    # box's cache (tile (-6,3) at s16, absentOnDisk=1). Downgrading the fine-tier
    # gate to get a frame would have made that frame unreproducible, which is the
    # one thing the gate exists to prevent.
    sky_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -950, 1700)
    sky_tint.set_editor_property("constant", unreal.LinearColor(0.30, 0.46, 0.72, 1.0))
    sky_lit = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -800, 1650)
    if not mel.connect_material_expressions(sky_tint, "", sky_lit, "A"):
        raise RuntimeError("connect sky_tint -> sky_lit.A failed")
    if not mel.connect_material_expressions(day_gate, "", sky_lit, "B"):
        raise RuntimeError("connect day_gate -> sky_lit.B failed")

    # Fresnel with water's real normal-incidence reflectance, 0.02. The Normal
    # input is deliberately LEFT UNCONNECTED so the node uses this material's own
    # shading normal -- which is the panning ripple authored in the W3 section
    # below. Connecting `normal_xyz` here instead would be a bug that compiles:
    # that value is TANGENT space and this pin wants world space.
    fresnel = mel.create_material_expression(material, unreal.MaterialExpressionFresnel, -650, 1650)
    fresnel.set_editor_property("exponent", 5.0)
    fresnel.set_editor_property("base_reflect_fraction", 0.02)

    reflection = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -500, 1650)
    if not mel.connect_material_expressions(sky_lit, "", reflection, "A"):
        raise RuntimeError("connect sky_lit -> reflection.A failed")
    if not mel.connect_material_expressions(fresnel, "", reflection, "B"):
        raise RuntimeError("connect fresnel -> reflection.B failed")

    surface_light = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -340, 1540)
    if not mel.connect_material_expressions(reflection, "", surface_light, "A"):
        raise RuntimeError("connect reflection -> surface_light.A failed")
    if not mel.connect_material_expressions(glint, "", surface_light, "B"):
        raise RuntimeError("connect glint -> surface_light.B failed")

    # FOAM IS ADDITIVE ON TOP OF THIS, and this is the pin that makes that true
    # in the direction that matters. Froth is a scattering medium: it is the one
    # part of a water surface that does NOT mirror the sky, and letting the
    # reflection survive underneath it would put a sky sheen on whitewater. The
    # existing foam lerps on colour, opacity and roughness are unchanged and
    # still run after this, so foam continues to win where it is present and
    # contributes nothing at all where it is zero -- which is every still lake
    # and every static cavern pool.
    one_minus_foam = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -340, 1660)
    if not mel.connect_material_expressions(foam, "", one_minus_foam, ""):
        raise RuntimeError("connect foam -> one_minus_foam failed")

    surface_unfoamed = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -190, 1600)
    if not mel.connect_material_expressions(surface_light, "", surface_unfoamed, "A"):
        raise RuntimeError("connect surface_light -> surface_unfoamed.A failed")
    if not mel.connect_material_expressions(one_minus_foam, "", surface_unfoamed, "B"):
        raise RuntimeError("connect one_minus_foam -> surface_unfoamed.B failed")

    # Masked to top faces by the SAME top_face_mask the foam uses, for the same
    # reason given there: a submerged side wall is not a sky-facing surface and
    # must not reflect one. This is a normal test, not VertexColor.B -- see the
    # foam section for why B would ring every body with a stripe.
    surface_emissive = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -40, 1600)
    if not mel.connect_material_expressions(surface_unfoamed, "", surface_emissive, "A"):
        raise RuntimeError("connect surface_unfoamed -> surface_emissive.A failed")
    if not mel.connect_material_expressions(top_face_mask, "", surface_emissive, "B"):
        raise RuntimeError("connect top_face_mask -> surface_emissive.B failed")

    # EMISSIVE, not BaseColor. A reflection is light leaving the surface, not
    # albedo: routing it through BaseColor would make it get multiplied by AO and
    # by the diffuse lighting term, so a reflected sky would darken in an
    # occluded corner, which is backwards.
    if not mel.connect_material_property(surface_emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("connect surface_emissive -> EmissiveColor failed")

    # Foam is nearly opaque: whitewater is a scattering medium, not a tinted
    # one, and leaving it at the depth-derived opacity would make a breaking
    # front read as pale glass over the rocks rather than as froth.
    foam_opacity = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -190, -250)
    foam_opacity.set_editor_property("r", 0.95)

    # Opacity gets the same Fresnel before foam does anything to it: looking
    # straight down you see the bed (the "clear" half of the brief), and at a
    # grazing angle the surface closes up into a reflective sheet (the
    # "reflective" half). One node makes both true, and it is the same node the
    # sky reflection is weighted by, so the surface never reflects light it is
    # too transparent to be reflecting.
    sheen_opacity = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -190, -200)
    sheen_opacity.set_editor_property("r", 1.0)
    view_opacity = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -110, -290)
    if not mel.connect_material_expressions(depth_opacity, "", view_opacity, "A"):
        raise RuntimeError("connect depth_opacity -> view_opacity.A failed")
    if not mel.connect_material_expressions(sheen_opacity, "", view_opacity, "B"):
        raise RuntimeError("connect sheen_opacity -> view_opacity.B failed")
    if not mel.connect_material_expressions(fresnel, "", view_opacity, "Alpha"):
        raise RuntimeError("connect fresnel -> view_opacity.Alpha failed")

    opacity = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -40, -290)
    if not mel.connect_material_expressions(view_opacity, "", opacity, "A"):
        raise RuntimeError("connect view_opacity -> opacity.A failed")
    if not mel.connect_material_expressions(foam_opacity, "", opacity, "B"):
        raise RuntimeError("connect foam_opacity -> opacity.B failed")
    if not mel.connect_material_expressions(foam, "", opacity, "Alpha"):
        raise RuntimeError("connect foam -> opacity.Alpha failed")
    if not mel.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY):
        raise RuntimeError("connect opacity -> Opacity failed")

    # Roughness tightened slightly (0.1 -> 0.08) and Specular made explicit
    # (0.5 -- the engine's own unconnected-pin default, stated rather than
    # left implicit so the two are visibly tuned as a pair) now that the
    # panning normal ripple below actually reaches the lit result (see the
    # translucency-lighting-mode comment above). A tighter specular lobe
    # turns a moving normal into a moving GLINT rather than a moving blur,
    # which is the cue that reads as "surface in motion" at a glance.
    #
    # W5: roughness is no longer a flat Constant -- it lerps to 0.62 under foam.
    # Froth is the one part of a water surface that is NOT a mirror, and leaving
    # the tight 0.08 lobe on it would put a sharp specular highlight on top of
    # whitewater, which reads as wet plastic. Specular stays flat.
    calm_roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -190, -180)
    calm_roughness.set_editor_property("r", 0.08)
    foam_roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -190, -130)
    foam_roughness.set_editor_property("r", 0.62)
    roughness = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -40, -170)
    if not mel.connect_material_expressions(calm_roughness, "", roughness, "A"):
        raise RuntimeError("connect calm_roughness -> roughness.A failed")
    if not mel.connect_material_expressions(foam_roughness, "", roughness, "B"):
        raise RuntimeError("connect foam_roughness -> roughness.B failed")
    if not mel.connect_material_expressions(foam, "", roughness, "Alpha"):
        raise RuntimeError("connect foam -> roughness.Alpha failed")
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness -> Roughness failed")

    specular = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -40, -110)
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
