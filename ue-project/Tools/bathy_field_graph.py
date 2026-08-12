"""The baked bathymetry field, as material nodes -- shared by water and terrain.

WHAT THIS SAMPLES. bake_ver 27 ships two int16 planes per fine tile: lake water
depth (10 mm units) and a SIGNED distance to the nearest shoreline (100 mm
units, positive inside water). UVoxelBathyFieldSubsystem (Source/VoxelEarth/
VoxelBathyField.cpp) copies a 512x512 camera-following window of both into a
single RGBA16F texture asset, /Game/Voxel/T_VoxelBathyInfo, one texel per
1.875 m source pixel, and publishes the window's placement through MPC_VoxelSky:

    R  water depth, METRES, 0 where dry
    G  signed distance to shore, METRES, + in water, - on land, +/-100 clamp
    B  validity, 1 where the bake answered, 0 where it did not
    A  reserved

    BathyFieldOrigin   world UU of the window's minimum corner (xy)
    BathyFieldInvSize  1 / (window width in UU)
    BathyFieldValid    1 once a window has been published this run

WHY IT IS A SHARED MODULE AND NOT COPIED INTO EACH MATERIAL. Three materials
read the field -- M_WaterVoxel (depth grading, shoreline foam, wave damping),
M_VoxelTerrain and M_VoxelClipmap (wet shores) -- and they must agree EXACTLY on
the UV mapping. A half-texel disagreement between the water and the ground it
meets puts the waterline in two different places, which is the one artefact this
whole feature exists to remove. Same argument sky_star_graph.py makes for the
star UV: the seam is only invisible while there is one implementation of it.

THREE WAYS THE FIELD IS ABSENT, AND ALL THREE MUST FALL BACK. A consumer that
honours only some of them draws unbaked water as infinitely shallow:

  1. NO WINDOW AT ALL. BathyFieldValid == 0: no fine tier in this run, or the
     texture asset failed the C++ size/format guard, or nothing has been
     published yet. This is a whole-material switch.
  2. NO DATA AT THIS CELL. The texture's B channel == 0: the owning tile is not
     resident, or the world was baked before bake_ver 27. Per pixel.
  3. OUTSIDE THE WINDOW. The window is only 960 m across. Past its edge the
     clamp address mode would smear the border texel across the rest of the
     world, so `validity` fades to 0 over the last ~7% of the window instead.

`sample_bathy_field` folds all three into ONE number, `validity`, and every
consumer multiplies its effect by it. There is no fourth thing to remember.
"""

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BATHY_TEXTURE = "/Game/Voxel/T_VoxelBathyInfo.T_VoxelBathyInfo"
BATHY_TEXTURE_PARAM = "BathyInfoTex"

# Must match UVoxelBathyFieldSubsystem::kSize / kTexelUU. Only used for comments
# and for the fade band below; the actual mapping comes from the MPC, so these
# two going stale cannot break the UV -- it can only make a comment wrong.
WINDOW_TEXELS = 512
TEXEL_UU = 187.5

# Where the window's usable area ends, in normalised distance from its centre
# (0.5 is the edge). The fade has to finish BEFORE 0.5 or the clamped border
# texel is visible as a band of whatever depth happened to be there.
#
# 0.42 -> 0.485 is about 12 m of fade at the window edge, 460 m from the camera.
# Wide enough not to be an edge, narrow enough not to eat useful reach.
FADE_START = 0.42
FADE_END = 0.485


def sample_bathy_field(b):
    """Sample the field at this pixel's world XY. Returns a dict of expressions.

    `b` must be a SkyGraphBuilder (or anything with GraphBuilder's algebra plus
    a CHECKED collection_param) -- the MPC binding has to be name-checked,
    because an unresolved CollectionParameter compiles to a CONSTANT rather than
    failing (MaterialExpressions.cpp:17179-17193). A silently-constant origin
    would sample one fixed texel for the entire world.

    Keys:
      depth_m    float, lake water depth in metres, 0 where dry
      shore_m    float, signed distance to shore in metres (+ water, - land)
      validity   float in [0,1], 0 where the field must not be used at all
      uv         the float2 UV, for anything that wants to sample it again
    """
    origin = b.collection_param("BathyFieldOrigin")
    inv_size = b.collection_param("BathyFieldInvSize")
    valid_flag = b.collection_param("BathyFieldValid")

    # WORLD XY, ABSOLUTE. MaterialExpressionWorldPosition defaults to the
    # absolute world position, which under LWC is a large-world value; the
    # subtract below is what the compiler resolves into ordinary float precision.
    #
    # The origin comes from the MPC as a float4 and is therefore only accurate to
    # about 1 UU at 100 km from the origin. That is 1 cm of field offset at the
    # far edge of a world this project never reaches -- the shipped world sits
    # within a few kilometres of the origin, where a float4 is exact to well
    # under a millimetre.
    world_pos = b.node(unreal.MaterialExpressionWorldPosition)
    world_xy = b.mask(world_pos, "", r=True, g=True)
    origin_xy = b.mask(origin, "", r=True, g=True)
    uv = b.mul(b.sub(world_xy, origin_xy), inv_size)

    # ONE SAMPLE, four channels. SAMPLERTYPE_LINEAR_COLOR because the texture is
    # PF_FloatRGBA with sRGB off: these are metres and a flag, not colours, and a
    # colour sampler would apply a gamma curve to them.
    tex = b.node(unreal.MaterialExpressionTextureSampleParameter2D)
    tex.set_editor_property("parameter_name", BATHY_TEXTURE_PARAM)
    texture = unreal.load_object(None, BATHY_TEXTURE)
    if texture is None:
        raise RuntimeError(
            "failed to load %s -- run Tools/create_bathy_info_texture.py FIRST. It is the "
            "sole author of that asset, and without it this material would either fail to "
            "compile or silently default its texture parameter to whatever the engine picks, "
            "which would shade every lake from an unrelated image." % BATHY_TEXTURE)
    tex.set_editor_property("texture", texture)
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    b.link(uv, "", tex, "UVs")

    # --- validity, all three absences folded together -----------------------
    #
    # Edge fade: Chebyshev distance from the window centre (max of the two axis
    # distances), so the fade follows the SQUARE window rather than a circle
    # inscribed in it -- a circular fade would throw away the corners, which are
    # a fifth of the area.
    half = b.const(0.5)
    centred = b.sub(uv, half)
    d = b.maximum(b.abs_(b.mask(centred, "", r=True)), b.abs_(b.mask(centred, "", g=True)))
    edge_fade = b.one_minus(b.ramp(d, "", b.const(FADE_START), b.const(FADE_END)))

    validity = b.mul(b.mul(b.mask(tex, "", b=True), valid_flag), edge_fade)

    return {
        "depth_m": b.mask(tex, "", r=True),
        "shore_m": b.mask(tex, "", g=True),
        "validity": validity,
        "uv": uv,
    }


def build_slant_depth(b, depth_m, camera_vector_z):
    """Turn the baked VERTICAL depth into the path length light actually travels.

    A baked vertical depth is NOT a Beer-Lambert path length, and using it as one
    makes water viewed at a grazing angle read wrongly transparent -- the whole
    lake goes pale as you lower the camera, which is the single most obvious tell
    that a depth term is fake. 0 A.D.'s water_high.fs is the usual reference here
    and divides by a hand-tuned clamped function of the view ray's vertical
    component:

        depth / (min(0.5, eyeVec.y) * 1.5 * min(0.5, eyeVec.y) * 2.0)

    WE DO NOT USE THAT CURVE, and it is worth saying why rather than just
    diverging. It is an unclamped 1/cos with an arbitrary floor bolted on, and
    the floor is doing a job that PHYSICS already does for free: the view ray
    REFRACTS at the water surface. Snell at IOR 1.33 bends a ray arriving at 90
    degrees from vertical down to 48.8 degrees inside the water, so the path
    through a slab of thickness d can never exceed d / cos(48.8) = 1.52 d. The
    elongation is self-limiting, no magic constant required:

        cos(theta_r) = sqrt(1 - (1 - vz^2) / n^2)        n = 1.33, n^2 = 1.7689
        slant        = d / cos(theta_r)

    which is bounded in [d, 1.52 d] for every possible view direction, is exactly
    1 looking straight down, and needs no clamp to stay finite. It is also
    consistent with what the engine itself does one layer down -- SLW refracts
    the view ray before evaluating its phase function
    (SingleLayerWaterShading.ush, WaterRefract).

    `camera_vector_z` is the Z component of CameraVector (pixel -> camera, unit
    length), so vz^2 is what the formula wants and the sign never matters.
    """
    vz2 = b.mul(camera_vector_z, camera_vector_z)
    one_minus_vz2 = b.sub(b.const(1.0), vz2)
    # 1 / n^2 for water. Named as a parameter so a stylised look can flatten the
    # grazing response without editing the graph, but the DEFAULT is the physical
    # value and anything above 1 makes the elongation unbounded again.
    inv_n2 = b.scalar("BathyRefractInvN2", 1.0 / 1.7689)
    cos_r = b.node(unreal.MaterialExpressionSquareRoot)
    b.link(b.saturate(b.sub(b.const(1.0), b.mul(one_minus_vz2, inv_n2))), "", cos_r, "")
    # max() and not a raw divide: saturate() above can reach exactly 0 only if
    # inv_n2 is pushed to 1, but a divide by zero in a shader is a NaN that
    # propagates into the frame, and one node is cheaper than that conversation.
    return b.div(depth_m, b.maximum(cos_r, b.const(0.05)))
