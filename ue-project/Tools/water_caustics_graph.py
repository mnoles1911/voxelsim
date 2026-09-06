"""The underwater CAUSTIC field, as one importable builder (Phase F1).

WHAT THIS IS. A procedural caustic light term -- the dancing bright filaments
sunlight paints on a floor seen through water -- for every material that draws
a submerged surface: M_VoxelTerrain and M_VoxelClipmap (the lake and sea
floors ARE the terrain materials; there is no separate floor material), and
M_Underwater (the same floors seen from below the surface). Authored for
docs/water-ocean-tides-plan-2026-09-04.md Phase F1, the one genuinely new
renderer feature in Phase F; the owner ask it answers is "Ocean and lake
floors should have caustic reflections."

PROCEDURAL, NOT A TEXTURE, NOT A RENDER TARGET. Three panned, MUTUALLY-WARPED
interference layers evaluated in one Custom node as a function of ABSOLUTE
WORLD XY (metres) and material time -- the same two coordinates the wave field
runs on, for the same reason (water_wave_graph's "no period, both draw paths
agree" argument): three consumers evaluate this field independently, and world
XY is the only coordinate on which the pattern a swimmer sees below can agree
with the pattern a walker sees from the beach. Real caustics are the wave
field's second derivative focused through refraction; deriving them from the
actual 8-octave field would be physically purer and was deliberately NOT done
-- the honest derivation is a screen-space or mesh-space focusing pass this
renderer does not have, and a fake derived from the real field costs the whole
field per consumer per pixel while still being a fake. An interference pattern
that merely LOOKS like focused wave light is the industry-standard cheat, and
making it its own small field keeps its cost visible and its look tunable
without touching the waves.

MUTUALLY-WARPED, concretely: layer 1 displaces the domain layer 2 is sampled
on, and both displace layer 3's. That is what breaks the fixed-lattice
regularity a plain sum of sines has -- the same defect class as the repeating
normal ripple this project already deleted once ("looks like a repeating
tile") -- while staying three sin() products deep.


=============================================================================
THE INTERFACE -- WHAT THIS READS, WHAT THE CALLER OWNS, WHO DRIVES IT
=============================================================================

TWO MPC_VoxelSky BINDINGS, both name-checked against the collection as read
back (an unresolved CollectionParameter compiles to a CONSTANT rather than
failing -- MaterialExpressions.cpp:17179-17193 -- the standing rule of this
toolchain):

  SunDirection      (vector, exists) written every frame by VoxelSkySubsystem.
                    Its Z is sin(sun altitude); it gates the whole term (no
                    caustics below a sun-altitude floor -- MOON CAUSTICS ARE A
                    NON-GOAL, stated in the plan and honoured by simply never
                    reading MoonDirection here) and it sets the sun-ray path
                    length through the water for the depth attenuation.
  CausticIntensity  (scalar, NEW -- authored by create_sky_material.py's
                    SCALAR_PARAMS, which is the authoritative list). THE
                    ENGINE-SIDE CONTRACT: UVoxelWaterSubsystem pushes the
                    float cvar `voxel.Water.Caustics` into this parameter
                    every tick, alongside the tide parameters and with the
                    same existence-checked write (VoxelRippleField.cpp:477-494
                    pattern), so cvar 0 makes every consumer's term multiply
                    to zero -- one switch, all materials, pixel-identical off.
                    Until that push lands the MPC default (1.0) holds; see the
                    default's argument in create_sky_material.py.

PLUS ONE MATERIAL SCALAR PER CONSUMER, CausticsEnabled (default 1.0): the
material-side kill switch the plan's quality row (F6) and per-material
experiments need -- an instance override or MID write can zero ONE consumer
(e.g. shafts of the underwater half) without touching the others, where the
MPC gain above moves all of them together.

WHAT THE CALLER OWNS (mirroring build_wave_field's contract):

  * pos_m      float2, ABSOLUTE world XY in METRES, at the SHADED POINT (for
               the terrain materials that is the floor texel itself; for the
               post-process it is the scene pixel's reconstructed world
               position, which in MD_PostProcess is what AbsoluteWorldPosition
               already means -- create_underwater_material.py:541).
  * time_expr  the scalar time driving the pan. Passed in, not created here,
               for the same reason build_wave_field takes it: a consumer with
               a freeze arm substitutes one node and provably freezes the
               whole term.
  * depth_m    metres of water ABOVE the shaded point, 0-or-negative = dry.
               THE LAKE AND THE SEA ARRIVE THROUGH ONE CHANNEL: bathy R.
               The B5 ocean fill (VoxelBathyField.cpp) writes ocean depth --
               computed as seaNow - ground, in C++, where the real datum is
               in scope -- into the SAME R plane the lake bake fills, so a
               terrain consumer passes bathy["depth_m"] * bathy["validity"]
               and covers both waters. This module deliberately does NOT
               compute sea depth from MPC SeaSurfaceZUU: create_sky_material
               .py's tide block states NO MATERIAL MAY USE SeaSurfaceZUU AS A
               DATUM (no TideValid flag exists to tell "not published" from
               "published as zero"), and the F1 plan line "ocean depth from
               seaNow - ground" is satisfied where that subtraction already
               legally lives -- the C++ fill. The post-process consumer has
               no bathy sample and passes its SubmergedDepthM (the CAMERA's
               depth, C++-driven) as a documented proxy: the error is largest
               looking far along a shallowing bed, where the distance fade
               below is already killing the term.
  * wiring the returned `light` into its output -- the terrain materials add
    it on EMISSIVE (an additive light term; BaseColor would multiply it by AO
    and diffuse lighting, the same argument at water_sky_reflection_graph's
    emissive note), the post-process adds it into its composite BEFORE its
    own view-path transmittance multiply is available to scale it.

WHAT THE MODULE OWNS, identically for every consumer, so it cannot drift:

  * the field (one Custom node, three warped layers, pow() sharpened);
  * the sun-altitude gate: saturate((sunZ - 0.10) / 0.15) -- zero below ~5.7
    degrees of altitude, full above ~14.5. A ramp and not a step for the
    reason M_Underwater's day fade is a ramp (a hard cut at the horizon is
    the most visible artefact available here); the floor is the plan's "off
    below a sun-altitude floor", and it also bounds the secant below;
  * the submersion gate: saturate(depth_m / 0.10) -- fades in over the first
    10 cm so the waterline does not draw a hard caustic edge on dry sand;
  * the depth attenuation, via water_optics ABSORPTION -- NEVER literals: the
    per-metre RGB coefficient is derived here from ABSORPTION_DISTANCE_M and
    ABSORPTION_COLOR exactly as both water generators derive theirs, so the
    caustic light goes teal with depth on the same numbers the water over it
    absorbs by. The PATH is angle-true down the SUN ray -- depth_m /
    max(sunZ, 0.05) -- which is the bathy_field_graph.build_slant_depth
    lesson (2026-09-05, the secant default) applied to the one ray this term
    is about: a low sun's light crosses more water, so caustics die earlier
    in the evening than at noon, with the same 20x elongation cap and for
    the same NaN-vs-nothing reason;
  * the distance fade: 1 at CausticFadeStartM (40 m, the plan's "~40 m --
    measure", hence a PARAMETER), 0 at CausticFadeEndM (64 m). Caustics are
    a near-field effect -- past a few tens of metres the real thing averages
    to nothing -- and the terrain base pass is the plan's named cost risk,
    so the far field pays only the fade's arithmetic, not the field's.

The final chain is

  light = field * sunGate * submerged * distanceFade
        * CausticIntensity(MPC) * CausticsEnabled * exp(-absorbRGB * sunPath)

a float3, non-negative, zero wherever any gate is zero.

WHAT THIS MODULE DOES NOT DO, stated so it is not discovered from a
screenshot: no moon caustics (non-goal, above); no light shafts (that is F3,
which will consume THIS field for the shaft density rather than invent a
second one); no reading of scene colour or scene depth (the terrain consumers
are opaque base-pass materials and the rule is inherited anyway); no second
authority on any optical constant.
"""

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The four optical constants -- imported, never typed, per water_optics.py's
# rule ("neither is allowed to type a coefficient as a literal"). This module
# is the fourth consumer of the absorption pair.
import water_optics  # noqa: E402

# -ln(0.02): Unity's 2%-survival convention, same constant, same reason as the
# water generators -- the derived coefficient must be THEIR coefficient.
_LN50 = 3.9120230054

DEFAULTS = {
    # Interference wavelength of the first (largest) layer, metres. Real pool
    # caustics cell at roughly the dominant surface-ripple wavelength; 2.6 m
    # sits between the wave field's mid octaves.
    "CausticScaleM": 2.6,
    # Pan/phase speed multiplier. At 1.0 the layers drift at ~0.5-1.7 rad/s;
    # 0.55 reads as lake-lazy rather than swimming-pool-frantic. DELIBERATELY
    # INDEPENDENT of the live MPC WaveTimeScale knob that slows the wave field
    # (water_wave_graph.WAVE_TIME_SCALE_PARAM carries the map): caustic
    # flicker rate is a separate perceptual quantity from swell speed, and
    # this parameter is where it is tuned.
    "CausticSpeed": 0.55,
    # pow() exponent on the interference sum: higher = thinner, brighter
    # filaments over a darker ground. 6 is the classic filament look; below
    # ~3 the pattern reads as blotches, not caustics.
    "CausticSharpness": 6.0,
    # The material-side kill switch (see the docstring). A parameter on every
    # consumer, default ON -- the runtime OFF authority is the MPC intensity
    # the engine cvar drives.
    "CausticsEnabled": 1.0,
    # The near-field window. Start is the plan's "~40 m -- measure", so both
    # are parameters and the measurement is an instance override, not a
    # regeneration.
    "CausticFadeStartM": 40.0,
    "CausticFadeEndM": 64.0,
    # Baked (not parameters -- one knob per axis, and the runtime knob for
    # "how much caustic" is the intensity pair above):
    "SunGateSinLo": 0.10,   # term is 0 below sin(alt) 0.10 (~5.7 deg)
    "SunGateSinHi": 0.25,   # full above 0.25 (~14.5 deg)
    "SubmergeRampM": 0.10,  # waterline fade-in depth
    "SunSecantFloor": 0.05, # 20x path cap -- build_slant_depth's clamp, reused
}

# Three panned, mutually-warped interference layers. Kept as HLSL for the
# water_sky_reflection_graph/GLINT_CODE reason: as checked node wiring this is
# ~35 nodes of sin/dot/add in which nothing says the warp went on the wrong
# layer; as HLSL it reads as the mathematics it is, and the BOUNDARY is still
# checked -- every input wired by name with a raise-on-failure connect.
#
# The direction constants are irrational-ish unit-vector pairs chosen so no
# two layers share a lattice; the frequencies are mutually non-integer
# multiples so the pattern's true period is far larger than anything a camera
# holds in frame (the anti-"repeating tile" property, cheaply).
CAUSTIC_CODE = """
// Three panned, mutually-warped interference layers -> one caustic intensity.
// PosM is ABSOLUTE world XY in metres; T is material time.
float2 p = PosM / max(ScaleM, 0.01);
float  t = T * Speed;
float2 d1 = float2( 0.8572,  0.5150);
float2 d2 = float2(-0.5878,  0.8090);
float2 d3 = float2( 0.1219, -0.9925);
// layer 1: the coarse cell structure
float a = sin(dot(p, d1) * 6.2832 + t) * sin(dot(p, d2) * 5.1100 - t * 0.87);
// layer 2, sampled on a domain layer 1 has displaced
float2 pw = p + 0.35 * float2(a, -a);
float b = sin(dot(pw, d2) * 8.1300 + t * 1.31) * sin(dot(pw, d1) * 7.0700 - t * 0.71);
// layer 3, displaced by both
float2 pw2 = pw + 0.27 * float2(b, b);
float c = sin(dot(pw2, d3) * 11.900 + t * 1.73) * sin(dot(pw2, d1) * 9.4000 + t * 0.53);
// interference sum -> [0,1] -> sharpened filaments
float v = saturate(0.5 + 0.5 * (a * 0.45 + b * 0.35 + c * 0.40));
return pow(v, max(Sharpness, 1.0));
"""


def _node(b, cls, x, y):
    # Explicit positions in a region (x -1300..-200, y 2400..3000) no consumer
    # currently occupies; the terrain generators re-flow their layout at save
    # anyway, so these only have to be distinct.
    return b.mel.create_material_expression(b.material, cls, x, y)


def _scalar_param(b, name, default, x, y):
    node = _node(b, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", float(default))
    return node


def _collection_param(b, collection, name, x, y):
    """Checked MPC binding against the collection AS READ BACK.

    Takes the collection explicitly rather than reading b.collection because
    this module's consumers do not share a builder class: the terrain
    generators hand a SkyGraphBuilder, M_Underwater hands its own labelled
    Graph (create_underwater_material.py:273, which deliberately is not a
    GraphBuilder), and the only surface this module may rely on is
    `.material` + `.mel`. Same raise-with-inventory discipline as every other
    binding in the toolchain.
    """
    have = [str(p.get_editor_property("parameter_name"))
            for p in collection.get_editor_property("scalar_parameters")]
    have += [str(p.get_editor_property("parameter_name"))
             for p in collection.get_editor_property("vector_parameters")]
    if name not in have:
        raise RuntimeError(
            "MPC_VoxelSky has no parameter %r -- it has %s. If %r was just added to "
            "create_sky_material.py's SCALAR_PARAMS (Phase F1 adds CausticIntensity), the "
            "collection on disk predates it: re-run create_sky_material.py and then the FULL "
            "dependent chain (tools/voxel-sky-chain-regen.ps1 -- the sky generator DELETES "
            "and recreates the asset, so every dependent must rebuild). An unresolved "
            "CollectionParameter compiles to a constant rather than failing, so this raise "
            "is the only thing between a stale MPC and silently dead caustics."
            % (name, sorted(have), name))
    node = _node(b, unreal.MaterialExpressionCollectionParameter, x, y)
    node.set_editor_property("collection", collection)
    node.set_editor_property("parameter_name", name)
    return node


def _link(b, src, src_out, dst, dst_in, what):
    if not b.mel.connect_material_expressions(src, src_out, dst, dst_in):
        raise RuntimeError(
            "connect %s (%s.%s -> %s.%s) failed"
            % (what, type(src).__name__, src_out or "<default>",
               type(dst).__name__, dst_in or "<default>"))


def build_caustics(b, collection, pos_m, time_expr, depth_m, defaults=None):
    """The whole caustic term. Returns a dict of expressions.

    Arguments:
      b           a builder exposing `.material` and `.mel` (SkyGraphBuilder
                  or M_Underwater's Graph -- see _collection_param for why the
                  bar is set this low on purpose).
      collection  the loaded MPC_VoxelSky object (checked bindings are made
                  against it as read back).
      pos_m       float2 expression, ABSOLUTE world XY in METRES at the shaded
                  point. The caller owns absoluteness -- see the module
                  docstring.
      time_expr   scalar time expression (a Time node, or a Constant on a
                  freeze arm).
      depth_m     scalar expression, metres of water above the shaded point;
                  <= 0 means dry and the term is zero there.
      defaults    optional dict merged over DEFAULTS, water_wave_graph style.

    Returns keys:
      light      float3 -- the finished additive term (see the chain in the
                 module docstring). Wire to the consumer's emissive/composite.
      field      float1 -- the raw sharpened interference field, for F3's
                 shafts or a debug view.
      sun_gate   the altitude ramp, in case a consumer wants to gate a
                 sibling term identically.
      transmit   the float3 depth attenuation.
    """
    d = dict(DEFAULTS)
    if defaults:
        d.update(defaults)

    # --- the field ----------------------------------------------------------
    scale = _scalar_param(b, "CausticScaleM", d["CausticScaleM"], -1300, 2400)
    speed = _scalar_param(b, "CausticSpeed", d["CausticSpeed"], -1300, 2460)
    sharp = _scalar_param(b, "CausticSharpness", d["CausticSharpness"], -1300, 2520)

    field = _node(b, unreal.MaterialExpressionCustom, -1100, 2440)
    field.set_editor_property("description", "CausticField")
    field.set_editor_property("code", CAUSTIC_CODE)
    field.set_editor_property("output_type",
                              unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    ins = []
    for nm in ("PosM", "T", "ScaleM", "Speed", "Sharpness"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", nm)
        ins.append(ci)
    field.set_editor_property("inputs", ins)
    _link(b, pos_m, "", field, "PosM", "pos_m -> CausticField.PosM")
    _link(b, time_expr, "", field, "T", "time -> CausticField.T")
    _link(b, scale, "", field, "ScaleM", "CausticScaleM -> CausticField.ScaleM")
    _link(b, speed, "", field, "Speed", "CausticSpeed -> CausticField.Speed")
    _link(b, sharp, "", field, "Sharpness", "CausticSharpness -> CausticField.Sharpness")

    # --- the sun gate, and the sun ray's Z for the secant below -------------
    sun_dir = _collection_param(b, collection, "SunDirection", -1300, 2600)
    sun_z = _node(b, unreal.MaterialExpressionComponentMask, -1150, 2600)
    sun_z.set_editor_property("r", False)
    sun_z.set_editor_property("g", False)
    sun_z.set_editor_property("b", True)
    sun_z.set_editor_property("a", False)
    _link(b, sun_dir, "", sun_z, "", "SunDirection -> z (sin altitude)")

    # saturate((sunZ - lo) * 1/(hi - lo)) -- the reciprocal is folded in
    # Python because lo/hi are baked (see DEFAULTS).
    gate_lo = _node(b, unreal.MaterialExpressionConstant, -1150, 2660)
    gate_lo.set_editor_property("r", float(d["SunGateSinLo"]))
    gate_num = _node(b, unreal.MaterialExpressionSubtract, -1000, 2620)
    _link(b, sun_z, "", gate_num, "A", "sunZ -> gate numerator A")
    _link(b, gate_lo, "", gate_num, "B", "gate floor -> gate numerator B")
    gate_scale = _node(b, unreal.MaterialExpressionConstant, -1000, 2680)
    gate_scale.set_editor_property(
        "r", 1.0 / (float(d["SunGateSinHi"]) - float(d["SunGateSinLo"])))
    gate_mul = _node(b, unreal.MaterialExpressionMultiply, -870, 2620)
    _link(b, gate_num, "", gate_mul, "A", "gate numerator -> mul A")
    _link(b, gate_scale, "", gate_mul, "B", "gate 1/band -> mul B")
    sun_gate = _node(b, unreal.MaterialExpressionSaturate, -760, 2620)
    _link(b, gate_mul, "", sun_gate, "", "gate -> saturate")

    # --- the submersion gate ------------------------------------------------
    sub_scale = _node(b, unreal.MaterialExpressionConstant, -1150, 2740)
    sub_scale.set_editor_property("r", 1.0 / float(d["SubmergeRampM"]))
    sub_mul = _node(b, unreal.MaterialExpressionMultiply, -1000, 2740)
    _link(b, depth_m, "", sub_mul, "A", "depth_m -> submerge ramp A")
    _link(b, sub_scale, "", sub_mul, "B", "1/SubmergeRampM -> submerge ramp B")
    submerged = _node(b, unreal.MaterialExpressionSaturate, -870, 2740)
    _link(b, sub_mul, "", submerged, "", "submerge ramp -> saturate")

    # --- the depth attenuation, down the SUN ray ----------------------------
    #
    # Path = depth / max(sunZ, floor): bathy_field_graph.build_slant_depth's
    # 2026-09-05 secant lesson applied to the sun's ray. The absorption RGB is
    # DERIVED from water_optics here, in Python, exactly as both water
    # generators derive theirs -- (-ln(0.02)/distance) * (1 - colour) -- and
    # baked as one Constant3Vector: this module adds no AbsorptionDistanceM
    # parameter of its own because the water above the floor is the authority
    # on the water's optics, and a second tunable copy here could be tuned
    # into disagreement with it (the water_optics docstring's whole argument).
    ax, ay, az = water_optics.ABSORPTION_COLOR
    k = _LN50 / float(water_optics.ABSORPTION_DISTANCE_M)
    absorb3 = _node(b, unreal.MaterialExpressionConstant3Vector, -1150, 2820)
    absorb3.set_editor_property("constant", unreal.LinearColor(
        k * (1.0 - float(ax)), k * (1.0 - float(ay)), k * (1.0 - float(az)), 1.0))

    secant_floor = _node(b, unreal.MaterialExpressionConstant, -1150, 2880)
    secant_floor.set_editor_property("r", float(d["SunSecantFloor"]))
    sun_z_floored = _node(b, unreal.MaterialExpressionMax, -1000, 2860)
    _link(b, sun_z, "", sun_z_floored, "A", "sunZ -> secant max A")
    _link(b, secant_floor, "", sun_z_floored, "B", "secant floor -> max B")
    sun_path_m = _node(b, unreal.MaterialExpressionDivide, -870, 2840)
    _link(b, depth_m, "", sun_path_m, "A", "depth_m -> sun path A")
    _link(b, sun_z_floored, "", sun_path_m, "B", "max(sunZ,floor) -> sun path B")

    optical = _node(b, unreal.MaterialExpressionMultiply, -740, 2840)
    _link(b, absorb3, "", optical, "A", "absorb RGB/m -> optical A")
    _link(b, sun_path_m, "", optical, "B", "sun path m -> optical B")
    neg1 = _node(b, unreal.MaterialExpressionConstant, -740, 2900)
    neg1.set_editor_property("r", -1.0)
    neg_optical = _node(b, unreal.MaterialExpressionMultiply, -620, 2860)
    _link(b, optical, "", neg_optical, "A", "optical -> negate A")
    _link(b, neg1, "", neg_optical, "B", "-1 -> negate B")
    transmit = _node(b, unreal.MaterialExpressionExponential, -510, 2860)
    _link(b, neg_optical, "", transmit, "", "-optical -> exp")

    # --- the distance fade --------------------------------------------------
    #
    # Its own WorldPosition/CameraPositionWS pair rather than a caller-supplied
    # distance: every consumer wants exactly this quantity and handing it in
    # would be three copies of one subtraction. LWC-safe for the reason
    # create_voxel_material.py's ring fade states (both operands are LWC-typed,
    # the compiler subtracts in emulated doubles).
    fade_start = _scalar_param(b, "CausticFadeStartM", d["CausticFadeStartM"], -1300, 2960)
    fade_end = _scalar_param(b, "CausticFadeEndM", d["CausticFadeEndM"], -1300, 3020)
    wp = _node(b, unreal.MaterialExpressionWorldPosition, -1150, 2960)
    cam = _node(b, unreal.MaterialExpressionCameraPositionWS, -1150, 3020)
    dist_uu = _node(b, unreal.MaterialExpressionDistance, -1000, 2980)
    _link(b, wp, "", dist_uu, "A", "world pos -> distance A")
    _link(b, cam, "", dist_uu, "B", "camera pos -> distance B")
    m_to_uu = _node(b, unreal.MaterialExpressionConstant, -1000, 3040)
    m_to_uu.set_editor_property("r", 100.0)
    start_uu = _node(b, unreal.MaterialExpressionMultiply, -870, 3000)
    _link(b, fade_start, "", start_uu, "A", "CausticFadeStartM -> UU A")
    _link(b, m_to_uu, "", start_uu, "B", "100 -> UU B")
    end_uu = _node(b, unreal.MaterialExpressionMultiply, -870, 3060)
    _link(b, fade_end, "", end_uu, "A", "CausticFadeEndM -> UU A")
    _link(b, m_to_uu, "", end_uu, "B", "100 -> UU B")
    fade_num = _node(b, unreal.MaterialExpressionSubtract, -740, 2980)
    _link(b, dist_uu, "", fade_num, "A", "distance -> fade numerator A")
    _link(b, start_uu, "", fade_num, "B", "start UU -> fade numerator B")
    fade_den = _node(b, unreal.MaterialExpressionSubtract, -740, 3040)
    _link(b, end_uu, "", fade_den, "A", "end UU -> fade denominator A")
    _link(b, start_uu, "", fade_den, "B", "start UU -> fade denominator B")
    fade_div = _node(b, unreal.MaterialExpressionDivide, -620, 3000)
    _link(b, fade_num, "", fade_div, "A", "fade numerator -> div A")
    _link(b, fade_den, "", fade_div, "B", "fade denominator -> div B")
    fade_sat = _node(b, unreal.MaterialExpressionSaturate, -510, 3000)
    _link(b, fade_div, "", fade_sat, "", "fade ratio -> saturate")
    fade = _node(b, unreal.MaterialExpressionOneMinus, -420, 3000)
    _link(b, fade_sat, "", fade, "", "1 - fade ratio")

    # --- the two intensities ------------------------------------------------
    intensity = _collection_param(b, collection, "CausticIntensity", -1300, 2680)
    enabled = _scalar_param(b, "CausticsEnabled", d["CausticsEnabled"], -1300, 2740)

    # --- the chain (see the module docstring for the whole product) ---------
    t1 = _node(b, unreal.MaterialExpressionMultiply, -600, 2560)
    _link(b, field, "", t1, "A", "field -> chain")
    _link(b, sun_gate, "", t1, "B", "sun gate -> chain")
    t2 = _node(b, unreal.MaterialExpressionMultiply, -500, 2600)
    _link(b, t1, "", t2, "A", "chain -> chain")
    _link(b, submerged, "", t2, "B", "submersion gate -> chain")
    t3 = _node(b, unreal.MaterialExpressionMultiply, -400, 2640)
    _link(b, t2, "", t3, "A", "chain -> chain")
    _link(b, fade, "", t3, "B", "distance fade -> chain")
    t4 = _node(b, unreal.MaterialExpressionMultiply, -300, 2680)
    _link(b, t3, "", t4, "A", "chain -> chain")
    _link(b, intensity, "", t4, "B", "CausticIntensity (MPC) -> chain")
    t5 = _node(b, unreal.MaterialExpressionMultiply, -220, 2720)
    _link(b, t4, "", t5, "A", "chain -> chain")
    _link(b, enabled, "", t5, "B", "CausticsEnabled -> chain")
    light = _node(b, unreal.MaterialExpressionMultiply, -140, 2780)
    _link(b, t5, "", light, "A", "chain -> light")
    _link(b, transmit, "", light, "B", "depth transmit RGB -> light")

    return {
        "light": light,
        "field": field,
        "sun_gate": sun_gate,
        "transmit": transmit,
    }
