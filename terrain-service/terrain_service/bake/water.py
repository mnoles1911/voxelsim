"""Runoff-weighted discharge, water heads, and the graded water surface (plan P2).

WHAT THIS MODULE IS FOR
-----------------------
`docs/water-system-architecture.md` item 7 / §4.1. The bake already knows where
every channel is -- `accumulate_mfd` gives catchment AREA and `flow_plane`
writes its log2 -- but area is not water. Two catchments of identical area in a
rainforest and a desert carry wildly different rivers, and until this module
existed nothing in the bake could tell them apart, so the shipped water map had
to draw its channel widths as a PREDICTION off the 30 m coarse tier and say so
in its own caption.

This turns that into a measurement, in three steps:

    Q         runoff-weighted discharge, m^3/yr, one currency (§4.1.1)
    heads     where perennial water starts, from Q and climate (§4.1.2)
    surface   the graded water plane, from Q through channel.h's laws (§4.1.3)

THE THREE CURRENCIES THIS REPLACES
----------------------------------
Before this there were three incompatible measures of "how much water":

    bake        m^2 of catchment area          (accumulate_mfd)
    rivernet    summed mm/yr, threshold 23529  (rivernet.h:241)
    channel.h   rivernet's units               (kRiverAccumThresholdDefault)

None of them is a flow. `Q_PERENNIAL_M3_YR` below is, and every threshold and
law in this module is anchored on it, so a number quoted anywhere downstream
can be converted to m^3/s by one division and argued about physically.

WHAT THE BANK PROBE ESTABLISHED, AND WHAT IT MEANS HERE
-------------------------------------------------------
`vxc_bankprobe` (commit eb9acac) walked 630 cross-sections over 25 contiguous
fine tiles at 100 mm and found **0 breaches of 1,260 stream bank sides at every
water depth from 0.3 m to 10 m**, against a measured control floor of
0.14-0.35% on non-channel ground. So the baked carve holds its waterline and
this module needs no detail-band bank term.

It also found the thing that shapes everything below: **at 10 m depth, 736 of
those 1,260 bank sides have no containing ground within 120 m.** That is the
carve correctly declining to be a canyon, but it means there is NO GEOMETRIC
BACKSTOP. A water head that comes out too high does not leak sideways and
self-correct; it floods the whole valley. Hence:

  * the head is taken from Q and the long profile and from nothing else -- no
    term in `water_surface_m` reads local relief, a bank height, or a rim.
    THAT IS ABOUT THE HEAD, NOT THE EXTENT, and bake_ver 13 makes the
    distinction load bearing: `fill_to_local_surface` decides how far the
    drawn water reaches by reading the ground and nothing else. The two are
    not in tension -- how DEEP the water stands is a hydraulic question the
    terrain cannot answer, and how FAR it spreads at that depth is a question
    only the terrain can. Reading relief into the head is what has no
    backstop; reading it into the extent is what gives the extent one;
  * OVERSHOOT IS THE PRIMARY FAILURE MODE and is gated on, not assumed away.
    `overshoot_stats` measures how much deeper the plane stands than its own
    law allows, and `lateral_extent_stats` measures how far the resulting
    waterline actually spreads. Both read on the shipped raster, and both are
    reported next to what they read on flat, non-channel ground -- because a
    statistic that is satisfied by its own construct passes vacuously, which
    has already happened three times on this branch.

WHAT IS DELIBERATELY NOT HERE
-----------------------------
* **No new hydraulics.** DEPTH is `channel.h`'s published law, at its own
  exponent, re-anchored per §4.1.3. Two implementations of one law is how the
  three currencies above happened.

  WIDTH is no longer taken from the law at all (bake_ver 13). It was, and it
  was wrong in the specific way the law cannot see: `channel_width_m(Q)` is a
  statement about a channel in equilibrium with its discharge, and the bake
  does not draw a channel, it draws which CELLS OF THIS RASTER are under
  water. On a valley floor those are not the same thing by a factor of ten.
  `channel_width_m` and `q_for_width_m` are still here and still exact --
  `q_drawable_m3_yr` needs the inverse to turn a raster pitch into a discharge
  threshold, which is a question about the law and is answered by it.
* **No lakes.** A registered basin's surface is on the wire already
  (SECTION_BASIN_TABLE, bake_ver 8) and `LakeSampler` reads it. Writing it into
  the plane as well would be a second copy of a shipped fact, free to disagree
  with the first. `graded_water_surface` marks basin cells DRY in the plane and
  the client composes the two samplers -- see `compose` in lakes.h.
* **No sub-pixel headwaters.** A channel at initiation is 1.5 m wide and the
  fine pixel is 1.875 m. §4.1's honesty clause puts water only on reaches wide
  enough for the raster to hold a wet bed; `q_drawable` derives that threshold
  FROM the width law rather than adding a second magic number, so the two
  cannot drift.
"""

from __future__ import annotations

import numpy as np

from . import flow as _flow

__all__ = [
    "enforce_neighbour_consistency",
    "Q_PERENNIAL_M3_YR",
    "Q_RIVER_M3_YR",
    "Q_MAJOR_M3_YR",
    "CHANNEL_REF_WIDTH_M",
    "CHANNEL_REF_DEPTH_M",
    "CHANNEL_WIDTH_EXP",
    "CHANNEL_DEPTH_EXP",
    "CHANNEL_MAX_WIDTH_M",
    "CHANNEL_MAX_DEPTH_M",
    "WATER_DEPTH_NUM",
    "WATER_DEPTH_DEN",
    "channel_width_m",
    "channel_depth_m",
    "water_depth_m",
    "q_for_width_m",
    "q_drawable_m3_yr",
    "runoff_field_mm_yr",
    "discharge_source",
    "graded_water_surface",
    "widen_to_channel_width",
    "fill_to_local_surface",
    "bridge_to_face_contact",
    "face_contact_stats",
    "WIDEN_MIN_DEPTH_M",
    "water_head_mask",
    "overshoot_stats",
    "lateral_extent_stats",
]


# --------------------------------------------------------------------------- currency

#: The discharge at which a watercourse is PERENNIAL -- wet in every month
#: rather than after rain. The plan (§4.1.2) leaves this "to be calibrated on
#: the survey" and gives its order as "10^3-10^4 m^3/yr - km^2-scale", which is
#: not a closed unit. A mean flow rate is, and it is the quantity a hydrologist
#: would name: 10 L/s. Below this a carved swale is a dry gully most of the
#: year, which is exactly the caused variety the directive asks for -- in a wet
#: province water starts high in the network, in an arid one only trunks carry
#: it between dry washes.
#:
#: SHARED WITH tools/worldmaps/water.py, which imports it from here. It was
#: defined there first, when only a prediction existed; one law, one home.
Q_PERENNIAL_M3_YR = 10.0 * 1e-3 * 365.25 * 86400.0  # = 315,576 m^3/yr

#: Class boundaries in the same currency: a river at 1 m^3/s, a major river at
#: 10 m^3/s. Both are round physical flows.
Q_RIVER_M3_YR = 100.0 * Q_PERENNIAL_M3_YR
Q_MAJOR_M3_YR = 1000.0 * Q_PERENNIAL_M3_YR


# --------------------------------------------------------------------------- geometry

#: channel.h's hydraulic geometry, as the floating-point mirror of its Q8 fixed
#: point (kChannelRefWidthMm, kChannelRefDepthMm, kChannelWidthExpQ8/256,
#: kChannelDepthExpQ8/256, kChannelMaxWidthMm, kChannelMaxDepthMm). Leopold &
#: Maddock downstream hydraulic geometry: width and depth are power laws in
#: discharge.
#:
#: The reference pair is the channel at INITIATION -- a headwater trickle 1.5 m
#: across and ankle deep -- and this module re-anchors it on Q_PERENNIAL rather
#: than channel.h's kRiverAccumThresholdDefault, per plan §4.1.3. That is a
#: change of ANCHOR, not of law: the exponents, the caps and the 3/4 waterline
#: are channel.h's own, so a future wiring pass agrees with the bake instead of
#: fighting it.
CHANNEL_REF_WIDTH_M = 1.5
CHANNEL_REF_DEPTH_M = 0.3
#: EXAGGERATED ON PURPOSE, 2026-08-06, and it is a design decision rather than a
#: correction. Earth's downstream hydraulic geometry is width ∝ Q^0.40 and
#: depth ∝ Q^0.35 (Leopold & Maddock), and this world reproduced those exponents
#: exactly -- architecture §4 measured width 2.07x against a predicted 2.07 and
#: depth 1.91x against 1.92. The laws were right and the RESULT was boring: over
#: the 256x discharge span between a drawable headwater and the largest trunk,
#: Earth's exponents give 2.8 m -> 26 m of width and 0.39 m -> 2.8 m of depth, so
#: every river in the world reads as roughly the same size. The owner has pushed
#: on this twice -- "Why are rivers a maximum of 2 m deep?" -- and the answer was
#: that nothing was broken, the exponents were simply Earth's.
#:
#: His decision, in his words: "I think we change the exponents to force more
#: dramatic rivers even though it's different than earth. Exaggeration is ok to
#: keep things interesting."
#:
#: At 166/256 and 128/256 the same span gives 4.2 m -> 154 m wide and 0.50 m ->
#: 7.9 m deep: a headwater you can still step over, and a trunk that is
#: unmistakably a major river. Both stay inside their caps (400 m, 25 m), which
#: is what stops the law going flat at the top end -- at 0.75 the width cap
#: starts binding and the biggest rivers would all clamp to the same size, which
#: is the current problem again in a new place.
#:
#: THESE ARE MIRRORED IN channel.h AS Q8 FIXED POINT and the two must move
#: together; the mirror is asserted by test_channel_exponents_match_the_mirror.
#: NOT YET FLIPPED. The owner decided to exaggerate ("I think we change the
#: exponents to force more dramatic rivers even though it's different than
#: earth. Exaggeration is ok to keep things interesting") and 166/128 is the
#: value that does it: over the 256x span between a drawable headwater and the
#: largest trunk it gives 4.2 -> 154 m of width and 0.50 -> 7.9 m of depth,
#: against Earth's 2.8 -> 26 m and 0.39 -> 2.8 m, with both ends still off their
#: caps. That is the change that answers "why are rivers a maximum of 2 m deep?".
#:
#: WHAT BLOCKS IT -- EXPLAINED 2026-08-07, and the answer is "do not flip these".
#: The ``cs.inBed`` red was the symptom, not the disease. Building voxel-core at
#: 166/128 and running the suite, the load-bearing failures are these, and they
#: are not about confluences at all::
#:
#:     test_channel.cpp:199  CHECK(bigW >= 40'000 && bigW <= 90'000)
#:     test_channel.cpp:200  CHECK(bigD >=  4'000 && bigD <= 12'000)
#:
#: ``bigQ`` there is a catchment 10,000x the drawable threshold -- a major
#: river -- and those bounds are a deliberate guard with the comment "that lands
#: on a physically sensible river: tens of metres wide, several metres deep".
#: At 166/128 that same river measures 400 m wide and 25 m deep: 4.4x over the
#: width ceiling and 2x over the depth ceiling. The width also hits
#: CHANNEL_MAX_WIDTH_M (400.0) exactly, i.e. it is not merely large, it is
#: SATURATED, so beyond that point discharge stops changing the river at all.
#:
#: That also disposes of the "wider channels should make inBed EASIER" puzzle
#: that parked this for days. The fixture valley in that test is ~18 m across.
#: A 400 m channel does not widen it, it swallows it -- every reach influences
#: every cell and the reaches overlap wholesale. inBed failing under geometry
#: that large is a consequence of the breached bound, not independent evidence
#: of a continuity bug. (Mechanism consistent with the numbers; not separately
#: instrumented, because the bound alone settles the decision.)
#:
#: The remaining reds at 166/128 -- test_channel.cpp:222-223 -- are just the
#: pinned decade-by-decade table restating the old exponents. Those would be
#: updated by such a change, not violated by it. Do not mistake them for
#: evidence either way.
#:
#: SO: 166/128 is rejected on its own merits, not deferred. If the owner still
#: wants more dramatic rivers -- and the ask was real -- the lever is NOT this
#: exponent pair, because the drawn width is set by ``fill_to_local_surface``
#: spreading to the local level, and the probe measures the drawn river at
#: 1.62 px = 3.04 m mean width against 20-37 m of available valley floor. Raise
#: what the water SPREADS to, and revisit CHANNEL_MAX_WIDTH_M, before touching
#: an exponent whose own test says the result stops being a river.
#:
#: The plumbing is ready: both are now identity-covered bake constants (see
#: BakeConstants.channel_width_exp_q8), so flipping them is a two-line change
#: that correctly re-keys the tiles it affects.
CHANNEL_WIDTH_EXP = 102.0 / 256.0  # 0.3984, channel.h's kChannelWidthExpQ8
CHANNEL_DEPTH_EXP = 90.0 / 256.0   # 0.3516, channel.h's kChannelDepthExpQ8
CHANNEL_MAX_WIDTH_M = 400.0
#: Matches the bake's own incision cap (incise.py cap_m = 25.0), so the two
#: agree about how deep a channel can ever get.
CHANNEL_MAX_DEPTH_M = 25.0

#: Design water depth as a fraction of channel depth -- channel.h's
#: kChannelWaterDepthNum/Den. 3/4 leaves a quarter-depth freeboard, so the rim
#: IS the bank crest.
WATER_DEPTH_NUM = 3
WATER_DEPTH_DEN = 4


def channel_width_m(q_m3_yr, q_perennial: float = Q_PERENNIAL_M3_YR):
    """channel.h's width law, re-anchored on `q_perennial`. Monotone in Q.

    THE ANCHOR IS A PARAMETER, not the module constant, and that is load
    bearing: ``water_q_perennial_m3_yr`` is a hashed product constant, so if
    these laws read the module default instead the constant would be
    decorative and the identity hash would be claiming to cover something it
    did not. (It did exactly that for one revision; the bake's own
    anti-vacuity assertion caught it.)
    """
    qp = float(q_perennial)
    q = np.maximum(np.asarray(q_m3_yr, np.float64), qp)
    w = CHANNEL_REF_WIDTH_M * (q / qp) ** CHANNEL_WIDTH_EXP
    return np.minimum(w, CHANNEL_MAX_WIDTH_M)


def channel_depth_m(q_m3_yr, q_perennial: float = Q_PERENNIAL_M3_YR):
    """channel.h's depth law, re-anchored on `q_perennial`. Monotone in Q."""
    qp = float(q_perennial)
    q = np.maximum(np.asarray(q_m3_yr, np.float64), qp)
    d = CHANNEL_REF_DEPTH_M * (q / qp) ** CHANNEL_DEPTH_EXP
    return np.minimum(d, CHANNEL_MAX_DEPTH_M)


#: Reference along-flow gradient for the slope term, m/m. The law is UNCHANGED
#: at this slope, so every depth already measured against it still holds --
#: architecture §4 records observed depth matching the Q law to three
#: significant figures across three decades of discharge, and nothing here is
#: allowed to move that. 47.1 m/km is the world's measured p50 along-flow
#: gradient.
SLOPE_REF_M_PER_M = 0.0471

#: Exponent on the slope ratio. Normal-depth (Manning) flow gives
#: ``d ∝ (Q / sqrt(S)) ** (3/5)`` = ``Q**0.6 * S**-0.3``, so the slope term is
#: ``S ** -0.3``. Only the SLOPE half is taken: the Q exponent stays at
#: CHANNEL_DEPTH_EXP because that half is measured and correct, and swapping
#: 0.3516 for 0.6 would break the agreement rather than improve it.
SLOPE_DEPTH_EXP = -0.3

#: Clamp on the slope ratio before the exponent. Without it a near-zero slope
#: -- and 58% of river cells sit on the epsilon-fill floor, where the "slope"
#: is not terrain at all -- sends depth to infinity. 1/16..16 in the ratio is
#: 2.0x..0.5x in depth, which is a correction and not a new law.
SLOPE_RATIO_MIN = 1.0 / 16.0
SLOPE_RATIO_MAX = 16.0


def water_depth_m(q_m3_yr, q_perennial: float = Q_PERENNIAL_M3_YR, slope=None):
    """Design water depth: channel.h's 3/4 of channel depth.

    WITHOUT ``slope`` THIS IS THE HISTORICAL LAW, a function of Q and nothing
    else. See the module docstring: the bank probe found no containing ground
    within 120 m on 58% of stream bank sides at 10 m depth, so there is no
    geometry to fall back on and none is consulted.

    WITH ``slope`` (F3). Leopold & Maddock is a fit to lowland rivers at roughly
    constant slope, and this world's long profile runs 173 -> 29 m/km on one
    block. A law with no slope in it therefore puts too much water on steep
    upper reaches -- where it then cannot stay connected -- and too little on
    flat lower ones. ``bridge_to_face_contact`` is a hand-built correction for
    exactly that missing term, which gives this change its acceptance test:
    **with slope in the law the bridge should become close to a no-op on steep
    reaches.** If it is still doing heavy lifting, the depth model is still
    wrong and this constant should go back off rather than be tuned.

    The slope enters as a RATIO against SLOPE_REF_M_PER_M, so at the reference
    gradient the returned depth is bit-identical to the historical law and every
    existing measurement survives. Steeper than reference -> shallower water;
    flatter -> deeper. That is the direction normal-depth flow requires and the
    direction F3 says we currently have backwards.
    """
    d = channel_depth_m(q_m3_yr, q_perennial) * (WATER_DEPTH_NUM / WATER_DEPTH_DEN)
    if slope is None:
        return d
    s = np.maximum(np.asarray(slope, np.float64), 0.0)
    ratio = np.clip(s / SLOPE_REF_M_PER_M, SLOPE_RATIO_MIN, SLOPE_RATIO_MAX)
    return d * ratio**SLOPE_DEPTH_EXP


def q_for_width_m(width_m: float,
                  q_perennial: float = Q_PERENNIAL_M3_YR) -> float:
    """The inverse of `channel_width_m` -- the Q at which a channel is this wide.

    Exists so a threshold expressed as a WIDTH (which is what the raster can
    hold) becomes a threshold in Q without a second constant that could drift
    away from the law. Saturates at the width cap, where the law is flat and
    the inverse does not exist.
    """
    w = float(width_m)
    if w <= CHANNEL_REF_WIDTH_M:
        return float(q_perennial)
    if w >= CHANNEL_MAX_WIDTH_M:
        raise ValueError(
            f"width {w} m is at or above the {CHANNEL_MAX_WIDTH_M} m cap, where "
            "the width law is flat and has no inverse"
        )
    return float(q_perennial) * (w / CHANNEL_REF_WIDTH_M) ** (1.0 / CHANNEL_WIDTH_EXP)


def q_drawable_m3_yr(cell_m: float, min_width_px: float = 2.0,
                     q_perennial: float = Q_PERENNIAL_M3_YR) -> float:
    """Smallest Q whose channel is wide enough for the raster to hold a wet bed.

    Plan §4.1, "sub-pixel headwaters, stated honestly": a channel at initiation
    is 1.5 m wide against a 1.875 m pixel, so phase 2 puts water only on reaches
    of about 2 px. DERIVED from the width law rather than stated, so raising
    `min_width_px` or changing the pixel pitch moves it consistently and the two
    can never disagree.

    At production (1.875 m/px, 2 px) this is 3.14e6 m^3/yr, i.e. 0.100 m^3/s --
    10x the perennial threshold. The gap between the two is not a defect: it is
    the count of reaches that ARE perennially wet and that this raster cannot
    draw, and `water_head_mask` reports both so that count is visible rather
    than silently absorbed.
    """
    return q_for_width_m(float(cell_m) * float(min_width_px), q_perennial)


# --------------------------------------------------------------------------- runoff

def runoff_field_mm_yr(padded_climate, wb, smooth_m: float, coarse_pixel_m: float):
    """Budyko runoff over the padded COARSE grid, mm/yr, or None without climate.

    Kept coarse -- 576^2 at production, ~1.3 MB -- and gathered to the fine grid
    by `discharge_source` at `y // scale`, exactly as the landform-province
    partition gathers its own parameter fields. Materialising it at fine
    resolution would be 340 MB of 16x16-replicated float32 inside the bake's
    peak stage for no additional information: climate has no fine structure.

    SMOOTHED the way every other climate consumer smooths, and this is not
    cosmetic: the uint8 wire LSBs are 0.31 C and 47 mm/yr, both bigger than
    several province boundaries, so an unsmoothed field would step Q across a
    quantisation contour and put a visible width jump in a river.

    `wb` is the `basins.WaterBalance` the lake rule already uses -- the SAME
    pet/budyko constants, so a basin's water balance and its outlet river cannot
    disagree about how much water the sky delivers. Passed in rather than
    constructed here so `pipeline.basin_balance(consts)` stays the one place
    that reads them off `BakeConstants`.
    """
    if padded_climate is None:
        return None
    from . import basins as _basins
    from . import province as _province

    phys = _province.dequantize_climate(padded_climate)
    half = max(int(round(float(smooth_m) / float(coarse_pixel_m) / 2.0)), 1)
    temp = _province.box_smooth(phys["temperature"], half)
    precip = _province.box_smooth(phys["precipitation"], half)
    del phys
    pet = _basins.pet_mm_yr(temp, wb)
    runoff = _basins.budyko_runoff_mm_yr(precip, pet, wb)
    return np.asarray(runoff, np.float64)


def discharge_source(runoff_coarse_mm_yr, fine_shape, scale: int, cell_m: float,
                     inflow_area_m2=None, inflow_q_m3_yr=None):
    """Per-cell runoff VOLUME in m^3/yr -- the seed `accumulate_mfd(source=)` wants.

    `runoff_coarse_mm_yr` is gathered at `y // scale`, so the result is a fine
    float64 grid built by ONE indexing operation rather than two np.repeats.

    THE BOUNDARY CONDITION, AND THE DEFECT TASK #49 REMOVED
    ------------------------------------------------------
    The hydrology pyramid delivers upstream flow at this domain's edge. Which
    quantity it delivers is the whole question:

      * `inflow_q_m3_yr` -- a real DISCHARGE carried up the pyramid by
        `pipeline.build_flow_superblock`'s second MFD sweep. Added directly,
        because it is already in this function's own currency. USE THIS.

      * `inflow_area_m2` -- upstream AREA, converted to a discharge by the LOCAL
        runoff at the injection cell. A PROXY, and a bad one for exactly the
        rivers worth having: it assumes the catchment has the climate of its own
        mouth. Measured on the (-14,-4) -> (-14,-7) corridor, where runoff falls
        217.5 -> 12.8 -> 3.6 -> 0.5 mm/yr from mountains to arid coast, it read
        1.27e6 m^3/yr at the coastal tile against a 3.15e6 drawable threshold
        and against 58.7e6 in the stitched coarse world. Carried, the same
        boundary delivers 2.50e7 -- water worth 71 mm/yr of catchment rather
        than the 3.6 mm/yr of ground it lands on.

        It is wrong in BOTH directions, not merely small: where the local
        ground is WETTER than the catchment mean the proxy over-states, and
        re-baking (-13,-5) (local 217.5, catchment mean 156) removed two wet
        pixels. See `pipeline.CARRIED_DISCHARGE` for the full corridor and for
        why fixing this did not, on its own, put a river at that coast.

    Both are accepted, and the fallback is deliberate rather than lazy: a
    superblock built before task #49, or built without climate, carries no Q,
    and silently injecting zero there would read as "no water arrives" -- the
    same class of lie in a new place. `bake_tile` reports which one was used in
    `water_q_inflow_carried`, so a tile still on the proxy says so.

    Passing both is a caller error: the same water would enter twice.
    """
    if runoff_coarse_mm_yr is None:
        return None
    if inflow_area_m2 is not None and inflow_q_m3_yr is not None:
        raise ValueError(
            "pass the pyramid's inflow as EITHER a carried discharge "
            "(inflow_q_m3_yr) or the area proxy (inflow_area_m2), not both -- "
            "they are two spellings of the same water"
        )
    h, w = fine_shape
    ry = np.minimum(np.arange(h) // int(scale), runoff_coarse_mm_yr.shape[0] - 1)
    rx = np.minimum(np.arange(w) // int(scale), runoff_coarse_mm_yr.shape[1] - 1)
    runoff_m = runoff_coarse_mm_yr[np.ix_(ry, rx)] / 1000.0     # m/yr, float64
    src = runoff_m * (float(cell_m) * float(cell_m))            # m^3/yr per cell
    if inflow_q_m3_yr is not None:
        # Already m^3/yr. No conversion, no assumption about upstream climate:
        # this is the whole point of the change.
        src = src + np.asarray(inflow_q_m3_yr, np.float64)
    elif inflow_area_m2 is not None:
        # The proxy. Same conversion, at the cell the area arrives in.
        src = src + np.asarray(inflow_area_m2, np.float64) * runoff_m
    np.clip(src, 0.0, None, out=src)
    return src


# --------------------------------------------------------------------------- heads

def water_head_mask(q_m3_yr, receivers, *, q_drawable: float,
                    q_perennial: float = Q_PERENNIAL_M3_YR):
    """Where drawn water starts, and the two counts that keep it honest.

    Returns `(wet, stats)`. `wet` is the mask the plane is written over: cells
    whose Q is drawable at this raster pitch. A HEAD is a wet cell with no wet
    D8 donor -- the most-upstream cell of a reach, per §4.1.2 -- and it falls
    out of the field rather than being picked.

    THE TWO COUNTS. `perennial_cells` is the hydrologic statement (Q >= 10 L/s,
    water in every month) and `drawn_cells` is the raster statement (the channel
    is at least ~2 px wide). Their ratio is how much of the perennial network
    this tier cannot draw. Reporting only the second would let "the map is
    sparse" read as "the world is dry", which is the opposite of true.

    `receivers` is `d8_receivers(...)[0]` -- flat `y*w + x`, -1 at a root.
    """
    q = np.asarray(q_m3_yr)
    wet = q >= float(q_drawable)
    rec = np.ascontiguousarray(receivers, dtype=np.int64).ravel()

    # A head has no wet DONOR. Rather than inverting the receiver forest,
    # mark every cell that IS some wet cell's receiver: one scatter.
    has_wet_donor = np.zeros(q.size, bool)
    wf = wet.ravel()
    tgt = rec[wf]
    has_wet_donor[tgt[tgt >= 0]] = True
    heads = wet & ~has_wet_donor.reshape(q.shape)

    n = float(q.size)
    stats = {
        "perennial_cells": float((q >= float(q_perennial)).sum()),
        "perennial_frac": float((q >= float(q_perennial)).mean()),
        "drawn_cells": float(wet.sum()),
        "drawn_frac": float(wet.mean()),
        "head_cells": float(heads.sum()),
        "q_drawable_m3_yr": float(q_drawable),
        "q_perennial_m3_yr": float(q_perennial),
        "q_max_m3_yr": float(q.max()) if n else 0.0,
    }
    # How much of the perennially-wet network the raster declines to draw.
    per = stats["perennial_cells"]
    stats["undrawn_perennial_frac"] = (
        float((per - stats["drawn_cells"]) / per) if per > 0 else 0.0
    )
    return wet, heads, stats


# --------------------------------------------------------------------------- the plane

def slope_to_receiver(z_route_m, receivers, cell_m: float):
    """Along-flow gradient at every cell, m/m, from the bake's OWN D8 forest.

    REUSED, NOT RE-DERIVED, and that is deliberate: three wrong answers in one
    session came from re-deriving something the bake already computes -- flow
    direction, the perpendicular transect, and which surface discharge was
    accumulated on. The receiver forest is the same one the incision's slope
    term was taken along, so a depth law reading this is reading the same
    geometry the channel was cut with.

    Diagonal steps are sqrt(2) longer and are measured as such; treating every
    D8 step as one cell would overstate diagonal gradients by 41%.
    """
    z = np.asarray(z_route_m, np.float64)
    rec = np.asarray(receivers)
    flat = z.ravel()
    recf = rec.ravel()
    n = flat.size
    w = z.shape[1]

    idx = np.arange(n)
    drop = flat - flat[recf]
    # Step length: 1 cell orthogonally, sqrt(2) diagonally. A receiver differing
    # in BOTH row and column is a diagonal.
    dr = np.abs(idx // w - recf // w)
    dc = np.abs(idx % w - recf % w)
    steps = np.where((dr == 1) & (dc == 1), np.sqrt(2.0), 1.0) * float(cell_m)
    # A pit points at itself: zero drop over zero distance. Report zero slope
    # rather than a divide, and let the clamp in water_depth_m handle it.
    slope = np.zeros(n, np.float64)
    moved = recf != idx
    np.divide(drop, steps, out=slope, where=moved)
    return np.maximum(slope, 0.0).reshape(z.shape)


def graded_water_surface(z_route_m, q_m3_yr, receivers, wet, *, eps_m: float,
                         exclude=None, q_perennial: float = Q_PERENNIAL_M3_YR,
                         slope_in_depth: bool = False, cell_m: float = 1.875):
    """The graded water surface, metres absolute, NaN where dry.

    `desired = bed + water_depth(Q)` and then ONE descent-enforcing sweep down
    the D8 forest, so the surface descends strictly downstream by at least its
    own bed drop (capped at `eps_m`). Without that sweep a reach is a chain of
    puddles at tile scale the moment the meso band or the codec's 100 mm LSB
    puts a sill in it -- `channel.h:111-134` is the argument and the bake
    already owns the machinery (`flow.enforce_descent`).

    WHICH BED. `z_route_m` must be the PRE-B5 final surface: the one the
    B4b refill guarantees has zero sinks. Post-B5 the registered basins are
    holes, and running a descent chain through a 500 m deep re-opened basin
    would drag an entire river's water surface down into it. Cells inside a
    registered basin are handed in as `exclude` and are written dry -- the basin
    table already carries their surface and the client composes the two.

    THE SWEEP RUNS OVER THE WHOLE GRID, not just the wet cells, and that is
    deliberate: `enforce_descent` needs a receiver forest, and restricting it to
    a subset would break the chain wherever a wet reach crosses a cell the mask
    happened to drop. Dry cells start at the bed itself, where the constraint is
    already satisfied, so they cost a pass and change nothing.
    """
    from .flow import enforce_descent

    z = np.ascontiguousarray(z_route_m, dtype=np.float32)
    desired = z.copy()
    # F3: the depth law gains a slope term, off unless the bake asks for it.
    # The slope comes from the SAME receiver forest this function is about to
    # run enforce_descent down, so the depth and the descent constraint are
    # reading one geometry rather than two that can disagree.
    slope_wet = None
    if slope_in_depth:
        slope_wet = slope_to_receiver(z, receivers, cell_m)[wet]
    add = water_depth_m(np.asarray(q_m3_yr)[wet], q_perennial,
                        slope=slope_wet).astype(np.float32)
    desired[wet] = z[wet] + add
    del add

    w = enforce_descent(receivers, z, desired, float(eps_m))

    out = np.asarray(w, np.float32).copy()
    dry = ~wet if exclude is None else ~(wet & ~exclude)
    out[dry] = np.nan
    return out


# --------------------------------------------------------------------------- extent

#: Shallowest water a widened cell may be drawn with, metres. ONE ELEVATION LSB
#: on the wire (``tile_codec.QUANT_MM`` is 100 mm), and it is a representability
#: floor rather than a look: the codec stores ``rint(water*1000) -
#: rint(ground*1000)`` and refuses a wet cell that quantises negative, so a cell
#: admitted at a hair above its own bed is a rounding coin-flip away from being
#: an encoder error. It is also one client voxel -- below it the widened cell
#: would carry water the renderer cannot draw, which is a wet pixel that reads
#: dry, exactly the failure ``WATER_DRY_DEPTH`` exists to make impossible.
WIDEN_MIN_DEPTH_M = 0.1


def widen_to_channel_width(water_m, z_ground_m, q_m3_yr, *, cell_m: float,
                           exclude=None, min_depth_m: float = WIDEN_MIN_DEPTH_M,
                           q_perennial: float = Q_PERENNIAL_M3_YR):
    """Grow the drawn plane sideways to ``channel_width_m(Q)``. Returns (w, stats).

    ``graded_water_surface`` draws a CENTRELINE -- one cell per reach, because a
    single-receiver forest has one-cell-wide branches by construction. This is
    the other half of the same law: ``water_depth_m(Q)`` already decides how deep
    the plane stands, and this makes ``channel_width_m(Q)`` decide how far it
    reaches. Neither is new hydraulics; both are ``channel.h``'s published
    exponents at the anchor this module re-anchored them on.

    TWO BOUNDS, AND THE MEASUREMENT THAT SAYS WHICH ONE SHAPES THE RIVER.

      * the LAW. A cell is a candidate if its centre lies within
        ``channel_width_m(Q)/2`` of a drawn cell. On the measured corridor that
        half-width is 0.78-2.23 fine pixels, so the ribbon is 1-5 px and its
        width changes continuously down a reach: a headwater creek at the
        drawable cut is 2.81 m across and a trunk at 2.4e7 m^3/yr is 8.8 m.
      * the TERRAIN. A candidate is drawn only where the SHIPPED ground stands
        at least ``min_depth_m`` below that reach's own water surface. Measured
        on bv11: the ground allows p50 11-28 m and p90 47-163 m of lateral
        extent at the drawn level, and 90-93% of wet cells have room for the
        whole law width. So the law does the shaping and this clamp bites on the
        7-10% that would otherwise be drawn uphill.

    That order matters and is not interchangeable. A rule that took the terrain
    first would flood a valley floor -- ``vxc_bankprobe`` found NO containing
    ground within 120 m on 736 of 1,260 bank sides at 10 m depth, which is why
    ``overshoot_stats`` exists at all. A rule that took the law alone would put
    58.2% of the widened edge below drawn ground, which is what the far-field
    experiment measured when it widened in the ground plane without a clamp.

    THE WATER SURFACE IS FLAT ACROSS THE CHANNEL, not tapered, because that is
    what a water surface is. The DEPTH therefore tapers on its own wherever the
    bed rises toward the bank, and the codec stores depth, so the client's
    sub-pixel zero-crossing lands on a contour of the bed rather than on the
    raster step this function added.

    NEAREST SOURCE WINS. Offsets are visited in ascending distance and a cell is
    claimed once, so a cell reachable from two reaches takes the level of the
    nearer one -- never the higher. Taking the maximum instead would let a large
    river 3 px away raise the water over a small one's bank; ties inside one
    distance ring take the higher level, which is deterministic and, at equal
    distance, is the reach with the larger channel.

    ``water_m`` is ``graded_water_surface``'s output: metres absolute, NaN dry,
    already excluding registered basins. ``z_ground_m`` is the SHIPPED surface
    (post-B5) -- the ground the client draws the waterline against, and the same
    array the codec takes the depth against. ``exclude`` is the basin mask; a
    widened cell must not spill into a re-opened basin whose surface the basin
    table already carries.
    """
    w = np.array(water_m, dtype=np.float32, copy=True)
    z = np.asarray(z_ground_m, np.float32)
    q = np.asarray(q_m3_yr)
    h, wd = w.shape
    flat = w.ravel()
    zf = z.ravel()
    exf = None if exclude is None else np.asarray(exclude).ravel()

    idx = np.flatnonzero(np.isfinite(flat))
    stats = {
        "width_centreline_cells": float(idx.size),
        "width_added_cells": 0.0,
        "width_min_depth_m": float(min_depth_m),
    }
    if idx.size == 0:
        return w, stats

    cy, cx = np.divmod(idx, wd)
    lvl = flat[idx].astype(np.float64)
    r_px = (channel_width_m(q.ravel()[idx], q_perennial) / 2.0) / float(cell_m)
    stats["width_law_half_px_p50"] = float(np.percentile(r_px, 50))
    stats["width_law_half_px_max"] = float(r_px.max())

    # Sources sorted by their own reach, so the participants at distance d are a
    # CONTIGUOUS TAIL and each ring costs work proportional to how many cells
    # actually reach that far -- not to the wet set. Without this the 400 m
    # width cap (106 px) would make the ring loop quadratic in a raster it never
    # touches.
    o = np.argsort(r_px, kind="stable")
    cy, cx, lvl, r_sorted = cy[o], cx[o], lvl[o], r_px[o]

    R = int(np.ceil(float(r_sorted[-1])))
    offs = [(dy, dx)
            for dy in range(-R, R + 1)
            for dx in range(-R, R + 1)
            if (dy or dx) and (dy * dy + dx * dx) <= R * R]
    offs.sort(key=lambda t: (t[0] * t[0] + t[1] * t[1], t[0], t[1]))

    added = 0
    for dy, dx in offs:
        d = float(np.hypot(dy, dx))
        start = int(np.searchsorted(r_sorted, d, side="left"))
        if start >= r_sorted.size:
            continue
        sy = cy[start:] + dy
        sx = cx[start:] + dx
        inb = (sy >= 0) & (sy < h) & (sx >= 0) & (sx < wd)
        if not inb.any():
            continue
        tgt = sy[inb] * wd + sx[inb]
        cand = lvl[start:][inb]
        # Sparse tests only: index the candidates, never the whole grid. A
        # per-ring pass over the padded domain would cost more than the bake.
        free = ~np.isfinite(flat[tgt])
        if exf is not None:
            free &= ~exf[tgt]
        ok = free & (zf[tgt] <= cand - float(min_depth_m))
        if not ok.any():
            continue
        t = tgt[ok]
        v = cand[ok].astype(np.float32)
        # Claim first, then reduce: np.maximum against NaN propagates NaN, so
        # the claim has to establish a floor before duplicates inside this ring
        # can be resolved against each other.
        flat[t] = np.float32(-np.inf)
        np.maximum.at(flat, t, v)
        added += int(np.unique(t).size)

    stats["width_added_cells"] = float(added)
    stats["width_added_frac"] = float(added) / float(idx.size + added) if (
        idx.size + added) else 0.0
    return w, stats


# --------------------------------------------------------------------------- fill

#: Exponent on the slope ratio for the EXTENT threshold. Gentler than the depth
#: term's -0.3 and in the opposite sense: this raises the bar a cell must clear
#: to count as inundated, so steeper ground needs to stand FURTHER below the
#: water surface before it is called wet.
SLOPE_EXTENT_EXP = 1.0

#: Clamp on the extent slope ratio. The floor matters more than the ceiling:
#: 58% of river cells sit on the epsilon-fill floor where the "slope" is the
#: fill's own increment, and there the threshold must not collapse to zero and
#: inundate every marginal cell.
SLOPE_EXTENT_MIN = 1.0 / 4.0
SLOPE_EXTENT_MAX = 8.0


def enforce_neighbour_consistency(water_m, z_ground_m, *, max_iter: int = 512):
    """No cell's water may stand higher than adjacent water on HIGHER ground.

    THE DEFECT THIS FIXES, measured before it was written. On tile (-4,-4) at
    bake_ver 15, 13.59% of downstream steps along traced reaches have the water
    surface RISING -- p90 627 mm against a 100 mm wire LSB, max 4 m -- and 100%
    of them are between 8-CONNECTED pixels, so tracing explains none of it. The
    owner saw it from the air: "the magenta blocks actually seem to flow
    slightly up hill".

    WHY `enforce_descent` DOES NOT ALREADY COVER THIS. It guarantees a
    non-increasing surface along the D8 RECEIVER FOREST, and it does. Two pixels
    can sit side by side on the drawn channel and drain to DIFFERENT receivers,
    in which case the sweep never compares them; `fill_to_local_surface` then
    hands each the level of the reach IT drains into. The guarantee is real and
    simply does not cover the pairs a player walks along.

    THE RULE, and it is a physical impossibility rather than a smoothing. Take
    adjacent cells ``c`` and ``b`` with ``ground[b] > ground[c]``, both wet. If
    ``S[c] > S[b]`` then, since ``S[b] > ground[b]``, we have
    ``S[c] > ground[b]`` -- c's water stands above b's GROUND, so nothing
    separates them and they are one body. One body has one level, and water
    runs to the lower one. So ``S[c] := S[b]``.

    IT CANNOT DRY A CELL, structurally, which is what makes it safe to apply
    unconditionally: the new value is an upstream neighbour's surface, that
    neighbour stands on higher ground, so the new surface still clears this
    cell's own ground. Measured: wet count unchanged to 0.00%.

    IT IS NOT "WATER FINDS ITS LEVEL", and that distinction was bought with a
    measurement. Equalising every connected adjacent pair -- the obvious rule --
    removes 98% of the rises and drops the surface a median 925 mm on
    essentially EVERY wet cell, because a flowing channel has a gradient by
    definition and min-propagation is transitive, so it chains the global
    minimum up the whole network and flattens the long profile. This rule is
    directional: only a cell standing higher than its own feeder is lowered.

    COST, measured on (-4,-4) to convergence (96 iterations): rises 561,643 ->
    0, wet count unchanged, surface lowered on 274,817 cells (75% of wet) by
    p50 634 mm. That 75% of the plane violates a physical impossibility is
    itself the finding -- the level assignment is not seamed at rare junctions,
    it is systematically inconsistent, which is what
    water-system-architecture §11b means by two authorities composed with
    nothing forcing them to agree.

    ``water_m``  metres absolute, NaN dry. Modified into a copy and returned.
    ``z_ground_m``  the SHIPPED surface, the same array
    ``fill_to_local_surface`` tests against.
    """
    w = np.array(water_m, dtype=np.float32, copy=True)
    z = np.ascontiguousarray(z_ground_m, dtype=np.float32)
    if w.shape != z.shape:
        raise ValueError(f"water {w.shape} does not match ground {z.shape}")

    # ONE SWEEP IN DESCENDING GROUND ORDER, not a relaxation.
    #
    # The constraint only ever propagates from higher ground to lower, so if
    # cells are visited highest-first then every upstream neighbour is already
    # final when a cell is reached, and one pass is the exact answer. This is
    # the same argument `flow.enforce_descent` uses to avoid iterating, and here
    # it is worth a great deal: the relaxation this replaces needed 96 rounds of
    # eight shifted comparisons over the PADDED domain -- 85 M cells, a 340 MB
    # temporary per shift -- to reach the identical fixed point. `max_iter` is
    # kept in the signature only so a caller written against the old shape does
    # not break; it is unused.
    del max_iter
    h, wd = w.shape
    wet_idx = np.flatnonzero(np.isfinite(w))
    if wet_idx.size == 0:
        return w, {"level_consistency_sweeps": 1.0,
                   "level_consistency_lowerings": 0.0,
                   "level_consistency_wet_cells": 0.0}

    flat_w = w.ravel()
    flat_z = z.ravel()
    order = wet_idx[np.argsort(-flat_z[wet_idx], kind="stable")]

    lowered = 0
    for idx in order:
        idx = int(idx)
        y, x = divmod(idx, wd)
        cur = flat_w[idx]
        zc = flat_z[idx]
        best = cur
        for dy in (-1, 0, 1):
            ny = y + dy
            if ny < 0 or ny >= h:
                continue
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                nx = x + dx
                if nx < 0 or nx >= wd:
                    continue
                j = ny * wd + nx
                if flat_z[j] <= zc:
                    continue                      # not upstream of this cell
                nv = flat_w[j]
                if nv == nv and nv < best:        # wet (NaN fails ==) and lower
                    best = nv
        if best < cur:
            flat_w[idx] = best
            lowered += 1

    stats = {
        "level_consistency_sweeps": 1.0,
        "level_consistency_lowerings": float(lowered),
        "level_consistency_wet_cells": float(wet_idx.size),
    }
    return w, stats


def _shift2(a, dy, dx, fill):
    """`a` shifted by (dy, dx), edges filled. Local to the consistency pass."""
    out = np.full_like(a, fill)
    h, wd = a.shape
    out[max(0, -dy):h - max(0, dy), max(0, -dx):wd - max(0, dx)] = \
        a[max(0, dy):h - max(0, -dy), max(0, dx):wd - max(0, -dx)]
    return out


def fill_to_local_surface(water_m, z_ground_m, receivers, *, cell_m: float,
                          exclude=None,
                          min_depth_m: float = WIDEN_MIN_DEPTH_M,
                          slope=None):
    """Fill sideways to the LOCAL water surface. Returns ``(w, stats)``.

    THE RULE THIS REPLACES, AND WHY. ``widen_to_channel_width`` paints a ribbon
    ``channel_width_m(Q)`` across -- discharge in, metres of width out. That is
    a formula deciding extent, and on the measured corridor it decides a ribbon
    1-5 px wide while the terrain at the drawn water level allows p50 11-28 m
    and p90 47-163 m. The result is a river that crosses a valley floor as a
    strip with open air, not bank, on either side of it. THE TERRAIN SHOULD
    SHAPE THE WATER: the same discharge belongs wide and shallow on a
    floodplain and narrow and deep in a gorge, and only the ground can say
    which one this is.

    THE RULE::

        level(c) = the water surface of the FIRST DRAWN CHANNEL CELL on c's own
                   downstream D8 path            (NaN if there is none)
        wet(c)  <=>  z_ground(c) <= level(c) - min_depth_m

    That is Height Above Nearest Drainage, inverted: a cell is inundated when
    it stands below the surface of the water it would flow into.

    WHY THE LEVEL COMES FROM THE FLOW PATH AND NOT FROM THE NEAREST CHANNEL,
    which is the whole design and was decided by measurement rather than by
    argument. "Nearest" is the reading the words invite -- carry each reach's
    surface outward, stop where the ground rises above it -- and it FLOODS THE
    CONTINENT. Run on the corridor's four bv12 tiles it takes 317,665 wet cells
    to 66,546,420, a factor of 209, covering 27-40% of a tile with water whose
    median added DEPTH is 7.8 m and whose maximum is 78 m; the median cell sits
    550-630 m from any channel. The mechanism is not exotic and it is not a bug
    in the flood: a cell one step off a channel that lies on a ledge is below
    that channel's surface, and so is the cell below it, and the cell below
    that -- the nearest channel never changes as you descend, so the level is
    never re-anchored and the fill runs down the hillside carrying a level it
    left behind. Water does not stand 8 m deep on a hillside; it runs back to
    the river. See ``docs/measurements/river-lateral-fill-2026-08-04.txt``.

    Anchoring on the flow path fixes exactly that, because the level a cell
    inherits is the surface of the water it actually drains into. The hillside
    cell above a channel at 460 m gets 460 m, not the 500 m reach it happens to
    be nearest, and it is dry. Nothing else had to be added -- no distance cap,
    no width term, no relief term. THE CAP THAT WAS IN THE FIRST VERSION IS
    GONE ON PURPOSE: a fill that needs a radius to stop is a formula wearing a
    flood's clothes.

    THE THREE PROPERTIES THIS HAS BY CONSTRUCTION. Two are exact; the third is
    bounded by a quantity of the river's own, and is stated as that bound rather
    than rounded up to "level".

      * **Connected.** If ``c`` is wet then so is its receiver: they share a
        level, and the receiver is strictly lower on the surface the forest was
        built on. So every wet cell has a wet descending path to the channel,
        which is what "connected to the channel" has to mean for water.
      * **Level across a section, to the channel's own gradient.** A bank cell
        that drains STRAIGHT into the reach inherits its float exactly -- the
        same number, not a tolerance. A cell whose steepest descent is DIAGONAL
        arrives a row or two downstream and inherits that row's surface, so the
        honest bound on a section's spread is ``gradient x lateral offset``,
        which is what the unit test asserts. Measured on the corridor that is
        p90 0.29-0.88 m against the shipped ribbon's own p90 of 0.46-2.07 m:
        this surface is FLATTER than the one it replaces, and the adjacent-cell
        step distribution agrees at every percentile up to p99.
      * **Descends downstream.** The level field is piecewise the centreline
        surface, and ``graded_water_surface`` already guarantees that never
        rises going down. This function does not touch a single drawn cell's
        value; it only decides which further cells share one.

    WHY IT CANNOT RUN AWAY. The wet set around a reach is ``{c : c drains to
    this reach and stands below its surface}``. Ground rises monotonically as
    you walk up a flow path, so that set is a collar whose edge is where the
    ground crosses the level -- there is no path by which a level can be
    carried into terrain that a lower reach already owns, because such terrain
    drains to the lower reach and takes ITS number.

    IT AGREES WITH THE LAKE HALF. ``basins.lake_extent_mask`` defines a lake as
    the cells below a surface that are connected to the seed; this is the same
    sentence with the level made a per-cell field instead of a scalar and the
    channel in place of the seed, so "filling" means one thing across the bake.

    ``water_m``  ``graded_water_surface``'s output: metres absolute, NaN dry,
    registered basins already excluded. Every finite cell keeps its own value.
    ``z_ground_m``  the SHIPPED (post-B5) surface, for the reason
    ``widen_to_channel_width`` used it: it is the ground the client draws the
    waterline against and the array the codec takes the stored depth against.
    Outside the registered basins it is the routing surface cell for cell, so
    the descent that makes the wet set connected holds on the array actually
    tested.
    ``receivers``  ``d8_receivers(z_route, ...)[0]`` -- the SAME forest
    ``graded_water_surface`` graded along, so the water plane and its extent
    cannot disagree about which way the river runs.
    ``exclude``  the registered-basin mask. A basin is a BARRIER, not merely a
    hole: its surface is on the wire in ``SECTION_BASIN_TABLE`` and the client
    composes the two samplers, so a cell whose water would arrive through a
    basin is left to the basin's own row rather than given a second, disagreeing
    level here.
    ``min_depth_m``  the codec representability floor, see ``WIDEN_MIN_DEPTH_M``.
    """
    w = np.array(water_m, dtype=np.float32, copy=True)
    z = np.ascontiguousarray(z_ground_m, np.float32)
    if z.shape != w.shape:
        raise ValueError(f"water {w.shape} and ground {z.shape} disagree")
    rec = np.ascontiguousarray(receivers, dtype=np.int32).ravel()
    if rec.size != w.size:
        raise ValueError(
            f"receivers has {rec.size} cells, water has {w.size}")
    blocked = (
        np.zeros(w.size, np.uint8) if exclude is None
        else np.ascontiguousarray(exclude, bool).ravel().view(np.uint8)
    )

    src = np.isfinite(w)
    n_src = int(src.sum())
    stats = {
        "width_centreline_cells": float(n_src),
        "width_added_cells": 0.0,
        "width_added_frac": 0.0,
        "width_min_depth_m": float(min_depth_m),
    }
    if n_src == 0:
        return w, stats

    level = np.full(w.size, np.nan, np.float32)
    level[src.ravel()] = w.ravel()[src.ravel()]
    _fill_levels(level, rec, blocked, src.ravel().view(np.uint8))

    reached = np.isfinite(level)
    hand = z.ravel() - level                       # Height Above Nearest Drainage

    # THE SLOPE TERM. Off unless a slope field is handed in.
    #
    # WHAT IT FIXES. The rule above spreads water sideways to the local surface
    # with no reference to how steep the ground is, so the same discharge makes
    # the same sheet on a floodplain and on a 17% mountainside. Flying it, the
    # owner: "for rivers flowing down steep surfaces the placement of water does
    # not make sense such that the magenta voxels are not being placed on a
    # downslope where gravity would actually guide and push the water on a path
    # of least resistance", and the descent read as "a manmade magenta
    # staircase, not a natural water flow falling down a gulley".
    #
    # He is describing water that failed to CONCENTRATE. Fast water on a steep
    # bed occupies less cross-section for the same Q, so it should run as a
    # thread in the steepest line rather than pond across the slope.
    #
    # NOT A RADIUS CAP, and that is deliberate: this function's own docstring
    # retired one -- "a fill that needs a radius to stop is a formula wearing a
    # flood's clothes" -- after a measured 209x flood. This scales the DEPTH
    # THRESHOLD instead, which keeps every property the rule has by
    # construction. Connectedness, levelness across a section and downstream
    # descent all survive because the threshold changes WHICH cells clear the
    # bar, never where a level came from.
    #
    # The physical reading: a cell 5 cm below the water surface on a steep bed
    # is not inundated, it is draining. On a floodplain the same 5 cm is a
    # backwater. Same rule, and the ground says which.
    md = np.float32(min_depth_m)
    if slope is not None:
        s = np.maximum(np.asarray(slope, np.float64).ravel(), 0.0)
        ratio = np.clip(s / SLOPE_REF_M_PER_M, SLOPE_EXTENT_MIN, SLOPE_EXTENT_MAX)
        md = (np.float64(min_depth_m) * ratio**SLOPE_EXTENT_EXP).astype(np.float32)
    wet = reached & (hand <= -md)
    wet |= src.ravel()
    flat = w.ravel()
    flat[wet & ~src.ravel()] = level[wet & ~src.ravel()]

    added = int(wet.sum()) - n_src
    stats["width_added_cells"] = float(added)
    stats["width_added_frac"] = (
        float(added) / float(n_src + added) if (n_src + added) else 0.0
    )
    # HOW MUCH OF THE DOMAIN EVEN HAS AN ANSWER. A cell whose flow path leaves
    # the tile, or ends in a registered basin, without meeting drawn water has
    # NO local surface and is dry for want of a number rather than for want of
    # room. Reporting it separates "the terrain contained the water here" from
    # "nothing here drains to a drawn river", which look identical in a wet
    # count and mean opposite things.
    stats["fill_drains_to_channel_cells"] = float(int(reached.sum()))
    stats["fill_drains_to_channel_frac"] = float(reached.mean())
    # THE MARGIN THE FILL STOPPED ON. `hand` is metres of ground above the local
    # water surface; on the cells that drain to a channel and stayed DRY it is
    # how much freeboard the terrain had. A p50 of centimetres would mean the
    # extent is riding on the codec's own LSB and the picture is one rounding
    # away from changing; metres means the ground decided it.
    dry_reached = reached & ~wet
    if dry_reached.any():
        h = hand[dry_reached]
        for p in (50, 90):
            stats[f"fill_dry_freeboard_p{p}_m"] = float(np.percentile(h, p))
    if added:
        stats["fill_added_depth_p50_m"] = float(
            np.median(-hand[wet & ~src.ravel()]))
        stats["fill_added_depth_max_m"] = float(
            (-hand[wet & ~src.ravel()]).max())
    return w, stats


@_flow._jit(cache=True)
def _fill_levels(level, rec, blocked, is_src):
    """Carry each drawn cell's water surface UP its own donors. In place.

    One topological sweep in receiver-before-donor order, the same shape as
    ``flow._descent_enforce`` and for the same reason: ``rec`` is a receiver
    forest on a surface where every receiver is strictly lower, so it is an
    acyclic forest and one pass down-then-up is the exact answer rather than an
    iteration to convergence.

    Three kinds of cell terminate a chain and none of them is an ordering
    accident:

      * a DRAWN cell (``is_src``) -- it already carries its own graded surface
        and is what every cell above it inherits;
      * a BLOCKED cell (a registered basin) -- its level is NaN, so everything
        draining through it inherits NaN and is dry here, leaving that water to
        the basin table that already describes it;
      * a ROOT (``rec < 0``) -- the flow path left the domain without meeting
        drawn water, so there is no local surface and NaN is the honest answer.

    The stack is grown by doubling rather than allocated at ``n``: a full-domain
    int32 stack is 340 MB at production for a chain depth that is in practice
    thousands.
    """
    n = level.size
    done = np.zeros(n, np.uint8)
    for i in range(n):
        if is_src[i] != 0:
            done[i] = 1
        elif blocked[i] != 0:
            done[i] = 1
            level[i] = np.float32(np.nan)
    stack = np.empty(1024, np.int32)
    for start in range(n):
        if done[start] != 0:
            continue
        top = 0
        c = start
        while True:
            if top >= stack.size:
                bigger = np.empty(2 * stack.size, np.int32)
                bigger[:top] = stack[:top]
                stack = bigger
            stack[top] = c
            top += 1
            done[c] = 1
            t = rec[c]
            if t < 0 or done[t] != 0:
                break
            c = t
        for i in range(top - 1, -1, -1):
            c = stack[i]
            t = rec[c]
            if t < 0:
                level[c] = np.float32(np.nan)
            else:
                level[c] = level[t]


# --------------------------------------------------------------------------- contact

def _diag_slices(h, w, dc):
    """The four aligned views of a diagonal adjacency, as slice pairs.

    ``a`` at (r, c), ``b`` at (r+1, c+dc), and the two CORNERS the flow would
    have to pass through, ``e1`` at (r+1, c) and ``e2`` at (r, c+dc). Written
    once because getting one of the four windows off by a row is a bug that
    reads as "the fix does nothing on half the diagonals".
    """
    if dc > 0:
        return ((slice(0, h - 1), slice(0, w - 1)),      # a
                (slice(1, h), slice(1, w)),              # b
                (slice(1, h), slice(0, w - 1)),          # e1 = (r+1, c)
                (slice(0, h - 1), slice(1, w)))          # e2 = (r, c+dc)
    return ((slice(0, h - 1), slice(1, w)),
            (slice(1, h), slice(0, w - 1)),
            (slice(1, h), slice(1, w)),
            (slice(0, h - 1), slice(0, w - 1)))


def face_contact_stats(water_m, z_ground_m, *, min_depth_m: float = 0.0,
                       components: bool = True):
    """How much of the drawn water actually TOUCHES the water beside it.

    THE ACCEPTANCE CRITERION, computed the way the client draws rather than the
    way the mask looks. ``lakes.h`` resolves the water surface per VOXEL COLUMN
    by NEAREST fine pixel (``waterSurfaceMmAtVoxel``: ``floorDiv(vx*100, 1875)``),
    so a pixel's water is one flat slab spanning ``[ground, surface]`` across all
    18.75 voxels of its footprint, and ``implicitWaterFill`` fills a voxel iff it
    is above the ground and below that surface. Therefore two plan-adjacent
    pixels share a VOXEL FACE iff

        surface(lower) > ground(higher)

    and nothing else -- not "both are wet", which is what a mask tells you.

    TWO ADJACENCIES, AND ONLY ONE OF THEM IS A FACE. A diagonal neighbour shares
    an EDGE. Two columns touching at a corner are two objects with air between
    them, which is precisely the owner's "several cubes of water placed in a
    general direction but disconnected going down the slope". So the connected
    components here are over 4-adjacency AND the overlap test, i.e. voxel
    6-connectivity, and ``plan8_components`` is reported beside them as the
    number the old mask-labelling probes were reading.

    Returns a stats dict. Sparse throughout: the wet set is ~1-3% of a padded
    domain and a dense label image of it is gigabytes.

    ``components=False`` drops the union-find and keeps only the vectorised
    half. That is what the BAKE asks for: the labelling is a Python loop over
    millions of edges and would cost more than the water stage it is grading,
    while the two numbers that gate -- how many face adjacencies fail to touch,
    and how many cells touch nothing -- are pure numpy. The component counts are
    the offline probe's job (``tools/river_column_contact.py``).
    """
    w = np.asarray(water_m)
    z = np.asarray(z_ground_m)
    wet = np.isfinite(w)
    n = int(wet.sum())
    out = {"contact_wet_cells": float(n)}
    if n == 0:
        return out

    ry, rx = np.nonzero(wet)
    W = w.shape[1]
    key = ry.astype(np.int64) * W + rx
    surf = w[ry, rx].astype(np.float64)
    grnd = z[ry, rx].astype(np.float64)
    md = float(min_depth_m)

    parent = np.arange(n, dtype=np.int64)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def pairs(dy, dx):
        nk = (ry.astype(np.int64) + dy) * W + (rx.astype(np.int64) + dx)
        inb = ((ry + dy >= 0) & (ry + dy < w.shape[0])
               & (rx + dx >= 0) & (rx + dx < W))
        pos = np.clip(np.searchsorted(key, nk), 0, n - 1)
        hit = inb & (key[pos] == nk)
        return np.flatnonzero(hit), pos[hit]

    n_face = n_touch = 0
    has_face = np.zeros(n, bool)
    face_edges = []
    plan_edges = []
    for dy, dx in ((0, 1), (1, 0)):
        ia, ib = pairs(dy, dx)
        up_a = grnd[ia] >= grnd[ib]
        g_hi = np.where(up_a, grnd[ia], grnd[ib])
        s_lo = np.where(up_a, surf[ib], surf[ia])
        ok = s_lo > g_hi + md
        n_face += int(ia.size)
        n_touch += int(ok.sum())
        has_face[ia[ok]] = True
        has_face[ib[ok]] = True
        face_edges.append((ia[ok], ib[ok]))
        plan_edges.append((ia, ib))
    out.update({
        "contact_face_adjacencies": float(n_face),
        "contact_face_touching": float(n_touch),
        "contact_face_broken": float(n_face - n_touch),
        "contact_isolated_cells": float(int((~has_face).sum())),
        "contact_isolated_frac": float((~has_face).mean()),
    })
    if not components:
        return out
    for dy, dx in ((1, 1), (1, -1)):
        plan_edges.append(pairs(dy, dx))

    def label(edges):
        parent[:] = np.arange(n)
        for ia, ib in edges:
            for a, b in zip(ia.tolist(), ib.tolist()):
                ra, rb = find(a), find(b)
                if ra != rb:
                    parent[max(ra, rb)] = min(ra, rb)
        roots = np.array([find(i) for i in range(n)], np.int64)
        _, lab = np.unique(roots, return_inverse=True)
        return np.bincount(lab)

    face_sizes = label(face_edges)
    plan_sizes = label(plan_edges)
    out.update({
        "contact_face_components": float(face_sizes.size),
        "contact_face_largest_frac": float(face_sizes.max()) / float(n),
        "contact_plan8_components": float(plan_sizes.size),
        "contact_plan8_largest_frac": float(plan_sizes.max()) / float(n),
        # The ratio IS the defect, in one number: how many times more pieces the
        # water breaks into once you ask whether the pieces touch.
        "contact_shatter_ratio": float(face_sizes.size) / float(plan_sizes.size),
    })
    return out


def bridge_to_face_contact(water_m, z_ground_m, *, cell_m: float, exclude=None,
                           min_depth_m: float = WIDEN_MIN_DEPTH_M):
    """Make the drawn water CONTINUOUS DOWN A SLOPE. Returns ``(w, stats)``.

    THE DEFECT THIS EXISTS FOR, in the owner's words after flying bake_ver 13:
    "on a steeper downslope in the river, there will be several cubes of water
    placed in a general direction but disconnected going down the slope ...
    obvious empty space and air all around the placed water." Measured on the
    shipped bv13 corridor, that is not a figure of speech. The same wet cells
    labelled in plan give 24-75 components per tile and labelled the way the
    client draws them give 3,409-7,987 -- a factor of 45 to 310 -- and on ground
    steeper than 0.15 the share of wet cells with NO touching neighbour at all
    runs 3.2% to 20.3%.

    WHY A "FLOOD TO A LEVEL" RULE PRODUCES IT, which is the physics. Filling
    each cell to the horizontal surface of the channel cell it drains to is
    right for PONDED water and wrong for FLOWING water: real water running down
    a gradient forms a thin sheet roughly parallel to the bed, not a staircase
    of puddles. On a raster of columns the difference is visible immediately.
    Writing g for the bed and d for the depth, two face-adjacent columns share a
    voxel face iff ``d_lower > g_upper - g_lower``, so contact fails as soon as
    the bed drops between adjacent pixels by more than the water is deep -- a
    bed gradient of ``d / cell_m``, which at the measured p50 depth is 0.23-0.30.
    On three of the four corridor tiles 13.0-16.6% of drawn cells sit above
    their own such gradient.

    THE SECOND MECHANISM, WHICH IS THE LARGER ONE AND IS PURE RASTERISATION.
    The drawn network is 8-connected: a D8 flow path steps diagonally, and on a
    steep reach the lateral fill adds nothing on either side because the ground
    rises within one pixel, so the water collapses to a ONE PIXEL WIDE DIAGONAL
    CHAIN. Diagonal columns touch at a corner, not a face. Ablated on the same
    tiles, the diagonal alone accounts for 2,874-5,782 of the components and the
    vertical gap for 871-4,103; each one alone shatters the river.

    THE RULE, in two steps, both local and both deterministic::

        CORNER   for every diagonally-adjacent wet pair with neither corner wet,
                 wet the LOWER-bedded corner at the HIGHER of the two surfaces,
                 provided its ground stands at least min_depth below that --
                 the same submergence test `fill_to_local_surface` uses, applied
                 to the corner the flow actually passes through.

        BRIDGE   every wet cell's surface is raised to at least
                 ``max over face-adjacent wet u of (ground(u) + min_depth)``.

    WHY THIS IS THE BLEND THE TWO REGIMES NEED, AND WHY THERE IS NO THRESHOLD IN
    IT. In genuinely ponded water the bridge is EXACTLY A NO-OP, by construction
    and not by tuning: every wet cell in a pool shares one level and each is
    submerged, so ``surface >= ground(u) + min_depth`` already holds for every
    wet neighbour u and the max changes nothing. The raise is
    ``max(0, drop - depth)``, which is zero on level water and grows
    continuously with bed slope. So the surface is horizontal where the water
    ponds and bed-parallel where it runs, the transition is the terrain's own,
    and there is no slope constant anywhere to put a seam along the river.

    THE THREE THINGS IT CANNOT DO, each a bound rather than a hope:

      * **It cannot flood.** ``ground(u) + min_depth <= surface(u)`` for every
        wet u, so a raised surface never exceeds the surface of a cell one pixel
        away that was already drawn at it. The maximum of the water surface over
        the wet set is therefore unchanged, and no cell is lifted above water
        that the fill had already put beside it.
      * **It cannot tilt still water.** See the no-op argument above; the unit
        test asserts it on a flat pool rather than taking the argument's word.
      * **It cannot run away.** The bridge reads BEDS only, never the surfaces
        it is writing, so one pass is the fixed point -- there is no iteration
        and no ordering dependence. The corner step likewise reads only the wet
        set it was handed.

    WHERE IT STOPS, HONESTLY. A diagonal whose BOTH corners stand above the
    water is refused rather than bridged, because wetting one would put water on
    a rise and the bridge would then drag the whole reach up to meet it. Those
    are counted in ``bridge_corner_refused`` and are the residual the acceptance
    statistic still sees.

    ``water_m``      the plane as the extent rule leaves it: metres absolute,
                     NaN dry, registered basins already excluded. Face-adjacent
                     values are read but only ever raised.
    ``z_ground_m``   the SHIPPED (post-B5) surface -- the array the codec takes
                     the stored depth against and the one the client's waterline
                     is drawn on, for the same reason
                     ``fill_to_local_surface`` uses it.
    ``exclude``      the registered-basin mask. A basin is a barrier here too: a
                     corner inside one is left to the basin table.
    ``min_depth_m``  the codec representability floor and one client voxel, so a
                     bridged pair overlaps by a voxel the renderer can draw
                     rather than by a rounding.
    """
    w = np.array(water_m, dtype=np.float32, copy=True)
    z = np.ascontiguousarray(z_ground_m, np.float32)
    if z.shape != w.shape:
        raise ValueError(f"water {w.shape} and ground {z.shape} disagree")
    h, wd = w.shape
    md = np.float32(min_depth_m)
    ex = None if exclude is None else np.ascontiguousarray(exclude, bool)

    wet = np.isfinite(w)
    n0 = int(wet.sum())
    stats = {
        "bridge_corner_added": 0.0,
        "bridge_corner_refused": 0.0,
        "bridge_raised_cells": 0.0,
        "bridge_min_depth_m": float(min_depth_m),
    }
    if n0 == 0:
        return w, stats

    # -- CORNER. Both orientations; each writes into `w`/`wet` before the next
    # is evaluated, which is deliberate: a corner added by the first pass is a
    # legitimate connection for the second and re-adding it would be waste.
    added = refused = 0
    for dc in (1, -1):
        sa, sb, se1, se2 = _diag_slices(h, wd, dc)
        need = wet[sa] & wet[sb] & ~wet[se1] & ~wet[se2]
        if not need.any():
            continue
        lvl = np.maximum(w[sa], w[sb])
        g1, g2 = z[se1], z[se2]
        take1 = g1 <= g2
        g_e = np.where(take1, g1, g2)
        ok = need & (g_e <= lvl - md)
        if ex is not None:
            # A corner inside a registered basin is not ours to wet.
            ok &= ~np.where(take1, ex[se1], ex[se2])
        refused += int((need & ~ok).sum())
        for which, sl in ((take1, se1), (~take1, se2)):
            m = ok & which
            if not m.any():
                continue
            sub_w = w[sl].copy()
            sub_wet = wet[sl].copy()
            added += int((m & ~sub_wet).sum())
            # HIGHER WINS where one corner serves two diagonals, and the
            # `where` is not defensive: a dry cell holds NaN, and NaN
            # propagates through np.maximum, so a plain max would erase the
            # level it was just given. Taking the higher keeps the corner
            # connected to BOTH pairs instead of to whichever was visited last.
            sub_w[m] = np.maximum(
                np.where(sub_wet[m], sub_w[m], np.float32(-np.inf)), lvl[m])
            sub_wet[m] = True
            w[sl] = sub_w
            wet[sl] = sub_wet

    # -- BRIDGE. Accumulate the required floor from the BEDS of face-adjacent
    # wet cells, then take one max. Reading `z` and never `w` is what makes the
    # single pass exact.
    floor = np.full(w.shape, -np.inf, np.float32)
    for dy, dx in ((1, 0), (0, 1)):
        sa = (slice(0, h - dy), slice(0, wd - dx))
        sb = (slice(dy, h), slice(dx, wd))
        both = wet[sa] & wet[sb]
        za, zb = z[sa], z[sb]
        np.maximum(floor[sb],
                   np.where(both & (za > zb), za + md, np.float32(-np.inf)),
                   out=floor[sb])
        np.maximum(floor[sa],
                   np.where(both & (zb > za), zb + md, np.float32(-np.inf)),
                   out=floor[sa])
    lift = wet & (floor > w)
    n_lift = int(lift.sum())
    if n_lift:
        amounts = (floor[lift] - w[lift]).astype(np.float64)
        w[lift] = floor[lift]
        for p in (50, 90, 99):
            stats[f"bridge_raise_p{p}_m"] = float(np.percentile(amounts, p))
        stats["bridge_raise_max_m"] = float(amounts.max())
        stats["bridge_raise_mean_m"] = float(amounts.mean())

    stats["bridge_corner_added"] = float(added)
    stats["bridge_corner_refused"] = float(refused)
    stats["bridge_raised_cells"] = float(n_lift)
    stats["bridge_raised_frac"] = float(n_lift) / float(n0 + added)
    stats["bridge_cells_before"] = float(n0)
    stats["bridge_cells_after"] = float(n0 + added)
    return w, stats


# --------------------------------------------------------------------------- gates

def overshoot_stats(water_m, z_ground_m, q_m3_yr, wet, *, cell_m: float,
                    z_route_m=None, q_perennial: float = Q_PERENNIAL_M3_YR):
    """THE OVERSHOOT GATE. How much deeper does the plane stand than its own law?

    `excess = (water - ground) - water_depth(Q)`, per wet cell, in metres. Zero
    means the head landed exactly where the law put it. Positive means the
    descent sweep, or a mismatch between the bed the law assumed and the bed the
    bake actually carved, has raised the water above its design depth -- and
    with no containing ground within 120 m on most bank sides (see the module
    docstring) that surplus does not leak away, it spreads.

    WHY THIS STATISTIC AND NOT A BANK TEST. A bank test asks "did the water get
    out", which on this terrain is answered by geometry that mostly is not
    there. This asks "is the water the size the physics says", which is
    answerable everywhere, is a pure function of Q and the two rasters, and --
    the point -- reads LARGE on flat ground rather than being satisfied by it. A
    flat valley floor is precisely where a descent chain pools, so a statistic
    that went quiet there would be measuring its own construct.

    THE EXCESS DECOMPOSES EXACTLY, and reporting only the total would hide
    which half is at fault. On a wet cell `desired = z_route + law`, so

        excess = (water - ground) - law
               = (z_route - ground)      the BED OFFSET: how far the routing
                                         surface stands above the shipped one
               + (water - desired)       the DESCENT RAISE: what the grading
                                         sweep added to keep the profile falling

    The first is a carve/datum disagreement and should be ~0 outside re-opened
    basins; the second is the pooling term, and it is the one with no geometric
    backstop. Pass `z_route_m` to get both reported separately.

    NO CONTROL MASK, deliberately. The obvious control -- run the same
    arithmetic on non-channel ground -- is VACUOUS: off the network there is no
    water, so a "control excess" would be computed from a water surface this
    function itself invented, and it would read exactly 0 by construction. That
    is the failure mode this branch has already hit three times. The floor that
    means something is a LATERAL one and it is measured in
    `lateral_extent_stats(control=...)`, where the same water depths are walked
    over ordinary hillside and the terrain gets to answer.

    `ground` is the SHIPPED surface (post-B5), because that is the ground the
    client will draw the waterline against, not the routing bed.
    """
    def _pct(a, ps=(50, 90, 95, 99)):
        if a.size == 0:
            return {f"p{p}": 0.0 for p in ps}
        v = np.percentile(a, ps)
        return {f"p{p}": float(x) for p, x in zip(ps, v)}

    q = np.asarray(q_m3_yr)
    depth = (np.asarray(water_m)[wet] - np.asarray(z_ground_m)[wet]).astype(np.float64)
    law = water_depth_m(q[wet], q_perennial)
    excess = depth - law

    out = {"cells": float(wet.sum()), "cell_m": float(cell_m)}
    out.update({f"depth_{k}_m": v for k, v in _pct(depth).items()})
    out["depth_max_m"] = float(depth.max()) if depth.size else 0.0
    out.update({f"law_depth_{k}_m": v for k, v in _pct(law).items()})
    out.update({f"excess_{k}_m": v for k, v in _pct(excess).items()})
    out["excess_max_m"] = float(excess.max()) if excess.size else 0.0
    out["excess_mean_m"] = float(excess.mean()) if excess.size else 0.0
    # The gate proper: the fraction of the drawn network standing more than a
    # metre, and more than the bank probe's validated 10 m ceiling, deeper than
    # its law. A metre is one design depth for a small river and ten voxels.
    for thr in (0.5, 1.0, 2.0, 5.0):
        out[f"excess_over_{thr}m_frac"] = (
            float((excess > thr).mean()) if excess.size else 0.0
        )
    out["depth_over_probe_ceiling_frac"] = (
        float((depth > 10.0).mean()) if depth.size else 0.0
    )
    if z_route_m is not None:
        zr = np.asarray(z_route_m)[wet].astype(np.float64)
        bed_offset = zr - np.asarray(z_ground_m)[wet].astype(np.float64)
        descent_raise = excess - bed_offset
        out.update({f"bed_offset_{k}_m": v for k, v in _pct(bed_offset).items()})
        out["bed_offset_max_m"] = float(bed_offset.max()) if bed_offset.size else 0.0
        out.update({f"descent_raise_{k}_m": v for k, v in _pct(descent_raise).items()})
        out["descent_raise_max_m"] = (
            float(descent_raise.max()) if descent_raise.size else 0.0
        )
        # How much of the network the grading sweep touched at all. A sweep that
        # raised nothing means the carve was already graded; a sweep that raised
        # most of the network means the plane is being held up by its own
        # constraint rather than by the terrain, which is the overshoot regime.
        out["descent_raised_frac"] = (
            float((descent_raise > 0.001).mean()) if descent_raise.size else 0.0
        )
    return out


def lateral_extent_stats(water_m, z_ground_m, q_m3_yr, wet, receivers, *,
                         cell_m: float, max_px: int = 64, sample: int = 20000,
                         seed: int = 0, control=None,
                         q_perennial: float = Q_PERENNIAL_M3_YR):
    """How wide is the waterline actually, against how wide the law says?

    For a random sample of wet cells, step perpendicular to the cell's own D8
    direction and count contiguous pixels with `ground <= water`, both ways.
    That is the bank probe's transect at raster pitch, and it is the direct
    answer to "does the head flood valley-wide": a plane that overshoots on
    ground with no containing bank shows up here as a wet run of tens of pixels
    where the law asked for two.

    SAMPLED, not exhaustive, and the sample is the point rather than a
    concession: 20,000 transects is 30x the bank probe's 630 and the statistic
    is a distribution, so the marginal section buys precision nobody reads while
    an exhaustive walk over 800k wet cells x 128 steps would dominate the bake.

    `max_px` bounds the walk. A run that reaches the bound is COUNTED
    SEPARATELY (`saturated_frac`) rather than recorded as `max_px`, because a
    censored measurement averaged in with uncensored ones understates exactly
    the failure this gate exists to catch.

    `control` is the same walk on non-channel ground at the same water levels,
    which is the floor: some lateral extent is the terrain being flat, not the
    plane being wrong.
    """
    rng = np.random.default_rng(seed)
    z = np.asarray(z_ground_m, np.float64)
    w = np.asarray(water_m, np.float64)
    q = np.asarray(q_m3_yr, np.float64)
    h, wd = z.shape
    rec = np.ascontiguousarray(receivers, dtype=np.int64).ravel()

    def _walk(idx):
        cy, cx = np.divmod(idx, wd)
        r = rec[idx]
        # Flow direction from the D8 receiver; a root (rec < 0) gets the
        # arbitrary +x, which is fine -- it is one cell per tree.
        ry, rx = np.divmod(np.where(r >= 0, r, idx), wd)
        dy, dx = ry - cy, rx - cx
        # Perpendicular, integer, non-zero: (-dy, dx) rotated.
        py, px = -dx, dy
        both = (py == 0) & (px == 0)
        py = np.where(both, 0, py)
        px = np.where(both, 1, px)
        lvl = w.ravel()[idx]
        run = np.zeros(idx.size, np.int32)
        sat = np.zeros(idx.size, bool)
        for sgn in (1, -1):
            alive = np.ones(idx.size, bool)
            for step in range(1, max_px + 1):
                yy = cy + sgn * step * py
                xx = cx + sgn * step * px
                inb = (yy >= 0) & (yy < h) & (xx >= 0) & (xx < wd)
                ok = np.zeros(idx.size, bool)
                sel = alive & inb
                if not sel.any():
                    break
                ok[sel] = z[yy[sel], xx[sel]] <= lvl[sel]
                run += (alive & ok).astype(np.int32)
                # A cell that walks off the domain is censored, not contained.
                sat |= alive & ok & (step == max_px)
                alive &= ok
                if not alive.any():
                    break
        return run, sat

    def _summarise(mask, prefix, with_law):
        flat = np.flatnonzero(mask.ravel())
        out = {f"{prefix}cells": float(flat.size)}
        if flat.size == 0:
            return out
        take = flat if flat.size <= sample else rng.choice(flat, sample, replace=False)
        run, sat = _walk(take)
        # +1 for the cell itself; the walk counts only the two flanks.
        width_m = (run.astype(np.float64) + 1.0) * float(cell_m)
        out[f"{prefix}sampled"] = float(take.size)
        for p in (50, 90, 95, 99):
            out[f"{prefix}width_p{p}_m"] = float(np.percentile(width_m, p))
        out[f"{prefix}width_max_m"] = float(width_m.max())
        out[f"{prefix}saturated_frac"] = float(sat.mean())
        if with_law:
            law_w = channel_width_m(q.ravel()[take], q_perennial)
            ratio = width_m / np.maximum(law_w, 1e-9)
            for p in (50, 90, 95, 99):
                out[f"{prefix}law_width_p{p}_m"] = float(np.percentile(law_w, p))
                out[f"{prefix}ratio_p{p}"] = float(np.percentile(ratio, p))
            out[f"{prefix}ratio_max"] = float(ratio.max())
            out[f"{prefix}ratio_over_4_frac"] = float((ratio > 4.0).mean())
        return out

    stats = _summarise(wet, "", True)
    if control is not None:
        stats.update(_summarise(control, "control_", False))
    return stats
