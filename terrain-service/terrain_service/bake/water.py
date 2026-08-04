"""Runoff-weighted discharge, water heads, and the graded water surface (plan P2).

WHAT THIS MODULE IS FOR
-----------------------
`docs/watershed-system-plan.md` item 7 / §4.1. The bake already knows where
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
    term in `water_surface_m` reads local relief, a bank height, or a rim;
  * OVERSHOOT IS THE PRIMARY FAILURE MODE and is gated on, not assumed away.
    `overshoot_stats` measures how much deeper the plane stands than its own
    law allows, and `lateral_extent_stats` measures how far the resulting
    waterline actually spreads. Both read on the shipped raster, and both are
    reported next to what they read on flat, non-channel ground -- because a
    statistic that is satisfied by its own construct passes vacuously, which
    has already happened three times on this branch.

WHAT IS DELIBERATELY NOT HERE
-----------------------------
* **No new hydraulics.** Width and depth are `channel.h`'s published laws, at
  its own exponents, re-anchored per §4.1.3. Two implementations of one law is
  how the three currencies above happened.
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

__all__ = [
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


def water_depth_m(q_m3_yr, q_perennial: float = Q_PERENNIAL_M3_YR):
    """Design water depth: channel.h's 3/4 of channel depth.

    THIS IS THE HEAD, and it is a function of Q and nothing else. See the
    module docstring: the bank probe found no containing ground within 120 m on
    58% of stream bank sides at 10 m depth, so there is no geometry to fall
    back on and none is consulted.
    """
    return channel_depth_m(q_m3_yr, q_perennial) * (
        WATER_DEPTH_NUM / WATER_DEPTH_DEN
    )


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

def graded_water_surface(z_route_m, q_m3_yr, receivers, wet, *, eps_m: float,
                         exclude=None, q_perennial: float = Q_PERENNIAL_M3_YR):
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
    add = water_depth_m(np.asarray(q_m3_yr)[wet], q_perennial).astype(np.float32)
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
