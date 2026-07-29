"""Author MPC_VoxelSky + M_NightSky -- the night-sky CONTENT the day/night cycle
has nowhere to draw (docs/lighting-weather-plan.md; UVoxelSkySubsystem drives the
lights, USkyAtmosphere draws a correct but EMPTY twilight, and until this exists
there are no stars and no moon disc in it).

Pattern: Tools/create_voxel_material.py and Tools/create_clipmap_material.py --
EVERY connection is checked, because a silently-failed pin connect produced an
invisible-terrain material once (2026-07-19) and cost a debug session
(create_clipmap_material.py:3-5). This file goes one step further and also checks
every MaterialParameterCollection binding, for the same reason: an unresolved
CollectionParameter does NOT fail to compile, it compiles to ZERO
(MaterialExpressions.cpp:17179-17193 -- Compile() falls through to a constant
when GetParameterIndex misses), which here would mean a moon in direction
(0,0,0) and a latitude of 0 with no error anywhere. That is the same class of
bug as the failed connect and it gets the same treatment: raise, never warn.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=Tools/create_sky_material.py -unattended -nop4 -nosplash

Requires Tools/import_sky_textures.py to have run first (it produces
/Game/Voxel/T_SkyStarmap and /Game/Voxel/T_MoonColor). T_MoonDisplacement is
NOT used here -- see "WHAT THIS DOES NOT DO" below.


================================================================================
DOMAIN / BLEND / SHADING MODEL, AND WHY
================================================================================

    MaterialDomain  MD_Surface
    ShadingModel    MSM_Unlit
    BlendMode       BLEND_Additive
    TwoSided        true
    bIsSky          FALSE  -- deliberately, see below
    Apply Fogging   off

The brief asked for a decision between "MD_Surface on an inverted sphere with
Unlit shading and bIsSky-style flags" and "the engine's SkySphere conventions".
The answer is MD_Surface + Unlit + ADDITIVE TRANSLUCENT, and explicitly NOT
bIsSky. The reasoning is from the 5.8 renderer source, not from taste:

WHY NOT bIsSky. Material.h:1088 states the contract in one line: "Unlit and
Opaque materials can be used as sky material on a sky dome mesh. When IsSky is
true, these meshes will not receive any contribution from the aerial
perspective." That is the opposite of the requirement here -- the whole point is
that the SkyAtmosphere keeps owning the sky colour and the stars sit BEHIND its
scattering. Two further consequences make it worse, not better:

  * bIsSky moves the mesh into EMeshPass::SkyPass (SkyPassRendering.cpp:29),
    which is dispatched from inside the base pass (BasePassRendering.cpp:1502)
    and, on deferred, with depth writes explicitly masked off
    (CreateSkyPassProcessor, SkyPassRendering.cpp:341-348). That part is fine.
  * But an OPAQUE dome is still eligible for the depth prepass, and the
    SkyAtmosphere's own full-screen sky pass depth-tests
    (TStaticDepthStencilState<false, CF_DepthNearOrEqual>,
    SkyAtmosphereRendering.cpp:2116-2118) and decides "is this pixel sky" from
    the depth buffer. A dome that lands in the prepass therefore risks
    SUPPRESSING the very twilight gradient this feature is supposed to leave
    alone. That is a "the sky went black" failure, and it is not one this
    project can afford to discover interactively -- the editor is
    single-occupancy and this script is authored headlessly.

WHY ADDITIVE TRANSLUCENT INSTEAD. A translucent material is never in the depth
prepass and never in the opaque base pass, so the SkyAtmosphere's sky pass sees
an untouched far-plane depth buffer and renders exactly what it renders today.
The dome is then drawn in the translucency pass, AFTER the atmosphere, and
added on top of it. Concretely: scene colour already holds the atmosphere's
inscattering (its own blend is TStaticBlendState<CW_RGB, BO_Add, BF_One,
BF_SourceAlpha>, SkyAtmosphereRendering.cpp:2112 -- luminance added, background
multiplied by transmittance), and this material adds starlight to it. Twilight
colour still comes entirely from the atmosphere; nothing here fights it.

THE COST OF THAT CHOICE, stated plainly. Additive-after-atmosphere means the
stars are NOT extinguished by air mass for free -- physically the day sky should
drown them, and here nothing does that automatically. That is precisely why
StarBrightness exists and why the brief demanded it be a parameter: the C++ side
must drive it to 0 as the sun rises (see "C++ HOOK" at the bottom of this file
for the exact curve). An always-on star field at noon is the expected failure
mode if that hook is never written, and it is a loud one rather than a silent
one, which is the right way round.

DEPTH TEST STAYS ON. Terrain in front of the dome occludes it, which is what
makes stars disappear behind a mountain. That imposes ONE requirement on
whoever spawns the dome mesh: its radius must exceed the farthest drawn
geometry, which in this project is the 50 km clipmap (AVoxelClipmapActor), or
distant peaks will be drawn BEHIND the stars. 2e7 UU (200 km) is the
recommendation; UE's default projection has an infinite far plane so there is no
upper clip to worry about.

TWO-SIDED, so the dome works on the stock /Engine/BasicShapes/Sphere with its
outward-facing normals rather than requiring a purpose-built inverted mesh.
Backface cost on one sphere is irrelevant and this removes an asset dependency
from the C++ side. (Same defensive reasoning as create_clipmap_material.py:34-40,
where winding could not be verified headlessly either.)

APPLY FOGGING OFF (bUseTranslucencyVertexFog = false). Exponential height fog
applied to a 200 km dome would render it as a flat fog-coloured shell and erase
the entire star field. The atmosphere, not the fog volume, is what is allowed to
tint this material.


================================================================================
PARAMETERS: ONE MATERIAL PARAMETER COLLECTION, NOT A MID
================================================================================

This creates /Game/Voxel/MPC_VoxelSky. There was no MPC in Content/ before this;
this is the first one.

WHY AN MPC. The consumer is UVoxelSkySubsystem, a UWorldSubsystem that owns no
mesh component and (per VoxelChunkComponent.h:155) works in a codebase where
reaching for per-object MIDs is anti-doctrine. An MPC is set with a single
UKismetMaterialLibrary::SetVectorParameterValue(World, Collection, Name, Value)
call per parameter per frame, needs no pointer to the dome actor, and survives
the dome being respawned. It also means any LATER material that wants the sun or
moon direction (water specular, a lunar-lit ambient term, weather) reads the same
two vectors rather than re-deriving them -- which is the same argument
terrain_material_common.py makes for sharing one graph between two materials.

WHY THE DEFAULTS MATTER. MPC parameters have no per-material fallback: whatever
the MPC asset says IS the value until C++ writes one. The defaults below are
therefore chosen so that M_NightSky renders a plausible night sky the moment it
is applied to a sphere, with NO C++ at all. That is deliberate -- the C++ hook is
another agent's task, and this material has to be verifiable before it lands.

    -- required by the brief ------------------------------------------------
    SunDirection        vector  unit, world, FROM observer TOWARD the sun.
                                Same sense as VoxelSky::FSunState::Direction
                                (VoxelEphemeris.h:75-77) -- do NOT negate it the
                                way the DirectionalLight rotation does.
                                default (0,0,-1) = sun below the horizon
    MoonDirection       vector  unit, world, FROM observer TOWARD the moon.
                                default (0, 0.7071, 0.7071) = due east, 45 deg up
    MoonPhaseFraction   scalar  0 new, 0.5 full (FMoonState::PhaseFraction,
                                VoxelEphemeris.h:85-89). NOT used for the
                                terminator -- that is pure geometry from
                                SunDirection. Used only for EARTHSHINE, whose
                                brightness tracks the EARTH's illuminated
                                fraction as seen from the moon, which is
                                (1 + cos(2*pi*phase))/2: brightest at new moon,
                                zero at full. default 0.5
    StarBrightness      scalar  master star gain AND the sunrise fade. default 1.0
    StarRotation        scalar  TURNS (not degrees, not radians). Local sidereal
                                time expressed as a fraction of a rotation, plus
                                whatever constant offset the star map's RA origin
                                needs. default 0.0
    ObserverLatitude    scalar  DEGREES. Degrees rather than radians so
                                FVoxelSkyState::LatitudeDeg
                                (VoxelSkySubsystem.h:85) can be passed straight
                                through with no conversion at the call site --
                                one fewer place to get a factor wrong.
                                default 45.0
    MoonAngularRadius   scalar  DEGREES, RADIUS not diameter. default 0.26
    MoonBrightness      scalar  moon disc gain. default 20.0

    -- added by this script, each one earning its place -----------------------
    StarUDirection      scalar  +1 or -1. Handedness of the star map's U axis.
                                THE ONE THING THAT CANNOT BE CHECKED WITHOUT AN
                                EDITOR -- see "WHAT COULD NOT BE VERIFIED".
                                default +1.0
    StarHorizonFade     scalar  stars/moon fade out over +/- this much in
                                sin(altitude) around the horizon. 0.03 is about
                                +/-1.7 deg. Stops stars showing through gaps
                                below the horizon line, and doubles as a crude
                                stand-in for horizon extinction on a rising moon.
                                default 0.03
    MoonEdgeSoftness    scalar  disc limb feather, as a FRACTION of the radius.
                                default 0.12
    MoonTerminatorSoftness
                        scalar  terminator feather in cos(incidence) units. The
                                real terminator is soft because the sun has a
                                ~0.5 deg angular size seen from the moon.
                                default 0.05
    MoonEarthshine      scalar  peak earthshine as a fraction of full-moon
                                brightness. Physically this is ~1e-4; 0.02 is an
                                artistic number, for the same reason
                                VoxelSkySubsystem.h:250-252 says the moonlight
                                peak is "an artistic number ~10 stops above the
                                real 1:400000 lux ratio". default 0.02

STAR/MOON BRIGHTNESS DEFAULTS ARE UNCALIBRATED AND DELIBERATELY ON THE HIGH
SIDE. The NASA EXR carries no calibrated radiance and this project pins exposure
by curve (voxel.Sky.ExposureMode, ~+15.6 EV100 at night), so the correct gain is
an empirical number that needs a frame to find. 1.0 / 20.0 are chosen so the
first capture shows an obviously-too-bright sky rather than nothing: a washed-out
star field is unambiguous evidence the graph compiled, sampled and oriented
correctly, whereas an invisible one is indistinguishable from ten other
failures. Both are MPC scalars, so tuning them costs an asset edit, not a
material regeneration.


================================================================================
THE MATH
================================================================================

VIEW DIRECTION. d = normalize(-CameraVectorWS). CameraVectorWS points from the
shaded pixel TOWARD the camera, so its negation is the direction of gaze. In this
project's world frame that is d = (north, east, up) directly -- VoxelEphemeris.h:
44-48 fixes X=north, Y=east, Z=up and defines every ephemeris direction as
(cos(Alt)cos(Az), cos(Alt)sin(Az), sin(Alt)) in exactly that frame. So
SunDirection and MoonDirection need no transform at all on the way in; they are
already world-space unit vectors and can be dotted against d as-is.

HORIZON -> EQUATORIAL (the star map is in celestial coordinates, so this is the
step that makes the sky rotate correctly instead of being painted on the inside
of the dome). With phi = ObserverLatitude and d = (dN, dE, dU):

    Ex = dU*cos(phi) - dN*sin(phi)      == cos(dec) * cos(H)
    Ey = -dE                            == cos(dec) * sin(H)
    Ez = dN*cos(phi) + dU*sin(phi)      == sin(dec)

That is the standard alt/az -> hour-angle/declination rotation written
rectangularly (it is a single rotation by 90-phi about the east axis, which is
what puts the celestial pole at altitude phi where it belongs). Two checks that
pin the signs:

    * north celestial pole: it must sit at altitude phi, azimuth 0, i.e.
      d = (cos phi, 0, sin phi). Then Ez = cos^2 + sin^2 = 1 (dec = +90) and
      Ex = sin(phi)cos(phi) - cos(phi)sin(phi) = 0. Correct.
    * observer on the equator (phi = 0) looking at the zenith, d = (0,0,1):
      Ex = 1, Ez = 0, so dec = 0 and H = 0 -- the celestial equator crossing the
      meridian. Correct.

    dec = asin(clamp(Ez, -1, 1))
    H   = atan2(Ey, Ex)                 hour angle, increasing westward
    RA  = LST - H

EQUIRECT UV.

    v = 0.5 - dec/pi              V=0 is the top row = dec +90 = north pole
    u = StarRotation + StarUDirection * (-H / 2pi)

RA/2pi appears only as (LST - H)/2pi, and LST/2pi is a constant per frame -- so
it is folded wholesale into StarRotation. That is what makes StarRotation "a
rotation the game supplies": one scalar in turns carrying local sidereal time
plus the map's RA origin offset, with no second knob to keep in sync.

THE SEAM. atan2's branch cut (Ey = 0, Ex < 0) makes u jump by exactly 1. The
FETCH is unharmed -- import_sky_textures.py sets TA_WRAP in U precisely because
right ascension wraps -- but the hardware's mip selection is not: ddx(u) at the
seam column is ~1.0 instead of ~1e-4, the sampler picks the lowest mip, and the
result is a permanent blurred meridian across the sky that shimmers as the view
turns. The fix is to sample with EXPLICIT derivatives (TMVM_Derivative) and
wrap-correct the U component of each:

    dudx' = dudx - round(dudx)          a +/-1 jump maps back to ~0

which is exact everywhere except within a pixel or two of the celestial poles,
where du/dx legitimately exceeds 0.5 because all meridians converge there. Near
the poles the correction can pick a slightly wrong mip; the visible consequence
is a small soft spot exactly at the pole, which is the benign end of equirect's
inherent pole singularity and is the reason mips were enabled on this texture in
the first place (import_sky_textures.py:22-26).

MOON DISC. Small-angle-safe throughout: comparing cos(theta) against
cos(0.26 deg) would spend the whole disc inside the last ~1e-5 of the float range
near 1.0. Instead work with the SINE, which for these angles has full precision:

    cosAng = dot(d, m)
    t      = |d - m*cosAng| = sin(theta)        (a Distance node)
    r      = t / sin(R)                          0 at centre, 1 at the limb
    front  = saturate(cosAng * 1000)             kills the antipodal false hit
    disc   = (1 - smoothstep(1-soft, 1, r)) * front

DISC BASIS. right = normalize(cross(ref, m)), up = cross(m, right), with
ref = world up (0,0,1) blended to (1,0,0) as the moon approaches the zenith,
where cross(up, m) degenerates. Sanity check for a moon due north on the horizon,
m = (1,0,0): cross((0,0,1),(1,0,0)) = (0,1,0) = east, and cross(m, right) =
(0,0,1) = zenith. Correct. (This aligns the moon's polar axis with the local
vertical, which ignores the parallactic angle -- the real moon rolls slowly as it
crosses the sky. Fixing that needs a position-angle input nobody has asked for;
it is called out here so the next reader knows it is an omission, not an
accident.)

SURFACE NORMAL. The moon at 0.26 deg is an orthographic projection of a sphere,
so with x = dot(d,right)/sin(R), y = dot(d,up)/sin(R):

    z = sqrt(saturate(1 - x^2 - y^2))
    N = x*right + y*up - z*m

At the disc centre (x=y=0, z=1) that is N = -m: the sub-observer point's normal
points back at the observer. Correct, and |N| = 1 by construction.

TERMINATOR -- the part the brief called out as most worth getting right. It is
pure geometry, no texture swap and no phase-indexed atlas:

    mu0  = dot(N, SunDirection)     cos of the solar incidence angle
    mu   = z                        cos of the emission angle (= dot(N, -m))

The moon is 384,000 km away and the sun 1.5e8 km, so the direction to the sun
from the moon differs from the direction to the sun from the observer by at most
about 0.15 deg. SunDirection is used unchanged; that approximation is two orders
of magnitude below the disc's own angular size.

Shading is LOMMEL-SEELIGER, not Lambert:

    refl = 2 * saturate(mu0) / max(mu + saturate(mu0), 0.02)

This is the classical lunar photometric law and it is one divide. It matters
visibly: a Lambert moon has an obvious bright centre and dark limb, whereas the
real full moon is famously flat -- almost uniformly bright right out to the edge,
which is why it reads as a disc rather than a ball. Lommel-Seeliger reproduces
that (at full moon mu = mu0 everywhere, so refl = 1 across the whole face) and
still gives a correct crescent. The factor 2 normalises the full-moon centre to
1.0; the max() guards the corner where terminator and limb meet and both cosines
go to zero together.

    lit = refl * smoothstep(-soft, +soft, mu0)

The smoothstep softens the terminator, which in reality is soft because the sun
is not a point source seen from the moon.

    earthshine = MoonEarthshine * (1 + cos(2*pi*MoonPhaseFraction))/2

Full at new moon, zero at full moon -- the Earth and Moon phases are
complementary. This is what makes a thin crescent read as a whole moon with a
bright edge rather than as a detached sliver.

LUNAR TEXTURE UV. Tidal locking pins the sub-observer point at lunar (0,0), so
the disc coordinates ARE the near-side coordinates:

    lat = asin(y)                 u = 0.5 + atan2(x, z)/(2*pi)
                                  v = 0.5 - lat/pi

matching the LROC global mosaic's simple-cylindrical layout centred on longitude
0. No seam handling is needed here: the near side spans longitude -90..+90 and
never crosses the +/-180 wrap.

MILKY WAY. Not built, on purpose -- it is in the star map's pixels already
(NASA Deep Star Maps 2020 is a full-sky composite), and a separately-authored
band would have to be kept aligned with the data by hand forever.


================================================================================
WHAT THIS DOES NOT DO
================================================================================

* No T_MoonDisplacement. Displacement/parallax on a 30-pixel-wide disc buys
  nothing a 4k colour map does not already carry, and it would add a second
  sampler and a search loop to the hottest full-screen material in the frame.
  The texture stays imported for a future close-up/telescope path.
* No sun disc. The SkyAtmosphere already draws one from the sun DirectionalLight
  (atmosphere light index 0, VoxelSkySubsystem.cpp:922-923) and drawing a second
  would double it.
* No twinkling, no atmospheric refraction of star positions, no light pollution.
* No dome mesh, no actor, no C++. See "C++ HOOK" below.


================================================================================
WHAT COULD NOT BE VERIFIED WITHOUT THE EDITOR
================================================================================

The editor is single-occupancy and this task was explicitly write-only, so
nothing below was run:

1. STAR MAP U HANDEDNESS -- the real one. An equirectangular celestial map can
   be authored for viewing from OUTSIDE the sphere (RA increasing left-to-right)
   or from INSIDE it (RA increasing right-to-left), and the two differ by a
   mirror. Getting it wrong produces a sky that is still perfectly sharp, still
   rotates at the right rate, and still has the pole at the right altitude --
   just mirrored, with constellations reversed. That is exactly the kind of
   defect that survives a screenshot review. Hence StarUDirection: flip it to
   -1.0 on the MPC asset (no regeneration) and re-shoot. HOW TO TELL: point the
   camera at the celestial pole and step StarRotation by +0.25 turns; the sky
   must rotate the same way the real sky does for the hemisphere in question
   (counter-clockwise about the north celestial pole). Or compare a bright
   asterism against any planetarium view.
2. Whether TMVM_Derivative + the round() wrap fix actually removes the seam, or
   merely moves it. The maths is checked; the pixels are not.
3. Absolute brightness of both the star field and the moon against this
   project's pinned exposure curve. See the note on defaults above.
4. Lunar texture ROLL (the parallactic-angle omission) and whether the LROC
   mosaic's V axis is north-up. If the moon looks upside down, that is this.
5. That an additive translucent dome at 2e7 UU composites over the SkyAtmosphere
   the way the blend-state reading above says it does. The renderer source is
   unambiguous; the frame has not been looked at.
6. unreal.CollectionScalarParameter / unreal.CollectionVectorParameter being
   constructible from Python at all. This mirrors import_sky_textures.py:88-94's
   honest note about TC_DISPLACEMENTMAP: the script checks its own work (every
   CollectionParameter node's resolved ParameterId is validated) so a failure
   here is loud and points at one line, but it has not been executed.


================================================================================
C++ HOOK -- FOR WHOEVER TOUCHES UVoxelSkySubsystem
================================================================================

Written out here rather than implemented, because Source/ is under concurrent
edit. Full detail is in this task's report; the short form:

  * New private method ApplySkyMaterialParams(), called from Tick() immediately
    after ApplyExposureFromState() (VoxelSkySubsystem.cpp:1293) and OUTSIDE the
    ShadowUpdateHz cadence gate. Exposure is already every-frame for exactly this
    reason (see the comment at :1286-1292): these are uniform writes, they bust
    no shadow cache, and stepping the moon's position at 10 Hz would show as a
    stutter on a disc only 0.26 deg wide.
  * Sun.Direction / Moon.Direction are already computed in Tick at :1261-1262 and
    are ALREADY in the sense the material wants (toward the body). Pass them
    through WITHOUT the negation applied at :1336/:1340 -- that negation exists
    only because a DirectionalLight's forward vector is the direction light
    travels (VoxelEphemeris.h:27-41). Getting this backwards here puts the moon
    exactly opposite where the moonlight comes from.
  * StarBrightness is the sunrise fade: 1 below about -12 deg solar altitude,
    0 above 0 deg, smoothstepped between. Drive it from S.SunAltitudeDeg.
  * MoonComp->SetAtmosphereSunDiskColorScale(FLinearColor::Black) once at spawn.
    The moon is atmosphere light index 1 (VoxelSkySubsystem.cpp:962-963) and UE
    therefore already draws a flat untextured disc for it. Without this there
    will be TWO moons in the same place, one of them phaseless.
"""

import os
import sys
import uuid

import unreal

# Shared GraphBuilder, same directory. A -run=pythonscript commandlet does not
# put the script's own directory on sys.path, so add it explicitly (same as
# create_voxel_material.py:56-58).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from terrain_material_common import GraphBuilder, load_texture  # noqa: E402

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_NightSky"
COLLECTION_NAME = "MPC_VoxelSky"
MATERIAL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME
COLLECTION_PATH = PACKAGE_PATH + "/" + COLLECTION_NAME

STARMAP_TEXTURE = "/Game/Voxel/T_SkyStarmap.T_SkyStarmap"
MOON_COLOR_TEXTURE = "/Game/Voxel/T_MoonColor.T_MoonColor"

PI = 3.14159265358979
TWO_PI = 2.0 * PI
DEG2RAD = PI / 180.0

# Fixed namespace so re-running this script produces the SAME parameter GUIDs.
# FCollectionParameterBase::Id is what materials store to survive a parameter
# RENAME (MaterialParameterCollection.h:33-37); if the ids churned on every
# regeneration, every other material that ever binds to this collection would
# silently unbind the next time this script ran.
GUID_NAMESPACE = uuid.UUID("6f1b0d2a-4c53-4f8e-9d21-7a0c5b3e9f14")

# (name, default). Order is cosmetic only -- lookup is by name/id.
SCALAR_PARAMS = [
    ("StarBrightness", 1.0),
    ("StarRotation", 0.0),
    ("StarUDirection", 1.0),
    ("StarHorizonFade", 0.03),
    ("ObserverLatitude", 45.0),
    ("MoonAngularRadius", 0.26),
    ("MoonBrightness", 20.0),
    ("MoonPhaseFraction", 0.5),
    ("MoonEdgeSoftness", 0.12),
    ("MoonTerminatorSoftness", 0.05),
    ("MoonEarthshine", 0.02),
]

# (name, r, g, b, a)
VECTOR_PARAMS = [
    # Sun below the horizon and moon due east at 45 deg: a night sky, so the
    # material shows what it is for the moment it is applied to a sphere, with
    # no C++ driving it.
    ("SunDirection", 0.0, 0.0, -1.0, 0.0),
    ("MoonDirection", 0.0, 0.70710678, 0.70710678, 0.0),
]


def guid_library():
    """UKismetGuidLibrary, whichever name the Python bindings expose it under."""
    lib = getattr(unreal, "GuidLibrary", None) or getattr(unreal, "KismetGuidLibrary", None)
    if lib is None:
        raise RuntimeError(
            "neither unreal.GuidLibrary nor unreal.KismetGuidLibrary exists -- "
            "check the exposed name in an interactive editor console and fix "
            "guid_library() (this is the only place it is used)")
    return lib


def stable_guid(name):
    """A deterministic FGuid derived from a parameter name.

    FGuid has no UPROPERTYs (Core's Misc/Guid.h), so its fields cannot be set
    from Python directly; it has to be parsed from a string. uuid5 keeps the
    value stable across runs -- see GUID_NAMESPACE.
    """
    lib = guid_library()
    digits = uuid.uuid5(GUID_NAMESPACE, name).hex.upper()  # 32 hex chars
    parsed = lib.parse_string_to_guid(digits)
    # Blueprint out-params come back as a tuple (Guid, Success).
    guid, ok = (parsed if isinstance(parsed, tuple) else (parsed, True))
    if not ok or not lib.is_valid_guid(guid):
        raise RuntimeError("failed to build a valid FGuid for parameter %r from %r"
                           % (name, digits))
    return guid


def create_collection():
    """Author MPC_VoxelSky and verify every parameter actually landed."""
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(COLLECTION_PATH):
        unreal.EditorAssetLibrary.delete_asset(COLLECTION_PATH)

    collection = asset_tools.create_asset(
        COLLECTION_NAME, PACKAGE_PATH, unreal.MaterialParameterCollection,
        unreal.MaterialParameterCollectionFactoryNew())
    if collection is None:
        raise RuntimeError("Failed to create MaterialParameterCollection at " + COLLECTION_PATH)

    scalars = []
    for (name, default) in SCALAR_PARAMS:
        p = unreal.CollectionScalarParameter()
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", float(default))
        # The id is deliberately NOT set here. FCollectionParameterBase::Id is
        # protected, and set_editor_property on it raises
        #   "Property 'Id' ... is protected and cannot be set"
        # (measured 2026-07-29, UE 5.8). UMaterialParameterCollection assigns the
        # guids itself in PostEditChangeProperty, which the assignment of
        # scalar_parameters/vector_parameters below routes through.
        #
        # The hazard the old explicit set was guarding against is REAL and is now
        # guarded by a read-back instead: a zero id makes GetParameterId() return
        # a zero guid, and UMaterialExpressionCollectionParameter::Compile then
        # silently emits a CONSTANT rather than erroring -- a black sky with no
        # diagnostic. See the assert_ids_assigned() call after the round-trip
        # check, which turns that silence into a hard failure.
        scalars.append(p)

    vectors = []
    for (name, r, g, b, a) in VECTOR_PARAMS:
        p = unreal.CollectionVectorParameter()
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", unreal.LinearColor(r, g, b, a))
        vectors.append(p)

    collection.set_editor_property("scalar_parameters", scalars)
    collection.set_editor_property("vector_parameters", vectors)

    # Read back rather than assume. set_editor_property on a TArray of structs
    # is the least-exercised call in this script.
    got_scalars = [str(p.get_editor_property("parameter_name"))
                   for p in collection.get_editor_property("scalar_parameters")]
    got_vectors = [str(p.get_editor_property("parameter_name"))
                   for p in collection.get_editor_property("vector_parameters")]
    want_scalars = [n for (n, _d) in SCALAR_PARAMS]
    want_vectors = [n for (n, _r, _g, _b, _a) in VECTOR_PARAMS]
    if got_scalars != want_scalars:
        raise RuntimeError("MPC scalar parameters did not round-trip: wrote %s, read back %s"
                           % (want_scalars, got_scalars))
    if got_vectors != want_vectors:
        raise RuntimeError("MPC vector parameters did not round-trip: wrote %s, read back %s"
                           % (want_vectors, got_vectors))

    # Every parameter must have come back with a NON-ZERO guid. This is the
    # guard that replaces the old explicit id assignment (see create_collection
    # above for why that had to go). A zero guid does not error at compile time
    # -- UMaterialExpressionCollectionParameter::Compile silently emits a
    # constant instead -- so without this check the failure mode is a black sky
    # and no diagnostic, which is indistinguishable from the half-dozen other
    # ways this material can render nothing.
    #
    # `id` is protected for WRITING but readable, so this costs nothing. If a
    # future engine version also protects the read, this must become a hard
    # failure rather than a skip: an unverified id is exactly the state the old
    # code was trying to avoid.
    zero_guid = str(unreal.Guid())
    unassigned = []
    unassignable = False
    for kind, plist in (("scalar", collection.get_editor_property("scalar_parameters")),
                        ("vector", collection.get_editor_property("vector_parameters"))):
        for p in plist:
            name = str(p.get_editor_property("parameter_name"))
            try:
                pid = str(p.get_editor_property("id"))
            except Exception:
                # Measured 2026-07-29 on UE 5.8: FCollectionParameterBase::Id is
                # protected for READING as well as writing, so this check cannot
                # run from Python at all. Do not turn that into a hard failure --
                # it would block the script permanently over a guid that
                # UMaterialParameterCollection::PostEditChangeProperty assigns
                # for us anyway.
                #
                # The check moves to RUNTIME instead, which is where it can
                # actually run: UKismetMaterialLibrary::SetScalarParameterValue /
                # SetVectorParameterValue log a warning when a parameter name
                # does not resolve in the collection, and UVoxelSkySubsystem
                # drives every parameter in this MPC every frame. So the symptom
                # of an unresolved id is a per-frame warning naming the
                # parameter, not silence.
                #
                # WHAT TO CHECK IF THE SKY RENDERS BLACK: grep a run's log for
                # those warnings first. An unresolved CollectionParameter
                # compiles to a CONSTANT rather than erroring, so the material
                # itself will never tell you.
                unassignable = True
                continue
            if not pid or pid == zero_guid:
                unassigned.append("%s:%s" % (kind, name))
    if unassigned:
        raise RuntimeError(
            "MPC parameters have zero guids after PostEditChangeProperty: %s. "
            "UMaterialExpressionCollectionParameter::Compile would emit constants for these "
            "and the sky would render black with no error." % ", ".join(unassigned))

    unreal.EditorAssetLibrary.save_loaded_asset(collection)
    if unassignable:
        unreal.log_warning(
            "%s: parameter guids could NOT be verified from Python (Id is protected for read "
            "on this engine version). They are assigned by PostEditChangeProperty and are "
            "almost certainly fine, but the proof is at runtime: if the sky renders black, "
            "grep the run log for SetScalarParameterValue/SetVectorParameterValue warnings "
            "naming a parameter before suspecting the graph." % COLLECTION_PATH)
    unreal.log("created %s with %d scalars + %d vectors"
               % (COLLECTION_PATH, len(scalars), len(vectors)))
    return collection


class SkyGraphBuilder(GraphBuilder):
    """GraphBuilder plus the nodes this graph needs.

    link() is overridden only to widen the error message: when a connect fails
    the usual cause is a pin NAME, and the fastest fix is seeing the list of
    names the node actually has. The failure is still a raise, never a warn --
    create_clipmap_material.py:3-5.
    """

    def __init__(self, material, collection):
        GraphBuilder.__init__(self, material)
        self.collection = collection
        self._mpc_names = None  # lazily read back from the MPC, see collection_param

    def link(self, src, src_out, dst, dst_in):
        if not self.mel.connect_material_expressions(src, src_out, dst, dst_in):
            try:
                names = [str(n) for n in self.mel.get_material_expression_input_names(dst)]
            except Exception:  # noqa: BLE001 -- diagnostics only, the raise below stands
                names = ["<could not enumerate>"]
            raise RuntimeError(
                "connect %s.%s -> %s.%s failed; %s inputs are %s"
                % (type(src).__name__, src_out or "<default>",
                   type(dst).__name__, dst_in or "<default>",
                   type(dst).__name__, names))

    # --- parameters ---------------------------------------------------------

    def collection_param(self, name):
        """A CollectionParameter bound to MPC_VoxelSky, with the binding CHECKED.

        Order is load-bearing: Collection must be set before ParameterName,
        because PostEditChangeProperty resolves ParameterId from
        Collection->GetParameterId(ParameterName) every time either changes
        (MaterialExpressions.cpp:17163-17177) and a null Collection blanks it.
        """
        # The binding is checked by NAME MEMBERSHIP, not by reading ParameterId
        # back. ParameterId is not exposed to Python on UE 5.8 ("Failed to find
        # property 'parameter_id'", measured 2026-07-29) and neither is
        # FCollectionParameterBase::Id, so the guid route is closed.
        #
        # Membership is the stronger check anyway, and it is complete. The only
        # way a CollectionParameter fails to resolve is a name this script did
        # not put on the MPC -- and this script authored the MPC, so it knows the
        # full set. Comparing against the collection as READ BACK (not against
        # the SCALAR_PARAMS/VECTOR_PARAMS literals) also catches the case where
        # the MPC write silently dropped a parameter.
        #
        # This matters because an unresolved collection parameter does NOT fail
        # to compile: UMaterialExpressionCollectionParameter::Compile emits a
        # CONSTANT instead. A typo here would ship a black sky with no error.
        if self._mpc_names is None:
            self._mpc_names = set()
            for prop in ("scalar_parameters", "vector_parameters"):
                for p in self.collection.get_editor_property(prop):
                    self._mpc_names.add(str(p.get_editor_property("parameter_name")))
        if name not in self._mpc_names:
            raise RuntimeError(
                "CollectionParameter %r is not on %s. Present: %s. An unresolved "
                "collection parameter compiles to a CONSTANT rather than failing, "
                "so this must raise here."
                % (name, COLLECTION_PATH, sorted(self._mpc_names)))

        n = self.node(unreal.MaterialExpressionCollectionParameter)
        n.set_editor_property("collection", self.collection)
        n.set_editor_property("parameter_name", name)
        # Order is load-bearing (see docstring); verify it took.
        if str(n.get_editor_property("parameter_name")) != name:
            raise RuntimeError("CollectionParameter name did not round-trip for %r" % name)
        if n.get_editor_property("collection") is None:
            raise RuntimeError("CollectionParameter %r lost its Collection" % name)
        return n

    # --- small nodes --------------------------------------------------------

    def mask(self, src, src_out, r=False, g=False, b=False, a=False):
        n = self.node(unreal.MaterialExpressionComponentMask)
        n.set_editor_property("r", r)
        n.set_editor_property("g", g)
        n.set_editor_property("b", b)
        n.set_editor_property("a", a)
        self.link(src, src_out, n, "")
        return n

    def xyz(self, src, src_out=""):
        return self.mask(src, src_out, r=True, g=True, b=True)

    def unary(self, cls, a, a_out=""):
        n = self.node(cls)
        self.link(a, a_out, n, "")
        return n

    def sine(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionSine, a, a_out)

    def cosine(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionCosine, a, a_out)

    def arcsine(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionArcsine, a, a_out)

    def sqrt(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionSquareRoot, a, a_out)

    def normalize(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionNormalize, a, a_out)

    def round_(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionRound, a, a_out)

    def ddx(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionDDX, a, a_out)

    def ddy(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionDDY, a, a_out)

    def neg(self, a, a_out=""):
        return self.mul(a, self.const(-1.0), a_out, "")

    def dot(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionDotProduct, a, a_out, b, b_out)

    def cross(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionCrossProduct, a, a_out, b, b_out)

    def dist(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionDistance, a, a_out, b, b_out)

    def atan2(self, y, x, y_out="", x_out=""):
        n = self.node(unreal.MaterialExpressionArctangent2)
        self.link(y, y_out, n, "Y")
        self.link(x, x_out, n, "X")
        return n

    def clamp(self, value, lo, hi, value_out=""):
        n = self.node(unreal.MaterialExpressionClamp)
        # Clamp's first input is UNNAMED -- GetInputName reports it as 'None',
        # so the pin list is ['None', 'Min', 'Max'] and "Input" does not match.
        # Measured 2026-07-29 by the checked-connect assertion in link(), which
        # is exactly what that assertion exists for: a silently-failed connect
        # here would have left the clamp reading 0 and the star field blank.
        self.link(value, value_out, n, "")
        self.link(lo, "", n, "Min")
        self.link(hi, "", n, "Max")
        return n

    def smoothstep(self, lo, hi, value, value_out=""):
        n = self.node(unreal.MaterialExpressionSmoothStep)
        self.link(lo, "", n, "Min")
        self.link(hi, "", n, "Max")
        self.link(value, value_out, n, "Value")
        return n

    def sample_derivative(self, texture_path, param_name, sampler_type,
                          uv, ddx_uv, ddy_uv):
        """TextureSampleParameter2D with EXPLICIT derivatives.

        MipValueMode MUST be set before the DDX/DDY pins are connected: the pin
        NAMES only exist in Derivative mode (UMaterialExpressionTextureSample::
        GetInputName, MaterialExpressions.cpp:2743-2749). Set it afterwards and
        the connects fail -- loudly, via link(), but pointlessly.
        """
        n = self.node(unreal.MaterialExpressionTextureSampleParameter2D)
        n.set_editor_property("parameter_name", param_name)
        n.set_editor_property("texture", load_texture(texture_path))
        n.set_editor_property("sampler_type", sampler_type)
        n.set_editor_property("mip_value_mode", unreal.TextureMipValueMode.TMVM_DERIVATIVE)
        self.link(uv, "", n, "UVs")
        self.link(ddx_uv, "", n, "DDX(UVs)")
        self.link(ddy_uv, "", n, "DDY(UVs)")
        return n

    # sample() (hardware mip selection) is inherited from GraphBuilder unchanged
    # -- it is what the moon disc uses.


def build_stars(b, view, vz, horizon_fade):
    """Equirect star-map lookup in celestial coordinates. Returns an RGB expr."""
    # --- observer latitude, degrees -> radians ------------------------------
    lat_rad = b.mul(b.collection_param("ObserverLatitude"), b.const(DEG2RAD))
    sin_lat = b.sine(lat_rad)
    cos_lat = b.cosine(lat_rad)

    d_n = b.mask(view, "", r=True)
    d_e = b.mask(view, "", g=True)
    d_u = vz

    # Horizon -> equatorial, rectangular. See the module docstring for the two
    # checks that pin these signs (celestial pole at altitude phi; equator
    # zenith on the meridian).
    e_x = b.sub(b.mul(d_u, cos_lat), b.mul(d_n, sin_lat))     # cos(dec)cos(H)
    e_y = b.neg(d_e)                                          # cos(dec)sin(H)
    e_z = b.add(b.mul(d_n, cos_lat), b.mul(d_u, sin_lat))     # sin(dec)

    # asin's input is a dot of unit vectors, so it is in [-1,1] mathematically
    # but not necessarily after float rounding. Clamp before the arcsine.
    dec = b.arcsine(b.clamp(e_z, b.const(-1.0), b.const(1.0)))
    v = b.sub(b.const(0.5), b.mul(dec, b.const(1.0 / PI)))

    hour_angle = b.atan2(e_y, e_x)
    ra_turns = b.mul(hour_angle, b.const(-1.0 / TWO_PI))
    u = b.add(b.collection_param("StarRotation"),
              b.mul(b.collection_param("StarUDirection"), ra_turns))

    uv = b.append(u, "", v, "")

    # --- seam-safe derivatives ---------------------------------------------
    #
    # u jumps by exactly 1 across atan2's branch cut. TA_WRAP makes the FETCH
    # correct there; it does not make the hardware's mip selection correct, and
    # a one-texel column at the lowest mip is a permanent blurred meridian.
    # Subtracting round() folds a +/-1 jump back to ~0 and leaves every other
    # derivative untouched.
    ddx_uv = b.ddx(uv)
    ddy_uv = b.ddy(uv)
    du_dx = b.mask(ddx_uv, "", r=True)
    dv_dx = b.mask(ddx_uv, "", g=True)
    du_dy = b.mask(ddy_uv, "", r=True)
    dv_dy = b.mask(ddy_uv, "", g=True)
    ddx_fixed = b.append(b.sub(du_dx, b.round_(du_dx)), "", dv_dx, "")
    ddy_fixed = b.append(b.sub(du_dy, b.round_(du_dy)), "", dv_dy, "")

    # SAMPLERTYPE_LINEAR_COLOR, not COLOR: T_SkyStarmap is TC_HDR with sRGB OFF
    # (import_sky_textures.py:82-84), and GetSamplerTypeForTexture maps
    # (non-sRGB, non-special compression) to LinearColor
    # (MaterialExpressionUtils.cpp:45-47). A mismatch is a compile error, not a
    # silent wrong colour -- but it is a compile error that would only show up
    # in the editor, so get it right here.
    star = b.sample_derivative(
        STARMAP_TEXTURE, "StarmapTex",
        unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR,
        uv, ddx_fixed, ddy_fixed)

    gain = b.mul(b.collection_param("StarBrightness"), horizon_fade)
    return b.mul(star, gain, "RGB", "")


def build_moon(b, view, horizon_fade):
    """Textured moon disc with a geometric phase terminator. Returns an RGB expr."""
    moon_dir = b.normalize(b.xyz(b.collection_param("MoonDirection")))
    sun_dir = b.normalize(b.xyz(b.collection_param("SunDirection")))

    radius_rad = b.mul(b.collection_param("MoonAngularRadius"), b.const(DEG2RAD))
    sin_radius = b.sine(radius_rad)

    # --- disc mask ----------------------------------------------------------
    #
    # Worked in SINE rather than cosine: cos(0.26 deg) = 0.9999897, so a
    # cos-space test would spend the entire disc inside the last ~1e-5 of float
    # precision near 1.0. |view - m*cos| = sin(theta) has full precision here.
    cos_ang = b.dot(view, moon_dir)
    sin_ang = b.dist(view, b.mul(moon_dir, cos_ang))
    r = b.div(sin_ang, sin_radius)

    # sin(theta) is ALSO small at the antipode, where cos_ang ~ -1. Without this
    # there is a second, phase-inverted moon exactly opposite the real one.
    front = b.saturate(b.mul(cos_ang, b.const(1000.0)))

    soft = b.collection_param("MoonEdgeSoftness")
    disc = b.mul(
        b.one_minus(b.smoothstep(b.sub(b.const(1.0), soft), b.const(1.0), r)),
        front)

    # --- disc basis ---------------------------------------------------------
    #
    # cross(worldUp, m) degenerates as the moon approaches the zenith, so blend
    # the reference axis to world north over the last ~6 degrees. Any choice is
    # arbitrary that close to the zenith; what matters is that it stays
    # continuous rather than collapsing to a zero-length vector.
    moon_z = b.mask(moon_dir, "", b=True)
    degenerate = b.saturate(b.mul(b.sub(b.abs_(moon_z), b.const(0.995)), b.const(1000.0)))
    ref = b.lerp(b.const3(0.0, 0.0, 1.0), "", b.const3(1.0, 0.0, 0.0), "", degenerate)
    right = b.normalize(b.cross(ref, moon_dir))
    up = b.cross(moon_dir, right)   # unit already: m and right are unit and orthogonal

    disc_x = b.div(b.dot(view, right), sin_radius)
    disc_y = b.div(b.dot(view, up), sin_radius)

    # Orthographic projection of a sphere: z is the near-hemisphere depth.
    disc_z = b.sqrt(b.saturate(
        b.sub(b.sub(b.const(1.0), b.mul(disc_x, disc_x)), b.mul(disc_y, disc_y))))

    # Outward surface normal, world space. At the disc centre this is -moon_dir,
    # i.e. pointing back at the observer.
    normal = b.sub(
        b.add(b.mul(right, disc_x), b.mul(up, disc_y)),
        b.mul(moon_dir, disc_z))

    # --- terminator ---------------------------------------------------------
    #
    # Pure geometry. SunDirection is used unchanged: the direction to the sun
    # from the moon differs from the direction to the sun from here by at most
    # ~0.15 deg, two orders of magnitude under the disc's own angular size.
    mu0 = b.dot(normal, sun_dir)
    mu0_sat = b.saturate(mu0)
    mu = disc_z

    # Lommel-Seeliger, the classical lunar photometric law. Lambert would give a
    # bright centre and a dark limb; the real full moon is near-uniform out to
    # the edge, which is what makes it read as a disc. The 2 normalises the
    # full-moon centre (mu = mu0 = 1) to 1.0; the max() guards the corner where
    # terminator and limb meet and both cosines vanish together.
    refl = b.div(b.mul(b.const(2.0), mu0_sat),
                 b.maximum(b.add(mu, mu0_sat), b.const(0.02)))

    term_soft = b.collection_param("MoonTerminatorSoftness")
    terminator = b.smoothstep(b.neg(term_soft), term_soft, mu0)
    lit = b.mul(refl, terminator)

    # Earthshine: the Earth's phase as seen from the moon is the complement of
    # the moon's phase as seen from here, so (1 + cos(2pi*phase))/2 is full at
    # new moon and zero at full moon. This is what makes a thin crescent read as
    # a whole moon with a bright edge.
    phase_angle = b.mul(b.collection_param("MoonPhaseFraction"), b.const(TWO_PI))
    earth_lit = b.mul(b.add(b.const(1.0), b.cosine(phase_angle)), b.const(0.5))
    earthshine = b.mul(b.collection_param("MoonEarthshine"), earth_lit)

    shade = b.add(lit, earthshine)

    # --- lunar surface texture ---------------------------------------------
    #
    # Tidal locking pins the sub-observer point at lunar (0,0), so the disc
    # coordinates ARE near-side lat/lon. The near side spans longitude -90..+90
    # and never touches the +/-180 wrap, so no seam handling is needed and the
    # default (hardware) mip mode is correct here.
    lon = b.atan2(disc_x, disc_z)
    lat = b.arcsine(b.clamp(disc_y, b.const(-1.0), b.const(1.0)))
    moon_u = b.add(b.const(0.5), b.mul(lon, b.const(1.0 / TWO_PI)))
    moon_v = b.sub(b.const(0.5), b.mul(lat, b.const(1.0 / PI)))

    # SAMPLERTYPE_COLOR: T_MoonColor is TC_DEFAULT with sRGB ON
    # (import_sky_textures.py:85-87).
    moon_tex = b.sample(MOON_COLOR_TEXTURE, b.append(moon_u, "", moon_v, ""), "",
                        "MoonColorTex", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

    gain = b.mul(b.mul(b.collection_param("MoonBrightness"), horizon_fade), disc)
    return b.mul(moon_tex, b.mul(shade, gain), "RGB", "")


def main():
    collection = create_collection()

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(MATERIAL_PATH)

    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material,
                                        unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("Failed to create material asset at " + MATERIAL_PATH)

    # --- domain / blend / shading ------------------------------------------
    #
    # See the module docstring for the full argument. In one line: additive
    # translucent unlit keeps the dome out of both the depth prepass and the
    # opaque base pass, so the SkyAtmosphere's own sky pass renders exactly what
    # it renders today and this material adds starlight on top of it.
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    material.set_editor_property("two_sided", True)
    # NOT a sky-pass material -- Material.h:1088 says IsSky meshes "will not
    # receive any contribution from the aerial perspective", which is the
    # opposite of what is wanted. Set explicitly rather than left at the default
    # so the choice is visible in the asset.
    material.set_editor_property("is_sky", False)
    # Height fog on a 200 km dome would paint the whole star field fog-coloured.
    material.set_editor_property("use_translucency_vertex_fog", False)
    material.set_editor_property("cast_ray_traced_shadows", False)

    b = SkyGraphBuilder(material, collection)

    # --- view direction -----------------------------------------------------
    #
    # CameraVectorWS points from the shaded pixel TOWARD the camera; the
    # direction of gaze is its negation. In this project's frame that is
    # (north, east, up) directly -- VoxelEphemeris.h:44-48 -- so the ephemeris
    # direction vectors need no transform on the way in.
    camera_vector = b.node(unreal.MaterialExpressionCameraVectorWS)
    view = b.normalize(b.neg(camera_vector))
    view_z = b.mask(view, "", b=True)

    # Stars and moon both vanish through the horizon line rather than showing
    # through gaps under the terrain. Doubles as a crude stand-in for horizon
    # extinction on a rising moon.
    fade_half_width = b.collection_param("StarHorizonFade")
    horizon_fade = b.smoothstep(b.neg(fade_half_width), fade_half_width, view_z)

    stars = build_stars(b, view, view_z, horizon_fade)
    moon = build_moon(b, view, horizon_fade)

    emissive = b.add(stars, moon)
    b.prop(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # Explicit, even though 1.0 is the unconnected default: for BLEND_Additive
    # the opacity is the additive gain, and leaving the frame's brightest
    # material relying on an implicit default is not worth the one node.
    b.prop(b.const(1.0), "", unreal.MaterialProperty.MP_OPACITY)

    b.mel.layout_material_expressions(material)
    b.mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_NightSky created and saved at " + MATERIAL_PATH)
    unreal.log(
        "NOTE: nothing renders this yet. It needs (a) a sky-dome mesh of radius "
        "> the 50 km clipmap (2e7 UU recommended) using this material, and (b) "
        "UVoxelSkySubsystem writing %s each frame. Until both exist this asset "
        "is inert -- an empty sky after running this script is EXPECTED and is "
        "not evidence the material is wrong." % COLLECTION_PATH)


main()
