"""Author M_Ocean: the open sea's surface, on the SAME derivation the lake uses.

REWRITTEN 2026-09-04 (ocean/tides plan B5, docs/water-ocean-tides-plan-2026-09-04.md).
What was here before is gone and this docstring is the record of why, because the
old material was not a rougher version of this one -- it was an unrelated one.


=============================================================================
WHAT WAS DELETED, AND WHY IT IS NOT A REGRESSION TO DELETE IT
=============================================================================

The previous M_Ocean was authored in July as a deliberate placeholder for a
2-triangle 65 km plane: BLEND_TRANSLUCENT, a hand-typed
Constant3Vector(0.015, 0.12, 0.16) for base colour, a hand-typed 0.8 opacity, a
hand-typed 0.05 roughness, and a normal built from two panning sine waves at
0.015 and 0.021 radians per UU. Its own docstring called it "deliberately basic"
and said "real wave shading/foam/caustics are W5 polish". It was honest about
what it was.

Every one of those five numbers is now WRONG IN A SPECIFIC WAY, and they are
worth listing because "it was a placeholder" is not by itself an argument:

  * THE COLOUR AND THE OPACITY WERE A SECOND, SILENT AUTHORITY ON WHAT WATER
    LOOKS LIKE. docs/water-architecture.md:196-215 states the rule this project
    paid for once: there are two renderers of water (the surface, and
    M_Underwater's post-process Beer-Lambert), neither can see the other's
    shading, and the ONLY thing keeping them the same liquid is that both import
    water_optics.py -- "neither is allowed to type a coefficient as a literal".
    The old M_Ocean typed four. It shipped a third liquid, and nobody would have
    found out from a log: you would have found out by swimming from a lake into
    the sea and watching the water change.
  * A TRANSLUCENT WATER SURFACE CANNOT BE A SINGLE LAYER WATER SURFACE. The
    engine rejects it by name (MaterialShared.cpp:6425). So the old ocean could
    not have the absorption, the scattering, the phase function or the
    transmitted scene the lake has had since 2026-08-11, at any tuning.
  * THE OLD RIPPLE REPEATED. Two 1-D sines added into one normal component,
    sampled from absolute world position -- the same shape as the normal ripple
    create_water_voxel_material.py deleted for the owner's "looks like a
    repeating tile", with the same arithmetic behind it. It is not ported. It is
    not resurrected as a "cheap distant option" either; there is one wave field.
  * AND IT HAD NO WIND, NO DEPTH, NO BREAK AND NO FOAM, so the sea did not
    respond to the weather the lake beside it was responding to.

The replacement types NO optical constant and NO wave constant of its own.


=============================================================================
SHARED MODULES, SEPARATE ASSETS -- AND WHY THAT IS NOT A COMPROMISE
=============================================================================

water_wave_graph.py's docstring named this file, three weeks before this change,
as the second surface the wave field was made a module for: "M_Ocean's implicit
plane is the obvious candidate -- it is a different material authored by
create_ocean_material.py and today it has its own unrelated ripple." This is
that convergence, and it now takes all five shared modules:

  water_optics.py        the four physical constants. Absorption distance,
                         absorption colour, scattering, phase g. IMPORTED, never
                         typed. Both this file and the lake print
                         water_optics.summary_lines() into their run log, so "were
                         these two built from the same numbers" is answerable
                         from two logs without opening either asset.
  water_wave_graph.py    the eight-octave wind-driven field, the McCowan shore
                         break, and the WPO distance fade. Same wind inputs
                         (MPC_VoxelSky.WindVectorMS / WindFieldValid), same
                         fallback, same parameter names and defaults.
  bathy_field_graph.py   the one sample of /Game/Voxel/T_VoxelBathyInfo and the
                         one Snell slant conversion, so the sea's waterline lands
                         where the terrain's wet-shore darkening puts it.
  ripple_field_graph.py  the player-local ripple/wake field, guarded exactly as
                         the lake guards it (see THE RIPPLE ARM below).
  water_sky_reflection_graph.py
                         the surface-light chain: sun glint, moon glint, the
                         Fresnel sky term with its night branch, reflected
                         stars. Promoted out of the lake generator and consumed
                         here on 2026-09-05 -- see THE SURFACE-LIGHT CHAIN in
                         the body, and the section below for the ruling.

WHAT IS NOT SHARED IS THE ASSET, and that is deliberate rather than lazy: the
VERTEX-COLOUR CONTRACT DIFFERS, which is the one thing a single material could
not straddle. See THE VERTEX COLOURS below.

The pattern is the project's own, twice over: sky_star_graph.py owns the one star
lookup M_NightSky and M_SkyAtmosphereDome share, terrain_material_common.py owns
the one biome graph M_VoxelTerrain and M_VoxelClipmap share. Both make the same
argument, which is the whole reason: TWO COPIES OF ONE DERIVATION DO NOT FAIL
LOUDLY. They drift, and the result is still sharp, still animated, still
plausible, and wrong, with nothing in a frame to say so.


=============================================================================
THE VERTEX COLOURS: WHAT THE SHARED SITES WANT, AND WHAT THIS MESH HAS
=============================================================================

M_WaterVoxel reads three vertex-colour channels off the mesher and this mesh
carries NONE of them. The ocean grid (plan B5, AVoxelOceanActor's concentric-ring
PMC) is plain geometry: the mesh lane ships plain verts, by agreement, because
inventing a vertex-colour contract for a mesh with no cells in it would be
inventing data.

  R  CA fill fraction. Drives the lake's stepped fill-drop WPO so a partly-filled
     voxel's top boundary sits at its own fill height. THE OCEAN HAS NO CELLS
     AND NO FILL FRACTION -- its surface is a datum, not a stack of quantised
     water columns -- so there is no fill-drop term in this material at all.
     Absent, not defaulted.
  G  greedy-mesher AO. There is no mesher and no occlusion to encode; the ocean
     grid is a flat sheet with nothing above it. The lake multiplies its base
     colour by this; here that multiply is simply not built.
  B  the TOP-FACE FLAG -- 1 if a vertex sits on the +Z boundary of its own voxel.
     THIS IS THE ONE THAT MATTERS, and it is the one the shared wave/ripple site
     leans on hardest. In M_WaterVoxel it masks BOTH halves of the field:
       - on the WPO half it is structural. An unmasked vertex offset would lift a
         side wall's BOTTOM vertices off the floor they are sealed against and
         open the mesh.
       - on the normal half it is cosmetic and still right: a submerged side wall
         should not shimmer.
     A CONSTANT 1 IS THE CORRECT SUBSTITUTE HERE, AND IT IS CORRECT RATHER THAN
     CONVENIENT: every vertex of the ocean grid IS a top vertex. There are no
     side walls, no bottom vertices and no sealed floor -- it is one horizontal
     sheet. The mask's entire job is to distinguish the top of a water voxel from
     the rest of it, and this mesh is all top.
     So the multiply is OMITTED rather than built against a Constant(1). A dead
     multiply by one in the graph would suggest the slot is being filled by
     something; it is not, and this paragraph is the record of the decision.
  A  per-brick FOAM ACTIVITY from the CA. There is no CA at sea. The lake takes
     max() of this with three other foam signals; here foam comes from the wave
     break alone (see FOAM V1).

CHECKED RATHER THAN ASSUMED, because a cross-lane agreement about a data format
is exactly the kind of thing that is agreed and then not done. AVoxelOceanActor's
grid builder writes `Colors.Add(FColor(255, 255, 255, 0))` at every vertex --
R = G = B = 1, A = 0. So the two lanes agree by arithmetic as well as by
intention: a VertexColor.B multiply would be a multiply by one, and an AO
multiply by G would be too. This material contains no VertexColor node at all,
which means the agreement can only fail in the SAFE direction -- if that mesh
ever starts writing meaningful colours, the material ignores them (a look that
does not change) rather than misreading them (a look that changes wrongly).

IT ALSO IGNORES THE MESH'S UVs, deliberately and for the reason the ORIGINAL
M_Ocean documented in its own comment before any of this: the grid's UV channel
is MESH-LOCAL (`FVector2D(X / 100.0, Y / 100.0)`), and a mesh-local coordinate on
a plane that RECENTRES with the camera makes the whole surface swim under the
viewer. Everything spatial in this material comes from absolute world position.


=============================================================================
THE DEPTH CONTRACT -- THE ONE CROSS-LANE AGREEMENT IN THIS FILE
=============================================================================

THE OCEAN READS THE SAME BathyField AS THE LAKE. /Game/Voxel/T_VoxelBathyInfo,
placed by the same three MPC_VoxelSky parameters (BathyFieldOrigin,
BathyFieldInvSize, BathyFieldValid), sampled by the same
bathy_field_graph.sample_bathy_field. There is no second field, no second window
and no second UV convention -- which is the point, because a half-texel
disagreement between the sea and the beach it meets puts the waterline in two
places, and that is the artefact the whole bathymetry feature exists to remove.

WHAT THE SIBLING LANE IS ADDING (plan B5 / VoxelBathyField.cpp): the fill is
extended to write OCEAN depth into R, and a depth proxy into G, for ground below
sea level. Until that lands this material still generates and still runs -- it
simply sees R = 0 and validity = 1 over the sea inside the window, which is the
"dry" answer, and behaves exactly as the outside-the-window case below.

OUTSIDE THE 960 m WINDOW, AND THIS IS THE PART THAT DIVERGES FROM THE LAKE ON
PURPOSE:

  * NO BREAK. water_wave_graph's node computes `band = 1 - lerp(1, saturate(xb),
    Validity)` and `surf = lerp(1, saturate(xr), Validity)`, so validity 0 gives
    breaking = 0 and damping = 1. That degradation is already proven in the
    module and this file relies on it rather than reproducing it. It matters more
    at sea than on a lake: depth reads 0 where the field has no answer, and 0
    depth without the validity gate would mean "breaking everywhere", i.e. a
    horizon of whitewater.
  * BASE ABSORPTION, FROM A DEEP-WATER CONSTANT. `OceanDeepWaterDepthM` (60 m)
    stands in for the depth wherever validity is 0, and OceanDepthAuthority is
    NOT multiplied by validity -- which is exactly the opposite of what
    M_WaterVoxel does with BathyDepthAuthority, and the difference is the whole
    reason this parameter has its own name.

    THE ARGUMENT, because "the ocean is deep" is an assertion and this needs to be
    a derivation. MSM_SingleLayerWater's own absorption depth is
    `BehindWaterSceneDepth - WaterSurfaceSceneDepth`, measured ALONG THE VIEW RAY,
    and it is unbounded and uncancellable -- the material's only hook multiplies
    the scene colour BEFORE the engine's transmittance, and a multiply cannot
    recover a value exp() has already flushed to zero. That is the mechanism
    behind the black rim on every lake basin
    (docs/lake-sheet-black-band-2026-08-29.md), and at a grazing view over the
    sea it is worse, not better: the ocean is the largest near-coplanar surface
    in the world and the horizon is the most grazing view in the frame.

    A LAKE OUTSIDE THE BATHYMETRY WINDOW HAS NO KNOWN DEPTH, so M_WaterVoxel is
    right to hand the whole absorption back to the engine there -- falling back
    to the pre-Phase-3 picture is the honest thing to do when you do not know.
    THE SEA IS NOT IN THAT POSITION. Its depth is not unknown; it is deep, by
    construction, everywhere the connectivity fill calls it ocean. So this
    material keeps 85% of the absorption on a BOUNDED analytic path (the slant
    of a 60 m column -- capped at 20x vertical by build_slant_depth's clamp
    since the 2026-09-05 secant default, [d, 1.52 d] under the original Snell
    arm; bounded either way, unlike the scene ray) and leaves 15% on the
    engine's exact-against-real-geometry term, which is what a boat hull and a
    swimmer need (plan D2). Deep water then converges on the single-scattering
    albedo -- water_optics prints it, currently (0.014, 0.187, 0.394) -- which is
    a saturated blue, which is what open sea is. Saturating is the CORRECT answer
    at sea and the WRONG one on a two-metre pond; that asymmetry is the whole
    reason the two materials weight this differently.

    60 m IS NOT MEASURED AND IS NOT PRETENDING TO BE. It is "deeper than the
    depth at which this water's absorption has already saturated" -- at the
    shipped extinction the red channel is gone by 5 m and blue by ~25 m, so
    anything past about 30 m is indistinguishable and the number is only
    load-bearing in that it must not be SMALL. Stated so nobody tunes it looking
    for an effect.

  * AND THE SHORE CLIP TURNS ITSELF OFF THERE, because it is gated on the same
    validity. A clip that fired on a field with no answer would delete the sea.


=============================================================================
FOAM V1 (+ F2 WHITECAPS, 2026-09-05)
=============================================================================

V1 is the wave field's fourth output; F2 maxes in wind-driven whitecap
coverage from the SAME field's slopes and wind (water_wave_graph.
build_whitecap_foam -- reuse, no second field; calm published wind = zero
whitecaps; off behind FoamV2Enabled and the cvar-driven MPC FoamV2Gain).
The V1 half is unchanged: `breaking` is already
gated four ways inside the WaveField node -- proportional to the wave (no wave,
no band, at any wind), riding the CRESTS so it travels rather than being a
painted stripe, an absolute 5 cm size gate underneath, and multiplied by the
bathymetry validity -- which is the same permanent-white-ring discipline the lake
applies, arrived at in the shared module rather than reproduced here.

The lake maxes it with three more signals (bed slope, CA activity, a baked
shoreline band). None of those exist at sea: there is no CA, and the shoreline
band and shelf gate are lake-shaped terms keyed to a basin's own bed. A beach's
surf line comes out of the break term for free, from the same McCowan H/d = 0.78
the lake uses, as soon as the depth contract above is filled in.

IT LANDS ON THREE PINS, exactly as the lake's does, and the third is the one
somebody will disconnect: BaseColor (whitewater is a diffuse layer ON the water
and the only thing that belongs on BaseColor for an SLW surface), Roughness
(0.08 -> 0.62, because a tight specular lobe on froth reads as wet plastic), and
MP_OPACITY. Read the note at the Opacity pin before touching it.


=============================================================================
THE SURFACE-LIGHT CHAIN: CONSUMED (2026-09-05), AND WHAT IS STILL NOT HERE
=============================================================================

THE SEA HAS THE SUN GLINT, THE MOON GLINT, THE FRESNEL SKY TERM AND THE
REFLECTED STARS NOW -- the four things this section used to open by saying it
did NOT have. The history matters enough to keep: the B5 rewrite shipped
without them because porting "roughly seven hundred lines" of
create_water_voxel_material.py would have been the exact
two-copies-of-one-derivation failure this whole file is about, and the honest
fix -- promote the chain into a shared surface module the way the wave field
and the optics were promoted -- was flagged as an open decision for the owner
to make from the first ocean captures. Both halves then happened on
2026-09-05: the chain became Tools/water_sky_reflection_graph.py (verbatim --
same nodes, same links, same parameter names, proven by the offline mock
regenerating M_WaterVoxel to identical counts), and the owner's ruling on the
first review came back "Sea should not be flat - it should have waves,
reflection, etc to look realistic", which closed the plan's Materials OPEN
item 1 in the consuming direction. The call site is THE SURFACE-LIGHT CHAIN
in the body; the flat-sea-at-the-coast defect that section of the plan
predicted -- two lake paths carrying a sheen and a sun path the sea beside
them lacked -- is retired by construction, because all three surfaces now
compute those terms from one module against one MPC.

NO CAUSTICS, NO REFRACTION, NO SCENE-COLOUR READ, still. Same rule as the
lake: reading scene DEPTH is order-invariant, reading scene COLOUR is not.
The star arm's texture fetch is the allowed shape -- T_SkyStarmap is the same
texture whatever else has been drawn -- and the chain's module docstring
carries that argument so it cannot be re-litigated per consumer.


Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash

MUST BE RUN AFTER create_sky_material.py, EVERY TIME IT RUNS. That script is the
sole author of MPC_VoxelSky and DELETES the asset on every run; this material now
binds eleven parameters on it (three bathymetry, three ripple, two wind, and --
since the surface-light chain landed -- SunDirection, MoonDirection and
MoonLightFraction; twelve counting StarBrightness on the default star arm), and
a material holding a binding to a collection that
was recreated under it compiles to UE's DEFAULT MATERIAL while every log line
reports success. That is the 2026-08-10 failure, and it cost a night. The order
is enforced by tools/voxel-sky-chain-regen.ps1, which is also where this
generator is listed; it discovers dependents by grepping for the binding, so it
would have refused to run at all had this file been left out of $ORDER.
"""

import os
import sys

import unreal

# A -run=pythonscript commandlet does not put the script's own directory on
# sys.path, so add it explicitly -- same as create_sky_material.py:417-420 and
# create_water_voxel_material.py:472-474.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sky_star_graph import SkyGraphBuilder  # noqa: E402
from bathy_field_graph import build_slant_depth, sample_bathy_field  # noqa: E402
from ripple_field_graph import build_disturbance_foam, sample_ripple_field  # noqa: E402
import ripple_field_graph  # noqa: E402
import water_optics  # noqa: E402
from water_sky_reflection_graph import build_sky_reflection  # noqa: E402
from water_hull_mask_graph import build_hull_mask, build_hull_ripple_mask  # noqa: E402
import water_wave_graph  # noqa: E402

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_Ocean"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME
COLLECTION_PATH = "/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky"

# THE THREE GENERATION-TIME ARMS ARE THE LAKE'S OWN VARIABLES, NOT NEW ONES,
# AND THAT IS THE POINT.
#
# VOXEL_WATER_FREEZE_TIME replaces the single MaterialExpressionTime with a
# Constant(0), which is how this project separates "the water is animating" from
# "the water is flickering" in a capture. It reads the LAKE's variable rather
# than an ocean-specific one because a frozen capture with a moving sea in it is
# not a frozen capture -- one flag has to stop every animated water surface in
# the frame or the instrument is lying about what it froze.
#
# VOXEL_WATER_LEGACY_WAVES likewise: it is the A/B for the whole wind-wave
# feature, and an A/B that changes the lake and leaves the sea on the new field
# measures a mixture.
#
# VOXEL_WATER_STAR_REFLECT likewise, since the sea started consuming the
# surface-light chain (2026-09-05): it is the build-or-not-build arm for the
# reflected-stars subgraph, it exists so that subgraph's GPU cost can be
# measured honestly from one binary (a zeroed uniform measures nothing -- the
# fetch, the atan2 and the derivative corrections all still execute), and one
# frame holds both waters, so an OFF-arm lake beside an ON-arm sea would
# measure a mixture. Same default as the lake -- unset builds the full
# material -- and the same reading of the raw value, so the two generators
# cannot parse one environment differently.
FREEZE_TIME_ENV = "VOXEL_WATER_FREEZE_TIME"
FREEZE_TIME = os.environ.get(FREEZE_TIME_ENV, "0").strip().lower() not in (
    "0", "off", "false", "no", "")
LEGACY_WAVES_ENV = "VOXEL_WATER_LEGACY_WAVES"
LEGACY_WAVES = os.environ.get(LEGACY_WAVES_ENV, "0").strip().lower() not in (
    "0", "off", "false", "no", "")
STAR_REFLECTION_ENV = "VOXEL_WATER_STAR_REFLECT"
STAR_REFLECTION = os.environ.get(STAR_REFLECTION_ENV, "1").strip().lower() not in (
    "0", "off", "false", "no", "")


def main():
    def log(line):
        unreal.log("M_Ocean " + line)

    # THE OPTICS GO INTO THE LOG BEFORE ANYTHING IS BUILT, for the reason
    # water_optics.summary_lines' own docstring gives: these scripts run headless
    # in a commandlet and the log is the only artefact anybody reads afterwards.
    # If the sea and the lake ever disagree on screen, the first question is
    # whether they were built from the same numbers, and this block plus the same
    # block in create_water_voxel_material.py's log answers it without opening
    # either asset.
    for line in water_optics.summary_lines():
        log(line)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    sky_collection = unreal.load_object(None, COLLECTION_PATH)
    if sky_collection is None:
        raise RuntimeError(
            "MPC_VoxelSky not found at %s -- this material binds it for the bathymetry "
            "window, the ripple window and the wind. Run Tools/create_sky_material.py "
            "first; it is the sole author of that asset." % COLLECTION_PATH)

    if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
        unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material, factory)
    if material is None:
        raise RuntimeError("Failed to create material asset at " + FULL_PATH)

    # --- SINGLE LAYER WATER: MASKED, TWO-SIDED -----------------------------
    #
    # MASKED AND NOT OPAQUE, and not translucent, and there is no fourth option:
    # the engine rejects a translucent SLW material by name
    # (MaterialShared.cpp:6425, "SingleLayerWater materials must be opaque or
    # masked"). Masked is chosen for the same reason M_WaterVoxel moved to it on
    # the same day -- the ocean needs a shore clip (see THE OPACITY MASK below),
    # and an OPAQUE material silently ignores MP_OPACITY_MASK, which would leave
    # the clip in the graph, in the asset, in the log, and doing nothing to a
    # single pixel.
    #
    # OPAQUE HERE HAS NEVER MEANT "YOU CANNOT SEE THROUGH IT". An SLW surface
    # writes depth and GBuffer like any opaque surface and then its own pass
    # composites the scene behind it through the absorption and scattering
    # coefficients wired below. "See the bottom in the shallows" is a physical
    # result of those coefficients, not an authored alpha -- which is exactly
    # what the old translucent placeholder could not do at any setting.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)

    # TWO-SIDED, AND THE OLD MATERIAL'S REASON STILL STANDS WORD FOR WORD: the
    # surface has to be visible from below once the player is swimming. The SLW
    # pass takes its cull mode from the same ComputeMeshCullMode path every other
    # pass uses (SingleLayerWaterRendering.cpp), so a two-sided SLW material is
    # not a special case anywhere.
    material.set_editor_property("two_sided", True)

    # Stated rather than inherited, same as M_WaterVoxel: the mask below is a
    # step() lerped against 1, so it is 0 or 1 wherever the bathymetry field is
    # confident, and any threshold strictly inside (0, 1) gives the same picture
    # there. 0.5 makes the behaviour in the field's own edge-fade band a written
    # choice instead of an engine default that could move between versions.
    material.set_editor_property("opacity_mask_clip_value", 0.5)

    blend_after = material.get_editor_property("blend_mode")
    if blend_after != unreal.BlendMode.BLEND_MASKED:
        raise RuntimeError(
            "blend mode did not take: %r, expected BLEND_MASKED." % (blend_after,))
    if not bool(material.get_editor_property("two_sided")):
        raise RuntimeError("two_sided did not take")
    clip_after = float(material.get_editor_property("opacity_mask_clip_value"))
    if abs(clip_after - 0.5) > 1.0e-6:
        raise RuntimeError(
            "opacity_mask_clip_value did not take: %r, expected 0.5." % (clip_after,))

    # THE SHADING MODEL IS DELIBERATELY *NOT* SET HERE. It goes on further down,
    # immediately after the SingleLayerWaterMaterialOutput node exists and is
    # wired, and the ordering is the whole point.
    #
    # Setting it here produces a correct asset AND emits one
    # "Failed to compile Material for platform PCD3D_SM6, Default Material will
    # be used in game / SingleLayerWater materials requires the use of
    # SingleLayerWaterMaterial output node" per shader map into the run log
    # first, because each set_editor_property fires PostEditChangeProperty and
    # recompiles a material that at that moment has no graph at all. Those lines
    # are transient and the final compile is clean -- and they are WORD FOR WORD
    # the string this project's release rule greps for. A generator that emits it
    # on every successful run destroys that check by making it always fire.
    # Measured and written up at create_water_voxel_material.py's copy of this
    # note; the property is moved rather than the guard weakened.

    b = SkyGraphBuilder(material, sky_collection)

    # ======================================================================
    # THE BATHYMETRY SAMPLE -- ONE, FIRST, SHARED BY EVERYTHING BELOW
    # ======================================================================
    #
    # Four consumers: the depth grading, the wave field's shore break, the wave
    # field's shore damping, and the opacity mask. One sample, one UV, one
    # validity -- see bathy_field_graph.py for the three separate ways the field
    # can be absent and why they are folded into that one number.
    bathy = sample_bathy_field(b)

    # ======================================================================
    # THE OPTICS -- IMPORTED, NEVER TYPED
    # ======================================================================
    #
    # THE 0.01 IS THE ONLY UNIT CONVERSION IN THIS FILE AND IT LIVES ON ONE NODE.
    # water_optics quotes everything PER METRE because that is how every
    # published figure is quoted and how a human can check it. The SLW node wants
    # 1/cm -- Epic's public docs say metres and are WRONG; the engine header
    # (MaterialExpressionSingleLayerWaterMaterialOutput.h:16) says "Unit is 1/cm"
    # and the shader agrees. Getting it backwards is a 100x error and it is the
    # single easiest mistake to make in this area, so, per that module's rule,
    # neither generator is allowed to type a 0.01 anywhere except here.
    per_cm = b.const(0.01)

    absorb_distance = b.scalar("AbsorptionDistanceM", water_optics.ABSORPTION_DISTANCE_M)
    # Star-unpacked rather than indexed, on purpose: a tuple of the wrong length
    # raises here, where indexing would silently drop a channel at the one site
    # whose whole job is to be the same three numbers the other two water
    # materials use.
    absorb_color = b.vector("WaterAbsorptionColor", *water_optics.ABSORPTION_COLOR)
    # -ln(0.02) = 3.9120230054281460586. The 2% survival convention is Unity's;
    # arbitrary, but it is the convention every published figure is quoted
    # against, so changing it would silently re-scale all of them.
    absorb_base = b.div(b.const(3.9120230054), absorb_distance)
    absorb_per_m = b.mul(absorb_base, b.one_minus(b.xyz(absorb_color)))
    absorb_per_cm = b.mul(absorb_per_m, per_cm)

    # THIS IS THE COLOUR OF DEEP WATER, which is not obvious from the name and
    # matters more at sea than anywhere else in the project: the engine's
    # single-scattering integral converges, as transmittance goes to zero, on
    # scattering / (scattering + absorption). Every pixel of open ocean past a
    # few tens of metres of path IS that number times the arriving light.
    # Absorption alone only REMOVES light -- crank it and the sea goes dark
    # rather than deep.
    # (4) per-channel extinction, same three scalars and defaults as the lake
    # (water_optics.ABSORPTION_CHANNEL_SCALE); 1/1/1 is the previous water.
    absorb_scale_rgb = b.append(
        b.append(b.scalar("WaterAbsorbScaleR", water_optics.ABSORPTION_CHANNEL_SCALE[0]), "",
                 b.scalar("WaterAbsorbScaleG", water_optics.ABSORPTION_CHANNEL_SCALE[1]), ""), "",
        b.scalar("WaterAbsorbScaleB", water_optics.ABSORPTION_CHANNEL_SCALE[2]), "")
    absorb_per_cm = b.mul(absorb_per_cm, absorb_scale_rgb)
    scatter_color = b.vector("ScatteringPerMetre", *water_optics.SCATTERING_PER_M)
    scatter_per_cm = b.mul(b.xyz(scatter_color), per_cm)

    phase_g = b.scalar("WaterPhaseG", water_optics.PHASE_G)

    # ======================================================================
    # DEPTH: THE BAKED FIELD, WITH A DEEP-WATER FALLBACK
    # ======================================================================
    #
    # The full argument is in THE DEPTH CONTRACT at the top of this file and it
    # is the most load-bearing paragraph in it. In short:
    #
    #   depth = lerp(OceanDeepWaterDepthM, bakedDepth, validity)
    #
    # so the sea always has A depth, and OceanDepthAuthority is applied WITHOUT
    # multiplying by validity -- unlike M_WaterVoxel's BathyDepthAuthority, which
    # is gated on it. A lake outside the bathymetry window has no known depth and
    # must hand the whole absorption back to the engine. The sea's depth is not
    # unknown, it is deep, and handing the engine an unbounded along-view-ray
    # absorption at the most grazing view in the frame is how you get a black
    # horizon (docs/lake-sheet-black-band-2026-08-29.md is the same mechanism at
    # a pond's edge).
    #
    # DIFFERENT NAME BECAUSE DIFFERENT QUANTITY. Giving this the lake's
    # BathyDepthAuthority name would put one knob on two behaviours, and the
    # first person to override it on an instance would be told a true thing about
    # one material and a false thing about the other.
    deep_depth_m = b.scalar("OceanDeepWaterDepthM", 60.0)
    depth_eff_m = b.lerp(deep_depth_m, "", bathy["depth_m"], "", bathy["validity"])
    depth_authority = b.scalar("OceanDepthAuthority", 0.85)

    # The engine's share of the absorption: whatever the analytic path did not
    # take. The two together add up to exactly one absorption, applied along two
    # different estimates of the same path.
    absorb_engine = b.mul(absorb_per_cm, b.one_minus(depth_authority))

    # ...and our share, which reaches the engine ONLY through
    # ColorScaleBehindWater -- a multiplier on the scene colour behind the water,
    # applied BEFORE the engine's own transmittance
    # (SingleLayerWaterShading.ush:238). There is no material input that
    # overrides the engine's depth; this is the only hook there is.
    #
    # THE ENGINE'S OWN SHORELINE FADE DOES REAL WORK FOR US HERE. :171 wraps this
    # in lerp(1, ColorScaleBehindWater, saturate(WaterVolumeDepth * 0.02)) -- a
    # fade to neutral over the first 50 cm of water -- so the analytic term
    # cannot draw a hard edge at the waterline even if its own gradient there
    # were abrupt. It is not a limitation being worked around; it is the reason
    # this hook is safe to use.
    behind_scale = b.vector("ColorScaleBehindWater", 1.0, 1.0, 1.0)
    slant_m = build_slant_depth(
        b, depth_eff_m,
        b.mask(b.node(unreal.MaterialExpressionCameraVectorWS), "", b=True))
    slant_uu = b.mul(slant_m, b.const(100.0))
    optical = b.mul(b.mul(absorb_per_cm, depth_authority), slant_uu)
    transmit = b.unary(unreal.MaterialExpressionExponential,
                       b.mul(optical, b.const(-1.0)))
    behind_scale_rgb = b.mul(b.xyz(behind_scale), transmit)

    # --- THE OUTPUT NODE ----------------------------------------------------
    #
    # WITHOUT THIS NODE THE MATERIAL DOES NOT COMPILE (MaterialShared.cpp:6436).
    # It is a CustomOutput, not a material property, so it is wired by input NAME
    # -- the UPROPERTY names off MaterialExpressionSingleLayerWaterMaterialOutput.h
    # -- and every link is checked like everything else in this toolchain. A
    # renamed pin in a future engine version raises here rather than silently
    # compiling to the node's default (Constant3(0,0,0) for the two coefficient
    # pins), which would be perfectly clear, perfectly invisible water.
    # (1) the turbidity floor, same derivation and scalars as the lake
    # (create_water_voxel_material.py, "THE TURBIDITY FLOOR"): boosted
    # scattering where the baked depth is small. depth_eff_m is 60 m wherever
    # the bathy is invalid, so open sea is untouched by construction.
    turb_floor = b.scalar("ShallowTurbidityFloor", 0.7)
    turb_depth = b.scalar("ShallowTurbidityDepthM", 1.5)
    turb_boost = b.scalar("ShallowScatterBoost", 15.0)
    turb = b.mul(b.mul(b.one_minus(b.ramp(depth_eff_m, "", b.const(0.0), turb_depth)), turb_floor),
                 bathy["validity"])
    scatter_per_cm = b.mul(scatter_per_cm, b.add(b.const(1.0), b.mul(turb, turb_boost)))
    slw_out = b.node(unreal.MaterialExpressionSingleLayerWaterMaterialOutput)
    b.link(scatter_per_cm, "", slw_out, "ScatteringCoefficients")
    b.link(absorb_engine, "", slw_out, "AbsorptionCoefficients")
    b.link(phase_g, "", slw_out, "PhaseG")
    b.link(behind_scale_rgb, "", slw_out, "ColorScaleBehindWater")

    # --- AND ONLY NOW IS IT A SINGLE LAYER WATER MATERIAL -------------------
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
    authored = material.get_editor_property("shading_model")
    if authored != unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER:
        raise RuntimeError(
            "shading model did not take: authored ShadingModel is %r, expected "
            "MSM_SINGLE_LAYER_WATER." % (authored,))
    # The CACHED FMaterialShadingModelField -- the one the renderer, the
    # primitive relevance flags and the SLW mesh pass actually read -- is NOT
    # reachable from Python on this engine build (measured; the struct is not
    # exported). So what is checked here is the AUTHORED field, and the cached
    # one is confirmed instead by the ABSENCE of the "requires the use of
    # SingleLayerWaterMaterial output node" compile error, which the regen chain
    # enforces on every run. Reporting a bindings gap as a pass would be the
    # worse lie, so this says which of the two it checked.
    log("SHADING MODEL: authored=%s (the CACHED ShadingModels field is not readable from "
        "Python on this build -- confirm it from the absence of a 'requires the use of "
        "SingleLayerWaterMaterial output node' compile error below)" % (authored,))

    # ======================================================================
    # THE WAVE FIELD
    # ======================================================================
    #
    # ONE Time node drives the whole field, which is what makes
    # VOXEL_WATER_FREEZE_TIME a single-node substitution rather than a graph
    # edit: freezing that one node provably freezes every animated term on this
    # surface, including the breaking foam, because the foam rides the same
    # crests off the same evaluation.
    if FREEZE_TIME:
        time_expr = b.const(0.0)
    else:
        time_expr = b.node(unreal.MaterialExpressionTime)

    # ABSOLUTE WORLD XY IN METRES, and this is the single most important line in
    # the section. create_water_voxel_material.py:2303-2320 has the long form;
    # the short form is that the pooled vertex factory's UV repeats every 32 m
    # and mirrors about the world axes, the far-field sheets anchor theirs at
    # their own bounding-box corner, and world XY is the only coordinate every
    # water draw path can agree on -- it has no period. THAT ARGUMENT IS ABOUT
    # THE OCEAN TOO, and more so: this surface MEETS both of the others at a
    # coastline, and three surfaces with three phase origins meet in three seams.
    #
    # WPT_EXCLUDE_ALL_SHADER_OFFSETS because this value feeds World Position
    # Offset, and reading a position that already contained this material's own
    # WPO would put the wave into its own input.
    world_pos = b.node(unreal.MaterialExpressionWorldPosition)
    world_pos.set_editor_property(
        "world_position_shader_offset",
        unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    pos_m = b.mul(b.mask(world_pos, "", r=True, g=True), b.const(0.01))

    # STARTING FROM water_wave_graph's OWN TABLE, not from a dict typed here.
    # Every number in it is argued in that module against a measurement, and a
    # copy of any of them in this file would be a second, silent authority on the
    # same quantity -- which is the failure this whole rewrite is about.
    if LEGACY_WAVES:
        WAVE_DEFAULTS = dict(water_wave_graph.LEGACY_RECIPE)
    else:
        WAVE_DEFAULTS = dict(water_wave_graph.DEFAULTS)

    wave = water_wave_graph.build_wave_field(
        b, pos_m, time_expr, bathy, defaults=WAVE_DEFAULTS, log=unreal.log_warning)
    wave_grad = wave["gradient"]      # float2, dH/dx and dH/dy
    wave_height_m = wave["height_m"]  # float, metres
    wave_breaking = wave["breaking"]  # float 0..1

    # The line that matters in this log is wind_source: MPC_VoxelSky when the two
    # wind parameters are on the collection, material-fallback when they are not
    # -- which is the tell that create_sky_material.py has not been re-run, i.e.
    # that this material and the lake may be reading different winds.
    for line in water_wave_graph.summary_lines(wave["wind_source"]):
        log(line)

    # ======================================================================
    # THE RIPPLE ARM -- GUARDED AND DEGRADING, exactly as the lake guards it
    # ======================================================================
    #
    # sample_ripple_field RAISES if the three RippleField* names are absent from
    # MPC_VoxelSky or if the render target does not exist, and it is right to: an
    # unresolved CollectionParameter compiles to a CONSTANT rather than failing
    # (MaterialExpressions.cpp:17179-17193), so a constant origin would sample
    # one fixed texel for the whole world, and an unbound texture parameter would
    # add whatever image the engine picks to the sea's normal on every pixel.
    # Both are silent. Raising is the correct default for a module that cannot
    # know who is calling it.
    #
    # IT IS THE WRONG BEHAVIOUR HERE FOR A SCHEDULING REASON, NOT A DISAGREEMENT.
    # The two prerequisites land with other lanes' regeneration steps; calling
    # straight through would make M_Ocean UNGENERATABLE until they do, and take
    # the wind waves and the optics convergence -- which have no such dependency
    # -- down with them. So this probes and degrades LOUDLY, which is the same
    # shape water_wave_graph.build_wind_input already uses for the same situation.
    # The degraded arm is not an approximation of the ripple, it is its exact
    # absence: the sums become the wave field alone, which is what
    # RippleFieldGain = 0 produces anyway, minus the texture fetch.
    #
    # THE PROBE IS THE SAME TWO CHECKS THE MODULE ITSELF WOULD MAKE, so it cannot
    # pass here and fail there, and WHEN THE PREREQUISITES LAND THIS ARM
    # DISAPPEARS ON ITS OWN -- there is no environment variable and no default to
    # remember to flip.
    #
    # AND THE OCEAN TAKES THE RIPPLE AT ALL BECAUSE THE PLAYER SWIMS AND SAILS IN
    # IT (plan D4's boat wake injects into this same field). Its window is only
    # +/-25.6 m of the camera and it fades to zero at the edge, so the cost at
    # sea is one texture fetch on water the player is standing in.
    ripple_missing = sorted(
        n for n in (ripple_field_graph.MPC_ORIGIN,
                    ripple_field_graph.MPC_INV_SIZE,
                    ripple_field_graph.MPC_GAIN)
        if n not in b.mpc_names())
    try:
        # try/except and not a bare None check: unreal.load_object's failure mode
        # for a package that is not on disk is not guaranteed to be a None return
        # on every engine build, and the entire point of this block is that a
        # missing ripple field must not take the ocean material down with it.
        ripple_rt = unreal.load_object(None, ripple_field_graph.FIELD_TEXTURE)
    except Exception:  # noqa: BLE001 -- absence is the thing being tested for
        ripple_rt = None

    if ripple_missing or ripple_rt is None:
        unreal.log_warning(
            "M_Ocean RIPPLE FIELD ARM: ABSENT -- building the sea WITHOUT interactive "
            "ripples. Missing MPC_VoxelSky parameters: %s. Render target %s: %s. The wind "
            "wave field is unaffected and the water is exactly the water it would be with "
            "RippleFieldGain at 0. TO FIX: re-run create_sky_material.py and "
            "create_ripple_field_materials.py before this script -- which is the order "
            "tools/voxel-sky-chain-regen.ps1 already enforces."
            % (ripple_missing or "none",
               ripple_field_graph.FIELD_TEXTURE,
               "missing" if ripple_rt is None else "present"))
        grad_total = wave_grad
        height_total = wave_height_m
        disturbance_foam = None
        ripple_arm = "ABSENT"
    else:
        ripple = sample_ripple_field(b)
        log("RIPPLE FIELD ARM: PRESENT (%s bound; RippleFieldGain gates it at runtime and "
            "the subsystem holds it at 0 until the first simulated frame exists)"
            % ripple_field_graph.FIELD_TEXTURE)
        # SUMMING GRADIENTS IS THE WHOLE ARGUMENT. Two independent height fields
        # on one surface superpose, so their GRADIENTS add and the normal
        # assembled from the sum is the normal of the combined surface. Averaging
        # or lerping unit normals systematically flattens slopes, which is how
        # multi-octave water ends up looking like a bin liner. The ripple is a
        # ninth octave from a different source; any other order is a different
        # and wrong surface.
        # GATED BY WaveTimeScale, same node the field's T multiply uses (the
        # lake does this identically -- the 2026-09-05 live-finding argument
        # is at water_wave_graph.WAVE_TIME_SCALE_PARAM): the ripple RT is the
        # one animated input the scale's time multiply cannot reach, so its
        # amplitude rides the same knob and scale 0 is a provably still sea.
        ripple_grad_gated = b.mul(ripple["grad_xy"], wave["time_scale"])
        ripple_height_gated = b.mul(ripple["height_m"], wave["time_scale"])
        # THE COCKPIT IS DRY at sea too (owner, live 2026-09-08). The hull's
        # plan ellipse from MPC_VoxelSky zeroes the ripple WPO and the
        # disturbance foam inside the hull; the lake carries the full
        # argument at the same site, the mechanism is water_hull_mask_graph's.
        hull_ripple = build_hull_ripple_mask(b)
        ripple_height_gated = b.mul(ripple_height_gated, hull_ripple["keep"])
        grad_total = b.add(wave_grad, ripple_grad_gated)
        height_total = b.add(wave_height_m, ripple_height_gated)
        # The wake's ART channel (owner verdict 2026-09-05: "Do we actually
        # have a wake art effect...?") -- whitewater from the disturbance the
        # field carries, built from the GATED taps so foam and displacement
        # are one channel on one knob. Maxed into the foam chain below;
        # derivation and defaults are ripple_field_graph's.
        disturbance_foam = build_disturbance_foam(
            b, ripple_grad_gated, ripple_height_gated)
        disturbance_foam["foam"] = b.mul(disturbance_foam["foam"], hull_ripple["keep"])
        ripple_arm = "PRESENT"

    # ======================================================================
    # NORMAL -- one evaluation, first of two consumers
    # ======================================================================
    #
    # A height field's tangent-space normal is (-dH/dx, -dH/dy, 1). Z IS PINNED
    # TO 1 AND THE RESULT IS NOT NORMALIZED, deliberately: it means octaves
    # combine as SURFACE GRADIENTS rather than as blended normals, which is the
    # only correct way to combine height fields (see the sum above).
    #
    # THE OCEAN GRID'S TANGENT BASIS IS WORLD-ALIGNED -- it is a horizontal sheet
    # built with an up normal -- so the X/Y gradient lands in the directions it
    # was computed in, exactly as it does on a lake sheet's top face and on a
    # water brick's Axis==2 quad. If the mesh lane ever tilts or re-bases this
    # grid, this is the line that silently becomes wrong.
    #
    # NO VertexColor.B MASK. Every vertex of this mesh is a top vertex; there is
    # nothing to mask. See THE VERTEX COLOURS at the top for the full contract
    # difference and why the multiply is omitted rather than built against a
    # Constant(1).
    normal_xyz = b.append(b.mul(grad_total, b.const(-1.0)), "", b.const(1.0), "")
    b.prop(normal_xyz, "", unreal.MaterialProperty.MP_NORMAL)

    # ======================================================================
    # WORLD POSITION OFFSET -- the SAME evaluation's height
    # ======================================================================
    #
    # THE DISTANCE FADE IS NOT AN LOD SAVING. water_wave_graph.build_wpo_distance_fade
    # carries the argument; the reason the OCEAN takes it is the same reason the
    # lake sheet does, one step removed: the ocean grid is a concentric-ring mesh
    # whose cells DOUBLE at every ring, so every ring boundary is a T-junction
    # between a dense edge and a sparse one. The mesh lane stitches the inner
    # edges 2:1, which fixes the topology; it does not fix the DISPLACEMENT,
    # because a stitched edge still interpolates a moved midpoint differently on
    # its two sides. Zero displacement past 72 m makes every ring boundary but
    # the innermost a join between two surfaces that have not moved -- and the
    # innermost is at +/-96 m of 1.5 m cells, comfortably outside the fade.
    #
    # THE FADE IS SHARED WITH THE LAKE AND THAT MATTERS AT THE COAST. Two water
    # surfaces meeting at a shoreline must stop displacing at the SAME distance
    # or the seam between them opens exactly where a player stands.
    #
    # ORDER, and it is the lake's order for the lake's reasons: fade and
    # WaveWpoFraction are applied while the field is still in METRES, upstream of
    # the one 100.0.
    wpo_fade = water_wave_graph.build_wpo_distance_fade(b, pos_m, defaults=WAVE_DEFAULTS)

    # WaveWpoFraction: THE SAME PARAMETER NAME AND THE SAME 0.25 AS THE LAKE, AND
    # THIS ONE IS NOT A COPIED CONSTANT -- IT IS A SHARED ONE ON PURPOSE.
    #
    # The lake's 0.25 was argued from a 10 cm voxel grid and a bank-clipping
    # bound, neither of which constrains a 1.5 m ocean cell, so on its own merits
    # the sea could displace more. It does not, because THE TWO SURFACES MEET.
    # Near-field water, the far-field sheet and this grid can all be in one frame
    # along one shoreline; if the sea moved its crests 8.6 cm where the lake
    # moved them 2.1 cm, the difference would be a visible step in the water
    # surface at the exact line where one draw path hands over to another -- the
    # class of seam the owner has already made this project delete once.
    #
    # One name, one default, one override: setting WaveWpoFraction on an instance
    # of either material and not the other is the way to break this, and that is
    # what the shared name is for -- it makes the mistake visible.
    wave_wpo_fraction = b.scalar("WaveWpoFraction", 0.25)
    height_uu = b.mul(b.mul(b.mul(height_total, wpo_fade), b.const(100.0)),
                      wave_wpo_fraction)
    # (0,0,1) * scalar broadcasts to (0, 0, h): the same idiom the lake uses for
    # its own vertical terms. THERE IS NO FILL-DROP TERM TO ADD IT TO -- the lake
    # sums this with a stepped WPO that seats each cell's top boundary at its own
    # CA fill height, and the ocean has no cells and no fill fraction. Its surface
    # is a datum; the actual datum motion (the tide) is a TRANSFORM on the actor,
    # not a WPO, so it costs nothing here and cannot desynchronise from the
    # physics that reads the same datum in C++.
    b.prop(b.mul(b.const3(0.0, 0.0, 1.0), height_uu), "",
           unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    # ======================================================================
    # FOAM V1 + F2 WHITECAPS, AND THE THREE PINS THE COMPOSITE LANDS ON
    # ======================================================================
    #
    # F2 (2026-09-05): the wind-driven whitecap coverage joins the shore
    # break. build_whitecap_foam reads the field's OWN gradient and wind --
    # no second field -- and carries all the reasoning (the crest-times-
    # coverage product, the zero-whitecaps calm arm, the FoamV2Gain/
    # FoamV2Enabled control pair). MAX and not add, per the lake's own foam
    # doctrine: a crest breaking in the surf zone that also carries a wind
    # cap is one patch of white water, not two whites summed past 1.
    whitecap = water_wave_graph.build_whitecap_foam(b, wave)
    foam_signals = b.maximum(wave_breaking, whitecap["whitecap"])
    # ...and DISTURBANCE FOAM, the wake's art channel (see the ripple arm
    # above) -- absent (None) exactly when the ripple arm is absent, so the
    # degraded build stays the degraded build.
    if disturbance_foam is not None:
        foam_signals = b.maximum(foam_signals, disturbance_foam["foam"])
    foam = b.saturate(foam_signals)

    # BASE COLOUR IS BLACK WHERE THERE IS NO FOAM, and that is correct rather
    # than a placeholder. For an SLW surface BaseColor is the albedo of the thin
    # diffuse layer sitting ON the water; the engine adds the volume's colour
    # separately from the coefficients above. Putting a blue here would be double
    # counting -- a blue diffuse SURFACE plus a blue volume behind it, which
    # reads as blue paint over blue water and is precisely what the old
    # placeholder's Constant3Vector(0.015, 0.12, 0.16) did. Epic's own water
    # material does the same thing for the same reason.
    #
    # Whitewater is the exception and the only thing that belongs here: a dense
    # scattering layer floating on the surface genuinely is a diffuse albedo, it
    # genuinely should catch the sun and the sky, and it genuinely should not be
    # tinted by the volume underneath it.
    #
    # NO AO MULTIPLY. The lake ends its base colour with VertexColor.G, the
    # greedy mesher's occlusion; this mesh has no mesher and nothing above it to
    # occlude.
    b.prop(b.lerp(b.const3(0.0, 0.0, 0.0), "", b.const3(0.82, 0.90, 0.94), "", foam),
           "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- OPACITY IS THE SWITCH THAT TURNS THE WHOLE WATER VOLUME ON ---------
    #
    # READ THIS BEFORE DISCONNECTING IT. On an SLW material MP_Opacity is NOT
    # alpha and is NOT gated by blend mode -- Material.cpp,
    # UMaterial::IsPropertyActive_Internal makes it active whenever the shading
    # models contain MSM_SingleLayerWater. It is
    #
    #     BasePassPixelShader.usf:1140
    #         const float BaseMaterialCoverageOverWater = Opacity;
    #         const float WaterVisibility = 1.0 - BaseMaterialCoverageOverWater;
    #
    # i.e. the fraction of the pixel covered by the OPAQUE MATERIAL SITTING ON
    # the water -- foam, ice, a lily pad.
    #
    # AN UNWIRED INPUT IS NOT A NEUTRAL INPUT. MP_Opacity's default is 1.0
    # (MaterialAttributeDefinitionMap.cpp:401), so an empty pin compiles to
    # coverage = 1, WaterVisibility = 0, and SingleLayerWaterShading.ush:74 is
    # never entered: the absorption, the scattering, the phase function and the
    # whole ColorScaleBehindWater chain above are computed and thrown away. What
    # is left is a black BaseColor with Specular 0.5 and Roughness 0.08 -- a flat
    # dark mirror through which nothing behind the water is ever visible at any
    # depth. It compiles clean, it warns about nothing, and it looks like a
    # plausible if boring ocean, which is why it survived a full review of the
    # lake material once.
    #
    # SO THE CORRECT VALUE IS THE FOAM COVERAGE, and that is the quantity the
    # engine is asking for rather than a convenient reuse.
    b.prop(foam, "", unreal.MaterialProperty.MP_OPACITY)

    # Froth is the one part of a water surface that is NOT a mirror; leaving the
    # tight 0.08 lobe on it would put a sharp specular highlight on whitewater,
    # which reads as wet plastic. Specular stays flat at 0.5 -- the engine's own
    # unconnected-pin default, stated rather than left implicit so the two are
    # visibly tuned as a pair.
    b.prop(b.lerp(b.const(0.08), "", b.const(0.62), "", foam), "",
           unreal.MaterialProperty.MP_ROUGHNESS)
    b.prop(b.const(0.5), "", unreal.MaterialProperty.MP_SPECULAR)

    # ======================================================================
    # THE SURFACE-LIGHT CHAIN (sun glint, moon glint, sky reflection,
    # reflected stars): CONSUMED AS OF 2026-09-05
    # ======================================================================
    #
    # water_sky_reflection_graph.build_sky_reflection is M_WaterVoxel's whole
    # emissive chain -- promoted verbatim earlier the same day, and wired in
    # HERE on the owner's ruling from the first ocean review: "Sea should not
    # be flat - it should have waves, reflection, etc to look realistic." That
    # ruling closes the plan's Materials OPEN item 1
    # (docs/water-ocean-tides-plan-2026-09-04.md); the sea and the lake now
    # take their sun path, moon path, Fresnel sky term and reflected stars
    # from ONE derivation, so they cannot drift apart at the coastline where
    # both are in frame. All of the chain's reasoning -- the area-light glint,
    # the 2.4-degree drawn moon, the retired LegacySkyReflectGain measurement
    # table -- lives in that module; read it there.
    #
    # THE SITE IS LOAD-BEARING TWICE OVER: after `foam` exists, because froth
    # must suppress the mirror (a scattering layer does not reflect the sky),
    # and after MP_NORMAL is wired above, because the chain mirrors about the
    # material's own shading normal -- the wave field's rippled normal --
    # which is what leaving `normal_ws` unset selects. That is what makes the
    # sun path break up over the same waves the lake's does.
    #
    # SINCE 2026-09-05 THE CHAIN ALSO CARRIES SurfacePresence, the
    # grazing-only sky sheen built for the owner's "too transparent ... from
    # distance" directive -- SHIPPED AT 0.0 (OFF) by his same-day verdict on
    # the 1.0/0.5 frames (the mirrored sky washed the teal he liked; his
    # "reads as water" is body colour, not sky). The distance answer on this
    # surface is now the angle-true absorption path
    # (bathy_field_graph.build_slant_depth, BathyRefractInvN2 secant
    # default), which matters MOST here: the sea is the largest near-coplanar
    # sheet in the world and the horizon is the most grazing view in the
    # frame. The sheen stays at zero gain as the documented next ladder; see
    # SURFACE PRESENCE in the module docstring.
    sky_light = build_sky_reflection(
        b,                                 # this file's one builder; the star
                                           # arm auto-places in its layout lane
        star_reflection=STAR_REFLECTION,   # the LAKE's arm variable, decided
                                           # identically -- one frame holds
                                           # both waters, so mixed arms would
                                           # measure a mixture (see the arm
                                           # block at the top)
        foam=foam,                         # saturate(breaking): whitewater is
                                           # the one part of the surface that
                                           # does not mirror the sky
        top_face_mask=None)                # every vertex of this grid is a top
                                           # vertex -- same argument, same safe
                                           # direction, as THE VERTEX COLOURS;
                                           # the module builds no dead multiply
                                           # for an absent signal

    # EMISSIVE, not BaseColor, same as the lake: a reflection is light LEAVING
    # the surface, not albedo. Routed through BaseColor it would be multiplied
    # by AO and the diffuse term, so a reflected sky would darken in an
    # occluded corner, which is backwards. This pin was unwired before today,
    # so nothing was disconnected to make room for it.
    #
    # DISTURBANCE FOAM RIDES EMISSIVE TOO (2026-09-06): the lake generator
    # carries the full argument (owner session 9: field proven LIVE, foam
    # baked, nothing visible; the 2026-08-30 BaseColor null). Same term, same
    # scalar name, same tint, so one regen ladder tunes both waters. No top
    # face mask for the same reason the reflection passes None above.
    surface_emissive = sky_light["emissive"]
    # (1b) the body-colour floor on emissive, under the foam -- the lake's
    # "THE BODY-COLOUR FLOOR" note applies verbatim.
    body_rgb = b.append(b.append(b.scalar("ShallowBodyR", 0.05), "", b.scalar("ShallowBodyG", 0.22), ""), "",
                        b.scalar("ShallowBodyB", 0.20), "")
    body_weight = b.mul(b.mul(turb, b.scalar("ShallowBodyEmissive", 0.30)), b.one_minus(foam))
    surface_emissive = b.add(surface_emissive, b.mul(body_rgb, body_weight))
    if disturbance_foam is not None:
        dist_emiss_gain = b.scalar("DisturbanceFoamEmissive", 0.6)
        dist_emiss = b.mul(disturbance_foam["foam"], dist_emiss_gain)
        foam_emiss_tint = b.node(unreal.MaterialExpressionConstant3Vector)
        foam_emiss_tint.set_editor_property(
            "constant", unreal.LinearColor(0.82, 0.90, 0.94, 1.0))
        dist_emiss_rgb = b.mul(foam_emiss_tint, dist_emiss)
        surface_emissive = b.add(surface_emissive, dist_emiss_rgb)
    b.prop(surface_emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # ======================================================================
    # THE OPACITY MASK: THE SHORE CLIP
    # ======================================================================
    #
    # SAME SHAPE AS M_WaterVoxel's B4 CLIP, SAME BAKED FIELD, SAME DIRECTION OF
    # ERROR -- and it is here for the same mechanism read at the coast rather
    # than at a pond's edge: the ocean plane extends over ground that rises to
    # meet it, so at a grazing view there is a strip where the surface and the
    # bed are very nearly coplanar and the engine's along-view-ray absorption
    # depth diverges. That strip is the black rim
    # (docs/lake-sheet-black-band-2026-08-29.md), and at sea it is a beach's
    # whole waterline.
    #
    # THE SLACK ERRS TOWARDS DRAWING TOO MUCH, on purpose, exactly as the sheet's
    # extent mask does (water-architecture.md:231-242): sea drawn 90 cm too far
    # up the beach is hidden behind opaque sand standing above it, whereas sea
    # drawn 90 cm short is a visible gap between the water and the shore -- the
    # owner's 2026-08-10 screenshot. Over-cover, never under-cover, for drawing.
    #
    # WHICH CHANNEL, AND THE ONE HONEST CAVEAT IN THIS FILE. This reads the G
    # channel through bathy["shore_m"]. Today G is the baked SIGNED DISTANCE to
    # the shoreline in metres, positive inside water. The sibling lane extending
    # the fill for ocean depth intends to write a DEPTH PROXY into G for
    # below-sea ground, which is a different quantity in a different unit.
    #
    # THE CLIP IS ROBUST TO BOTH READINGS AND THAT IS NOT LUCK -- IT IS WHY IT IS
    # A SIGN TEST. Both quantities are POSITIVE IN WATER and NEGATIVE OR ZERO ON
    # LAND, so step(-slack, G) puts the cut in the same place under either
    # meaning. What DOES change is the width of the slack band: 0.9 means 90 cm
    # of horizontal distance under one reading and 90 cm of depth under the
    # other, and on a 1:20 beach those differ by a factor of twenty. So the clip
    # cannot be WRONG under the other reading, only differently generous, in the
    # over-covering direction. If the fill lane settles on the depth proxy, this
    # slack wants revisiting -- flagged rather than assumed.
    #
    # AND IT IS GATED ON VALIDITY, which is what stops it deleting the sea. Where
    # the field has no answer -- outside the 960 m window, unstreamed tile, no
    # fine tier -- validity is 0, the lerp returns 1, and the mask is inactive.
    # The failure mode of getting this backwards is an ocean that vanishes
    # everywhere the bake has not answered, i.e. almost everywhere.
    # DEFAULT 0.0 (2026-09-04 review finding #2): the sign test reads the SAME
    # lake-only G plane, and the sea survives it only while VoxelBathyField's
    # ocean fill is overwriting G with a positive depth proxy -- i.e. the
    # ocean's entire visibility inside the 960 m window would hang off the
    # `-VoxelBathyOcean` control arm and off the streamer existing. Run the
    # documented byte-identical control (`-VoxelBathyOcean=0`) with the clip ON
    # and the sea VANISHES out to ~480 m. The plane never needed the clip
    # anyway: it sits at z <= 0 and terrain above sea level occludes it by
    # depth test, which is how the old 2-triangle plane always worked. OPT-IN
    # via instance override or `-VoxelWaterMatScalar` when a real use appears.
    clip_enabled = b.scalar("OceanShoreClipEnabled", 0.0)
    clip_slack = b.scalar("OceanShoreClipSlackM", 0.9)

    # step(Y, X) is `X >= Y ? 1 : 0` -- UMaterialExpressionStep::Compile calls
    # Compiler->Step(Y, X), threshold first, matching the HLSL intrinsic. Both
    # pins are OPTIONAL in the header (they fall back to ConstY = 0 and
    # ConstX = 1), so a failed connect here would NOT error: it would silently
    # compile to step(0, 1) = 1 and the clip would be a no-op everywhere. Hence
    # the checked links, and hence the hasattr check -- this is the one engine
    # binding the B4/B5 work adds that nothing else in this toolchain has ever
    # exercised. If a future build drops it, the exact replacement is
    # saturate(ceil(shore_m + slack)).
    if not hasattr(unreal, "MaterialExpressionStep"):
        raise RuntimeError(
            "unreal.MaterialExpressionStep does not exist on this engine build; substitute "
            "saturate(ceil(shore_m + slack)) rather than softening the clip -- a smooth clip "
            "against a 0.5 mask threshold moves the waterline instead of placing it.")
    shore_step = b.node(unreal.MaterialExpressionStep)
    b.link(b.mul(clip_slack, b.const(-1.0)), "", shore_step, "Y")
    b.link(bathy["shore_m"], "", shore_step, "X")

    # `-VoxelWaterMatScalar=OceanShoreClipEnabled:0` (or an instance override)
    # takes the lerp's alpha to zero, the mask becomes a constant 1, and every
    # clipped pixel comes back -- with no regeneration. That is the control arm
    # for the CLIP and it is exact. IT IS NOT A CONTROL ARM FOR THE BLEND MODE:
    # the material is BLEND_MASKED in both arms, so whatever masked costs in the
    # mesh pass is paid in both and cancels in the A/B -- but it does not cancel
    # against a capture taken before this rewrite. A timing comparison against
    # the old translucent plane is measuring several changes at once and should
    # be reported as such.
    opacity_mask = b.lerp(b.const(1.0), "", shore_step, "",
                          b.mul(bathy["validity"], clip_enabled))

    # THE HULL EXCLUSION MASK, multiplied in exactly as the lake multiplies it
    # (owner boat-session directive; the contract and the inert-default
    # argument are water_hull_mask_graph's). The sea is where the boat mostly
    # lives, so this surface is the one the canoe verdict was ABOUT.
    hull = build_hull_mask(b)
    opacity_mask = b.mul(opacity_mask, hull["keep"])
    b.prop(opacity_mask, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    mel = unreal.MaterialEditingLibrary
    mel.layout_material_expressions(material)
    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)

    # --- READ THE SAVED PACKAGE BACK ---------------------------------------
    #
    # NOT the UObject in memory, and the distinction is the point: the in-memory
    # read above can only see the AUTHORED ShadingModel, because this build does
    # not export the cached FMaterialShadingModelField to Python. This reads the
    # bytes that were just written.
    #
    # IT WORKS BECAUSE UE SERIALISES ENUM PROPERTIES BY NAME -- a saved
    # UMaterial's name table literally contains "MSM_SingleLayerWater" or
    # "MSM_DefaultLit" and "BLEND_Masked" or "BLEND_Translucent" -- and because a
    # saved UMaterial also names every expression CLASS it uses, which is what
    # makes the last two entries proof that the SLW output node and the shore
    # clip survived into the artefact rather than merely into the graph.
    #
    # BLEND_Opaque is reported but NOT asserted on: whether a defaulted enum name
    # survives into a package's name table is an engine serialisation detail, not
    # a claim about this asset, and a check that can fail for a reason that is not
    # the failure being looked for is not a check.
    package_ok = None
    try:
        with open(os.path.join(unreal.Paths.project_content_dir(),
                               "Voxel", "M_Ocean.uasset"), "rb") as fh:
            blob = fh.read().decode("latin-1")
        found = {name: (name in blob) for name in (
            "MSM_SingleLayerWater", "MSM_DefaultLit",
            "BLEND_Masked", "BLEND_Opaque", "BLEND_Translucent",
            "MaterialExpressionStep",
            "MaterialExpressionSingleLayerWaterMaterialOutput")}
        package_ok = (found["MSM_SingleLayerWater"]
                      and not found["MSM_DefaultLit"]
                      and found["BLEND_Masked"]
                      and not found["BLEND_Translucent"]
                      and found["MaterialExpressionStep"]
                      and found["MaterialExpressionSingleLayerWaterMaterialOutput"])
        log("PACKAGE READ-BACK: %s -- %s"
            % ("SINGLE LAYER WATER, MASKED, SHORE CLIP PRESENT" if package_ok else "WRONG",
               ", ".join("%s=%s" % (k, v) for k, v in sorted(found.items()))))
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "M_Ocean: could not read the saved package back (%r), so the shading model on "
            "disk is UNKNOWN. Do not read a capture taken on this build as evidence that the "
            "rewrite is live." % (exc,))
    if package_ok is False:
        raise RuntimeError(
            "the saved M_Ocean.uasset is NOT a Single Layer Water MASKED material with a Step "
            "node in it. The renderer would draw it as an opaque DefaultLit surface with a "
            "black base colour (BaseColor is foam-only), i.e. a black sea -- or, if only the "
            "Step is missing, as an ocean with no shore clip and a black rim along every "
            "beach. Nothing else in this run would have said so.")

    # THE RAN-FLAG, and it is deliberately not "success". This project's standing
    # rule is that a stage must log something that distinguishes "ran and found
    # nothing" from "did not run" -- and for a material built by re-running this
    # script with environment arms flipped, the thing that must be distinguishable
    # is WHICH ARM was built. So each arm is named, the variable's raw value is
    # echoed beside it, and the ripple arm is stated: an ABSENT ripple arm on a
    # run whose log also says the collection had all three names is a
    # contradiction this line makes visible.
    # The star arm is read back off the SAVED graph, not echoed from the flag,
    # for the lake's reason (a variable that never reached the process builds
    # the OTHER arm silently): the ON arm's chain contains exactly one
    # StarmapTex texture parameter, the OFF arm's contains none.
    try:
        star_nodes = len([p for p in mel.get_texture_parameter_names(material)
                          if str(p) == "StarmapTex"])
    except Exception:  # noqa: BLE001 -- see the lake's -1-is-UNKNOWN note
        star_nodes = -1
    if star_nodes >= 0 and STAR_REFLECTION != (star_nodes > 0):
        raise RuntimeError(
            "star-reflection arm disagrees with the graph: arm=%s but %d StarmapTex "
            "texture parameters are on the saved material. One of the two is a lie "
            "and the perf A/B would inherit it." % (STAR_REFLECTION, star_nodes))
    log("BUILT: waves=%s (%s=%r), time=%s (%s=%r), stars=%s (%s=%r, StarmapTex "
        "params=%s), ripple=%s, wind=%s"
        % ("LEGACY" if LEGACY_WAVES else "WIND",
           LEGACY_WAVES_ENV, os.environ.get(LEGACY_WAVES_ENV, "<unset>"),
           "FROZEN" if FREEZE_TIME else "LIVE",
           FREEZE_TIME_ENV, os.environ.get(FREEZE_TIME_ENV, "<unset>"),
           "ON" if STAR_REFLECTION else "OFF",
           STAR_REFLECTION_ENV, os.environ.get(STAR_REFLECTION_ENV, "<unset>"),
           "UNKNOWN" if star_nodes < 0 else star_nodes,
           ripple_arm, wave["wind_source"]))
    # The CPU wave mirror header -- same call, same argument, as the lake's
    # (byte-deterministic, write-if-changed; see write_cpu_mirror_header).
    water_wave_graph.write_cpu_mirror_header(log=unreal.log)

    log("created and saved at " + FULL_PATH)


main()
