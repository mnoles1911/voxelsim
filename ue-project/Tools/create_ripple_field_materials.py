"""Author the interactive RIPPLE FIELD's render targets and its two step materials.

WHAT THE FEATURE IS, in the owner's words: "ripples in the water when the player
jumps in or other objects/entities/assets are in the water ... via a 2D mask that
is local to small region near the player or entity ... running a separate shader
simulation using only surface data."

WHAT THIS SCRIPT MAKES. Five assets, all under /Game/Voxel:

    RT_VoxelRippleStateA  512x512 RG32f   the wave equation's two time levels
    RT_VoxelRippleStateB  512x512 RG32f   the other half of the ping-pong
    RT_VoxelRippleField   512x512 RGBA16f what the water material samples
    M_VoxelRippleStep     one simulation step, A -> B or B -> A
    M_VoxelRippleDerive   heights -> (gradient, height), state -> field

`UVoxelRippleFieldSubsystem` (Source/VoxelEarth/VoxelRippleField.cpp) owns all
five at runtime: it creates one dynamic material instance per material, sets
their parameters, and draws them with
UKismetRenderingLibrary::DrawMaterialToRenderTarget. Read that file's header
comment for the simulation's parameters and the window's placement rules; this
file is only the shader half, and it deliberately states the same numbers only
where a default has to be typed.

=============================================================================
THE SIMULATION, AND WHY THIS ONE
=============================================================================

The classic explicit wave equation on a height field, two time levels:

    h(t+dt) = 2*h(t) - h(t-dt) + (c*dt/dx)^2 * laplacian(h(t))

with a 5-point laplacian, then attenuated by a per-step factor F that carries the
damping, the shore mask and the sponge together:

    h(t+dt) <- F * h(t+dt),   and the h(t) handed on as the next step's
    h(t-dt) <- F * h(t)       h(t-dt) is scaled by the SAME F

BOTH LEVELS, ALWAYS. Scaling only the newer one halves the realised decay rate,
because this scheme's velocity is h(t) - h(t-dt) and a level damped one time
fewer than its partner feeds amplitude straight back in. That was a live bug
until 2026-08-12 -- a configured 1.8 s half-life ran at 3.6 s -- and STEP_CODE's
"THE PER-STEP ATTENUATION" carries the algebra, the measurement and the reason
this form is exact rather than approximate.

It is three lines of arithmetic, it is what most
shipped games use for interactive water, and it is stable as long as the Courant
number C = c*dt/dx stays under 1/sqrt(2) = 0.7071 in two dimensions. The shipped
configuration runs C = 0.2667, a 2.65x margin -- see
VoxelRippleField.h's kFixedDt/kWaveSpeedMPS for how that number is arrived at and
for the clamp that stops a console variable from walking past it.

NOT FFT/Tessendorf, NOT a fluid solver. This is a cosmetic surface layer near the
camera. `docs/water-architecture.md` §1 is the authority on what actually
simulates water in this game (a GPU PBF solver, scalar hydrology as the
authority), and nothing here is allowed to grow toward that.

TWO PASSES, NOT ONE, AND WHY THE SECOND ONE EXISTS. The step above produces
HEIGHTS. A normal needs the SLOPE, which means differencing neighbouring texels,
and the question is only where that happens. In the water material it costs four
extra taps on every water pixel -- and since 2026-08-11 lake sheets draw lakes at
every range (docs/water-architecture.md §2), water can be most of the screen. In
a second 512x512 pass it costs 262,144 pixels once, whatever the water covers,
and leaves the water material at ONE tap. It also solves a second problem for
free: the ping-pong means the newest heights are in a target whose IDENTITY
alternates every step, and a material asset cannot follow that without a dynamic
instance -- which the far-field lake sheet deliberately does not have
(VoxelWaterSheetActor.h:47-52). The derive pass always writes the SAME target, so
the water material samples one fixed asset. That is the same argument
VoxelBathyField.h makes under "WHY AN ASSET".

=============================================================================
THE THREE FORMAT DECISIONS, EACH OF WHICH HAS A FAILURE MODE BEHIND IT
=============================================================================

1. THE STATE TARGETS ARE RG32f, NOT RG16f, AND THE STATE IS BIASED BY +0.5.
   Both halves of that are one decision and it is written up at
   ripple_field_graph.py's STATE_BIAS. Short form: a height field is SIGNED, this
   box cannot currently verify that a negative emissive value survives the canvas
   draw path into a float render target, and biasing into fp16 would put the
   quantisation step (0.000488 near 0.5) at the same magnitude as the wave
   equation's whole update term (~0.0007 m). fp32 removes that; 2 MB per target
   is the price.

2. TARGET GAMMA IS PINNED TO 1.0 AND READ BACK. A render target whose display
   gamma is not 1 has a pow() applied on write. Applied to a biased height field
   that is not a wrong colour, it is a wrong NUMBER, silently, in a feedback loop
   -- the next step reads it back in and the error compounds. The engine already
   returns 1.0 for float formats, so this line changes nothing today; it is here
   because the day someone switches a target to RGBA8 to save memory is the day
   that stops being true, and the read-back below is what will say so.

3. THE STATE TARGETS FILTER NEAREST, THE FIELD TARGET FILTERS BILINEAR. The
   simulation reads its own texels at exact centres and the window scrolls by
   WHOLE TEXELS (see "THE MOVING WINDOW" below), so nearest is not an
   approximation -- it is what makes a scroll lossless. Bilinear there would
   low-pass the entire field on every step, 60 times a second, and the ripples
   would visibly dissolve while the player walked. The FIELD target is read by
   the water material at arbitrary world positions and must be smooth, so it
   filters bilinear.

=============================================================================
THE MOVING WINDOW
=============================================================================

The field follows the camera, and unlike the bathymetry window it cannot be
refilled from anything -- its contents are simulated and exist nowhere else. So
the scroll is folded into the step itself: the step material reads its source at

    SrcUv = Uv + ShiftUv,   ShiftUv = (OriginNow - OriginPrev) / WindowSizeUU

which is free (an add on a UV that was being computed anyway). C++ SNAPS THE
WINDOW ORIGIN TO WHOLE TEXELS, so ShiftUv is always an exact multiple of 1/512
and a nearest tap reproduces the old texel exactly. A sub-texel shift would
resample the whole field through a filter every step, which is the "smear" this
avoids.

Anything scrolling in from outside the previous window has no history, and
reading it with clamp addressing would streak the border texel across the new
band. The step's `inWindow` guard writes flat water there instead.

=============================================================================
WHERE RIPPLES ARE ALLOWED
=============================================================================

The baked bathymetry (bake_ver 27) already says where water is and how far the
nearest shore is, to a decimetre, and this pass masks by it: the field is
multiplied every step by saturate(shore_m / ShoreMaskM), which is 0 on land, 0 at
the waterline and 1 once you are ShoreMaskM inside the water. That both keeps
ripples off dry ground and gives them a shore to die against.

THE SIGNED DISTANCE IS WHY THIS IS POSSIBLE AT 10 cm WHEN THE SOURCE IS 1.875 m.
bathy_field_graph.py:43-48 makes the point: the bake ships a signed distance
rather than a binary mask exactly so a consumer can find the zero crossing at
decimetre precision instead of on a 1.875 m raster step. A binary wet mask here
would give every lake a 1.875 m staircase of reflecting wall.

WHERE THERE IS NO BAKED FIELD, THERE IS NO MASK -- `lerp(1, wet, validity)`, the
same idiom and the same reason as the wave field's shore damping
(create_water_voxel_material.py:2614-2616): a run with no fine tier must look
like today's, not like a feature silently switched off. It costs nothing to
allow: the ripple field is only ever READ by the water material, so a ripple over
dry land is invisible whatever the sim believes.
"""

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ripple_field_graph import (  # noqa: E402
    FIELD_TEXELS,
    PACKAGE_PATH,
    STATE_A_TEXTURE,
    STATE_BIAS,
    STEP_SPLAT_SLOTS,
    TEXEL_UU,
    WINDOW_UU,
)
from terrain_material_common import GraphBuilder  # noqa: E402

# Object paths above are "/Game/Voxel/X.X"; the asset-tools calls below want the
# bare name and the package directory, so split once here rather than typing
# either half twice.
STATE_A_NAME = "RT_VoxelRippleStateA"
STATE_B_NAME = "RT_VoxelRippleStateB"
FIELD_NAME = "RT_VoxelRippleField"
STEP_NAME = "M_VoxelRippleStep"
DERIVE_NAME = "M_VoxelRippleDerive"

# --- the numbers the materials are AUTHORED with ----------------------------
#
# Every one of these is also written by C++ on every step, from the constants and
# console variables in VoxelRippleField.h. They are defaults, not the authority,
# and they exist so that opening the material in the editor shows something that
# behaves rather than a field of zeros. Where a default here and the C++ constant
# it mirrors disagree, C++ wins at runtime and the editor preview is wrong -- so
# they are quoted with their source.
DEFAULT_TEXEL_UV = 1.0 / float(FIELD_TEXELS)      # kSize
DEFAULT_COURANT_SQ = 0.2667 * 0.2667              # (kWaveSpeedMPS * kFixedDt / texel)^2
# 2^(-kFixedDt / kHalfLifeSec) at 1/60 s and 1.8 s. It is the REALISED amplitude
# factor per step, not its square root -- which it was not until 2026-08-12; see
# "THE PER-STEP ATTENUATION" in STEP_CODE for the recurrence, the measurement and
# the reason both time levels have to carry it.
DEFAULT_DAMP_PER_STEP = 0.99360
DEFAULT_SHORE_MASK_M = 0.25                       # kShoreMaskM
DEFAULT_TEXEL_M = TEXEL_UU / 100.0                # 0.1 m, the gradient's denominator

# THE SPONGE. The outer band of the window absorbs instead of reflecting.
#
# A wave equation with a hard edge REFLECTS off it, and a square reflecting wall
# 25.6 m from the camera would ring every disturbance back at the player as a
# box-shaped echo -- unmistakable and unphysical. The cheap standard answer is a
# sponge layer: multiply by slightly less than 1 in a band at the edge, so an
# outgoing wave is attenuated instead of turned around.
#
# 0.38 in normalised Chebyshev distance from the centre is 19.5 m out, so the
# band is 6.1 m wide. A wave crosses it in 6.1/1.6 = 3.8 s, which at 60 steps a
# second is 229 steps at an average factor of about 0.90 -- an attenuation of
# 3.3e-11. There is nothing left to reflect.
#
# THAT NUMBER WAS 5.8e-6 UNTIL 2026-08-12, because the sponge shared the damping
# bug: it multiplied only h(t+dt), so its realised per-step factor was the square
# root of the one written here (0.9487 where this band's average says 0.90).
# Nothing visibly reflected either way -- five orders of margin is still five
# orders -- which is exactly why it had to be found by simulating the recurrence
# instead of by looking at the water. See "THE PER-STEP ATTENUATION" in
# STEP_CODE; the fix is shared with the damping and these two numbers are now
# the ones the shader actually applies.
DEFAULT_SPONGE_START = 0.38
DEFAULT_SPONGE_FLOOR = 0.80

# Ceiling on |h|, in metres, applied every step. NOT a tuning knob -- a
# runaway is the failure mode of every explicit integrator, and a render target
# that has gone to inf or NaN STAYS that way: nothing ever overwrites it with
# anything but a function of itself, so one bad frame poisons the field until the
# world is torn down. 2 m is ~20x the largest disturbance this system is designed
# to inject, so it can only ever engage on a genuine divergence.
DEFAULT_HEIGHT_CEILING_M = 2.0


def enum_member(enum_type_name, candidates, why):
    """Return (name, value) for the first candidate that exists on the enum.

    Same helper, same reasoning, as create_underwater_material.py:245-270 -- it
    is reimplemented rather than imported because importing it would execute that
    module's body (which loads water_optics and a pile of post-process constants)
    as a side effect of asking for twelve lines. The reasoning it carries is the
    part that matters: raise with the FULL list of what the enum actually has, so
    the next person hits it on an engine upgrade and can pick the new spelling
    without opening an interactive console, and NEVER silently accept the engine
    default.
    """
    enum_type = getattr(unreal, enum_type_name, None)
    if enum_type is None:
        raise RuntimeError(
            "unreal.%s does not exist on this engine build's Python bindings. %s "
            "Candidates this script would have accepted: %s."
            % (enum_type_name, why, candidates))
    available = sorted(n for n in dir(enum_type) if not n.startswith("_") and n == n.upper())
    for name in candidates:
        value = getattr(enum_type, name, None)
        if value is not None:
            return name, value
    raise RuntimeError(
        "none of %s exist on unreal.%s -- it has %s. %s Pick the member that matches and add "
        "it to the candidate list at the top of this file; do NOT silently accept the engine "
        "default, which is the failure this check exists to prevent."
        % (candidates, enum_type_name, available, why))


def make_render_target(name, format_candidates, filter_candidates, clear_color, why):
    """Author one UTextureRenderTarget2D and VERIFY every property took.

    Everything here is read back. A render target that quietly came out 256x256
    RGBA8 -- the factory's default -- would still draw, still sample, and still
    produce a picture: a quarter-resolution, 8-bit, gamma-encoded picture of a
    signed height field, i.e. flat water with faint blocky artefacts. That is a
    failure that looks like a tuning problem, which is the class of failure this
    project's generators exist to convert into a raise.
    """
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = PACKAGE_PATH + "/" + name

    # DELETE, THEN VERIFY THE DELETE. The first re-run of this script failed and
    # the error pointed at the wrong thing.
    #
    # The original code called delete_asset and moved on. When the delete does
    # not take, create_asset returns None and the raise below reads "Failed to
    # create ...", which looks like a factory or an RHI problem -- and cost a
    # round trip chasing exactly that. The engine's real explanation sits one
    # line earlier in the log and is easy to miss:
    #
    #   LogAssetTools: Error: The asset 'RT_VoxelRippleStateA' already exists in
    #   package '/Game/Voxel/RT_VoxelRippleStateA'. CanCreateAsset cannot ask the
    #   user as the application is running unattended and will return false.
    #
    # So this script was not idempotent: it worked on a clean tree and failed on
    # every re-run. That is the worst way round -- the first run of a new
    # generator is the one somebody watches, and the re-runs are the ones fired
    # and forgotten.
    #
    # Both delete calls return a bool that the original discarded, and a plain
    # delete_asset declines QUIETLY when the package is still loaded or
    # referenced, which is why the retry evicts the loaded object.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        if not unreal.EditorAssetLibrary.delete_asset(path):
            loaded = unreal.EditorAssetLibrary.load_asset(path)
            if loaded is not None:
                unreal.EditorAssetLibrary.delete_loaded_asset(loaded)
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            raise RuntimeError(
                "%s already exists and could NOT be deleted, so it cannot be recreated. "
                "Whatever is reported below about creation failing is a consequence of "
                "this, not a factory or RHI fault. Close any editor holding it, or delete "
                "ue-project/Content/Voxel/%s.uasset by hand, then re-run." % (path, name))

    factory_cls = getattr(unreal, "TextureRenderTargetFactoryNew", None)
    if factory_cls is None:
        raise RuntimeError(
            "unreal.TextureRenderTargetFactoryNew does not exist on this engine build's "
            "Python bindings, so this script cannot create %s. Check the exposed factory "
            "name in an interactive editor console; there is no fallback, because a render "
            "target created by any other route would not be an ASSET and the water material "
            "could not name it." % path)

    rt = asset_tools.create_asset(name, PACKAGE_PATH, unreal.TextureRenderTarget2D, factory_cls())
    if rt is None:
        raise RuntimeError("Failed to create render target asset at " + path)

    fmt_name, fmt_value = enum_member("TextureRenderTargetFormat", format_candidates, why)
    filt_name, filt_value = enum_member(
        "TextureFilter", filter_candidates,
        "The filter decides whether a scroll is lossless (nearest) or a low-pass (bilinear); "
        "see this file's format section item 3.")
    addr_name, addr_value = enum_member(
        "TextureAddress", ["TA_CLAMP"],
        "Wrap addressing on a world-space window would make a disturbance at one edge appear "
        "at the opposite edge, 51.2 m away.")

    rt.set_editor_property("size_x", FIELD_TEXELS)
    rt.set_editor_property("size_y", FIELD_TEXELS)
    rt.set_editor_property("render_target_format", fmt_value)
    rt.set_editor_property("clear_color", clear_color)
    rt.set_editor_property("filter", filt_value)
    rt.set_editor_property("address_x", addr_value)
    rt.set_editor_property("address_y", addr_value)
    # See the module docstring, format decision 2. This is 1.0 today for every
    # float format; it is set and read back so it stays 1.0 tomorrow.
    rt.set_editor_property("target_gamma", 1.0)

    got_x = int(rt.get_editor_property("size_x"))
    got_y = int(rt.get_editor_property("size_y"))
    got_fmt = rt.get_editor_property("render_target_format")
    got_gamma = float(rt.get_editor_property("target_gamma"))
    if got_x != FIELD_TEXELS or got_y != FIELD_TEXELS:
        raise RuntimeError(
            "%s came out %dx%d, not %dx%d. UVoxelRippleFieldSubsystem refuses to run against a "
            "render target of the wrong size (the same guard VoxelBathyField.cpp:76 applies to "
            "its texture), so this would disable the whole feature with one log line."
            % (path, got_x, got_y, FIELD_TEXELS, FIELD_TEXELS))
    if got_fmt != fmt_value:
        raise RuntimeError(
            "%s came out format %r, not %s. %s" % (path, got_fmt, fmt_name, why))
    if abs(got_gamma - 1.0) > 1e-6:
        raise RuntimeError(
            "%s has target_gamma %.6f, not 1.0. A gamma curve applied to a signed, biased "
            "height field inside a feedback loop compounds every step." % (path, got_gamma))

    unreal.EditorAssetLibrary.save_loaded_asset(rt)
    unreal.log("RippleField: %s = %dx%d %s, filter %s, %s, gamma 1.0, clear (%.3f %.3f %.3f %.3f)"
               % (path, got_x, got_y, fmt_name, filt_name, addr_name,
                  clear_color.r, clear_color.g, clear_color.b, clear_color.a))
    return rt


class RippleGraph(GraphBuilder):
    """GraphBuilder plus the four nodes these two graphs need.

    GraphBuilder rather than a fourth private copy of the same forty lines: these
    graphs are small, share no subgraph with the sky, and need no MPC binding
    (every value arrives through a dynamic material instance from C++), so
    SkyGraphBuilder's collection_param -- the reason the water material uses it --
    buys nothing here.
    """

    def texcoord(self):
        return self.node(unreal.MaterialExpressionTextureCoordinate)

    def mask(self, src, src_out, r=False, g=False, b=False, a=False):
        # GraphBuilder has no mask(); SkyGraphBuilder's is the one every other
        # graph in this directory uses, and this is a copy of it rather than a
        # dependency on the sky module, which these materials otherwise do not
        # need.
        n = self.node(unreal.MaterialExpressionComponentMask)
        n.set_editor_property("r", r)
        n.set_editor_property("g", g)
        n.set_editor_property("b", b)
        n.set_editor_property("a", a)
        self.link(src, src_out, n, "")
        return n

    def const2(self, x, y):
        n = self.node(unreal.MaterialExpressionConstant2Vector)
        n.set_editor_property("r", float(x))
        n.set_editor_property("g", float(y))
        return n

    def vector_param(self, name, r, g, b, a):
        n = self.node(unreal.MaterialExpressionVectorParameter)
        n.set_editor_property("parameter_name", name)
        n.set_editor_property("default_value", unreal.LinearColor(r, g, b, a))
        return n

    def vector_param4(self, name, r, g, b, a):
        """A VectorParameter reassembled into a genuine float4.

        WHY THIS IS NOT JUST vector_param. A MaterialExpressionVectorParameter's
        DEFAULT (unnamed) output is RGB -- three components. The fourth is
        reachable only as the separate "A" pin. Wire the default output into a
        Custom node input and the generated HLSL declares that input `float3`,
        silently, and nothing in the material editor says so.

        That cost a full chain run. The step material's splat slots carry
        (x, y, radius, strength) and the HLSL reads .xy, .z AND .w, so it
        declared `float4 SP[8] = { Splat0 ... Splat7 }` and the shader compiler
        refused with:

            /Engine/Generated/Material.ush: error: too few elements in vector
            initialization (expected 32 elements, have 24)

        32 is 8 slots x 4 components; 24 is 8 x 3. The arithmetic is the whole
        diagnosis, and it only appears at SHADER COMPILE time -- the generator
        itself succeeds, saves, and reports success, and M_VoxelRippleStep then
        draws as the engine default material.

        AppendVector(RGB, A) rebuilds the float4 the parameter always held. Two
        extra nodes per slot, folded to nothing by the compiler.
        """
        p = self.vector_param(name, r, g, b, a)
        rgb = self.mask(p, "", r=True, g=True, b=True)
        app = self.node(unreal.MaterialExpressionAppendVector)
        self.link(rgb, "", app, "A")
        self.link(p, "A", app, "B")
        return app

    def rt_sample(self, param_name, render_target_path, uv, uv_out=""):
        """A TextureSampleParameter2D bound to a render target asset.

        SAMPLERTYPE_LINEAR_COLOR: the channels are metres and slopes, not
        colours, and a colour sampler would put a gamma curve on them. The
        DEFAULT texture matters as much as the parameter name -- C++ overrides it
        on the dynamic instance every step for the ping-pong, but a material
        whose texture parameter defaults to nothing samples whatever the engine
        picks, which is how a step material ends up simulating a UI icon.
        """
        n = self.node(unreal.MaterialExpressionTextureSampleParameter2D)
        n.set_editor_property("parameter_name", param_name)
        target = unreal.load_object(None, render_target_path)
        if target is None:
            raise RuntimeError(
                "failed to load %s -- the render targets must be created before the materials "
                "that sample them, in this same script, in main()'s order." % render_target_path)
        n.set_editor_property("texture", target)
        n.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        self.link(uv, uv_out, n, "UVs")
        return n

    def custom(self, name, code, inputs, output_type):
        """A MaterialExpressionCustom with named inputs and no wiring yet.

        Identical in shape to create_water_voxel_material.py:692-713, including
        why it exists at all: the step below contains a loop over eight
        disturbance slots and a boundary test, which as material nodes would be
        roughly a hundred hand-wired connects in which nothing tells you the
        third splat went into the fourth slot. The BOUNDARY stays checked -- every
        input is wired with the same raise-on-failure connect as everything else,
        and the input NAMES are what the HLSL reads, so a rename that misses one
        end fails to compile loudly rather than reading a stale value.
        """
        n = self.node(unreal.MaterialExpressionCustom)
        n.set_editor_property("description", name)
        n.set_editor_property("code", code)
        n.set_editor_property("output_type", output_type)
        ins = []
        for nm in inputs:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", nm)
            ins.append(ci)
        n.set_editor_property("inputs", ins)
        return n


def finish_material(material, name):
    """Unlit + Opaque, set AFTER the graph, then verified, recompiled and saved.

    AFTER THE GRAPH, and that ordering is create_water_voxel_material.py:642-654's
    -- every set_editor_property fires PostEditChangeProperty, which recompiles a
    material that at that moment has no graph at all, and this project's release
    rule is to grep every run log for "Failed to compile Material" before
    trusting any visual result. A generator that emits that string on a
    successful run destroys the check.

    UNLIT is not cosmetic here. These materials are drawn to a render target
    through a canvas; their emissive output IS the simulation state. A lit
    shading model would run the whole lighting chain over that state and write
    something else.
    """
    shading_name, shading_value = enum_member(
        "MaterialShadingModel", ["MSM_UNLIT"],
        "These materials' emissive output IS numeric state written to a render target.")
    blend_name, blend_value = enum_member(
        "BlendMode", ["BLEND_OPAQUE"],
        "The step overwrites the whole target every draw; any blend mode that reads the "
        "destination would mix the target it is writing with the target it read.")
    material.set_editor_property("shading_model", shading_value)
    material.set_editor_property("blend_mode", blend_value)

    got_shading = material.get_editor_property("shading_model")
    got_blend = material.get_editor_property("blend_mode")
    if got_shading != shading_value:
        raise RuntimeError("%s shading model did not take: %r, expected %s"
                           % (name, got_shading, shading_name))
    if got_blend != blend_value:
        raise RuntimeError("%s blend mode did not take: %r, expected %s"
                           % (name, got_blend, blend_name))

    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("RippleField: %s built (%s, %s)" % (name, shading_name, blend_name))


def new_material(name):
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = PACKAGE_PATH + "/" + name
    # DELETE, THEN VERIFY THE DELETE. The first re-run of this script failed and
    # the error pointed at the wrong thing.
    #
    # The original code called delete_asset and moved on. When the delete does
    # not take, create_asset returns None and the raise below reads "Failed to
    # create ...", which looks like a factory or an RHI problem -- and cost a
    # round trip chasing exactly that. The engine's real explanation sits one
    # line earlier in the log and is easy to miss:
    #
    #   LogAssetTools: Error: The asset 'RT_VoxelRippleStateA' already exists in
    #   package '/Game/Voxel/RT_VoxelRippleStateA'. CanCreateAsset cannot ask the
    #   user as the application is running unattended and will return false.
    #
    # So this script was not idempotent: it worked on a clean tree and failed on
    # every re-run. That is the worst way round -- the first run of a new
    # generator is the one somebody watches, and the re-runs are the ones fired
    # and forgotten.
    #
    # Both delete calls return a bool that the original discarded, and a plain
    # delete_asset declines QUIETLY when the package is still loaded or
    # referenced, which is why the retry evicts the loaded object.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        if not unreal.EditorAssetLibrary.delete_asset(path):
            loaded = unreal.EditorAssetLibrary.load_asset(path)
            if loaded is not None:
                unreal.EditorAssetLibrary.delete_loaded_asset(loaded)
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            raise RuntimeError(
                "%s already exists and could NOT be deleted, so it cannot be recreated. "
                "Whatever is reported below about creation failing is a consequence of "
                "this, not a factory or RHI fault. Close any editor holding it, or delete "
                "ue-project/Content/Voxel/%s.uasset by hand, then re-run." % (path, name))
    material = asset_tools.create_asset(name, PACKAGE_PATH, unreal.Material,
                                        unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("Failed to create material asset at " + path)
    return material


# ============================================================================
# THE STEP
# ============================================================================
#
# BIAS is written into the HLSL from ripple_field_graph.STATE_BIAS rather than
# typed, so the encode here and the decode in the derive pass cannot drift.
STEP_CODE = """
// One wave-equation step. In: two time levels of a biased height field, plus up
// to %(slots)d disturbances. Out: float3(h_next + bias, h_curr + bias, 0).
//
// NO IDENTIFIER HERE MAY BE A COMMON SHADER MACRO. A Custom node's code is
// pasted into a translation unit that has already included the engine's
// Common.ush, which #defines PI -- so `const float PI = 3.14` expands to
// `const float 3.1415926535897932 = 3.14` and fails to compile with an error
// that names neither this node nor the macro. Hence StateBias, kPi, HC/HL/...
// rather than the shorter names this would otherwise want.
const float StateBias = %(bias).6f;
const float kPi = 3.14159265;

// THE SCROLL GUARD. SrcUv is this texel's position in the PREVIOUS window. If
// the window moved, some texels along the trailing edge were outside it and have
// no history; clamp addressing would have handed us the border texel instead, so
// a fast pan would streak one column of water across the new band. Flat water is
// the only honest answer for a patch of world the simulation has never seen.
float inWindow = (SrcUv.x >= 0.0 && SrcUv.x <= 1.0 && SrcUv.y >= 0.0 && SrcUv.y <= 1.0)
               ? 1.0 : 0.0;

float hc = (HC.x - StateBias) * inWindow;   // h(t)    at this texel
float hp = (HC.y - StateBias) * inWindow;   // h(t-dt) at this texel
float hl = (HL - StateBias) * inWindow;
float hr = (HR - StateBias) * inWindow;
float hd = (HD - StateBias) * inWindow;
float hu = (HU - StateBias) * inWindow;

// 5-point laplacian, in TEXELS. The physical cell size is folded into Courant2
// (= (c*dt/dx)^2), which is the only form in which dx appears anywhere -- so the
// stability condition is readable directly off that one number.
float lap = (hl + hr + hd + hu) - 4.0 * hc;
float hn  = 2.0 * hc - hp + Courant2 * lap;

// --- DISTURBANCES --------------------------------------------------------
//
// Each slot is (uv.x, uv.y, radius in UV, strength in metres); strength 0 is an
// empty slot and costs one multiply. C++ fills them from AddDisturbance().
//
// A RAISED COSINE, not a step and not a linear cone: its VALUE and its SLOPE
// both reach zero at the edge. A shape with a kink in it injects energy at every
// spatial frequency the grid can carry, including the two-texel checkerboard,
// which does not propagate -- it sits at the impact point and sparkles.
//
// ADDED TO h(t) ONLY, deliberately leaving h(t-dt) alone. That makes the
// disturbance a displacement AND an upward velocity in the same gesture (the
// scheme's velocity is h(t) - h(t-dt)), so it collapses into an outgoing ring
// within a few steps instead of sitting there and sagging.
float4 SP[%(slots)d] = { %(slotlist)s };
[unroll]
for (int i = 0; i < %(slots)d; ++i)
{
    float2 dxy = Uv - SP[i].xy;
    float  r   = length(dxy) / max(SP[i].z, 1e-6);
    float  bump = (r < 1.0) ? (0.5 * (1.0 + cos(kPi * r))) : 0.0;
    hn += bump * SP[i].w;
}

// --- WHERE RIPPLES ARE ALLOWED -------------------------------------------
//
// ShoreM is the baked SIGNED distance to the nearest shoreline in metres,
// positive in water. Valid is 0 where the bake had no answer, and where there is
// no answer there is no mask -- see the module docstring.
float wet  = saturate(ShoreM / max(ShoreMaskM, 0.01));
float mask = lerp(1.0, wet, saturate(Valid));
mask = lerp(1.0, mask, saturate(MaskEnable));

// --- THE SPONGE ----------------------------------------------------------
float2 cc    = abs(Uv - 0.5);
float  cheb  = max(cc.x, cc.y);
float  t     = saturate((cheb - SpongeStart) / max(0.5 - SpongeStart, 1e-4));
float  sponge = lerp(1.0, SpongeFloor, t);

// --- THE PER-STEP ATTENUATION, AND IT MULTIPLIES BOTH TIME LEVELS --------
//
// One factor, applied identically to the h(t+dt) we write and to the h(t) we
// hand on as the next step's h(t-dt). That symmetry is not tidiness; it is the
// difference between the configured half-life and twice the configured
// half-life.
//
// THE BUG THIS REPLACES, found 2026-08-12 by simulating the recurrence rather
// than by looking at water. Until today Damp multiplied hn ALONE while the h(t)
// written to .g carried the mask and nothing else, so the recurrence was
//
//     A(n+1) = D * (2*A(n) - A(n-1) + Courant2 * lap A(n))
//
// whose characteristic polynomial z^2 - D*(2 - Courant2*lambda)*z + D has root
// PRODUCT D. An oscillatory mode therefore decayed as sqrt(D) per step, not D.
// Measured on a 1-D ring at the shipped numbers: HalfLifeSec 1.8 came out at
// 3.626 s (per-step 0.996819, against the analytic sqrt(0.993603) = 0.996796
// and 3.600 s), and a splash was still at 0.35 of its amplitude after 6 s where
// voxel.Water.Ripple.HalfLifeSec's help text promises about a tenth. Same ring
// with this line: 1.844 s and 0.100 left at 6 s -- the residual 2 hundredths of
// a second is the measurement's one-period envelope window, not the scheme.
//
// WHY THE MISMATCH DOUBLED IT, in words: this scheme's VELOCITY is
// h(t) - h(t-dt). Shrinking only the newer level shrinks the displacement while
// leaving the older level, and therefore the implied velocity, at its undamped
// size -- so every step handed back part of the amplitude it had just taken.
//
// WHY SCALING BOTH IS EXACT rather than merely closer. Substituting
// h(n) = F^n * u(n) into the two-level recurrence with F on both levels turns it
// into the UNDAMPED scheme in u, exactly. The amplitude is then F^n for every
// mode there is -- travelling, standing, the uniform offset, the two-texel
// checkerboard -- with no change to wave speed and no change to dispersion.
//
// THE OTHER FIX AVAILABLE, and why it was not taken. Damping hn alone but with
// 2^(-2*dt/HalfLife) also lands the oscillatory modes on the right decay and is
// a one-line change on the C++ side. Rejected for two reasons:
//
//   1. IT FIXES ONLY THE DAMPING. The sponge below has the identical structure
//      and had the identical bug -- it multiplied hn alone, so its floor of
//      0.80 per step was really sqrt(0.80) = 0.8944, and the 6.1 m band's
//      advertised 1e-11 attenuation per crossing (see THE SPONGE in this file's
//      defaults) was really 5.8e-6. Five orders of margin, so nothing visibly
//      reflected and nobody would have found it from a screenshot -- and still
//      a second live instance of the same bug, sitting in the boundary
//      condition. Scaling both levels repairs it in the same line.
//   2. IT IS EXACT ONLY WHERE THE ROOTS ARE COMPLEX. sqrt(D^2) = D needs the two
//      roots to share a magnitude. At lambda = 0 -- a uniform offset across the
//      whole window, which is what a scroll guard or a clear leaves behind --
//      they do not: asymmetric damping turns z^2 - 2*D*z + D into a complex
//      pair, i.e. it makes a flat offset OSCILLATE instead of decaying.
//      Symmetric damping gives (z - F)^2, a real double root, and it just sinks.
//
// The mask was ALREADY on both levels and was therefore already right; all this
// line does is give Damp and the sponge the treatment the mask had.
float AttenPerStep = Damp * mask * sponge;
hn = hn * AttenPerStep;

// THE POISON GUARD, AND IT IS min/max RATHER THAN clamp() ON PURPOSE. An
// explicit integrator that diverges writes inf, and the NEXT step reads that
// back and multiplies it -- so one bad frame is permanent for the lifetime of
// the world, and no amount of staring at the material will show it, because the
// material is fine. clamp() has no defined behaviour on a NaN. D3D's min/max DO:
// they return the non-NaN operand, so this pair maps inf and NaN alike onto a
// finite number and the field heals itself on the next step. The redundant
// comparison after it is belt and braces and may legally be optimised away under
// fast math -- the min/max is the half that carries the guarantee.
hn = min(max(hn, -Ceiling), Ceiling);
if (!(hn == hn)) { hn = 0.0; }

// h(t) becomes h(t-dt), CARRYING THE SAME AttenPerStep the new level just took.
// See the long note above: the two time levels must have identical attenuation
// histories, because the difference between them is the scheme's velocity, and
// a level that has been damped one time fewer than its partner feeds amplitude
// back in at exactly the rate that halves the realised decay.
//
// Guarded like hn, and for one step's worth of extra reason: a poisoned .r
// arrives here as hc on the very next step, so clamping both channels means an
// inf or a NaN is gone from the state in one step rather than two or three.
float hcOut = hc * AttenPerStep;
hcOut = min(max(hcOut, -Ceiling), Ceiling);
if (!(hcOut == hcOut)) { hcOut = 0.0; }
return float3(hn + StateBias, hcOut + StateBias, 0.0);
"""


def build_step_material():
    material = new_material(STEP_NAME)
    b = RippleGraph(material)

    # WHICH CORNER TextureCoordinate's ORIGIN IS DOES NOT MATTER, and it is worth
    # writing that down because it is the first thing anyone will suspect when a
    # ripple's gradient looks mirrored. Every stage -- this step, the derive pass,
    # and M_WaterVoxel's sample -- maps world XY to UV with the SAME
    # (WorldXY - Origin) / WindowSizeUU, so a step of +Y in the world is a step of
    # +v in all three. The render target's physical row order never enters the
    # arithmetic, so it cannot flip a sign on its own.
    uv = b.texcoord()

    # SrcUv = Uv + ShiftUv. ShiftUv is (OriginNow - OriginPrev) / WindowSizeUU,
    # written by C++ once per frame and zero on every substep after the first --
    # the window moves once per frame, not once per simulation step.
    shift = b.vector_param("ShiftUv", 0.0, 0.0, 0.0, 0.0)
    shift_xy = b.mask(shift, "", r=True, g=True)
    src_uv = b.add(uv, shift_xy)

    # One texel, as a UV step. C++ writes this from kSize so there is one
    # authority for the resolution; the default mirrors it for the editor preview.
    texel = b.scalar("TexelUV", DEFAULT_TEXEL_UV)
    zero = b.const(0.0)
    neg_texel = b.mul(texel, b.const(-1.0))
    off_r = b.append(texel, "", zero, "")
    off_l = b.append(neg_texel, "", zero, "")
    off_u = b.append(zero, "", texel, "")
    off_d = b.append(zero, "", neg_texel, "")

    # FIVE SAMPLES, ONE PARAMETER. All five carry the name "PrevState", so the
    # single SetTextureParameterValue C++ makes per step moves all of them --
    # which is what makes the ping-pong one call instead of five, and makes it
    # impossible for four taps to read one target while the fifth reads the other.
    tap_c = b.rt_sample("PrevState", STATE_A_TEXTURE, src_uv)
    tap_r = b.rt_sample("PrevState", STATE_A_TEXTURE, b.add(src_uv, off_r))
    tap_l = b.rt_sample("PrevState", STATE_A_TEXTURE, b.add(src_uv, off_l))
    tap_u = b.rt_sample("PrevState", STATE_A_TEXTURE, b.add(src_uv, off_u))
    tap_d = b.rt_sample("PrevState", STATE_A_TEXTURE, b.add(src_uv, off_d))

    centre_rg = b.mask(tap_c, "", r=True, g=True)
    right_r = b.mask(tap_r, "", r=True)
    left_r = b.mask(tap_l, "", r=True)
    up_r = b.mask(tap_u, "", r=True)
    down_r = b.mask(tap_d, "", r=True)

    # --- the baked wet mask, sampled at THIS TEXEL'S WORLD POSITION ---------
    #
    # THE WORLD POSITION HAS TO BE RECONSTRUCTED, and that is the whole reason
    # bathy_field_graph.sample_bathy_field cannot be called here. This material is
    # drawn by a canvas over a render target: there is no geometry, so
    # MaterialExpressionWorldPosition returns something meaningless. The window's
    # placement arrives as parameters instead.
    #
    # The five lines below therefore duplicate that module's UV arithmetic, and
    # its docstring is explicit that duplication is the thing to avoid ("a
    # half-texel disagreement between the water and the ground it meets puts the
    # waterline in two different places"). Two things keep that from biting:
    # nothing here retypes the texture path, the parameter name, or the channel
    # layout -- they are imported -- and the arithmetic that IS retyped is one
    # multiply-add whose inputs come from the same MPC parameters. The clean fix
    # is a two-line change to bathy_field_graph.sample_bathy_field to accept an
    # optional world_xy; it is written up as a patch note in
    # docs/water-interactive-ripples.md rather than applied, because that file is
    # read by three shipped materials and this one is not built yet.
    #
    # THE EDGE FADE IS DELIBERATELY OMITTED from the validity used here. The bathy
    # window is 960 m across and this one is 51.2 m, both centred on the same
    # camera, so this window occupies the middle 5.3% of that one and can never
    # reach its fade band (which starts at 84% of the way out).
    import bathy_field_graph  # noqa: E402  -- imported here to keep the note above adjacent

    origin = b.vector_param("WindowOriginUU", 0.0, 0.0, 0.0, 0.0)
    window_size = b.scalar("WindowSizeUU", WINDOW_UU)
    world_xy = b.add(b.mask(origin, "", r=True, g=True), b.mul(uv, window_size))

    sky_collection = unreal.load_object(None, "/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky")
    if sky_collection is None:
        raise RuntimeError(
            "MPC_VoxelSky not found -- the step material binds to it for the bathymetry "
            "window's placement, which is what tells the simulation where water is. Run "
            "Tools/create_sky_material.py first; it is the sole author of that asset, and it "
            "DELETES AND RECREATES it, so every script that binds to it runs after it.")

    def collection_param(pname):
        # The same name-membership check SkyGraphBuilder.collection_param makes,
        # and for the identical reason: an unresolved CollectionParameter compiles
        # to a CONSTANT rather than failing (MaterialExpressions.cpp:17179-17193),
        # so a typo would silently sample one fixed bathymetry texel for the whole
        # window and every lake would be uniformly wet or uniformly dry.
        names = set()
        for prop in ("scalar_parameters", "vector_parameters"):
            for p in sky_collection.get_editor_property(prop):
                names.add(str(p.get_editor_property("parameter_name")))
        if pname not in names:
            raise RuntimeError(
                "CollectionParameter %r is not on MPC_VoxelSky. Present: %s. Re-run "
                "Tools/create_sky_material.py -- it is the sole author of that collection."
                % (pname, sorted(names)))
        n = b.node(unreal.MaterialExpressionCollectionParameter)
        n.set_editor_property("collection", sky_collection)
        n.set_editor_property("parameter_name", pname)
        if str(n.get_editor_property("parameter_name")) != pname:
            raise RuntimeError("CollectionParameter name did not round-trip for %r" % pname)
        if n.get_editor_property("collection") is None:
            raise RuntimeError("CollectionParameter %r lost its Collection" % pname)
        return n

    bathy_origin = collection_param("BathyFieldOrigin")
    bathy_inv_size = collection_param("BathyFieldInvSize")
    bathy_valid_flag = collection_param("BathyFieldValid")

    bathy_uv = b.mul(b.sub(world_xy, b.mask(bathy_origin, "", r=True, g=True)), bathy_inv_size)
    bathy_tex = b.node(unreal.MaterialExpressionTextureSampleParameter2D)
    bathy_tex.set_editor_property("parameter_name", bathy_field_graph.BATHY_TEXTURE_PARAM)
    bathy_texture = unreal.load_object(None, bathy_field_graph.BATHY_TEXTURE)
    if bathy_texture is None:
        raise RuntimeError(
            "failed to load %s -- run Tools/create_bathy_info_texture.py FIRST. Without it the "
            "ripple simulation has no idea where water is and would ring across dry land."
            % bathy_field_graph.BATHY_TEXTURE)
    bathy_tex.set_editor_property("texture", bathy_texture)
    bathy_tex.set_editor_property("sampler_type",
                                  unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    b.link(bathy_uv, "", bathy_tex, "UVs")

    shore_m = b.mask(bathy_tex, "", g=True)                       # signed metres, + in water
    validity = b.mul(b.mask(bathy_tex, "", b=True), bathy_valid_flag)

    # --- the knobs ----------------------------------------------------------
    courant2 = b.scalar("Courant2", DEFAULT_COURANT_SQ)
    damp = b.scalar("Damp", DEFAULT_DAMP_PER_STEP)
    mask_enable = b.scalar("MaskEnable", 1.0)
    shore_mask_m = b.scalar("ShoreMaskM", DEFAULT_SHORE_MASK_M)
    sponge_start = b.scalar("SpongeStart", DEFAULT_SPONGE_START)
    sponge_floor = b.scalar("SpongeFloor", DEFAULT_SPONGE_FLOOR)
    ceiling = b.scalar("Ceiling", DEFAULT_HEIGHT_CEILING_M)

    slot_names = ["Splat%d" % i for i in range(STEP_SPLAT_SLOTS)]
    # vector_param4, NOT vector_param: the HLSL below reads .xy, .z and .w from
    # each slot, and a bare VectorParameter's default output is only RGB. See
    # vector_param4's docstring for the shader error that produced.
    slots = [b.vector_param4(nm, 0.0, 0.0, 0.0, 0.0) for nm in slot_names]

    code = STEP_CODE % {
        "bias": STATE_BIAS,
        "slots": STEP_SPLAT_SLOTS,
        "slotlist": ", ".join(slot_names),
    }
    step = b.custom(
        "RippleStep", code,
        ["HC", "HL", "HR", "HD", "HU", "Uv", "SrcUv", "Courant2", "Damp",
         "ShoreM", "Valid", "MaskEnable", "ShoreMaskM",
         "SpongeStart", "SpongeFloor", "Ceiling"] + slot_names,
        unreal.CustomMaterialOutputType.CMOT_FLOAT3)

    wiring = [
        (centre_rg, "HC"), (left_r, "HL"), (right_r, "HR"), (down_r, "HD"), (up_r, "HU"),
        (uv, "Uv"), (src_uv, "SrcUv"), (courant2, "Courant2"), (damp, "Damp"),
        (shore_m, "ShoreM"), (validity, "Valid"), (mask_enable, "MaskEnable"),
        (shore_mask_m, "ShoreMaskM"), (sponge_start, "SpongeStart"),
        (sponge_floor, "SpongeFloor"), (ceiling, "Ceiling"),
    ] + list(zip(slots, slot_names))
    for src, pin in wiring:
        b.link(src, "", step, pin)

    b.prop(step, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish_material(material, STEP_NAME)
    return material


# ============================================================================
# THE DERIVE PASS
# ============================================================================

DERIVE_CODE = """
// State heights -> what the water material wants: float3(dH/dx, dH/dy, H).
//
// THE BIAS CANCELS IN THE GRADIENT AND ONLY THERE. A central difference is
// (hR + bias) - (hL + bias), so the storage bias falls out of the slope for
// free; the height in .z is the one place it has to be subtracted.
//
// StateBias, not BIAS: see the naming note in the step's code -- this text is
// pasted after the engine's Common.ush and must not collide with a macro.
const float StateBias = %(bias).6f;

// Central difference over TWO cells, hence the 2.0. TexelM is the cell size in
// METRES, so the result is metres of rise per metre of run -- dimensionless,
// which is the unit M_WaterVoxel's wave gradient is already in
// (create_water_voxel_material.py:2531-2532) and therefore the unit that lets
// the two simply add.
float gx = (HR - HL) / (2.0 * TexelM);
float gy = (HU - HD) / (2.0 * TexelM);
float h  = HC - StateBias;
return float3(gx, gy, h);
"""


def build_derive_material():
    material = new_material(DERIVE_NAME)
    b = RippleGraph(material)

    uv = b.texcoord()
    texel = b.scalar("TexelUV", DEFAULT_TEXEL_UV)
    zero = b.const(0.0)
    neg_texel = b.mul(texel, b.const(-1.0))

    # "State" is the FRONT buffer -- whichever of A/B the last step wrote. C++
    # sets it every frame; the default is A so the editor preview shows something.
    tap_c = b.rt_sample("State", STATE_A_TEXTURE, uv)
    tap_r = b.rt_sample("State", STATE_A_TEXTURE, b.add(uv, b.append(texel, "", zero, "")))
    tap_l = b.rt_sample("State", STATE_A_TEXTURE, b.add(uv, b.append(neg_texel, "", zero, "")))
    tap_u = b.rt_sample("State", STATE_A_TEXTURE, b.add(uv, b.append(zero, "", texel, "")))
    tap_d = b.rt_sample("State", STATE_A_TEXTURE, b.add(uv, b.append(zero, "", neg_texel, "")))

    texel_m = b.scalar("TexelM", DEFAULT_TEXEL_M)

    derive = b.custom("RippleDerive", DERIVE_CODE % {"bias": STATE_BIAS},
                      ["HC", "HL", "HR", "HD", "HU", "TexelM"],
                      unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    for src, pin in ((b.mask(tap_c, "", r=True), "HC"),
                     (b.mask(tap_l, "", r=True), "HL"),
                     (b.mask(tap_r, "", r=True), "HR"),
                     (b.mask(tap_d, "", r=True), "HD"),
                     (b.mask(tap_u, "", r=True), "HU"),
                     (texel_m, "TexelM")):
        b.link(src, "", derive, pin)

    b.prop(derive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish_material(material, DERIVE_NAME)
    return material


def main():
    # ORDER IS LOAD-BEARING: the materials sample the render targets by asset
    # reference, so the targets have to exist first. Within the targets, order is
    # free.
    #
    # THE STATE TARGETS CLEAR TO (BIAS, BIAS) AND NOT TO BLACK. Clearing to zero
    # would mean h = -0.5 m everywhere -- the entire lake half a metre below
    # itself, released at rest, which is the largest disturbance this simulation
    # can express. The C++ side clears to the same colour for the same reason
    # (VoxelRippleField.cpp, ClearState).
    state_clear = unreal.LinearColor(STATE_BIAS, STATE_BIAS, 0.0, 0.0)
    state_fmt = ["RTF_RG32F", "RTF_RG32f"]
    state_why = ("The simulation state is a signed height stored with a +%.1f bias; at 16-bit "
                 "float the quantisation near that bias (4.9e-4) is the same size as the wave "
                 "equation's whole update term (~7e-4 m). See this file's format section item 1."
                 % STATE_BIAS)
    make_render_target(STATE_A_NAME, state_fmt, ["TF_NEAREST"], state_clear, state_why)
    make_render_target(STATE_B_NAME, state_fmt, ["TF_NEAREST"], state_clear, state_why)
    make_render_target(
        FIELD_NAME, ["RTF_RGBA16F", "RTF_RGBA16f"], ["TF_BILINEAR"],
        unreal.LinearColor(0.0, 0.0, 0.0, 0.0),
        "The field the water material samples carries a gradient and a height, all signed and "
        "all near zero, where 16-bit float has ~1e-5 resolution -- no bias and no precision "
        "problem. There is no RGB-only float render target format, so the fourth channel is "
        "paid for and NOT WRITTEN: a canvas draw only guarantees the three emissive channels, "
        "so nothing may read .a.")

    build_step_material()
    build_derive_material()

    unreal.log(
        "RippleField: %d x %d texels at %.2f m = a %.1f m window (+/-%.1f m). Courant %.4f "
        "against the 2D stability limit 0.7071. Damping %.5f per step. State bias %.1f."
        % (FIELD_TEXELS, FIELD_TEXELS, TEXEL_UU / 100.0, WINDOW_UU / 100.0,
           WINDOW_UU / 200.0, DEFAULT_COURANT_SQ ** 0.5, DEFAULT_DAMP_PER_STEP, STATE_BIAS))
    unreal.log(
        "RippleField: NOTHING READS THE FIELD YET. M_WaterVoxel has to be patched to add it "
        "into the wave gradient and the wave height, and MPC_VoxelSky needs RippleFieldOrigin/"
        "RippleFieldInvSize/RippleFieldGain -- both patches are written out in "
        "docs/water-interactive-ripples.md. Until they are applied the simulation runs and "
        "the water looks exactly as it does today.")


if __name__ == "__main__":
    main()
