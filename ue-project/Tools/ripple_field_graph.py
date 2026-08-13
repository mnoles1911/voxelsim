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

    # SAMPLERTYPE_LINEAR_COLOR: these are metres and slopes, not colours, and a
    # colour sampler would apply a gamma curve to a signed height field. Same
    # argument, same sampler type, as the bathy texture.
    tex = b.node(unreal.MaterialExpressionTextureSampleParameter2D)
    tex.set_editor_property("parameter_name", FIELD_TEXTURE_PARAM)
    texture = unreal.load_object(None, FIELD_TEXTURE)
    if texture is None:
        raise RuntimeError(
            "failed to load %s -- run Tools/create_ripple_field_materials.py FIRST. It is "
            "the sole author of that render target, and without it this material would "
            "either fail to compile or silently default its texture parameter to whatever "
            "the engine picks, which would add an unrelated image to the water's normal "
            "on every pixel." % FIELD_TEXTURE)
    tex.set_editor_property("texture", texture)
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    b.link(uv, "", tex, "UVs")

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
    grad_xy = b.mul(b.mask(tex, "", r=True, g=True), weight)
    height_m = b.mul(b.mask(tex, "", b=True), weight)

    return {
        "grad_xy": grad_xy,
        "height_m": height_m,
        "uv": uv,
    }
