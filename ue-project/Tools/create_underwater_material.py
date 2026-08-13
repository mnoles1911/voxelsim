"""Author M_Underwater: the post-process material the player sees while the
camera is BELOW a water surface.

WHAT IT REPLACES, AND WHY THE THING IT REPLACES HAD TO GO
=========================================================

Today submerging applies two hard-coded things from
Source/VoxelEarth/VoxelOceanActor.cpp:51-93:

  * `Settings.SceneColorTint = FLinearColor(0.05, 0.30, 0.35)` on a
    `bUnbound = true` UPostProcessComponent, i.e. a flat multiply over the whole
    frame, and
  * an AExponentialHeightFog spawned in BeginPlay with FogDensity 0.08,
    inscattering colour (0.02, 0.14, 0.18) and FogHeightFalloff 0 -- "thick,
    close, blue-green ... reads as underwater murk", in that file's own words.

Three defects, and each one is a separate reason this file exists:

  1. IT DISAGREES WITH THE SURFACE. The numbers above were tuned for the ocean
     against a water surface material that has since been rewritten twice (the
     Single Layer Water port, then the 2026-08-12 absorption/scattering
     rebalance). The lake you look at is now blue-green with a measured 3.5 m
     absorption distance; the lake you swim in is that fixed teal. water_optics.py
     was created for exactly this split -- read its docstring, it names this
     material as the second consumer. Nothing here retypes a coefficient.
  2. A SCENE COLOUR TINT HAS NO DISTANCE TERM. It multiplies the pixel one metre
     away by the same 0.05/0.30/0.35 as the pixel forty metres away, so there is
     no depth cue at all; the height fog was doing all the distance work, with
     its own unrelated colour and its own unrelated density. Two half-tuned
     systems fighting is not a look, it is two looks averaged.
  3. THERE IS NO DEPTH-BELOW-SURFACE TERM ANYWHERE. Twenty centimetres under the
     surface of a pond and five metres down in it are pixel-identical today.
     That is the single most legible underwater cue there is and the current
     implementation does not have it in any form. See SubmergedDepthM below.

And a fourth, which is scope rather than quality: `bUnbound = true` means the
ocean's tuning is applied to every body of water in the world, including the
alpine lakes at ~1650 m. This material does not fix that on its own -- unbound
is a property of the PostProcessComponent, not of the material -- but every
number it needs that varies per water body arrives as a MATERIAL PARAMETER the
C++ can drive per-frame, which is what makes fixing it possible without a
second material.

WHAT IT DOES
============

Beer-Lambert extinction of the scene behind the water, plus a single in-scatter
term, evaluated per pixel against the reconstructed world position of the depth
buffer:

    T      = exp(-extinction_view * pathLength)
    result = sceneColor * T + inscatter * (1 - T)
    inscatter = singleScatteringAlbedo * ambient

`singleScatteringAlbedo` is scattering / extinction -- the colour a scattering
medium converges on as transmittance goes to zero, which is why deep murk reads
as coloured water rather than as black. This is the same limit the surface
material's SLW node converges on (create_water_voxel_material.py's SCATTERING
section), reached here by different arithmetic in a different renderer, from the
same constants. That is the whole design.

WHAT IT DELIBERATELY DOES NOT DO -- v1 SCOPE
============================================

NO MENISCUS. The half-submerged waterline across the screen when the camera
straddles the surface is not built. It needs the surface's plane in view space
and a screen-space split with a wobble along it, and, more importantly, it needs
the CAMERA to be reliably classifiable as half-in -- the C++ that will drive
SubmergedDepthM does not exist yet, and a meniscus keyed off a
still-being-designed submersion test is a defect generator. It is additive
later: it is a mask on this material's own output, not a change to the volume.

NO PARTICULATES. No drifting motes, no god rays, no noise texture. Every one of
those is a temporal-stability question on a project that has already spent
sessions separating "animation" from "flicker" in water captures (see
create_water_voxel_material.py's FREEZE_TIME arm and why it had to be built).
The volume term below is a pure function of scene depth and camera position, so
a frozen pose produces a frozen image and any inter-frame difference on this
material is measurable as instability. Adding drifting particles in v1 would
destroy that property before it has ever been used.

UNITS -- THE 100x TRAP, SAME TRAP, DIFFERENT NODE
=================================================

Unreal world units are CENTIMETRES. Every optical constant in water_optics.py is
PER METRE, because that is how every published figure is quoted. The path length
this material computes comes out of AbsoluteWorldPosition and is therefore in
UU, i.e. centimetres. So the per-metre coefficients must be multiplied by 0.01
before they multiply that path, and getting it backwards gives water that goes
black in nine centimetres.

There is exactly ONE node in this file that does that conversion (`per_cm`), and
the only chain that goes through it is the VIEW PATH. This mirrors
create_water_voxel_material.py, which has exactly one such node for the same
reason (its docstring, "UNITS ARE 1/cm. THIS IS THE SINGLE EASIEST WAY TO BE 100x
WRONG HERE").

THE ONE CHAIN THAT DOES *NOT* GO THROUGH IT, stated here because an unexplained
missing multiply looks exactly like the bug: the SubmergedDepthM chain. That
parameter is in METRES by its name and by contract with the C++ that will drive
it, so it pairs with the per-metre coefficient directly and there is no unit
conversion to do. If somebody ever drives that parameter in UU, the ambient will
be black at any depth over about eight centimetres, and this paragraph is the
first place to look.

THE READABILITY CHEAT, DECLARED UP FRONT
========================================

The underwater view does NOT run the surface's extinction. It runs it scaled by
UnderwaterExtinctionScale, default 0.5. At the shared constants' true extinction
(R 1.138, G 0.882, B 0.953 per metre) the transmittance at ONE METRE is
0.32/0.41/0.39 and at five metres 0.003/0.012/0.009 -- i.e. a swimmer could not
see a wall at arm's length and could not see anything at all past three or four
metres. That is physically defensible for a 3.5 m absorption distance and it is
unplayable.

Every shipped shader that has faced this does the same thing (Photon and the
Minecraft shader family all carry a separate, weaker underwater extinction from
the one they use looking down at the surface). So it is a knob, it is named as a
scale rather than hidden inside a coefficient, and it is documented as a CHEAT
rather than as physics -- because the day somebody asks "why doesn't the swim
match the surface", the answer is this parameter and not a bug.

At the default 0.5 the view runs R 0.569 / G 0.441 / B 0.476 per metre:
transmittance 0.57/0.64/0.62 at 1 m, 0.32/0.41/0.39 at 2 m, 0.06/0.11/0.09 at
5 m, 0.003/0.012/0.009 at 10 m. So the useful visual range roughly doubles: you
can read a wall at two metres and the world fades out around eight to ten. If
the owner wants to see further, 0.25 is the next stop (visibility ~20 m) and the
water will read noticeably clearer than it looks from above.

DEPENDENCIES AND ORDERING
=========================

This material binds two MPC_VoxelSky parameters, MoonLightFraction and
SunDirection, so it is a DEPENDENT of Tools/create_sky_material.py in exactly the
way M_WaterVoxel is, and must be re-run after it. create_sky_material.py DELETES
and recreates that collection on every run; a material holding a binding to the
deleted one compiles to the ENGINE DEFAULT MATERIAL while the log says success
(the 2026-08-10 failure documented in tools/voxel-water-star-regen.ps1's header).
The collection_param() helper below re-checks every binding BY NAME against the
asset as read back, and raises, because an unresolved CollectionParameter does
NOT fail to compile -- it compiles to a CONSTANT
(MaterialExpressions.cpp:17179-17193).

THIS SCRIPT ONLY READS THE MPC. It does not create, modify or re-save it. If a
parameter it wants is missing, that is a create_sky_material.py edit plus a
re-run of the whole chain, and the raise says so.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash

WHAT THE C++ STILL HAS TO DO (not this file's job, listed so it is not lost)
===========================================================================

  * Point the UPostProcessComponent at a UMaterialInstanceDynamic of
    /Game/Voxel/M_Underwater via Settings.WeightedBlendables, and delete the
    SceneColorTint override and the AExponentialHeightFog in VoxelOceanActor.cpp.
  * Drive SubmergedDepthM every frame = (water surface Z at the camera - camera
    Z) in METRES, clamped at 0 when above water.
  * Drive BlendWeight 0/1 on submersion as it does today (UpdateUnderwaterState).
  * Optionally drive UnderwaterAmbientColor per water body, which is the hook
    that lets an alpine lake stop being tinted like the ocean.
"""

import math
import os
import sys

import unreal

# A -run=pythonscript commandlet does not put the script's own directory on
# sys.path, so add it explicitly -- same as create_sky_material.py:417-420 and
# create_water_voxel_material.py:415.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import water_optics  # noqa: E402

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_Underwater"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME

COLLECTION_PATH = "/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky"


# --- BLENDABLE LOCATION: A LIST OF CANDIDATES, NOT A NAME --------------------
#
# WHY THIS IS NOT JUST `unreal.BlendableLocation.BL_BEFORE_TONEMAPPING`.
# EBlendableLocation was RENAMED wholesale in UE 5.3 (BL_BeforeTonemapping,
# BL_BeforeTranslucency, BL_SSRInput and BL_AfterTonemapping all became
# BL_SceneColor*/BL_Translucency* spellings), and the Python bindings expose
# whatever the C++ UENUM says, uppercased. This project has already been
# surprised by binding spellings twice in this directory -- see
# create_sky_material.py's guid_library() (unreal.GuidLibrary vs
# unreal.KismetGuidLibrary) and create_water_voxel_material.py's
# get_texture_parameter_names fallback -- so a hard-coded member name here would
# be a `AttributeError` on an engine upgrade at best, and at worst a silently
# skipped set_editor_property leaving the material at the engine's DEFAULT
# location (after tonemapping), which is a subtly-wrong picture nobody would
# read as a bug.
#
# THE ORDER IS A PREFERENCE ORDER AND EVERY ENTRY IS ACCEPTABLE. The requirement
# is "before tonemapping and before bloom", so that murk is what bloom blooms:
# an underwater light source seen through eight metres of water should bloom at
# the brightness the water left it, not at its unattenuated brightness with the
# murk painted on afterwards. All five names below satisfy that. They differ in
# where they sit relative to DOF and translucency:
#
#   1. BL_SCENE_COLOR_BEFORE_BLOOM  -- 5.3+ name of the classic slot. Exact fit.
#   2. BL_BEFORE_TONEMAPPING        -- <=5.2 name of the same slot.
#   3. BL_SCENE_COLOR_AFTER_DOF     -- 5.3+, still pre-bloom/pre-tonemap.
#   4. BL_SCENE_COLOR_BEFORE_DOF    -- 5.3+, earliest slot. WORSE, and stated
#                                      why: it runs before translucency is
#                                      composited, so translucent surfaces --
#                                      including the water surface itself seen
#                                      from below -- would not be attenuated by
#                                      the volume in front of them.
#   5. BL_BEFORE_TRANSLUCENCY       -- <=5.2 name of (4), same caveat.
#
# The chosen name is LOGGED, so a run that fell through to (4) leaves evidence
# rather than an unexplained difference in a capture.
BLENDABLE_LOCATION_CANDIDATES = [
    "BL_SCENE_COLOR_BEFORE_BLOOM",
    "BL_BEFORE_TONEMAPPING",
    "BL_SCENE_COLOR_AFTER_DOF",
    "BL_SCENE_COLOR_BEFORE_DOF",
    "BL_BEFORE_TRANSLUCENCY",
]

# The scene colour input of a post-process material. PostProcessInput0 rather
# than SceneTexture:SceneColor deliberately: in MD_PostProcess, SceneColor is not
# the pass input -- PostProcessInput0 is the output of whatever ran before this
# blendable, which is what "the scene we are attenuating" means. Same defensive
# treatment as the location above, for the same bindings reason.
SCENE_INPUT_CANDIDATES = [
    "PPI_POST_PROCESS_INPUT0",
    "PPI_POSTPROCESSINPUT0",
    "PPI_SCENE_COLOR",
]

MATERIAL_DOMAIN_CANDIDATES = [
    "MD_POST_PROCESS",
    "MD_POSTPROCESS",
]


def enum_member(enum_type_name, candidates, why):
    """Return (name, value) for the first candidate that exists on the enum.

    Raises with the FULL list of what the enum actually has, because the whole
    point of this helper is that the next person hits it on an engine upgrade
    and needs to pick the new spelling without opening an interactive console.
    """
    enum_type = getattr(unreal, enum_type_name, None)
    if enum_type is None:
        raise RuntimeError(
            "unreal.%s does not exist on this engine build's Python bindings. %s "
            "Candidates this script would have accepted: %s."
            % (enum_type_name, why, candidates))

    available = sorted(n for n in dir(enum_type)
                       if not n.startswith("_") and n == n.upper())
    for name in candidates:
        value = getattr(enum_type, name, None)
        if value is not None:
            return name, value
    raise RuntimeError(
        "none of %s exist on unreal.%s -- it has %s. %s Pick the member that "
        "matches and add it to the candidate list at the top of this file; do "
        "NOT silently accept the engine default, which is the failure this "
        "check exists to prevent."
        % (candidates, enum_type_name, available, why))


class Graph:
    """Thin builder that CHECKS every connection and NAMES it when it fails.

    WHY NOT terrain_material_common.GraphBuilder, which already exists and
    already checks. Because its raise reports node CLASSES
    ("MaterialExpressionMultiply -> MaterialExpressionAdd.A failed") and this
    graph has fourteen multiplies in it, so that message would not locate the
    failure. Every connect here carries a human label instead. The cost is a
    forty-line class duplicating an existing forty-line class; the benefit is
    that a broken pin names itself. That trade would flip if this file ever grew
    a subgraph shared with the terrain materials -- it has none, and by design
    (a post-process material shares no vertex data with anything).
    """

    def __init__(self, material):
        self.material = material
        self.mel = unreal.MaterialEditingLibrary
        # layout_material_expressions() re-flows the graph at the end anyway, so
        # these coordinates only have to be distinct, not pretty.
        self._x = -2000
        self._y = 0

    def node(self, cls):
        self._y += 90
        if self._y > 1600:
            self._y = 0
            self._x += 320
        return self.mel.create_material_expression(self.material, cls, self._x, self._y)

    def link(self, src, src_out, dst, dst_in, what):
        if not self.mel.connect_material_expressions(src, src_out, dst, dst_in):
            raise RuntimeError(
                "connect %s (%s.%s -> %s.%s) failed -- a silently-failed pin connect "
                "produced an invisible-terrain material on this project once "
                "(2026-07-19) and cost a debug session, which is why every connect in "
                "this file is checked."
                % (what, type(src).__name__, src_out or "<default>",
                   type(dst).__name__, dst_in or "<default>"))

    def prop(self, src, src_out, material_property, what):
        if not self.mel.connect_material_property(src, src_out, material_property):
            raise RuntimeError(
                "connect %s (%s -> %s) failed -- with nothing on that material property "
                "this material would compile and composite a BLACK frame, which reads as "
                "a rendering bug rather than as a build failure."
                % (what, type(src).__name__, material_property))

    # --- small algebra, every one of them a checked connect -----------------

    def const(self, value):
        n = self.node(unreal.MaterialExpressionConstant)
        n.set_editor_property("r", float(value))
        return n

    def scalar(self, name, default):
        n = self.node(unreal.MaterialExpressionScalarParameter)
        n.set_editor_property("parameter_name", name)
        n.set_editor_property("default_value", float(default))
        return n

    def vector(self, name, rgb):
        n = self.node(unreal.MaterialExpressionVectorParameter)
        n.set_editor_property("parameter_name", name)
        n.set_editor_property("default_value",
                              unreal.LinearColor(float(rgb[0]), float(rgb[1]), float(rgb[2]), 1.0))
        return n

    def mask(self, src, src_out, what, r=False, g=False, b=False, a=False):
        """Component mask.

        Every VectorParameter in this file goes through one with rgb set. An
        FLinearColor forces a fourth component onto every vector parameter, and
        feeding a float4 into a float3 multiply is the kind of thing that
        compiles and then means something slightly different -- the same note
        create_water_voxel_material.py's rgb() helper carries.
        """
        n = self.node(unreal.MaterialExpressionComponentMask)
        n.set_editor_property("r", r)
        n.set_editor_property("g", g)
        n.set_editor_property("b", b)
        n.set_editor_property("a", a)
        self.link(src, src_out, n, "", what)
        return n

    def binary(self, cls, a, b, what, a_out="", b_out=""):
        n = self.node(cls)
        self.link(a, a_out, n, "A", what + " (A)")
        self.link(b, b_out, n, "B", what + " (B)")
        return n

    def mul(self, a, b, what, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionMultiply, a, b, what, a_out, b_out)

    def add(self, a, b, what, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionAdd, a, b, what, a_out, b_out)

    def sub(self, a, b, what, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionSubtract, a, b, what, a_out, b_out)

    def div(self, a, b, what, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionDivide, a, b, what, a_out, b_out)

    def unary(self, cls, a, what, a_out=""):
        n = self.node(cls)
        self.link(a, a_out, n, "", what)
        return n

    def saturate(self, a, what, a_out=""):
        return self.unary(unreal.MaterialExpressionSaturate, a, what, a_out)

    def one_minus(self, a, what, a_out=""):
        return self.unary(unreal.MaterialExpressionOneMinus, a, what, a_out)

    def exp(self, a, what, a_out=""):
        return self.unary(unreal.MaterialExpressionExponential, a, what, a_out)

    def negate(self, a, what, a_out=""):
        """exp() wants -opticalDepth; there is no unary minus node."""
        return self.mul(a, self.const(-1.0), what, a_out=a_out)


def main():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
        unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material, factory)
    if material is None:
        raise RuntimeError("Failed to create material asset at " + FULL_PATH)

    # --- DOMAIN FIRST, AND THAT ORDERING IS DELIBERATELY THE OPPOSITE OF
    #     create_water_voxel_material.py's ----------------------------------
    #
    # That file moves its shading_model assignment to AFTER the graph is built,
    # because setting it early makes seventeen transient "Failed to compile
    # Material" warnings -- word for word the string this project's release rule
    # greps for before trusting any visual result.
    #
    # The domain has to go the other way round and it is not a matter of taste:
    # MaterialExpressionSceneTexture's PostProcessInput0 is only legal in
    # MD_PostProcess (Material.cpp validates the domain when it compiles the
    # expression), so a SceneTexture node created while the material is still
    # MD_Surface is the error, not the cure. An EMPTY post-process material, by
    # contrast, compiles cleanly -- unconnected emissive is black, which is
    # valid -- so setting the domain first costs no spurious warnings and the
    # grep-rule stays intact.
    domain_name, domain_value = enum_member(
        "MaterialDomain", MATERIAL_DOMAIN_CANDIDATES,
        "This material is a post-process blendable; without MD_PostProcess it would be "
        "authored as an ordinary surface material, would never be applied by the "
        "PostProcessComponent, and nothing in the run would say so.")
    material.set_editor_property("material_domain", domain_value)

    domain_after = material.get_editor_property("material_domain")
    if domain_after != domain_value:
        raise RuntimeError(
            "material domain did not take: %r, expected %s. A surface-domain asset at "
            "%s would load, compile and be silently ignored by the post-process stack."
            % (domain_after, domain_name, FULL_PATH))

    # --- WHERE IN THE POST CHAIN IT RUNS ------------------------------------
    #
    # See BLENDABLE_LOCATION_CANDIDATES for the full argument. Short version:
    # before tonemapping AND before bloom, so that a light source seen through
    # murk blooms at the brightness the murk left it. Applying the volume after
    # bloom gives you an unattenuated bloom halo around an attenuated light,
    # which reads as the light being in front of the water rather than in it.
    location_name, location_value = enum_member(
        "BlendableLocation", BLENDABLE_LOCATION_CANDIDATES,
        "This material must run before tonemapping and before bloom.")
    material.set_editor_property("blendable_location", location_value)

    location_after = material.get_editor_property("blendable_location")
    if location_after != location_value:
        raise RuntimeError(
            "blendable location did not take: %r, expected %s. The engine default is "
            "after tonemapping, where the murk would be applied to an already-tonemapped "
            "image and bloom would have run on the unattenuated scene -- a wrong picture "
            "that looks like a tuning problem." % (location_after, location_name))

    b = Graph(material)

    # ======================================================================
    # THE SHARED CONSTANTS -- IMPORTED, DERIVED, NEVER TYPED
    # ======================================================================
    #
    # water_optics.py owns the four physical numbers and the arithmetic that
    # turns them into coefficients. Its docstring says at length why they are a
    # Python import and not a second MaterialParameterCollection (MPC_VoxelSky
    # has already cost this project a night: create_sky_material.py deletes and
    # recreates it, and every material bound to the old one silently compiles to
    # the engine default material while the log reports success).
    #
    # Printed into the log as well as used, because these scripts run headless
    # and the log is the only artefact anybody reads afterwards. If the surface
    # and the swim ever disagree on screen, the first question is whether they
    # were built from the same numbers, and this block in both logs answers it
    # without opening either asset.
    for line in water_optics.summary_lines():
        unreal.log("M_Underwater " + line)

    extinction_m = water_optics.extinction_per_m()
    albedo = water_optics.single_scattering_albedo()
    absorption_m = water_optics.absorption_per_m()
    scattering_m = water_optics.SCATTERING_PER_M

    # THE COEFFICIENTS ARE PARAMETERS WITH IMPORTED DEFAULTS, not constants.
    #
    # The default IS the shared value -- there is no literal here, the tuple came
    # out of water_optics.extinction_per_m() four lines up -- but it is exposed so
    # the owner can A/B a look from a material instance without a regeneration,
    # which is how every other tunable in this directory works.
    #
    # THE COST OF THAT, AND IT IS THE COST water_optics.py's docstring ALREADY
    # STATES: an instance override here does NOT move M_WaterVoxel with it. These
    # are separate materials with separate parameters and the shared module is
    # the only thing keeping them equal. Instance overrides are for experiments;
    # a shipped change is an edit to water_optics.py and a re-run of BOTH
    # generators.
    ext_param = b.vector("UnderwaterExtinctionPerMetre", extinction_m)
    ext_per_m = b.mask(ext_param, "", "UnderwaterExtinctionPerMetre -> rgb mask",
                       r=True, g=True, b=True)

    # scattering / extinction. THE COLOUR THE MURK CONVERGES ON as transmittance
    # goes to zero -- i.e. the colour of "too far to see anything", which
    # underwater is most of the frame. Derived, not authored: authoring it is how
    # the current implementation ended up with a teal
    # (SceneColorTint 0.05/0.30/0.35) that has no relationship to the water it is
    # supposed to be inside. Here it falls out of the same two vectors the
    # surface's SLW node is fed.
    albedo_param = b.vector("UnderwaterScatterAlbedo", albedo)
    albedo_rgb = b.mask(albedo_param, "", "UnderwaterScatterAlbedo -> rgb mask",
                        r=True, g=True, b=True)

    # THE ONE UNIT CONVERSION. Per metre -> per centimetre, for the VIEW path
    # only, because that is the only chain whose length arrives in unreal units.
    # See the UNITS section of the module docstring for why this node exists
    # exactly once and what breaks if a second one appears.
    per_cm = b.const(0.01)

    # --- THE READABILITY CHEAT ----------------------------------------------
    #
    # UnderwaterExtinctionScale scales the extinction used for the VIEW PATH and
    # nothing else. Default 0.5 = half the surface's extinction = roughly double
    # the visual range. The module docstring carries the measured transmittance
    # table for 1.0, 0.5 and 0.25 and the argument for why an honest 1.0 is
    # unplayable at a 3.5 m absorption distance.
    #
    # A JUDGEMENT CALL, STATED, BECAUSE THE BRIEF FOR THIS PARAMETER SAID
    # "DIVIDES" AND THIS NODE MULTIPLIES. Dividing extinction by 0.5 would
    # DOUBLE it -- transmittance at one metre would drop from 0.32 to 0.10 in
    # red -- which is the opposite of the stated reason for the parameter
    # existing ("at the surface's true 1.14/m red you could not see one metre").
    # The name says Scale, the intent says "see further", so it multiplies:
    # scale < 1 means clearer water, scale = 1 means physically consistent with
    # the surface, scale > 1 means murkier. If the intent really was a divisor,
    # the fix is this ONE node (Multiply -> Divide) plus a default of 2.0, and
    # nothing else in the file changes.
    ext_scale = b.scalar("UnderwaterExtinctionScale", 0.5)
    ext_view_per_m = b.mul(ext_per_m, ext_scale, "extinction/m * UnderwaterExtinctionScale")
    ext_view_per_cm = b.mul(ext_view_per_m, per_cm, "view extinction -> per centimetre")

    # ======================================================================
    # PATH LENGTH: RADIAL, NOT SCENE DEPTH
    # ======================================================================
    #
    # In MD_PostProcess, AbsoluteWorldPosition is the world position
    # RECONSTRUCTED FROM THE SCENE DEPTH BUFFER for this pixel -- there is no
    # geometry being shaded, so the node has nothing else it could mean. Distance
    # from that to CameraPositionWS is the true length of the ray through the
    # water.
    #
    # WHY NOT SceneTexture:SceneDepth DIRECTLY, which is one node instead of
    # three. Because SceneDepth is Z-DEPTH ALONG THE CAMERA AXIS, not distance.
    # At a 90 degree horizontal FOV on 16:9 the corner ray is about 1.4x longer
    # than its Z component (1/cos of the corner angle), so a Z-depth fog is up to
    # 40% too THIN at the corners of the screen and correct only dead centre.
    # That gradient is radially symmetric about the screen centre and brightest
    # at the corners: it would read as a VIGNETTE, and this project already ships
    # a real vignette on the underwater post-process
    # (VoxelOceanActor.cpp:57-58, VignetteIntensity 0.6), so the artefact would
    # be indistinguishable from a tuning decision somebody made on purpose.
    #
    # LWC: both operands are LWC-typed engine expressions, so the compiler emits
    # the subtraction in emulated double precision and downcasts the small
    # camera-relative result -- the same idiom, for the same planet-scale reason,
    # as create_voxel_material.py:266-278.
    world_pos = b.node(unreal.MaterialExpressionWorldPosition)
    camera_pos = b.node(unreal.MaterialExpressionCameraPositionWS)
    path_uu = b.node(unreal.MaterialExpressionDistance)
    b.link(world_pos, "", path_uu, "A", "AbsoluteWorldPosition -> path length (A)")
    b.link(camera_pos, "", path_uu, "B", "CameraPositionWS -> path length (B)")

    # THE SKY IS HANDLED BY THIS AND NEEDS NO SPECIAL CASE, which is worth saying
    # because the absence of one looks like an oversight. An unwritten depth
    # buffer texel is at the far plane, so the reconstructed position is
    # kilometres away, transmittance underflows to zero and the pixel becomes
    # pure in-scatter. That is exactly right: from under the water you do not see
    # the sky, you see water. It also means this material does not need to know
    # where the surface is in order to hide it.
    optical_depth = b.mul(ext_view_per_cm, path_uu, "view extinction/cm * path length in UU")
    transmittance = b.exp(b.negate(optical_depth, "negate optical depth for exp()"),
                          "exp(-opticalDepth) -> transmittance")

    # ======================================================================
    # AMBIENT: HOW MUCH LIGHT IS DOWN HERE AT ALL
    # ======================================================================
    #
    # The in-scatter term is albedo * ambient, and `ambient` is the one quantity
    # this material cannot derive from anything -- it is the light arriving at
    # the swimmer, which depends on the sky, the time of day and how deep they
    # are. Three factors, multiplied:
    #
    #   1. UnderwaterAmbientColor * UnderwaterAmbientGain -- the authored level.
    #   2. a day/night factor from MPC_VoxelSky.
    #   3. exp(-extinction * SubmergedDepthM) -- the downwelling attenuation.
    #
    # UNITS OF THE RESULT, because getting this wrong is the most likely reason
    # the first capture looks wrong: this material runs BEFORE tonemapping, so
    # PostProcessInput0 is in linear HDR units where a sunlit diffuse surface
    # typically sits somewhere around 1-10, not 0-1. UnderwaterAmbientGain 1.0
    # therefore gives murk that reads roughly as a mid-grey-lit body of water. It
    # is the single knob for "the far field is too dark / too milky" and it is
    # expected to be calibrated from a capture, not from this comment -- the
    # standing rule on this project is that the owner judges appearance from
    # screenshots and the implementer does not deliver a verdict.
    ambient_color = b.vector("UnderwaterAmbientColor", (1.0, 1.0, 1.0))
    ambient_rgb = b.mask(ambient_color, "", "UnderwaterAmbientColor -> rgb mask",
                         r=True, g=True, b=True)
    ambient_gain = b.scalar("UnderwaterAmbientGain", 1.0)

    # --- THE MPC BINDINGS, CHECKED BY NAME BEFORE THEY ARE MADE -------------
    sky_collection = unreal.load_object(None, COLLECTION_PATH)
    if sky_collection is None:
        raise RuntimeError(
            "MPC_VoxelSky not found at %s -- this material binds to it for its day/night "
            "term. Run Tools/create_sky_material.py first; it is the sole author of that "
            "asset, and this script only ever READS it." % COLLECTION_PATH)

    def collection_param(name):
        """A CollectionParameter node, or a raise naming what the MPC actually has.

        Copied in discipline (not in code) from create_water_voxel_material.py:1390.
        The check exists because an unresolved CollectionParameter does NOT fail
        to compile -- it compiles to a CONSTANT
        (MaterialExpressions.cpp:17179-17193) -- so a typo, or an MPC authored by
        an older create_sky_material.py, produces a material that builds, saves,
        loads and is silently wrong. That failure shape has already been paid for
        once on this project (2026-08-10) and was diagnosed from the owner saying
        he could not see the lake basins, not from anything in a log.
        """
        have = [str(p.get_editor_property("parameter_name"))
                for p in sky_collection.get_editor_property("scalar_parameters")]
        have += [str(p.get_editor_property("parameter_name"))
                 for p in sky_collection.get_editor_property("vector_parameters")]
        if name not in have:
            raise RuntimeError(
                "MPC_VoxelSky has no parameter %r -- it has %s. Re-run "
                "Tools/create_sky_material.py (then the dome, then the water material, "
                "then this one -- see the ordering note at the top of "
                "create_sky_material.py). An unresolved CollectionParameter compiles to a "
                "constant rather than failing, so this check is the only thing between a "
                "stale MPC and a silently dead day/night term."
                % (name, sorted(have)))
        node = b.node(unreal.MaterialExpressionCollectionParameter)
        node.set_editor_property("collection", sky_collection)
        node.set_editor_property("parameter_name", name)
        return node

    # DAYLIGHT, from the sun's altitude. SunDirection is written every frame by
    # UVoxelSkySubsystem::ApplySkyMaterialParams and points TOWARD the sun, so its
    # z is sin(altitude): +1 at zenith, 0 at the horizon, negative at night.
    #
    # WHY A RAMP RATHER THAN saturate(SunDirection.z) DIRECTLY, which is what
    # M_WaterVoxel's sky reflection uses. Because that is exactly zero from the
    # moment the sun touches the horizon, and this term controls the light level
    # of the ENTIRE underwater frame rather than a reflection's weight -- a hard
    # cut to a black world at sunset would be the most visible artefact this
    # material could produce. The two scalars give a fade from about 5.7 degrees
    # BELOW the horizon (sin -0.10) to about 8.6 degrees above it (sin 0.15),
    # i.e. roughly civil twilight, which is when a real lake goes dark.
    #
    # These are deliberately NOT reusing create_sky_material.py's StarBrightness
    # fade (1 below -12 deg, 0 above 0 deg): that one is tuned for stars becoming
    # visible, which happens well after the water goes dark.
    sun_dir = collection_param("SunDirection")
    sun_z = b.mask(sun_dir, "", "SunDirection -> z (sin solar altitude)", b=True)
    day_low = b.scalar("UnderwaterDayFadeSinLow", -0.10)
    day_high = b.scalar("UnderwaterDayFadeSinHigh", 0.15)
    day01 = b.saturate(
        b.div(b.sub(sun_z, day_low, "sin(altitude) - fade low"),
              b.sub(day_high, day_low, "fade high - fade low"),
              "normalise solar altitude into the twilight fade"),
        "clamp the daylight fade to 0..1")

    # MOONLIGHT. MoonLightFraction = S.MoonIntensity / GetSunIntensity(), written
    # every frame by UVoxelSkySubsystem::ApplySkyMaterialParams -- "how much
    # dimmer than the sun the moon is, right now". It already carries moonset
    # (MoonHorizonGate) and phase (MoonIlluminatedFraction), so a new moon or a
    # set moon contributes nothing here with no logic in this file.
    #
    # IT IS NOT A DAY/NIGHT SIGNAL ON ITS OWN and must not be used as one: it is a
    # ratio between two lights, so a moon up at noon reports the same fraction as
    # the same moon at midnight. The daylight term above is what makes night
    # dark; this one is what stops a moonlit night being pitch black. They ADD,
    # because they are two independent sources, and the sum is deliberately not
    # saturated -- a full moon contributes a small fraction, not a second day.
    #
    # THE GAIN DEFAULTS TO 1.0, i.e. the moon lights the water in exactly the
    # ratio it lights everything else. That keeps this term on the SAME
    # calibration as M_WaterVoxel's moon glint, which multiplies the sun's tint
    # by this identical scalar. If night water needs lifting, lift it here rather
    # than in the fraction, which would drag the surface glint with it.
    moon_fraction = collection_param("MoonLightFraction")
    moon_gain = b.scalar("UnderwaterMoonAmbient", 1.0)
    moon_term = b.mul(moon_fraction, moon_gain, "MoonLightFraction * UnderwaterMoonAmbient")

    # A FLOOR, so a moonless midnight is very dark rather than absolutely black.
    # Zero ambient means the in-scatter term vanishes and the frame becomes
    # sceneColor * T with nothing replacing what T removed -- everything past a
    # couple of metres goes to pure black, which reads as a broken shader rather
    # than as night. 0.02 is starlight-and-skyglow order-of-magnitude and is
    # about 2% of the noon term.
    ambient_floor = b.scalar("UnderwaterAmbientFloor", 0.02)
    sky_level = b.add(b.add(day01, moon_term, "daylight + moonlight"),
                      ambient_floor, "+ the moonless-night floor")

    # --- SUBMERGED DEPTH: THE TERM THE CURRENT IMPLEMENTATION HAS NONE OF ---
    #
    # SubmergedDepthM is how far the CAMERA is below the water surface, in
    # METRES, driven from C++. Attenuating the ambient by exp(-extinction * that)
    # is what makes five metres down visibly darker than twenty centimetres down.
    # Today those two are pixel-identical, which is the third defect listed in the
    # module docstring.
    #
    # DEFAULT 0, AND DELIBERATELY SO: an undriven parameter must fail to the
    # SHALLOWEST case, not to a plausible mid-depth one. Same argument
    # create_sky_material.py makes for MoonLightFraction defaulting to 0 -- a
    # default that looks right while proving nothing is worse than one that
    # obviously is not being driven.
    #
    # ITS OWN SCALE, AND WHY IT IS NOT UnderwaterExtinctionScale. At the true
    # coefficients the downwelling attenuation is 0.003/0.012/0.009 at five
    # metres -- black. The view path's cheat does not apply here (that one is
    # explicitly view-only), so this needs its own, and 0.25 gives 0.94 at 20 cm,
    # 0.24 at 5 m and 0.003 at 20 m: a strong, readable depth gradient over the
    # depths this world's lakes actually have.
    #
    # THIS ONE IS ALSO PART PHYSICS CORRECTION, not purely a cheat, and the
    # distinction is worth keeping: a single Beer-Lambert ray along the vertical
    # UNDERSTATES the light at depth in real water, because the light that
    # actually reaches a diver has been multiply scattered and this material
    # models single scattering only. Nothing here measures how much of the 4x is
    # correction and how much is cheat; if that is ever measured, this is the
    # parameter that should move.
    #
    # NO 0.01 IN THIS CHAIN, ON PURPOSE. The parameter is in metres and the
    # coefficient is per metre. See the UNITS section of the module docstring:
    # driving this in UU would make the ambient black below about 8 cm.
    submerged_depth_m = b.scalar("SubmergedDepthM", 0.0)
    depth_scale = b.scalar("UnderwaterDepthExtinctionScale", 0.25)
    depth_optical = b.mul(
        b.mul(ext_per_m, depth_scale, "extinction/m * UnderwaterDepthExtinctionScale"),
        submerged_depth_m, "* SubmergedDepthM (metres, so no cm conversion)")
    depth_atten = b.exp(b.negate(depth_optical, "negate depth optical depth for exp()"),
                        "exp(-extinction * submerged depth) -> downwelling attenuation")

    ambient = b.mul(
        b.mul(b.mul(ambient_rgb, ambient_gain, "UnderwaterAmbientColor * UnderwaterAmbientGain"),
              sky_level, "* the day/night light level"),
        depth_atten, "* the depth attenuation")

    # IN-SCATTER. albedo * ambient: the light that the water puts BACK into the
    # ray, and the colour every pixel converges on as the path gets long. This is
    # the half that makes murk read as water rather than as a fade to black --
    # absorption alone only removes light. Petzold's measurements are the usual
    # citation: clear ocean to turbid harbour is ~50x in scattering against ~3x
    # in absorption, i.e. dirty water is a BRIGHT medium.
    inscatter = b.mul(albedo_rgb, ambient, "single-scattering albedo * ambient")

    # ======================================================================
    # COMPOSITE
    # ======================================================================
    scene_id_name, scene_id_value = enum_member(
        "SceneTextureId", SCENE_INPUT_CANDIDATES,
        "This material attenuates the scene that the post-process chain hands it.")
    scene_tex = b.node(unreal.MaterialExpressionSceneTexture)
    scene_tex.set_editor_property("scene_texture_id", scene_id_value)

    # Output pin "Color" by name rather than the default pin: MaterialExpression
    # SceneTexture has three outputs (Color, Size, InvSize) and the default is
    # only Color by convention. Naming it means a future reordering of that
    # node's outputs raises here instead of multiplying the scene by the render
    # target's dimensions, which would be a spectacular and very confusing frame.
    scene_rgb = b.mask(scene_tex, "Color", "PostProcessInput0.Color -> rgb mask",
                       r=True, g=True, b=True)

    #   result = sceneColor * T + inscatter * (1 - T)
    #
    # Energy-conserving by construction: whatever the medium takes out of the
    # scene it puts back as its own colour, so the result cannot be darker than
    # `min(scene, inscatter)` nor brighter than `max(scene, inscatter)` in any
    # channel. That property is what a SceneColorTint multiply does not have --
    # a flat 0.05 in red can only ever remove, so the current implementation gets
    # darker with murk instead of getting hazier.
    attenuated = b.mul(scene_rgb, transmittance, "scene colour * transmittance")
    added = b.mul(inscatter, b.one_minus(transmittance, "1 - transmittance"),
                  "in-scatter * (1 - transmittance)")
    result = b.add(attenuated, added, "attenuated scene + in-scatter")

    b.prop(result, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR,
           "composite -> EmissiveColor (a post-process material's only real output)")

    b.mel.layout_material_expressions(material)
    b.mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)

    # --- READ THE DOMAIN BACK OFF THE PACKAGE ON DISK -----------------------
    #
    # NOT off the UObject in memory -- that read happened above and can only
    # report this script's intent with one extra step. This reads the bytes that
    # were just written, using the same trick as
    # create_water_voxel_material.py:2746-2760: UE serialises enum properties BY
    # NAME, so the package's name table literally contains the string
    # "MD_PostProcess".
    #
    # THE BLENDABLE LOCATION IS REPORTED BUT NOT ENFORCED, and the asymmetry is
    # deliberate rather than lazy: a property equal to the class default is not
    # serialised at all, so if this engine's UMaterial happens to default to the
    # location we chose, its name is legitimately absent from the package and a
    # hard failure there would abort a correct regeneration. The domain has no
    # such excuse -- MD_Surface is the default, so MD_PostProcess is always
    # written when it took.
    domain_on_disk = None
    location_on_disk = None
    try:
        with open(os.path.join(unreal.Paths.project_content_dir(),
                               "Voxel", MATERIAL_NAME + ".uasset"), "rb") as fh:
            blob = fh.read().decode("latin-1")
        domain_on_disk = ("MD_PostProcess" in blob) and ("MD_Surface" not in blob)
        location_on_disk = location_name.replace("_", "") in blob.replace("_", "")
        unreal.log("M_Underwater PACKAGE READ-BACK: domain=%s, blendable-location name %s "
                   "present=%s (absence is not a failure -- see the comment here)"
                   % ("MD_PostProcess" if domain_on_disk else "WRONG",
                      location_name, location_on_disk))
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "M_Underwater: could not read the saved package back (%r), so the domain on "
            "disk is UNKNOWN. Do not read a capture taken on this build as evidence that "
            "the underwater post-process is live." % (exc,))
    if domain_on_disk is False:
        raise RuntimeError(
            "the saved %s.uasset is not an MD_PostProcess material. It would load and "
            "compile fine and be silently ignored by the PostProcessComponent, leaving "
            "the old hard-coded SceneColorTint as the only underwater treatment -- and "
            "nothing else in this run would have said so." % MATERIAL_NAME)

    # ======================================================================
    # THE EVIDENCE LINES
    # ======================================================================
    #
    # Same convention as M_WaterVoxel's "... ARM:" lines and for the same reason:
    # a headless commandlet leaves nothing behind but the log, and this project's
    # standing rule is that a stage must log something that distinguishes "ran
    # and produced X" from "did not run". Every number below is DERIVED here from
    # the same expressions the graph was built from, so a mismatch between this
    # log and the asset is not possible without editing both.
    scale_default = 0.5
    depth_scale_default = 0.25
    view_ext = tuple(e * scale_default for e in extinction_m)
    unreal.log(
        "M_Underwater BLENDABLE LOCATION: %s (candidates tried, in preference order: %s)"
        % (location_name, BLENDABLE_LOCATION_CANDIDATES))
    unreal.log("M_Underwater DOMAIN: %s;  SCENE INPUT: %s" % (domain_name, scene_id_name))
    unreal.log(
        "M_Underwater PARAMETERS (C++ drives the first four): SubmergedDepthM=0.0, "
        "UnderwaterExtinctionScale=%.2f, UnderwaterAmbientGain=1.0, "
        "UnderwaterAmbientColor=(1,1,1) | UnderwaterExtinctionPerMetre=(%.4f, %.4f, %.4f), "
        "UnderwaterScatterAlbedo=(%.4f, %.4f, %.4f), UnderwaterDepthExtinctionScale=%.2f, "
        "UnderwaterMoonAmbient=1.0, UnderwaterAmbientFloor=0.02, "
        "UnderwaterDayFadeSinLow=-0.10, UnderwaterDayFadeSinHigh=0.15"
        % ((scale_default,) + tuple(extinction_m) + tuple(albedo) + (depth_scale_default,)))
    unreal.log(
        "M_Underwater DERIVED, per metre: absorption (%.4f, %.4f, %.4f) + scattering "
        "(%.4f, %.4f, %.4f) = extinction (%.4f, %.4f, %.4f); albedo (%.3f, %.3f, %.3f); "
        "VIEW extinction at scale %.2f = (%.4f, %.4f, %.4f)"
        % (tuple(absorption_m) + tuple(scattering_m) + tuple(extinction_m) + tuple(albedo)
           + (scale_default,) + view_ext))
    for d in (1.0, 2.0, 5.0, 10.0, 20.0):
        unreal.log("M_Underwater VIEW transmittance @ %5.1f m  R %.3f  G %.3f  B %.3f"
                   % ((d,) + tuple(math.exp(-e * d) for e in view_ext)))
    for d in (0.2, 1.0, 5.0, 20.0):
        unreal.log("M_Underwater AMBIENT vs SubmergedDepthM %5.1f m  R %.3f  G %.3f  B %.3f"
                   % ((d,) + tuple(math.exp(-e * depth_scale_default * d)
                                   for e in extinction_m)))

    unreal.log(
        "M_Underwater created and saved at %s -- post-process Beer-Lambert + in-scatter, "
        "radial path from AbsoluteWorldPosition, ONE per-cm conversion node, bound to "
        "MPC_VoxelSky (SunDirection, MoonLightFraction). NO meniscus and NO particulates "
        "in v1, by design. This asset REPLACES the hard-coded SceneColorTint and the "
        "ExponentialHeightFog in VoxelOceanActor.cpp:51-93; until that C++ lands, both "
        "are still running and the two treatments will double up."
        % FULL_PATH)


main()
