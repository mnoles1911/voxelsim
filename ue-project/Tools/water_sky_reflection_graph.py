"""The water surface's SKY-REFLECTION / SUN-GLINT / MOON-GLINT / STAR chain,
as one importable builder.

WHAT THIS IS. The whole of M_WaterVoxel's emissive surface-light chain, moved
here verbatim on 2026-09-05: the analytic sun glint, the moon glint that
mirrors it node for node, the constant-sky Fresnel reflection with its
MoonLightFraction night branch, and the reflected-stars arm. Nothing about the
chain changed by moving it -- same nodes, same links, same parameter names,
same defaults, same editor positions -- and the offline mock harness proved it
by regenerating M_WaterVoxel to identical expression and link counts on both
sides of the move.

WHY IT IS A MODULE AND NOT ~700 LINES IN create_water_voxel_material.py. The
ocean/tides plan (docs/water-ocean-tides-plan-2026-09-04.md) records the gap as
its first open Materials item: M_Ocean has NO sky reflection, no sun glint, no
moon glint and no star reflection, so the sea reads flatter than the lakes it
meets at every coastline -- worst at a low sun, which is when everyone
photographs the sea. create_ocean_material.py's own docstring names the honest
fix and rules out the alternative: "promote that chain out of
create_water_voxel_material.py into a shared surface module the same way the
wave field and the optics were promoted -- NOT to copy it." This is that
promotion. It is the pattern the project now uses four times over --
sky_star_graph.py (one star lookup, two sky domes), terrain_material_common.py
(one biome graph, two terrain materials), water_optics.py (four constants,
three water renderers), water_wave_graph.py (one wave field, two water
surfaces) -- and every one of those module docstrings makes the same argument,
which is the whole reason: TWO COPIES OF ONE DERIVATION DO NOT FAIL LOUDLY.
They drift, and the result is still sharp, still animated, still plausible,
and wrong, with nothing in a frame to say so. A sun path on the lake that sat
a degree away from the sun path on the sea beside it would be exactly that
defect, in the one frame -- a shoreline -- guaranteed to show both.

M_Ocean CONSUMES THIS AS OF 2026-09-05, same day as the promotion. The
promotion deliberately landed first with the sea NOT wired, so the owner could
judge the flat sea from the first ocean captures; the ruling came back "Sea
should not be flat - it should have waves, reflection, etc to look realistic",
and the sketched tap-in at create_ocean_material.py's emissive section became
the live call (foam = saturate(breaking), top_face_mask = None -- every ocean
vertex is a top vertex -- and the lake's own VOXEL_WATER_STAR_REFLECT arm, so
the two waters cannot be built with disagreeing star arms into one frame).


=============================================================================
THE INTERFACE -- WHAT THIS CHAIN READS, AND WHAT THE CALLER OWNS
=============================================================================

THREE MPC_VoxelSky PARAMETERS, all bound with the same checked-membership
binding everything else in this toolchain uses (an unresolved
CollectionParameter compiles to a CONSTANT rather than failing --
MaterialExpressions.cpp:17179-17193 -- so every binding must raise here at
authoring time or a typo ships a dead term with no diagnostic anywhere):

  SunDirection      (vector)  written every frame by VoxelSkySubsystem's
                              ApplySkyMaterialParams. Reused rather than a
                              constant so the glint tracks sunrise and sunset
                              for free, and so a frozen -TimeScale 0 capture
                              gets the sun the rest of the frame was lit by.
                              Its Z is the sine of the sun's altitude, which
                              is the whole day gate.
  MoonDirection     (vector)  same author, same reason, for the moon glint.
  MoonLightFraction (scalar)  moon light / sun light, written every frame as
                              S.MoonIntensity / GetSunIntensity(). The one
                              multiply that turns the sun's calibrated glint
                              and the day sky's reflected colour into the
                              moon's -- see THE ONE SCALAR below.

Plus StarBrightness on the star arm, via sky_star_graph's own checked binding.

THE NORMAL IS A PARAMETER, NOT AN ASSUMPTION, and the default is "this
material's own shading normal". ReflectionVectorWS and Fresnel both read the
pixel's shading normal when their normal pins are left unconnected, which is
exactly what M_WaterVoxel wants: its normal input is the wave field's rippled
normal, so the glint lands on the wave that produced it and the mirrored sky
and both mirrored light sources are all mirrored about the SAME surface. A
caller whose reflection should use something other than its shading normal
passes `normal_ws` -- WORLD space, stated because the classic bug here
compiles: a tangent-space normal (the thing wired to MP_NORMAL) fed to these
pins is a wrong picture, not an error.

ROUGHNESS IS DELIBERATELY NOT AN INPUT. The chain is analytic: the glints
carry their own width (angular radius + shared falloff exponent) and the sky
term is a mirror gated by Fresnel, so nothing here reads or writes the
material's roughness pin. Both consumers keep authoring Roughness themselves
(the lake's 0.08 base and its foam lerp; the ocean's 0.08 -> 0.62 foam lerp).

WHAT THE CALLER OWNS:
  * wiring the returned `emissive` expression to MP_EMISSIVE_COLOR (this
    module never touches a material property -- same contract as
    build_wave_field);
  * the foam and top-face suppressions, as OPTIONAL inputs -- see the two
    parameters' notes in build_sky_reflection;
  * the star arm's build-or-not decision (M_WaterVoxel's
    VOXEL_WATER_STAR_REFLECT variable), passed as `star_reflection`. It is a
    build-time arm and not a zero multiply because a zeroed uniform measures
    nothing: the texture fetch, the atan2, the arcsine and the two derivative
    corrections all still execute (create_water_voxel_material.py, "STAR
    REFLECTION: BUILT OR NOT BUILT").

THE STANDING BAN IS INHERITED, NOT RESTATED PER NODE: nothing in this chain
reads scene COLOUR. No refraction, no planar reflection, no SSR probe -- the
value those read is the partially composed translucent stack, so the answer
depends on draw order. The star arm's texture fetch is allowed for exactly
that reason: T_SkyStarmap is the same texture whatever else has been drawn, so
every fragment computes the same answer in any order.

THE ONE KNOB THAT SHIPS AT ZERO, so nobody re-tunes a retired term: the
Fresnel sky reflection is multiplied by LegacySkyReflectGain, DEFAULT 0.0,
retired by the owner on 2026-08-12 on measurements. The Single Layer Water
port put both waters on the deferred path, where the engine composites
reflection captures and the real-time skylight unconditionally -- so this
term's constant-sky stand-in DOUBLED the engine's real reflection (each worth
~2/3 of the water's brightness, the pair ~98%), and, being added to Emissive,
it sat OUTSIDE the engine's reflection/volume energy split and buried the
depth grading instead of trading against it. The full measurement table is at
the LegacySkyReflectGain site below. The glints and the star arm are NOT
retired -- the engine's capture cannot produce an analytic sun path -- which
is why the chain is worth sharing at all.


=============================================================================
SURFACE PRESENCE (2026-09-05) -- THE GRAZING-ANGLE SHEEN, AND WHY IT IS NOT
THE RETIRED TERM COMING BACK
=============================================================================

THE DIRECTIVE, verbatim: "Water is too transparent and see through globally.
Even very shallow water bodies should clearly have a surface that looks like
water from distance."

THE MECHANISM OF THE COMPLAINT, established before the lever was chosen. At
distance the view is grazing, and real water's Fresnel reflectance runs to ~1
there: the surface reads as SKY regardless of what is under it. What supplies
that angle response in these materials today? Not this chain -- the one term
whose reflectance rises analytically to 1 at grazing is the Fresnel sky
reflection, and it ships multiplied by LegacySkyReflectGain = 0.0 (above). Its
2026-08-12 retirement was measured at a NON-grazing pose (camera 12 m up,
pitch -30, NoV ~0.5, Schlick F ~0.05), where doubling the engine's reflection
was the defect; nobody re-measured at grazing, and the retirement deleted the
angle response along with the double-count. What is left is the ENGINE's SLW
EnvBrdf split (Specular 0.5 -> F0 0.04, roughness 0.08, captures + skylight
under r.Water.SingleLayer.Reflection=2). That split does rise toward grazing,
but it can only REDISTRIBUTE energy toward whatever the scene-wide capture
holds -- a cubemap that under-represents the bright horizon sky a grazing
water surface actually mirrors -- and the wave normal keeps effective NoV off
zero, blunting the rise. So over 0.3-1 m flats at distance the transmitted
bed (working exactly as water_optics tunes it -- 70-86% visible at 0.5 m,
untouched here by instruction) wins, and the flats read as wet sand
(VoxelVerify00698). Opacity is NOT a usable lever on SLW: MP_OPACITY is the
coverage of the opaque layer ON the water, whose BaseColor is foam-black, so
a Fresnel-driven opacity floor would turn distant water DARK, not sky-bright.
The lever has to be emissive-side.

THE LEVER: a SECOND Fresnel branch, grazing-only, one scalar.

    sheen = sky_reflected                    (the same day/night/star-gated
                                              sky the retired term mirrors)
          * Fresnel(base 0.0, exponent 8)    (0.0039 at NoV 0.5; 0.43 at
                                              NoV 0.1; ~0.85 at NoV 0.02)
          * SurfacePresence                  (THE parameter; 0.0 = neutral,
                                              byte-equivalent to the pre-lever
                                              look)

added into surface_light UPSTREAM of the foam and top-face suppressions
(whitewater must not mirror; a side wall is not sky-facing), so it inherits
both, and the glints and the retired branch are untouched.

WHY THIS IS NOT LegacySkyReflectGain UNDER A NEW NAME, stated because the
first review of this change will ask. The retired term used the front
Fresnel's curve (base 0.02, exponent 5): F ~0.05 at the signed-off mid-lake
pose, which is why at gain 1 it re-brightened the whole lake and buried the
volume. The sheen's curve has ZERO base and exponent 8: at that same pose it
contributes 0.0039 x SurfacePresence x sky -- under half a percent of the
water's measured brightness at the shipped default, invisible -- and only
crosses the retired term's magnitude below ~15 degrees of elevation, where
NOTHING was signed off and the owner is now asking for exactly this. It is an
ANGLE-SEPARATION fix: the 2026-08-12 "bed visible from the middle of the
lake" verdict and the new "surface visible from a distance" directive are
about disjoint bands of NoV, and the exponent is what keeps them disjoint.

BEING ON EMISSIVE -- OUTSIDE THE ENGINE'S ENERGY SPLIT -- IS THE POINT HERE,
not a defect as it was for the retired term: at grazing the job is to
DOMINATE the transmitted bed, which a term inside the split can never do
harder than the capture is bright. The steep-angle cost that retired the old
term is bounded by the exponent instead of by the gain.

THE CALIBRATION, so the value scale is an argument and not a mood. True
Schlick reflectance for water at NoV 0.10 (about 6 degrees of elevation) is
~0.60; this curve gives 0.43 there, so SurfacePresence = 1.0 approximates the
physically correct sheen magnitude across the complaint band -- ~(0.13, 0.20,
0.31) linear of added sky against a lake bed the retirement table measured at
(0.041, 0.044, 0.021). The exponent is in DEFAULTS (SurfacePresenceExponent,
baked into the node, deliberately NOT a second runtime parameter) so a
regeneration can move the band without anyone gaining a second knob to
disagree about.

THE VERDICT (2026-09-05, same day, on the 1.0/0.5 ladder at the tidal-flats
and distance poses), verbatim: "00698 honestly looked better with its green
coloring. But however still look too transparent. 00714 looks very
transparent at a distance." The physically-correct sheen was REJECTED: at 1.0
it washed the deep teal toward pale sky-blue, which the owner reads as MORE
transparent -- his model of "reads as water" is the water's own saturated
body colour beating the bed, not a mirror. So THE DEFAULT IS NOW 0.0 (off,
byte-equivalent to the pre-sheen look) and the surface-presence job moved to
the optical-depth side: bathy_field_graph.build_slant_depth's secant default
(BathyRefractInvN2 = 1.0) makes the absorption path angle-true, so grazing
shallows saturate toward the deep-water colour he liked instead of mirroring
sky. THE PARAMETER AND THIS GRAPH TERM STAY, at zero gain: physically both
mechanisms operate at once on real water, the term costs nothing measurable
at zero, and a small gain (0.15-0.3) riding on top of the slant path is the
expected NEXT ladder if body colour alone still is not "a surface" -- one
instance value, no regeneration. Whoever runs that ladder: move ONLY this,
against the slant default, or the frames measure a mixture.
"""

import os
import sys

import unreal

# The star subgraph is SHARED, not re-derived -- sky_star_graph.py owns the one
# horizon->equatorial rotation, the one equirect UV and the one seam fix, and a
# reflection of the stars that drifted from the stars would be the exact
# two-samplers-of-one-map defect its docstring is about, on the surface most
# likely to show it (the real sky and its mirror image are in the same frame, a
# few hundred pixels apart).
#
# A -run=pythonscript commandlet does not put the script's own directory on
# sys.path, hence the explicit insert -- same as create_sky_material.py:417-420.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# (SkyGraphBuilder itself is deliberately NOT imported: this module never
# constructs one, it is handed one -- same contract as build_wave_field.)
from sky_star_graph import (  # noqa: E402
    build_horizon_fade,
    build_star_uv,
    sample_starmap,
)

# Every number the chain bakes, in one table, for the same reason
# water_wave_graph.DEFAULTS exists: a caller that wants a different calibration
# passes an override dict instead of a second copy of the chain, and a caller
# that wants the shipped look passes nothing. The scalar entries become
# MATERIAL PARAMETERS of the same name (instance-overridable without a
# regeneration); the two tints stay Constant3Vectors, as they always were.
DEFAULTS = {
    # Shared by both glints: the width of the falloff skirt around the flat
    # top. 900 was the old baked Phong exponent and is kept so a regeneration
    # with no instance overrides changes the skirt by nothing.
    "GlintSpecularExponent": 900.0,
    # The real sun's angular radius, near enough.
    "SunGlintAngularRadiusDeg": 0.55,
    # Deliberately the project's DRAWN moon, not the real one -- see the moon
    # glint block below.
    "MoonGlintAngularRadiusDeg": 2.4,
    # The star arm's one knob, physically neutral by default.
    "StarReflectGain": 1.0,
    # RETIRED 2026-08-12, see the module docstring and the site below.
    "LegacySkyReflectGain": 0.0,
    # THE GRAZING SHEEN's gain (SURFACE PRESENCE in the module docstring).
    # DEFAULT 0.0 = OFF, by owner verdict (2026-09-05, on the 1.0/0.5 ladder):
    # the mirrored sky washed the deep teal into pale blue, which he read as
    # MORE transparent -- "reads as water" to this owner is the water's own
    # body colour winning, not sky. That lever is bathy_field_graph's
    # BathyRefractInvN2 secant path now; this parameter STAYS, at zero, so a
    # small sheen (0.15-0.3) can ride on top of the slant path in a later
    # ladder without a regeneration. 1.0 approximates true Schlick magnitude
    # in the sub-15-degree band, for that ladder's calibration.
    "SurfacePresence": 0.0,
    # The sheen curve's exponent, BAKED into the Fresnel node rather than
    # exposed as a runtime parameter -- one knob, not two. 8 puts the term at
    # 0.0039 at the signed-off NoV 0.5 pose and 0.43 at NoV 0.1; lowering it
    # widens the sheen toward steeper angles and starts re-fighting the
    # 2026-08-12 retirement, so move it only with that trade in view.
    "SurfacePresenceExponent": 8.0,
    # Slightly over 1 and slightly warm: a specular sun return is brighter
    # than the sky it sits in, and clamping it to 1 is what makes a highlight
    # read as a painted white dot rather than as light.
    "GlintTint": (2.6, 2.45, 2.15),
    # The daylit sky's reflected colour. The night branch reuses it scaled by
    # MoonLightFraction rather than inventing a cooler blue -- the moon's
    # colour is settled in C++ (kMoonAlbedoTint) and a blue invented here
    # would be a second, unreachable opinion about the same question.
    "SkyTint": (0.30, 0.46, 0.72),
}

# =============================================================================
# THE GLINT IS A REPRESENTATIVE-POINT AREA LIGHT (2026-08-11), NOT A DOT
# PRODUCT RAISED TO A LARGE POWER.
#
# WHAT WAS WRONG WITH THE OLD ONE, in the terms the old comment set for
# itself. It computed pow(saturate(dot(R, SunDirection)), 900) and justified
# the 900 as "the sun subtends about half a degree", which is true and is
# also not what that expression does. A Phong lobe of exponent 900 has a
# half-width of roughly acos(0.5^(1/900)) ~= 2 degrees, but it is a smooth
# peak with no flat top, so the highlight it returns is a fading point, and
# it never returns the sun's actual angular SIZE. Against the old ripple --
# whose maximum surface tilt was about 4 degrees -- the mirror ray barely
# moved, so the result was a single small dot rather than the streak the
# moon-glint note below assumes.
#
# THE FIX, which is what Sea of Thieves ships. Treat the light as a SPHERE
# of angular radius alpha instead of a point. For each pixel, find the point
# on that sphere closest to the mirror ray and evaluate the lobe against
# THAT direction:
#
#     centerToRay = dot(L, R) * R - L        (perpendicular from L to the ray)
#     closest     = L + centerToRay * saturate(sinAlpha / |centerToRay|)
#
# When the mirror ray already points inside the disc the clamp does nothing
# and the result is exactly 1, so the highlight gets a FLAT TOP of the
# light's real angular size with the old lobe as its falloff skirt. That is
# the shape a real specular return off water has, and it is what lets the
# wave slope distribution smear it into a path instead of scattering a field
# of dots.
#
# ENERGY IS NOT INVENTED. Widening a light without renormalising makes the
# highlight brighter as well as bigger, which is how this technique usually
# goes wrong. Karis's sphere normalisation (a / (a + sinAlpha))^2, with the
# Phong exponent mapped to a roughness-like a = 2/(exp+2), is applied so the
# integral is preserved: at the shipped 0.55 deg the factor is ~0.98, so the
# sun highlight is essentially unchanged in total energy and only changed in
# shape. It matters much more for the moon at 2.4 deg, where it is ~0.72.
#
# ONE STRING, TWO CALLERS. The sun and the moon differ by the light direction
# and the angular radius and by nothing else, so this is written once and
# instantiated twice -- and the two cannot drift apart in their MATH, only in
# their two inputs, because there is one GLINT_CODE string.
#
# WHY IT IS AN HLSL CUSTOM NODE in a toolchain that is otherwise all checked
# pin connections: the closest-point-on-sphere solve is ~20 hand-wired nodes,
# and at that size the graph's advantage inverts -- nothing in 20
# correct-looking connects says the clamp went on the wrong term. The HLSL is
# ten lines and reads as the mathematics it is. The BOUNDARY is still checked
# the same way as everything else: every input is wired by NAME with a
# raise-on-failure connect, and the input names are what the HLSL reads, so a
# rename that misses one end fails loudly rather than reading a stale value.
# =============================================================================
GLINT_CODE = """
// Representative-point (closest-point-on-sphere) area specular.
// R is the mirror direction about this material's own rippled normal, so the
// highlight lands on the wave that produced it. L is the light direction.
float3 Rn = normalize(R);
float3 Ln = normalize(L);
float  sinAlpha = sin(radians(max(AngularRadiusDeg, 0.0)));
float3 centerToRay = dot(Ln, Rn) * Rn - Ln;
float  ctrLen = length(centerToRay);
float3 closest = Ln + centerToRay * saturate(sinAlpha / max(ctrLen, 1e-5));
float  d = saturate(dot(Rn, normalize(closest)));
// Karis sphere normalisation: widening the source must not add energy.
float  a = 2.0 / max(SpecExponent + 2.0, 2.0);
float  n = a / max(a + sinAlpha, 1e-5);
return pow(d, max(SpecExponent, 1.0)) * n * n;
"""


# --- the three node helpers, at EXPLICIT editor positions --------------------
#
# Explicit x/y rather than the builder's auto-placement lane, deliberately: the
# chain moved out of create_water_voxel_material.py with its editor layout
# intact, so the regenerated asset diffs clean against the pre-move one and the
# graph a human opens looks exactly like the graph the comments describe. Only
# the star arm auto-places, because it always did (it is written against
# SkyGraphBuilder, whose helpers place their own nodes).

def _node(b, cls, x, y):
    return b.mel.create_material_expression(b.material, cls, x, y)


def _scalar_param(b, name, default, x, y):
    """A named ScalarParameter, so it is tunable from a material instance
    without regenerating the asset."""
    node = _node(b, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    return node


def _custom_node(b, name, code, inputs, x, y, output_type):
    """A MaterialExpressionCustom with named inputs and no wiring yet; the
    caller connects them by name with checked connects. FCustomInput carries
    an FName and an FExpressionInput; only the name is settable from Python,
    the FExpressionInput is filled in by the connect calls that follow, which
    is why the array is built name-only here."""
    node = _node(b, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("description", name)
    node.set_editor_property("code", code)
    node.set_editor_property("output_type", output_type)
    ins = []
    for nm in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", nm)
        ins.append(ci)
    node.set_editor_property("inputs", ins)
    return node


def _collection_param(b, name, x, y):
    """A CollectionParameter bound to MPC_VoxelSky, with the binding CHECKED
    against the asset as read back.

    Same guarantee as SkyGraphBuilder.collection_param, at an explicit editor
    position: an unresolved CollectionParameter does not fail to compile, it
    compiles to a CONSTANT (MaterialExpressions.cpp:17179-17193), so a typo or
    a stale MPC must raise HERE, naming the parameter and listing what the
    collection actually has -- a thirty-second fix instead of a debugging
    session that starts from "I don't see any lake basins" (2026-08-10). The
    two failure shapes it catches are different: a DELETED parameter raises,
    and so does a parameter this chain expects that a not-yet-regenerated MPC
    does not have -- the ordering mistake documented at the top of
    create_sky_material.py.
    """
    if name not in b.mpc_names():
        raise RuntimeError(
            "MPC_VoxelSky has no parameter %r -- it has %s. If you just added it to "
            "create_sky_material.py's SCALAR_PARAMS/VECTOR_PARAMS, you have to RE-RUN that "
            "script (and then the dome, then the water generators, in that order -- see the "
            "ordering note at the top of create_sky_material.py). An unresolved "
            "CollectionParameter does not fail to compile, it compiles to a constant, so this "
            "check is the only thing between a typo and a silently dead term."
            % (name, sorted(b.mpc_names())))
    node = _node(b, unreal.MaterialExpressionCollectionParameter, x, y)
    node.set_editor_property("collection", b.collection)
    node.set_editor_property("parameter_name", name)
    return node


def build_sky_reflection(b, star_reflection=True, foam=None, top_face_mask=None,
                         normal_ws=None, defaults=None):
    """The whole surface-light chain. Returns a dict of expressions.

    Arguments:
      b             a SkyGraphBuilder bound to the material and to MPC_VoxelSky.
                    The named-position nodes never touch its auto-placement
                    lane; the star arm DOES auto-place through it, so a caller
                    that cares about star-node layout (M_WaterVoxel does, for
                    asset-diff cleanliness) passes a FRESH builder.
      star_reflection
                    build the reflected-stars arm or leave it out entirely.
                    A build-time arm, never a zero multiply -- the caller's
                    A/B story (VOXEL_WATER_STAR_REFLECT for the lake) depends
                    on the OFF arm genuinely not containing the fetch.
      foam          OPTIONAL scalar expression, 0..1 foam coverage. When given,
                    the chain is multiplied by (1 - foam): froth is a
                    scattering medium, the one part of a water surface that
                    does NOT mirror the sky, and letting the reflection survive
                    underneath it would put a sky sheen on whitewater. The
                    caller's own foam lerps on colour/opacity/roughness still
                    run after this, so foam continues to win where it is
                    present and contributes nothing where it is zero. Pass None
                    only for a surface with no foam signal at all; M_Ocean has
                    one (saturate(breaking)) and should pass it.
      top_face_mask OPTIONAL scalar expression, 1 on sky-facing surface. When
                    given, the chain is multiplied by it: a submerged side
                    wall is not a sky-facing surface and must not reflect one.
                    M_WaterVoxel passes its normal-test mask (not
                    VertexColor.B -- see its foam section for why B would ring
                    every body with a stripe). M_Ocean passes None for the
                    same reason it builds no VertexColor node: every vertex of
                    that mesh is a top vertex, and a dead multiply by a
                    constant 1 would suggest the slot is being filled by
                    something (its docstring, THE VERTEX COLOURS).
      normal_ws     OPTIONAL WORLD-SPACE normal expression for the reflection
                    and the Fresnel. Default None uses the material's own
                    shading normal on both nodes, which is what both waters
                    want -- their MP_NORMAL is the wave field's rippled
                    normal, so the glint lands on the wave that produced it.
                    WORLD space, not tangent: wiring the tangent-space
                    `normal_xyz` that feeds MP_NORMAL here instead is a bug
                    that compiles.
      defaults      optional dict merged over DEFAULTS, water_wave_graph
                    style: override a number without copying the chain.

    Returns keys:
      emissive        the finished chain -- glints + gated sky reflection,
                      foam-suppressed and top-face-masked where those inputs
                      were given. WIRE THIS TO MP_EMISSIVE_COLOR, and to
                      nothing else: a reflection is light LEAVING the surface,
                      not albedo. Routing it through BaseColor would multiply
                      it by AO and by the diffuse lighting term, so a
                      reflected sky would darken in an occluded corner, which
                      is backwards.
      refl_vec        the one ReflectionVectorWS every term mirrors about --
                      shared so the moon path and the reflected star field
                      cannot sit in two places on the same wave.
      surface_light   the chain before the foam/top-face suppressions --
                      glints + gated sky reflection + the SurfacePresence
                      sheen.
      glints          sun + moon glints (Fresnel-free, see below).
      reflection      the Fresnel-weighted, LegacySkyReflectGain-scaled sky
                      term (default gain 0.0 = retired, see module docstring).
      fresnel         the Fresnel node, in case a caller wants the same
                      grazing-angle weight for a term of its own.
      sheen           the SurfacePresence-scaled grazing sheen term (see
                      SURFACE PRESENCE in the module docstring).
      grazing_fresnel the sheen's own zero-base Fresnel node.
    """
    d = dict(DEFAULTS)
    if defaults:
        d.update(defaults)
    mel = b.mel
    material = b.material

    # SunDirection is written every frame by VoxelSkySubsystem
    # (ApplySkyMaterialParams). Reusing it rather than a constant is what keeps
    # the glint on the actual sun: the water tracks sunrise and sunset for
    # free, and a frozen -TimeScale 0 capture gets the sun the rest of the
    # frame was lit by.
    sun_dir_param = _collection_param(b, "SunDirection", -1300, 1300)

    sun_dir3 = _node(b, unreal.MaterialExpressionComponentMask, -1120, 1300)
    sun_dir3.set_editor_property("r", True)
    sun_dir3.set_editor_property("g", True)
    sun_dir3.set_editor_property("b", True)
    sun_dir3.set_editor_property("a", False)
    if not mel.connect_material_expressions(sun_dir_param, "", sun_dir3, ""):
        raise RuntimeError("connect sun_dir_param -> sun_dir3 failed")

    # SUN GLINT, computed analytically instead of left to the engine's
    # specular. Both consumers set Specular 0.5 / Roughness 0.08 and that
    # survives unchanged, but a water surface's specular response is the part
    # of its lighting that is least reliable to author against (M_WaterVoxel's
    # W3 note records the same lesson from the other side, where the
    # volumetric lighting mode ignored the material normal outright).
    # reflect(V) dot SunDirection is not subject to any of that: it is the
    # mirror direction against the surface's own rippled normal, so the glint
    # lands exactly where the wave that produced it is.
    refl_vec = _node(b, unreal.MaterialExpressionReflectionVectorWS, -1120, 1450)
    if normal_ws is not None:
        if not mel.connect_material_expressions(
                normal_ws, "", refl_vec, "CustomWorldNormal"):
            raise RuntimeError("connect normal_ws -> refl_vec.CustomWorldNormal failed")

    # The exponent is SHARED by both glints and is a parameter rather than a
    # baked 900. It sets the width of the falloff skirt around the flat top;
    # the flat top itself is the angular radius. See GLINT_CODE above for the
    # whole area-light argument.
    glint_exponent = _scalar_param(
        b, "GlintSpecularExponent", d["GlintSpecularExponent"], -1300, 1240)
    sun_glint_radius = _scalar_param(
        b, "SunGlintAngularRadiusDeg", d["SunGlintAngularRadiusDeg"], -1300, 1180)

    glint_pow = _custom_node(
        b, "SunGlint", GLINT_CODE, ["R", "L", "AngularRadiusDeg", "SpecExponent"],
        -800, 1380, unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    if not mel.connect_material_expressions(refl_vec, "", glint_pow, "R"):
        raise RuntimeError("connect refl_vec -> sun glint.R failed")
    if not mel.connect_material_expressions(sun_dir3, "", glint_pow, "L"):
        raise RuntimeError("connect sun_dir3 -> sun glint.L failed")
    if not mel.connect_material_expressions(sun_glint_radius, "", glint_pow, "AngularRadiusDeg"):
        raise RuntimeError("connect sun_glint_radius -> sun glint.AngularRadiusDeg failed")
    if not mel.connect_material_expressions(glint_exponent, "", glint_pow, "SpecExponent"):
        raise RuntimeError("connect glint_exponent -> sun glint.SpecExponent failed")

    # Slightly over 1 and slightly warm -- see GlintTint in DEFAULTS.
    glint_tint = _node(b, unreal.MaterialExpressionConstant3Vector, -650, 1520)
    glint_tint.set_editor_property(
        "constant", unreal.LinearColor(*(tuple(d["GlintTint"]) + (1.0,))))
    glint = _node(b, unreal.MaterialExpressionMultiply, -500, 1440)
    if not mel.connect_material_expressions(glint_pow, "", glint, "A"):
        raise RuntimeError("connect glint_pow -> glint.A failed")
    if not mel.connect_material_expressions(glint_tint, "", glint, "B"):
        raise RuntimeError("connect glint_tint -> glint.B failed")

    # ======================================================================
    # MOON GLINT -- the moon path on the water. Added 2026-08-11 because the
    # owner asked why the moon does not reflect off the lake.
    #
    # THERE WERE TWO REASONS IT DID NOT, and the material was only one of
    # them. The other was in C++ and is the bigger one: both directional
    # lights sat at ForwardShadingPriority 0, so the renderer picked the
    # single forward / translucent light by raw brightness and handed it to
    # the SUN at midnight (LightGridInjection.cpp:1500-1520). That half is
    # fixed in UVoxelSkySubsystem::ApplyLightsFromState; see
    # kForwardMoonPrimarySunBelowDeg. This half is the SPECULAR half, and
    # neither alone is enough.
    #
    # STRUCTURALLY IDENTICAL TO THE SUN GLINT ABOVE, deliberately, down to
    # the exponent and the tint. Every difference between them is one
    # multiply. The things it therefore inherits for free: the same
    # reflect(V) mirror direction against the surface's own rippled normal,
    # so the moon path breaks up over waves exactly as the sun's does; and
    # the same reason for computing it analytically rather than trusting the
    # engine's specular.
    #
    # THE ANGULAR RADIUS IS 2.4 DEGREES, WHICH REVERSES AN EARLIER DECISION,
    # AND THE REASON IT CAN BE REVERSED IS THAT THE MECHANISM CHANGED.
    #
    # What this comment used to say: the exponent stays at the sun's 900,
    # because the real moon subtends the same ~0.5 deg the sun does, and
    # because widening a Phong LOBE is the way to turn a highlight into a
    # plate of wet plastic across the whole basin. Both halves of that were
    # right about a Phong lobe.
    #
    # The glint is no longer a Phong lobe. It is a representative-point area
    # light (GLINT_CODE above), which separates two things the exponent used
    # to conflate: the light's angular SIZE, which is now the flat top, and
    # the falloff SKIRT, which is still the exponent and is still 900 and is
    # still shared with the sun. Widening the size no longer widens the
    # skirt, so the wet-plastic failure mode is not on the table -- the
    # highlight gets a 2.4 deg core and the same tight edge it always had,
    # and Karis normalisation takes its peak brightness down by ~28% to pay
    # for the extra area.
    #
    # 2.4 deg is deliberately the project's DRAWN moon, not the real one. The
    # moon in this sky is enlarged about nine times
    # (kMoonDrawnAngularRadiusDeg), and a path on the water whose source is
    # 0.5 deg while the disc above it is 2.4 deg is two different moons in
    # the same frame. The disc is the one the owner sees, so the water agrees
    # with the disc. That is the enlarged-disc cheat being applied
    # consistently rather than a second cheat.
    #
    # THE TINT IS THE SUN'S TINT SCALED BY MoonLightFraction, and that one
    # multiply is the whole physical content of this block. MoonLightFraction
    # is written every frame by UVoxelSkySubsystem::ApplySkyMaterialParams as
    # S.MoonIntensity / GetSunIntensity() -- "how much dimmer than the sun
    # the moon is, right now". So the moon's highlight is the sun's
    # calibrated highlight, dimmed by exactly the ratio the two LIGHTS are
    # dimmed by. Nothing here needs tuning and nothing here can drift from
    # the lighting rig.
    #
    # WHAT THAT ONE SCALAR CARRIES, all of it from the C++ side with no logic
    # here:
    #   * moonset      -- MoonHorizonGate is already inside S.MoonIntensity,
    #                     so the path fades out as the moon sets and is gone
    #                     once it is down
    #   * new moon     -- MoonIlluminatedFraction is in there too, so a new
    #                     moon lays no path at all
    #   * daylight     -- the sun-suppression term zeroes it before sunrise
    #   * the owner's knob -- voxel.Sky.MoonIntensity scales it, so dialling
    #                     the moonlight moves the water and the ground by the
    #                     same stops
    # Reconstructing any of those from MoonDirection and MoonPhaseFraction in
    # this graph was the alternative, and it would have been a second copy of
    # ApplyLightsFromState living in a Python asset generator, unable to see
    # the cvar, and wrong in a way no log line would report.
    # ======================================================================
    moon_dir_param = _collection_param(b, "MoonDirection", -1300, 1900)

    moon_dir3 = _node(b, unreal.MaterialExpressionComponentMask, -1120, 1900)
    moon_dir3.set_editor_property("r", True)
    moon_dir3.set_editor_property("g", True)
    moon_dir3.set_editor_property("b", True)
    moon_dir3.set_editor_property("a", False)
    if not mel.connect_material_expressions(moon_dir_param, "", moon_dir3, ""):
        raise RuntimeError("connect moon_dir_param -> moon_dir3 failed")

    # Scalar. NOT masked -- a CollectionParameter bound to a SCALAR compiles
    # as a float and a ComponentMask on it is both unnecessary and a pin-type
    # mismatch waiting to happen. The vector ones above are masked only to
    # drop the unused .a that FLinearColor forces on them.
    moon_light_fraction = _collection_param(b, "MoonLightFraction", -1300, 2060)

    # SAME Custom node source as the sun glint, instantiated a second time.
    # The only two differences are the light direction and the angular radius,
    # which is what "structurally identical, every difference is one multiply"
    # was always meant to mean.
    moon_glint_radius = _scalar_param(
        b, "MoonGlintAngularRadiusDeg", d["MoonGlintAngularRadiusDeg"], -1300, 2120)

    moon_glint_pow = _custom_node(
        b, "MoonGlint", GLINT_CODE, ["R", "L", "AngularRadiusDeg", "SpecExponent"],
        -800, 1900, unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    if not mel.connect_material_expressions(refl_vec, "", moon_glint_pow, "R"):
        raise RuntimeError("connect refl_vec -> moon glint.R failed")
    if not mel.connect_material_expressions(moon_dir3, "", moon_glint_pow, "L"):
        raise RuntimeError("connect moon_dir3 -> moon glint.L failed")
    if not mel.connect_material_expressions(moon_glint_radius, "", moon_glint_pow, "AngularRadiusDeg"):
        raise RuntimeError("connect moon_glint_radius -> moon glint.AngularRadiusDeg failed")
    if not mel.connect_material_expressions(glint_exponent, "", moon_glint_pow, "SpecExponent"):
        raise RuntimeError("connect glint_exponent -> moon glint.SpecExponent failed")

    moon_glint_scaled = _node(b, unreal.MaterialExpressionMultiply, -500, 1960)
    if not mel.connect_material_expressions(moon_glint_pow, "", moon_glint_scaled, "A"):
        raise RuntimeError("connect moon_glint_pow -> moon_glint_scaled.A failed")
    if not mel.connect_material_expressions(moon_light_fraction, "", moon_glint_scaled, "B"):
        raise RuntimeError("connect moon_light_fraction -> moon_glint_scaled.B failed")

    # glint_tint REUSED, not copied. One node, one calibration, and the moon
    # glint cannot acquire a different colour balance from the sun glint by an
    # edit that only remembers to change one of them.
    moon_glint = _node(b, unreal.MaterialExpressionMultiply, -380, 1900)
    if not mel.connect_material_expressions(moon_glint_scaled, "", moon_glint, "A"):
        raise RuntimeError("connect moon_glint_scaled -> moon_glint.A failed")
    if not mel.connect_material_expressions(glint_tint, "", moon_glint, "B"):
        raise RuntimeError("connect glint_tint -> moon_glint.B failed")

    # SKY REFLECTION. A constant sky colour gated by sun altitude, NOT a scene
    # capture and NOT a reflection probe: the standing ban is on reading scene
    # COLOUR, and a probe read is the same hazard wearing a different name.
    # SunDirection.z is the sine of the sun's altitude, so saturate() of it is
    # 0 from dusk to dawn and the water stops reflecting a blue sky it cannot
    # see -- the one piece of time-of-day behaviour this term genuinely needs,
    # and it costs one node.
    sun_alt = _node(b, unreal.MaterialExpressionComponentMask, -1120, 1600)
    sun_alt.set_editor_property("r", False)
    sun_alt.set_editor_property("g", False)
    sun_alt.set_editor_property("b", True)
    sun_alt.set_editor_property("a", False)
    if not mel.connect_material_expressions(sun_dir_param, "", sun_alt, ""):
        raise RuntimeError("connect sun_dir_param -> sun_alt failed")

    day_gate = _node(b, unreal.MaterialExpressionSaturate, -950, 1600)
    if not mel.connect_material_expressions(sun_alt, "", day_gate, ""):
        raise RuntimeError("connect sun_alt -> day_gate failed")

    # KNOWN LIMITATION, stated rather than left to be discovered.
    # saturate(SunDirection.z) is "is the sun up", which is not the same
    # question as "can THIS surface see the sky". A static cavern pool a
    # hundred metres underground at noon therefore still gets a sky-blue
    # reflection at grazing angles, from a sky it has no line of sight to. It
    # is Fresnel-weighted, so looking down into the pool -- how a cavern pool
    # is normally met -- it is near zero, and it is the same magnitude a
    # surface lake gets. Fixing it properly needs a sky-visibility signal that
    # does not exist on the voxel vertex format: VertexColor.G is the greedy
    # mesher's local AO, which is ~1 in the middle of any chamber large enough
    # to hold a pool and so cannot express it. NOT VERIFIED IN A CAPTURE:
    # -VoxelFloodTest found no flooded cavern at the lake site, and the
    # default cavern site's fine tiles are absent from this box's cache (tile
    # (-6,3) at s16, absentOnDisk=1). Downgrading the fine-tier gate to get a
    # frame would have made that frame unreproducible, which is the one thing
    # the gate exists to prevent.
    sky_tint = _node(b, unreal.MaterialExpressionConstant3Vector, -950, 1700)
    sky_tint.set_editor_property(
        "constant", unreal.LinearColor(*(tuple(d["SkyTint"]) + (1.0,))))
    sky_lit = _node(b, unreal.MaterialExpressionMultiply, -800, 1650)
    if not mel.connect_material_expressions(sky_tint, "", sky_lit, "A"):
        raise RuntimeError("connect sky_tint -> sky_lit.A failed")
    if not mel.connect_material_expressions(day_gate, "", sky_lit, "B"):
        raise RuntimeError("connect day_gate -> sky_lit.B failed")

    # THE NIGHT HALF OF THE SAME REFLECTION, and the second reason the lake
    # looked dead after dark.
    #
    # saturate(SunDirection.z) above is EXACTLY ZERO from dusk to dawn. That
    # was correct as far as it went -- the water should not mirror a blue
    # daytime sky it cannot see -- but the consequence was that the Fresnel
    # reflection, the term the eye reads as "liquid surface" before any wave
    # or glint registers, switched off completely every night. A surface with
    # no reflection at a grazing angle does not read as water at any hour.
    # Note this is INDEPENDENT of the moon glint added above: the glint is a
    # specular highlight a few degrees wide, and this is the broad sheen
    # across the whole basin. Fixing only one of them leaves the water looking
    # wrong in the other way.
    #
    # THE NIGHT SKY IS THE DAY SKY TIMES MoonLightFraction, with no new
    # constant. sky_tint is the daylit sky's reflected colour; the night sky
    # is lit by the moon exactly as the day sky is lit by the sun, so scaling
    # by (moon light / sun light) is not an approximation of convenience, it
    # is the same ratio the two skies actually stand in -- inside this
    # project's chosen moon cheat, which is the only frame of reference that
    # matters here. At the current defaults that puts the reflected sky ~1.8
    # stops below its daytime self once the exposure curve's night lift is
    # counted, which sits alongside the ground's -2.0 stops rather than
    # fighting it. Re-using the SAME scalar the moon glint uses means the
    # broad sheen and the highlight can never be tuned into disagreement, and
    # both vanish together at moonset and at new moon.
    #
    # THE COLOUR IS DELIBERATELY NOT SHIFTED BLUE -- see SkyTint in DEFAULTS.
    #
    # WHAT THIS DOES NOT DO: a moonless but starlit night still gets no sheen
    # from this term, because MoonLightFraction is 0. That is a real gap and
    # it is left open rather than papered over with an invented starlight
    # floor -- the honest value for one is a measurement nobody has taken.
    # Starlight is NOT absent from the water in that case: the SkyLight's
    # real-time capture carries the star term (voxel.Sky.StarAmbientGain,
    # routed into the capture through M_SkyAtmosphereDome's
    # ReflectionCapturePassSwitch) and reaches the surface as DIFFUSE ambient
    # on BaseColor. What it cannot do is produce a mirrored sky at a grazing
    # angle. That is the star arm below.
    night_sky = _node(b, unreal.MaterialExpressionMultiply, -800, 1780)
    if not mel.connect_material_expressions(sky_tint, "", night_sky, "A"):
        raise RuntimeError("connect sky_tint -> night_sky.A failed")
    if not mel.connect_material_expressions(moon_light_fraction, "", night_sky, "B"):
        raise RuntimeError("connect moon_light_fraction -> night_sky.B failed")

    # ADD, not LERP. The two branches are already mutually exclusive in
    # practice -- the day gate is 0 whenever the sun is down and
    # MoonLightFraction is 0 whenever the sun is up (ApplyLightsFromState's
    # sun-suppression term is what guarantees the second one) -- so a lerp
    # would need a third gate to express something the inputs already express,
    # and would be a place for the two to disagree.
    sky_total = _node(b, unreal.MaterialExpressionAdd, -680, 1700)
    if not mel.connect_material_expressions(sky_lit, "", sky_total, "A"):
        raise RuntimeError("connect sky_lit -> sky_total.A failed")
    if not mel.connect_material_expressions(night_sky, "", sky_total, "B"):
        raise RuntimeError("connect night_sky -> sky_total.B failed")

    # ======================================================================
    # REFLECTED STARS (2026-08-11) -- the gap the block above names, closed.
    #
    # The night branch above stops at a flat scaled sky colour: a moonless
    # night gets no sheen at all, because MoonLightFraction is 0, and even a
    # moonlit one gets a featureless wash where the eye expects points of
    # light. This block is the fix, and its perf question is answered with a
    # measurement instead of an estimate -- which is the whole reason
    # `star_reflection` is a build-time arm.
    #
    # WHAT IS ALLOWED HERE AND WHAT IS NOT. The standing ban is on reading
    # scene COLOUR: refraction, planar reflections, an SSR probe. All three
    # are banned because the value they read IS the partially composed
    # translucent stack, so the answer depends on draw order and two water
    # fragments at one pixel disagree. A TEXTURE fetch along the reflection
    # vector has none of that: T_SkyStarmap is the same texture whatever else
    # has been drawn, so every fragment computes the same answer in any order.
    #
    # THE DIRECTION IS refl_vec, THE SAME NODE THE TWO GLINTS USE. That is
    # not a saving of one node, it is the guarantee that the mirrored sky and
    # the two mirrored light sources are all mirrored about the SAME rippled
    # normal. If the stars used their own reflection the moon path could sit
    # in one place and the reflected star field in another, on the same wave.
    #
    # THE HORIZON FADE IS DOING REAL WORK HERE, not carried along from the
    # dome. build_horizon_fade smoothsteps on the Z of the direction it is
    # handed, and the direction handed to it here is the MIRROR ray, not the
    # gaze. So it asks "does the mirror ray point at the sky or into the
    # ground", and kills the term when the answer is the ground: on the
    # underside of a surface seen from below (both waters are two_sided), and
    # on the far face of a ripple steep enough to turn the mirror ray
    # downward. Without it those pixels would sample the star map's southern
    # rows and show stars coming out of the water's bed.
    #
    # THE NIGHT GATE IS StarBrightness, AND IT IS NOT MoonLightFraction.
    # StarBrightness is the MPC scalar C++ already writes every frame as the
    # sunrise fade (1 below about -12 deg solar altitude, 0 above 0 deg,
    # smoothstepped between -- VoxelSkySubsystem StarBrightnessForSunAltitude,
    # times voxel.Sky.StarGain). Using it means:
    #   * the reflected stars appear and fade at EXACTLY the moment the real
    #     ones do, because it is the same scalar M_NightSky's own gain uses,
    #   * they survive a new moon and a moonset, which is the precise gap the
    #     night branch above documents and could not close with
    #     MoonLightFraction -- that scalar is 0 on a moonless night, and a
    #     moonless night is when stars are most visible, not least.
    # Scaling this by MoonLightFraction as well would have been "consistent
    # with the night sky term" in wording and backwards in physics.
    #
    # BRIGHTNESS IS FRESNEL AND NOTHING ELSE. This term is added into
    # sky_total, so the multiply by `fresnel` below is the only scaling it
    # gets: about 0.02 looking straight down, rising toward 1 at a grazing
    # angle. That IS the reflectance of water, so no invented constant is
    # needed and the still-water grazing view the owner asked about -- where
    # Fresnel is near 1 -- is exactly the case that shows the most stars.
    # StarReflectGain is a plain material scalar (default 1.0, i.e.
    # physically neutral) left in as the one knob, so the term can be dimmed
    # or brightened from a material instance without another regeneration.
    #
    # WHAT THIS WILL AND WILL NOT LOOK LIKE, stated so a capture is not read
    # as a bug. The star map is sampled with EXPLICIT derivatives, so the mip
    # is chosen from how fast the reflection vector varies across the screen.
    # On STILL water the normal barely changes, the derivatives are tiny and
    # the stars are near-point sharp. On chop the mirror ray swings by
    # degrees per pixel, a low mip is selected, and the star field correctly
    # degrades to a broad glow rather than to a field of aliasing sparkle.
    # Both are right; only the first is the picture anyone imagines when they
    # ask for this.
    #
    # THE ONE KNOWN ARTEFACT. sky_star_graph's seam fix subtracts
    # round(du/dx), which assumes a real derivative is far below 0.5 turns
    # per pixel. On a steep ripple near the celestial poles that assumption
    # can fail and the mip comes out one or two levels too sharp for a few
    # pixels. On the DOME that case cannot arise (the gaze direction is
    # smooth); here it can. The visible consequence is a little shimmer in
    # the reflection of the polar sky on rough water, and it is bounded by
    # the same star field being dim there.
    # ======================================================================
    sky_reflected = sky_total
    if star_reflection:
        # The builder's own helpers rather than raw mel calls, because
        # build_star_uv and sample_starmap are written against SkyGraphBuilder.
        # They bring their own checked connects and their own checked
        # CollectionParameter binding, the same guarantee _collection_param
        # above gives this chain's named-position nodes.
        #
        # Normalized explicitly. ReflectionVectorWS is unit length in
        # practice, but build_star_uv's arcsine reads this vector's Z as
        # sin(dec) directly and a length that is 1.001 is a declination error,
        # not a brightness error -- it would move stars, silently.
        refl_dir = b.normalize(refl_vec)
        refl_z = b.mask(refl_dir, "", b=True)

        star_uv, star_ddx, star_ddy = build_star_uv(b, refl_dir, refl_z)
        starmap = sample_starmap(b, star_uv, star_ddx, star_ddy)

        star_gain = b.mul(b.collection_param("StarBrightness"),
                          build_horizon_fade(b, refl_z))
        star_gain = b.mul(star_gain, b.scalar("StarReflectGain", d["StarReflectGain"]))

        # "RGB" explicitly: a TextureSample's DEFAULT output is RGBA, and
        # multiplying a float4 into this chain would carry an alpha nobody
        # wants into the emissive sum. sky_star_graph's own docstring for
        # sample_starmap says to read RGB and not the default, for this reason.
        star_term = b.mul(starmap, star_gain, "RGB", "")

        # ADDED to sky_total, and therefore UPSTREAM of Fresnel, of the foam
        # suppression and of the top-face mask below. All three are wanted:
        # stars are a reflection so they must obey Fresnel; whitewater
        # scatters and must not mirror a star field; and a submerged side
        # wall is not a sky-facing surface. Attaching this after the Fresnel
        # multiply would have quietly opted out of all three.
        sky_reflected = b.add(sky_total, star_term)

    # Fresnel with water's real normal-incidence reflectance, 0.02. The
    # Normal input is deliberately LEFT UNCONNECTED unless the caller passed
    # normal_ws, so the node uses the material's own shading normal -- which
    # is the wave field's rippled normal on both waters. Wiring a
    # TANGENT-space value here instead would be a bug that compiles: this pin
    # wants world space (see `normal_ws` in the docstring).
    fresnel = _node(b, unreal.MaterialExpressionFresnel, -650, 1650)
    fresnel.set_editor_property("exponent", 5.0)
    fresnel.set_editor_property("base_reflect_fraction", 0.02)
    if normal_ws is not None:
        if not mel.connect_material_expressions(normal_ws, "", fresnel, "Normal"):
            raise RuntimeError("connect normal_ws -> fresnel.Normal failed")

    reflection_fresnel = _node(b, unreal.MaterialExpressionMultiply, -500, 1650)
    if not mel.connect_material_expressions(sky_reflected, "", reflection_fresnel, "A"):
        raise RuntimeError("connect sky_reflected -> reflection_fresnel.A failed")
    if not mel.connect_material_expressions(fresnel, "", reflection_fresnel, "B"):
        raise RuntimeError("connect fresnel -> reflection_fresnel.B failed")

    # THE ONE KNOB THAT SHIPS AT ZERO, AND WHY.
    #
    # Everything above -- the constant sky tint, the day gate, the
    # MoonLightFraction night branch, the reflected stars, the Fresnel weight
    # -- was authored for a TRANSLUCENT surface, whose environment specular
    # was the least reliable part of its lighting. A Single Layer Water
    # surface is not in that position: it goes through the deferred path and
    # receives the engine's own reflection from reflection captures and the
    # skylight, and this project's skylight is a real-time capture of its own
    # sky and moon. So there is a SECOND source of reflected sky on the
    # surface, and the two double up.
    #
    # IT IS MEASURED, AND THEY DO DOUBLE UP. 2026-08-12, the lake at
    # (-65102,-51084), camera 12 m up at pitch -30, noon, same exposure in
    # every arm (a land patch reads 0.1491-0.1494 display-linear in all
    # five). Mid-lake water, display-linear RGB:
    #
    #   both reflections (shipped)        0.245 / 0.388 / 0.510
    #   r.Water.SingleLayer.Reflection 0  0.084 / 0.169 / 0.265   (this term only)
    #   LegacySkyReflectGain 0            0.106 / 0.203 / 0.287   (engine term only)
    #   both off                          0.000 / 0.008 / 0.005   (the volume alone)
    #   the lake bed with no water at all 0.041 / 0.044 / 0.021
    #
    # So each sky reflection is on its own worth about two thirds of the
    # water's brightness, the two together are ~98% of it, and BOTH of them
    # are several times brighter than the lake bed they sit on top of. With
    # both removed the bed is plainly visible through the water and grades
    # with depth exactly as the optics intend.
    #
    # THE DEFAULT IS 0.0: THE OWNER RETIRED THIS TERM ON 2026-08-12, on the
    # measurements above. It is kept as a parameter rather than deleted so
    # the old look is one instance value away and the arms stay reproducible.
    #
    # WHY THIS ONE AND NOT THE ENGINE'S, since either would remove the
    # double. They are not equivalent, and the difference is exactly the
    # defect:
    #
    #   * The ENGINE'S reflection participates in the energy split. The
    #     shader does ScatteredLuminance *= (1 - EnvBrdf) and
    #     Reflection *= EnvBrdf, so reflection and volume TRADE OFF and sum
    #     to <= 1. Turn the water's depth grading up and the reflection makes
    #     room for it.
    #   * THIS term is added to Emissive, which is outside that split -- pure
    #     addition on top of everything else. That is why it did not merely
    #     brighten the water, it BURIED the volume: measured mid-lake the
    #     volume alone reads 0.000/0.008/0.005 against this term's
    #     0.106/0.203/0.287.
    #
    # And the engine's tracks the real sky -- the skylight is a real-time
    # capture, so it follows time of day, the moon and cloud, where a
    # constant-sky Fresnel cannot. Removing this term moved the relative
    # structure in the water (SD/mean of luma) from 0.127 to 0.304.
    #
    # THE HISTORY, so nobody re-adds it: this term was written when the water
    # was BLEND_TRANSLUCENT + MSM_DefaultLit, under the project's own ban
    # ("reflections stay constant-sky Fresnel, no dynamic reflection
    # capture"). With captures off the table the material had to fake a sky
    # reflection itself. The Single Layer Water port put the water on the
    # deferred path where the engine composites captures and the skylight
    # unconditionally -- so the port added the real thing without removing
    # the stand-in. The zero default is the removal.
    #
    # The star reflection rides this gain too, which is correct: the engine's
    # skylight capture already contains the stars via M_SkyAtmosphereDome's
    # ReflectionCapturePassSwitch, so if one is redundant both are.
    legacy_sky_gain = _scalar_param(
        b, "LegacySkyReflectGain", d["LegacySkyReflectGain"], -650, 1760)
    reflection = _node(b, unreal.MaterialExpressionMultiply, -420, 1650)
    if not mel.connect_material_expressions(reflection_fresnel, "", reflection, "A"):
        raise RuntimeError("connect reflection_fresnel -> reflection.A failed")
    if not mel.connect_material_expressions(legacy_sky_gain, "", reflection, "B"):
        raise RuntimeError("connect legacy_sky_gain -> reflection.B failed")

    # BOTH GLINTS ARE FRESNEL-FREE, day and night alike. The reflection above
    # is Fresnel-weighted and these are not, and that asymmetry is correct
    # rather than an oversight: Fresnel governs how much of the SKY a surface
    # mirrors, while a specular return off a rippled surface is dominated by
    # the slope distribution -- which the normal already supplies. Weighting
    # the glint by Fresnel too would delete the sun's own reflection when
    # looking straight down at calm water at noon, which is the one place
    # everyone has seen it.
    glints = _node(b, unreal.MaterialExpressionAdd, -340, 1440)
    if not mel.connect_material_expressions(glint, "", glints, "A"):
        raise RuntimeError("connect glint -> glints.A failed")
    if not mel.connect_material_expressions(moon_glint, "", glints, "B"):
        raise RuntimeError("connect moon_glint -> glints.B failed")

    surface_light = _node(b, unreal.MaterialExpressionAdd, -220, 1540)
    if not mel.connect_material_expressions(reflection, "", surface_light, "A"):
        raise RuntimeError("connect reflection -> surface_light.A failed")
    if not mel.connect_material_expressions(glints, "", surface_light, "B"):
        raise RuntimeError("connect glints -> surface_light.B failed")

    # --- SURFACE PRESENCE: the grazing-only sheen (2026-09-05) --------------
    #
    # The full argument -- the owner directive, why the engine's EnvBrdf split
    # cannot deliver it, why opacity cannot either, and why exponent 8 with a
    # zero base is what keeps this from being the retired LegacySkyReflectGain
    # term coming back -- is the SURFACE PRESENCE section of the module
    # docstring. Mechanically: (1-NoV)^8 against the material's own shading
    # normal (or normal_ws, same rule as the front Fresnel above), times the
    # SAME gated sky colour the rest of the chain mirrors -- so it obeys the
    # day gate, scales with the moon at night, carries the stars when the arm
    # is built, and vanishes with all of them at twilight -- times the one
    # scalar. Added into surface_light UPSTREAM of the foam and top-face
    # suppressions, so whitewater and side walls opt out for free.
    grazing_fresnel = _node(b, unreal.MaterialExpressionFresnel, -650, 1560)
    grazing_fresnel.set_editor_property("exponent", d["SurfacePresenceExponent"])
    grazing_fresnel.set_editor_property("base_reflect_fraction", 0.0)
    if normal_ws is not None:
        if not mel.connect_material_expressions(normal_ws, "", grazing_fresnel, "Normal"):
            raise RuntimeError("connect normal_ws -> grazing_fresnel.Normal failed")

    surface_presence = _scalar_param(
        b, "SurfacePresence", d["SurfacePresence"], -650, 1830)
    sheen = _node(b, unreal.MaterialExpressionMultiply, -420, 1560)
    if not mel.connect_material_expressions(sky_reflected, "", sheen, "A"):
        raise RuntimeError("connect sky_reflected -> sheen.A failed")
    if not mel.connect_material_expressions(grazing_fresnel, "", sheen, "B"):
        raise RuntimeError("connect grazing_fresnel -> sheen.B failed")

    sheen_gained = _node(b, unreal.MaterialExpressionMultiply, -300, 1560)
    if not mel.connect_material_expressions(sheen, "", sheen_gained, "A"):
        raise RuntimeError("connect sheen -> sheen_gained.A failed")
    if not mel.connect_material_expressions(surface_presence, "", sheen_gained, "B"):
        raise RuntimeError("connect surface_presence -> sheen_gained.B failed")

    surface_lit = _node(b, unreal.MaterialExpressionAdd, -160, 1500)
    if not mel.connect_material_expressions(surface_light, "", surface_lit, "A"):
        raise RuntimeError("connect surface_light -> surface_lit.A failed")
    if not mel.connect_material_expressions(sheen_gained, "", surface_lit, "B"):
        raise RuntimeError("connect sheen_gained -> surface_lit.B failed")

    # THE TWO OPTIONAL SUPPRESSIONS, in the order they always ran: foam first,
    # then the top-face mask. Their arguments are at the `foam` and
    # `top_face_mask` parameter notes in the docstring; what matters here is
    # that a None input builds NOTHING -- no dead multiply by a constant, for
    # the reason create_ocean_material.py's vertex-colour section gives: a
    # multiply by one in the graph suggests the slot is being filled by
    # something, and the honest record of "this surface has no such signal" is
    # the multiply's absence.
    emissive = surface_lit
    if foam is not None:
        one_minus_foam = _node(b, unreal.MaterialExpressionOneMinus, -340, 1660)
        if not mel.connect_material_expressions(foam, "", one_minus_foam, ""):
            raise RuntimeError("connect foam -> one_minus_foam failed")

        surface_unfoamed = _node(b, unreal.MaterialExpressionMultiply, -190, 1600)
        if not mel.connect_material_expressions(emissive, "", surface_unfoamed, "A"):
            raise RuntimeError("connect surface_light -> surface_unfoamed.A failed")
        if not mel.connect_material_expressions(one_minus_foam, "", surface_unfoamed, "B"):
            raise RuntimeError("connect one_minus_foam -> surface_unfoamed.B failed")
        emissive = surface_unfoamed

    if top_face_mask is not None:
        surface_emissive = _node(b, unreal.MaterialExpressionMultiply, -40, 1600)
        if not mel.connect_material_expressions(emissive, "", surface_emissive, "A"):
            raise RuntimeError("connect surface_unfoamed -> surface_emissive.A failed")
        if not mel.connect_material_expressions(top_face_mask, "", surface_emissive, "B"):
            raise RuntimeError("connect top_face_mask -> surface_emissive.B failed")
        emissive = surface_emissive

    return {
        "emissive": emissive,
        "refl_vec": refl_vec,
        "surface_light": surface_lit,
        "glints": glints,
        "reflection": reflection,
        "fresnel": fresnel,
        "sheen": sheen_gained,
        "grazing_fresnel": grazing_fresnel,
    }
