"""The water's optical constants, in ONE place, because there are now two
renderers that have to agree about them.

WHY THIS FILE EXISTS
====================

Until 2026-08-12 there was one place water got its colour -- M_WaterVoxel's
Single Layer Water node, looking at the surface from above -- and the numbers
lived inline in create_water_voxel_material.py, which was correct, because
nothing else read them.

Then the underwater view got built (create_underwater_material.py), and it is a
completely different renderer: a post-process material doing Beer-Lambert over
the depth buffer, with no access to the SLW node at all. It has to reach the
same answer from the same numbers or the player swims into a different liquid
than the one they were looking at.

That is not hypothetical. It was the shipped state for one commit. The surface
was taken to an absorption distance of 3.5 m with 5x the scattering (9d3bd0c),
and submerging still applied a hard-coded `SceneColorTint(0.05, 0.30, 0.35)`
plus a fixed-density exponential height fog that had been tuned for the OCEAN
at sea level, and was applied to lakes too because the post-process component is
`bUnbound`. Look at the pond: dark blue-green. Swim in the pond: the old teal.

It is the same class of split the owner made us delete for near-field versus
far-field water ("we either need to only have one type of rendered water ... or
they need to look identical/close enough such that player cannot visibly
perceive the change"). The fix there was to retire a path. There is no path to
retire here -- above-water and below-water genuinely are different renderers --
so the fix is to make them read one set of constants.

WHAT IS SHARED AND WHAT IS NOT
==============================

SHARED (this file): the four physical constants -- absorption distance,
absorption colour, scattering, phase g. These describe THE WATER. They are not
a look setting for one view of it.

NOT SHARED (each generator's own business): everything about how a given
renderer presents that water. The surface has foam, glints, ripple normals and a
sky reflection; the underwater view has an ambient light level and the
submerged-visibility divisor below. Those are view-specific and belong where
they are used.

WHY NOT A MATERIAL PARAMETER COLLECTION, which is the engine's own answer to
"two materials, one set of numbers": because MPC_VoxelSky has already cost this
project a night. create_sky_material.py DELETES and recreates that collection
every run, and every material holding a binding to the old one silently
compiles to the ENGINE DEFAULT MATERIAL while the log reports success (see
tools/voxel-water-star-regen.ps1's header, and the 2026-08-10 failure it
documents). A second collection would be a second instance of that hazard for
constants that do not change at runtime and are only ever edited by editing
these scripts. A Python import has no such failure mode: if this file is wrong
the generator raises, and if it is missing the generator does not run at all.

THE COST OF THAT CHOICE, STATED: the two materials expose these as SEPARATE
material parameters, so overriding AbsorptionDistanceM on a material instance of
M_WaterVoxel does NOT move M_Underwater with it. Instance overrides are for
experiments; the shipped values come from here and from a regeneration. If
somebody starts tuning by instance and wonders why the surface and the swim
disagree, this paragraph is the answer.

UNITS
=====

Everything here is PER METRE, which is how every published figure is quoted and
how a human can check it. Both generators convert to the units their own node
wants. For the SLW node that is 1/cm -- Epic's public docs say metres and are
WRONG, the engine header
(MaterialExpressionSingleLayerWaterMaterialOutput.h:16) says "Unit is 1/cm" and
the shader agrees. Getting that backwards is a 100x error and it is the single
easiest mistake to make in this whole area, so neither generator is allowed to
type a 0.01 anywhere except at the one node that does the conversion.
"""

# --- THE FOUR NUMBERS --------------------------------------------------------

# Unity HDRP's parameterisation: the depth at which 2% of light survives. It is
# used rather than a raw coefficient because nobody can picture 0.0112 1/cm, and
# because every shipped preset in the literature is quoted this way (HDRP's Pool
# is 5 m, its Ocean/River preset 1.5 m; we sit between them).
#
# 8.7 -> 3.5 on 2026-08-12, on the owner's call that the water was "too see
# through ... even in the middle of the lake I can see the voxel terrain lake
# bed". The measured reason it had to move this far: the dominant survivor was
# BLUE at 0.04 per metre, i.e. 89% through every metre, so NO depth this world
# contains could have hidden the bed -- shortening the distance alone would not
# have fixed it, and the colour below was rebalanced in the same change.
#
# 3.5 -> 5.5 later the same day: the owner judged 3.5 to have overshot and asked
# for "somewhere in between current state and the incredibly transparent/clear
# state that it was before", leaving the exact numbers to us.
#
# HOW THE MIDPOINT WAS TAKEN, because "halfway" is ambiguous here and the two
# obvious readings differ a lot. All three constants were moved together to put
# EXTINCTION at the channel-wise GEOMETRIC mean of the two states -- geometric
# and not arithmetic because attenuation is exponential, so the perceptual
# midpoint is the equal-RATIO point. An arithmetic mean of the coefficients
# would have landed much closer to the murky end than it looks on paper.
#
#     extinction per metre        R      G      B
#       8.7 m state (too clear)  0.455  0.145  0.095
#       3.5 m state (too murky)  1.138  0.882  0.953
#       geometric mean           0.719  0.358  0.301   <- the target
#
# and the three constants below are the parameterisation that produces it, with
# scattering taken as its own geometric mean first (so the deep-water colour
# moves with the clarity instead of being left at the murky setting) and
# absorption taking the remainder.
#
#     bed visible through...     0.5 m        1 m          3 m          6 m
#       too clear             .80/.93/.95  .64/.87/.91  .26/.65/.75  .07/.42/.57
#       too murky             .57/.64/.62  .32/.41/.39  .03/.07/.06  .00/.01/.00
#       SHIPPED               .70/.84/.86  .49/.70/.74  .12/.34/.40  .01/.12/.16
#
# In words: the bed is plainly visible in the shallows, clearly tinted but still
# readable at a metre, dim by three metres, and effectively gone at the pond's
# 6 m deep end -- where the colour is the water's own rather than the bed's.
ABSORPTION_DISTANCE_M = 5.5

# "What colour survives longest": the coefficient is scaled by (1 - this), so 0
# in a channel means that channel is fully absorbed at the distance above and 1
# means it is not absorbed at all.
#
# (0, 0.778, 0.911) -> (0, 0.30, 0.38) -> (0, 0.59, 0.74). Red is pinned at 0 in
# every one of the three: red is the channel that must die first for water to
# read as water, and leaving it at 0 makes the distance above mean exactly "how
# deep before red is gone", which is the only channel the artist knob has a
# clean physical meaning for.
ABSORPTION_COLOR = (0.0, 0.59, 0.74)

# Per metre. THIS IS THE COLOUR OF DEEP WATER: the single-scattering integral
# converges, as transmittance goes to zero, on scattering / (scattering +
# absorption) times the arriving light. It is also the half that keeps the lake
# looking like water rather than like a hole -- absorption alone only REMOVES
# light, so cranking it makes water dark rather than deep. Turbid water is a
# BRIGHT medium: Petzold measured clear ocean to turbid harbour as ~50x in
# scattering against only ~3x in absorption.
#
# 0.005/0.045/0.055 -> 0.02/0.10/0.26 -> 0.010/0.067/0.120, the last one the
# geometric mean of the first two (see ABSORPTION_DISTANCE_M for why geometric).
# IT WAS PULLED BACK ALONGSIDE THE ABSORPTION RATHER THAN LEFT ALONE, which is
# the part worth being deliberate about: scattering is what sets the deep-water
# colour, so holding it at the murky value while clearing the absorption would
# have produced clear shallows over an unchanged opaque middle -- a lake with a
# visible seam in it, which is not what "somewhere in between" means.
#
#     deep-water albedo (sigma_s / sigma_t)   R      G      B
#       too clear                            0.011  0.311  0.579
#       too murky                            0.018  0.113  0.273
#       SHIPPED                              0.014  0.187  0.394
SCATTERING_PER_M = (0.010, 0.067, 0.120)

# Forward-scattering anisotropy for the sun term. 0.35 is a deliberate middle:
# real particulate water is 0.9+, but at 0.9 the whole in-scatter collapses into
# a lobe a top-down lake view never enters.
PHASE_G = 0.35


# --- DERIVED, so both generators and any test agree on the arithmetic --------


def absorption_per_m():
    """The three per-metre absorption coefficients the two numbers above mean.

    extinction = -ln(0.02) / distance * (1 - colour). The 2% convention is
    Unity's; it is arbitrary, but it is the convention every published figure is
    quoted against, so changing it here would silently re-scale all of them.
    """
    k = 3.9120230054281460586 / ABSORPTION_DISTANCE_M  # -ln(0.02) / d
    return tuple(k * (1.0 - c) for c in ABSORPTION_COLOR)


def extinction_per_m():
    """Absorption PLUS scattering -- what actually attenuates a light path.

    Forgetting the scattering half is a real trap: it is what makes a "clear"
    setting fog the shallows, and it is 20% of the total in the blue channel
    here.
    """
    return tuple(a + s for a, s in zip(absorption_per_m(), SCATTERING_PER_M))


def single_scattering_albedo():
    """scattering / extinction -- the colour deep water converges on."""
    return tuple(s / e if e > 0.0 else 0.0
                 for s, e in zip(SCATTERING_PER_M, extinction_per_m()))


def transmittance(path_m):
    """exp(-extinction * path), per channel. For sanity-checking by hand."""
    import math
    return tuple(math.exp(-e * path_m) for e in extinction_per_m())


def summary_lines():
    """Human-readable derivation, printed by both generators into their log.

    Printed rather than merely computed because these scripts run headless in a
    commandlet and the log is the only artefact anybody reads afterwards. If the
    surface and the underwater view ever disagree on screen, the first question
    is whether they were built from the same numbers, and this line in both logs
    answers it without opening either asset.
    """
    a = absorption_per_m()
    e = extinction_per_m()
    w = single_scattering_albedo()
    out = [
        "WATER OPTICS (shared, water_optics.py) -- per metre:",
        "  absorption distance %.2f m, absorption colour (%.3f, %.3f, %.3f)"
        % ((ABSORPTION_DISTANCE_M,) + ABSORPTION_COLOR),
        "  absorption  R %.4f  G %.4f  B %.4f" % a,
        "  scattering  R %.4f  G %.4f  B %.4f" % SCATTERING_PER_M,
        "  extinction  R %.4f  G %.4f  B %.4f" % e,
        "  deep-water albedo  R %.3f  G %.3f  B %.3f" % w,
        "  phase g %.2f" % PHASE_G,
    ]
    for d in (0.3, 1.0, 2.0, 5.0, 25.0):
        out.append("  transmittance @ %5.1f m  R %.3f  G %.3f  B %.3f"
                   % ((d,) + transmittance(d)))
    return out
