"""The interactive RIPPLE FIELD, as material nodes -- shared by every consumer.

WHAT A "RIPPLE FIELD" IS, in one sentence: a small square of water surface that
follows the camera, on which a wave equation is simulated on the GPU, so that
things entering the water make rings that spread, bounce off the shore and die
away. It is a COSMETIC surface layer. It does not know about basins, volumes,
particles or the datum, and nothing downstream of it may start believing it does
-- `docs/water-architecture.md` §2 is emphatic that the bake owns where water is
and the scalar ledger owns how much of it there is.

THE SHAPE OF IT, and it is deliberately the same shape as the bathymetry field
(`bathy_field_graph.py`, `Source/VoxelEarth/VoxelBathyField.cpp`), because that
is this project's one solved instance of "get a camera-following world-space 2D
field into a material":

    /Game/Voxel/RT_VoxelRippleField -- 512 x 512, RGBA16f, one texel per 10 cm
    VOXEL, i.e. a 51.2 m square centred on the camera.

        R  dH/dx  surface gradient along world +X, dimensionless
        G  dH/dy  surface gradient along world +Y, dimensionless
        B  H      ripple height in METRES, signed, zero on still water
        A  DO NOT READ -- see the note in create_ripple_field_materials.py; the
           canvas draw path only guarantees the three emissive channels.

    RippleFieldOrigin   (MPC vector) world UU of the window's minimum corner, xy
    RippleFieldInvSize  (MPC scalar) 1 / (window width in UU), so the material's
                        UV is (WorldXY - Origin) * InvSize, one multiply-add
    RippleFieldGain     (MPC scalar) 0 disables the whole effect; 1 is shipped
                        strength. Written by UVoxelRippleFieldSubsystem, which
                        holds it at 0 until the first simulated frame exists.

WHY THE GRADIENT IS IN THE TEXTURE AND NOT DERIVED IN THE WATER MATERIAL. The
sim's state is a HEIGHT, and a normal needs its slope, so somebody has to
difference neighbouring texels. Doing it in the water material costs four taps
on every water pixel -- and water can be most of the screen. Doing it once, in a
512x512 pass, costs 262,144 pixels of work whatever the water covers, and leaves
the water material at ONE tap. The height stays in .B because the displacement
(World Position Offset) needs it and it is free to carry.

WHY AN ASSET AND NOT A DYNAMIC MATERIAL INSTANCE. Unchanged from
VoxelBathyField.h's "WHY AN ASSET" section, and the reason is still the far-field
lake sheet: VoxelWaterSheetActor.h:47-52 deliberately assigns the SHARED
/Game/Voxel/M_WaterVoxel to every section with no MID at all, so that near-field
and far-field water cannot diverge. A texture parameter that only a MID could set
would reach the near water and not the sheet -- and since 2026-08-11 the sheet is
what draws lakes at EVERY range, that is the wrong half.
"""

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# --- the asset contract, shared by the authoring script and every consumer ----

PACKAGE_PATH = "/Game/Voxel"

FIELD_TEXTURE = PACKAGE_PATH + "/RT_VoxelRippleField.RT_VoxelRippleField"
FIELD_TEXTURE_PARAM = "RippleFieldTex"

STATE_A_TEXTURE = PACKAGE_PATH + "/RT_VoxelRippleStateA.RT_VoxelRippleStateA"
STATE_B_TEXTURE = PACKAGE_PATH + "/RT_VoxelRippleStateB.RT_VoxelRippleStateB"

STEP_MATERIAL = PACKAGE_PATH + "/M_VoxelRippleStep.M_VoxelRippleStep"
DERIVE_MATERIAL = PACKAGE_PATH + "/M_VoxelRippleDerive.M_VoxelRippleDerive"

# How many disturbances one simulation step can inject, as material parameters
# Splat0..Splat7. MUST MATCH UVoxelRippleFieldSubsystem::kSplatSlots.
#
# WHY EIGHT AND WHY A FIXED NUMBER AT ALL. A material has no arrays, so each slot
# is a separate float4 parameter and the count is baked into the shader. Eight
# costs eight distance evaluations per texel -- 2.1 M operations across the field,
# which is noise next to the five texture taps the same pixel already does -- and
# at 60 steps a second it drains a queue at 480 disturbances per second. The
# player jumping in is one. A voxel volume breaking up on impact is a handful.
# Anything that could genuinely exceed this is a particle effect, not this.
STEP_SPLAT_SLOTS = 8

# MUST MATCH UVoxelRippleFieldSubsystem::kSize / kTexelUU
# (Source/VoxelEarth/VoxelRippleField.h). Nothing here can silently break the UV
# mapping if they drift -- the mapping arrives through the MPC at runtime, so
# these two are the DEFAULTS the materials are authored with and the numbers the
# comments quote. What they CAN break is the render-target size the authoring
# script creates, and the C++ side refuses to run against a render target of the
# wrong size rather than writing into it (the same guard VoxelBathyField.cpp:76
# applies to its texture).
FIELD_TEXELS = 512
TEXEL_UU = 10.0  # 10 cm, one voxel -- VoxelCoords::VoxelSizeUU
WINDOW_UU = FIELD_TEXELS * TEXEL_UU  # 5120 UU = 51.2 m across, +/-25.6 m

# THE STORAGE BIAS, and it is not paranoia.
#
# The simulation state is a signed height: a trough is a negative number. It is
# written to a render target through a material's EMISSIVE output, and this
# project cannot currently answer the question "does the canvas draw path
# preserve a negative emissive value into a float render target?" without an
# editor -- the 90-minute automated chain owns the only one on this box, and the
# answer differs by RHI and by engine version in the reports that exist.
#
# So the state is stored as h + STATE_BIAS and every reader subtracts it. If the
# answer turns out to be "yes, negatives survive", this becomes 0.0 in ONE place
# and both materials and the C++ clear colour follow -- see the mirrored constant
# at VoxelRippleField.h (kStateBias) which is the other half of the pair.
#
# WHAT IT COSTS, AND WHY THE STATE TARGETS ARE 32-BIT BECAUSE OF IT. At 16-bit
# float, the spacing between representable numbers near 0.5 is 2^-11 = 0.000488.
# The wave equation's whole update term is c^2*dt^2/dx^2 * laplacian(h), which at
# the shipped Courant number (0.267^2 = 0.0711) and a realistic laplacian of
# ~0.01 m is 7e-4 m -- the SAME ORDER as the quantisation step. The simulation
# would advance in ragged jumps, and it would look like a shader bug rather than
# a format choice. Biasing into fp32 (spacing 6e-8 near 0.5) costs 2 MB of video
# memory per state target and removes the question entirely.
STATE_BIAS = 0.5

# MPC_VoxelSky parameter names. create_sky_material.py is the sole author of that
# collection and these three must be in its SCALAR_PARAMS/VECTOR_PARAMS -- see
# the patch note in docs/water-interactive-ripples.md. SkyGraphBuilder.
# collection_param checks membership by name and RAISES, because an unresolved
# CollectionParameter compiles to a CONSTANT rather than failing
# (MaterialExpressions.cpp:17179-17193) and a constant origin would sample one
# fixed texel for the entire world.
MPC_ORIGIN = "RippleFieldOrigin"
MPC_INV_SIZE = "RippleFieldInvSize"
MPC_GAIN = "RippleFieldGain"

# Where the window's usable area ends, as a normalised distance from its centre
# (0.5 is the edge). Same two fractions bathy_field_graph.py uses, on a much
# smaller window: 0.42 is 21.5 m from the camera and 0.485 is 24.8 m, so the fade
# is 3.3 m wide.
#
# IT IS NOT REDUNDANT WITH THE SIMULATION'S OWN SPONGE LAYER, which already
# annihilates anything reaching the border (see create_ripple_field_materials.py,
# "THE SPONGE"). The sponge only acts on texels that have been STEPPED. A camera
# moving at 100 m/s drags 17 fresh texels per step into the window, and those
# have never been stepped at all -- this fade is what stops that band from being
# a visible edge in the water.
FADE_START = 0.42
FADE_END = 0.485

# THE FETCH ITSELF. Kept at module scope so the exact HLSL the water and the
# ocean run is greppable and diffable without reading a graph. The argument for
# hand-writing it at all is at the call site in sample_ripple_field.
#
# .rg = (dH/dx, dH/dy); .b = H in metres. .a is NOT read -- the canvas draw path
# only guarantees the three emissive channels (see the module docstring).
RIPPLE_FETCH_HLSL = """// The interactive ripple field, fetched at the coordinate the graph computed
// and nothing else. UV arrives as an ordinary float2 local; mip 0 is explicit,
// so there is no derivative chain and this is legal in the vertex shader too
// (height feeds World Position Offset). RGBA16f render target, 1 mip.
return Texture2DSampleLevel(RippleFieldTex, RippleFieldTexSampler, UV, 0);"""


def sample_ripple_field(b):
    """Sample the ripple field at this pixel's world XY. Returns a dict.

    `b` must be a SkyGraphBuilder (sky_star_graph.py) -- the MPC binding has to
    be name-checked, for the reason spelled out at MPC_ORIGIN above.

    Keys:
      grad_xy   float2, (dH/dx, dH/dy) in world XY, dimensionless, already
                faded out at the window edge and gained by RippleFieldGain
      height_m  float, ripple height in metres, same fade and gain
      uv        the float2 UV, for anything that wants to sample it again

    BOTH OUTPUTS ARE ZERO WHEN THERE IS NO FIELD, which is the property that
    lets a consumer add them unconditionally: RippleFieldGain defaults to 0.0 on
    the collection and stays 0 until UVoxelRippleFieldSubsystem has actually
    simulated a frame, so a run with the subsystem disabled, or with the render
    targets missing, renders EXACTLY the water it renders today.
    """
    origin = b.collection_param(MPC_ORIGIN)
    inv_size = b.collection_param(MPC_INV_SIZE)
    gain = b.collection_param(MPC_GAIN)

    # WPT_EXCLUDE_ALL_SHADER_OFFSETS, copied from the wave field's world_pos_abs
    # (create_water_voxel_material.py:2333-2336) and for its reason, which now
    # applies to this node too: the height this function returns feeds World
    # Position Offset, so reading a position that already contained this
    # material's own WPO would put the ripple into its own input. It is
    # numerically moot today -- every WPO term in M_WaterVoxel moves Z only and
    # this reads XY -- and it is still set, because the day someone makes a WPO
    # term touch X or Y is not the day anyone will remember to come back here.
    world_pos = b.node(unreal.MaterialExpressionWorldPosition)
    world_pos.set_editor_property(
        "world_position_shader_offset",
        unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)

    world_xy = b.mask(world_pos, "", r=True, g=True)
    origin_xy = b.mask(origin, "", r=True, g=True)
    uv = b.mul(b.sub(world_xy, origin_xy), inv_size)

    # --- THE FETCH IS HAND-WRITTEN HLSL, AND THAT IS THE WHOLE BUG FIX -------
    #
    # THE DEFECT (2026-09-07, eight instrumented frames, every one carrying a
    # must-fire control in its own pixels -- the table is in
    # docs/water-ocean-tides-plan-2026-09-04.md):
    #
    #   A MaterialExpressionTextureSampleParameter2D bound to
    #   /Game/Voxel/RT_VoxelRippleField returned the injected data when its UVs
    #   input was a CONSTANT (`constprobe`, VoxelVerify00954 -- the whole lake
    #   lights at the literal (0.615, 0.605)) or the SCREEN's uv (`fieldpic`,
    #   VoxelVerify00942 -- the five discs, right place, right 1.85:1 aspect),
    #   and returned NOTHING when its UVs input was the expression built two
    #   lines above -- whose per-pixel VALUE at those very pixels is pinned to
    #   (0.615, 0.605) by two 1.5 m-wide bands (`bandprobe`, VoxelVerify00952)
    #   and whose orientation, offset and scale are each separately measured
    #   (`uvpin`, VoxelVerify00946). Fresh sampler nodes, a plain WorldPosition
    #   in place of the no-offsets one, LOD forced to mip 0, and the LWC-safe
    #   camera-relative rewrite all reproduced the failure
    #   (`bothtap`/`mipprobe`/`fixprobe`, VoxelVerify00948/950/956).
    #
    # A sampler cannot return two answers for one coordinate. So the value the
    # ARITHMETIC produces and the value the compiled FETCH receives were not the
    # same value: the defect is in the texture-sample the MATERIAL COMPILER
    # EMITS for this parameter, not in anything this graph can be asked for.
    #
    # THE PROOF THAT THIS IS THE FIX, in ONE frame with the defect and the
    # control in the same pixels (`customtap`, VoxelVerify00960, shipping-pose
    # capture with five 8 m rings injected and frozen):
    #   R = this Custom-HLSL fetch at the uv below ......... 51.98% of the frame
    #   G = the ordinary sampler at THE SAME uv expression .  0.0000%
    #   B = the ordinary sampler at a CONSTANT uv .......... all water
    # One node apart, one frame, one set of pixels.
    #
    # WHY THIS FORM AND NOT ANOTHER. The Custom node's UV arrives as an ordinary
    # float2 local -- the same chunk the threshold instruments read, which is
    # exactly the value that was proven correct -- and the fetch is one line of
    # HLSL with no coordinate re-derivation, no derivative autogen and no LOD
    # chain of the compiler's choosing. Mip 0 is explicit: the render target has
    # a single mip (create_ripple_field_materials.py never asks for more), so
    # nothing is lost, and an explicit level is what makes this same expression
    # legal in the VERTEX shader -- which matters, because height_m below feeds
    # World Position Offset while grad_xy feeds the normal, and BOTH must come
    # off ONE read (see the module docstring's "ONE tap" argument).
    #
    # THE BINDING IS UNCHANGED. A MaterialExpressionTextureObjectParameter under
    # the SAME parameter name, with the same asset and the same sampler type, is
    # the same baked default the "WHY AN ASSET" section above requires -- the
    # far-field sheet still gets the field with no MID. The precedent for the
    # object -> Custom -> `<InputName>Sampler` convention in this project is
    # create_sunshadow_lf_material.py:106-131, which ships on it.
    #
    # SAMPLERTYPE_LINEAR_COLOR: these are metres and slopes, not colours, and a
    # colour sampler would apply a gamma curve to a signed height field. Same
    # argument, same sampler type, as the bathy texture.
    texture = unreal.load_object(None, FIELD_TEXTURE)
    if texture is None:
        raise RuntimeError(
            "failed to load %s -- run Tools/create_ripple_field_materials.py FIRST. It is "
            "the sole author of that render target, and without it this material would "
            "either fail to compile or silently default its texture parameter to whatever "
            "the engine picks, which would add an unrelated image to the water's normal "
            "on every pixel." % FIELD_TEXTURE)
    tex_obj = b.node(unreal.MaterialExpressionTextureObjectParameter)
    tex_obj.set_editor_property("parameter_name", FIELD_TEXTURE_PARAM)
    tex_obj.set_editor_property("texture", texture)
    tex_obj.set_editor_property(
        "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

    tex = b.node(unreal.MaterialExpressionCustom)
    tex.set_editor_property("description", "SampleRippleField")
    tex.set_editor_property("code", RIPPLE_FETCH_HLSL)
    tex.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    _inputs = []
    for _name in ("RippleFieldTex", "UV"):
        _ci = unreal.CustomInput()
        _ci.set_editor_property("input_name", _name)
        _inputs.append(_ci)
    tex.set_editor_property("inputs", _inputs)
    # b.link raises on a failed connect, which is the behaviour this needs: the
    # input NAMES are what the HLSL above reads, so a rename that misses one end
    # must fail loudly rather than compile against a stale local.
    b.link(tex_obj, "", tex, "RippleFieldTex")
    b.link(uv, "", tex, "UV")

    # Chebyshev distance from the window centre, so the fade follows the SQUARE
    # window rather than a circle inscribed in it -- bathy_field_graph.py:120-123
    # makes the same choice for the same reason (a circular fade throws away the
    # corners, which are a fifth of the area).
    half = b.const(0.5)
    centred = b.sub(uv, half)
    d = b.maximum(b.abs_(b.mask(centred, "", r=True)), b.abs_(b.mask(centred, "", g=True)))
    edge_fade = b.one_minus(b.ramp(d, "", b.const(FADE_START), b.const(FADE_END)))

    weight = b.mul(gain, edge_fade)

    # THE TERM THAT IS DELIBERATELY MISSING. Scaling a height field by a
    # spatially varying weight should, strictly, also add H * grad(weight) to the
    # gradient. It is left out. The fade runs over 3.3 m and the height it scales
    # is at most ~0.1 m, so the omitted term is bounded by 0.1/3.3 = 0.03 --
    # which is not negligible against a typical ripple gradient of 0.1-0.5, but
    # it only exists inside the fade band, where the whole contribution is on its
    # way to zero anyway. The wave field's patch term omits its own derivative
    # for the same kind of reason (create_water_voxel_material.py:2526-2529) and
    # quotes its own number; this is that number for this term.
    height_raw = b.mask(tex, "", b=True)
    grad_xy = b.mul(b.mask(tex, "", r=True, g=True), weight)
    height_m = b.mul(height_raw, weight)

    # height_raw / weight are the two HALVES of height_m, returned so an
    # instrument can ask which of them is zero without building a second
    # sampler that would not be the same fetch. Nothing shipping reads them --
    # a consumer that wants the ripple wants it gained and faded -- and adding
    # them changes not one node in the graph.
    return {
        "grad_xy": grad_xy,
        "height_m": height_m,
        "height_raw": height_raw,
        "weight": weight,
        "uv": uv,
    }


# F8/wake follow-up (owner, 2026-09-05 late: "Do we actually have a wake art
# effect...?"): the ripple field provably carried the boat's wake (debug-arm
# arcs on record) while the shipping composite spent it only on normal tilt
# and centimetre WPO -- imperceptible on dark water. DISTURBANCE FOAM is the
# missing visual channel: whitewater wherever the water is disturbed, driven
# by the field itself, so a moving boat trails a white wedge, rings read as
# white circles, and a splash flashes white and fades with the field's own
# decay -- no new state, no new timing, the sim already animates it.
#
# THE RESPONSE HAS A THRESHOLD, AND THE 2026-09-07 GREY-BLOB VERDICT IS WHY.
#
# The first shipped form was saturate((|grad| + |h| * 4) * 8): a straight line
# through the origin. Any texel above raw 0.125 -- 3 cm of ripple, or a slope
# of 1 in 8 -- pinned to FULL foam, and there was no value the field could hold
# that read as "barely disturbed". The boat leg that produced VoxelVerify00974
# logged its wake at max |grad| 0.19 and max |h| 0.044 m (`field verified LIVE
# -- centre patch max field value 0.1919, max state height 0.0436 m`), i.e.
# raw 0.36 at the hull, 2.9x past saturation before the gain had even done its
# work; and the sim's spread-out remainder -- millimetres of height across the
# whole 51 m window after eight seconds under way -- was ALSO past saturation.
# The owner's "hard grey blob ... a grey plane covering almost the entire
# world map" is that: a window-shaped mask of every texel the field had ever
# touched, cut off by the window's own 3.3 m edge fade. Whitewater is not a
# mask of "has the water moved"; it sits on the steep crests and nowhere else.
#
# So the response is now
#
#     x    = |grad| + |height_m| * HeightWeight
#     foam = saturate((x - Threshold) * Gain * Enabled)
#
# which is the same family the wind whitecaps already use (water_wave_graph.
# build_whitecap_foam: saturate((|gradient| - SlopeThresh) / (SlopeFull -
# SlopeThresh))): a dead band below Threshold, a linear knee above it. Texels
# the field has merely touched sit in the dead band and draw NOTHING, so the
# window edge is invisible by construction on undisturbed water -- the fade
# no longer has to hide anything and its width (FADE_START/FADE_END) is left
# alone, since widening it would also soften the ripple NORMAL for no reason.
#
# ALL FOUR NUMBERS ARE ScalarParameters, so the whole ladder runs on
# -VoxelWaterMatScalar=Name:Value[,Name:Value] against ONE regenerated asset
# (sheet material only -- the ocean takes the baked defaults). Read the
# sheet's "material scalar '<Name>' set to" echo for EVERY pair; a missing
# echo is a void arm, not a null (the 2026-09-07 06:00 first-pair-only trap).
DISTURBANCE_FOAM_DEFAULTS = {
    # Slope of the knee above the threshold: foam reaches 1.0 at
    # Threshold + 1/Gain. OWNER-DIRECTED 2026-09-08 after the live session
    # ("surface foam ... way too prevalent and spreads out in a circle
    # everywhere from the boat ... should only be near the wake and pretty
    # small"): threshold 0.05 -> 0.2, gain 6 -> 3, height weight 1 -> 0, so
    # only the crests (|grad| * RippleFieldGain > 0.2) carry foam and full
    # white needs 0.53. Ladder on the next launch as scalars.
    "DisturbanceFoamGain": 3.0,
    # Metres of ripple height that count like slope 1.0. Was 4.0 (a baked
    # constant); now a parameter, and 1.0 -- a wake is steep before it is
    # tall (the hull's 0.044 m is 0.044 of slope-equivalent against a
    # gradient of 0.19), so the gradient is the driver and the height is a
    # tie-breaker for a tall slow swell the gradient under-reads.
    "DisturbanceFoamHeightWeight": 0.0,
    # The dead band. Nothing below this raw value draws any foam at all.
    # 0.05 is a 1-in-20 slope or 5 cm of ripple with HeightWeight 1: above
    # the spread remainder of a wake (millimetres, slopes of ~0.01) and well
    # below its crests. PROVISIONAL, same ladder.
    "DisturbanceFoamThreshold": 0.2,
    # The arm's off switch, FoamV2Enabled-style: a pixel-identical off for
    # A/Bs without a regeneration. The real inert default is upstream --
    # RippleFieldGain 0 on an undriven collection zeroes the taps themselves.
    "DisturbanceFoamEnabled": 1.0,
}


def build_disturbance_foam(b, grad_xy, height_m, defaults=None):
    """Whitewater from the disturbance the ripple field is already carrying.

    `grad_xy` / `height_m` are the CONSUMER'S ripple tap expressions -- pass
    the WaveTimeScale-GATED ones (both waters gate the ripple contribution by
    that knob since the racing fix), so the foam and the displacement it
    explains are one channel: scale 0 stills the surface AND blanks the foam
    together, never one without the other. Under VOXEL_WATER_FREEZE_TIME the
    opposite holds and is correct: material time freezes but the ripple RT is
    the C++ sim's, so a frozen-arm capture shows the wake foam still moving
    -- the arm freezes the material's own animation, which this is not.

    Returns {"foam": expr, "raw": expr} -- max() `foam` into the existing foam
    composite (the lake's signal stack, the ocean's chain), per the standing
    max-not-add doctrine there: disturbed water breaking over an already-foamy
    crest is one patch of white, not two whites summed past 1. Riding the
    composite also buys the full foam contract for free: colour, opacity AND
    roughness move together, which is what makes the wedge read as whitewater
    rather than as paint. `raw` is the pre-threshold x, exposed for
    instruments only; nothing shipping reads it.
    """
    d = dict(DISTURBANCE_FOAM_DEFAULTS)
    if defaults:
        d.update(defaults)
    g2 = b.binary(unreal.MaterialExpressionDotProduct, grad_xy, "", grad_xy, "")
    gmag = b.unary(unreal.MaterialExpressionSquareRoot, g2)
    height_weight = b.scalar("DisturbanceFoamHeightWeight", d["DisturbanceFoamHeightWeight"])
    hterm = b.mul(b.abs_(height_m), height_weight)
    raw = b.add(gmag, hterm)
    threshold = b.scalar("DisturbanceFoamThreshold", d["DisturbanceFoamThreshold"])
    gain = b.scalar("DisturbanceFoamGain", d["DisturbanceFoamGain"])
    enabled = b.scalar("DisturbanceFoamEnabled", d["DisturbanceFoamEnabled"])
    knee = b.sub(raw, threshold)
    foam = b.saturate(b.mul(b.mul(knee, gain), enabled))
    return {"foam": foam, "raw": raw}
