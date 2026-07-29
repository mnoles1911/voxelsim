"""B2e — stream-power incision depth.

`depth = K * A^m * S^n`, the detachment-limited stream-power law. It is the pass that turns
a flow field into landform: without it the drainage network exists in the accumulation
array and nowhere in the ground.

**K is the single most important knob in the whole bake, and none of the summary statistics
distinguish a good value from a useless one — only a hillshade does.** Measured on a real
tile at 3.75 m/px:

| K      | mean incision | p99   | reads as                                          |
|--------|---------------|-------|---------------------------------------------------|
| 0.0004 | 0.00 m        | 0.01 m| a network in the flow field and nowhere else      |
| 0.012  | 0.13 m        | 0.63 m| gentle modulation; visible but not landform       |
| 0.05   | 0.56 m        | 2.61 m| valleys emerging                                  |
| 0.15   | 1.66 m        | 7.84 m| **properly dissected hillslope**                  |

Hence `K = 0.15` as the default. The plausible-looking first guess of 4e-4 was ~400x too
small and every aggregate statistic it produced looked perfectly reasonable.

**Re-calibrated 2026-07-29 on correctly-routed drainage.** The table above was measured on
a plain-fill field where every filled pit was a level lake, so MFD terminated there and
69.2 % of the land never reached the sea. With Barnes' epsilon fill the catchment areas grow
~2.9x, and since incision goes as `A^m` the same K now cuts ~1.6x deeper. The expectation
was therefore that K had to fall to ~0.09 — and on a hillshade of real tile (-5,3) at
1.875 m/px that turned out to be **wrong**: 0.09 leaves trunk channels legible but
tributaries not, 0.15 gives a legible dendritic network at both 7.7 km and 1.4 km, and 0.25
begins showing parallel grooving at the 1.4 km zoom. The 1.6x arithmetic is right about the
depth and wrong about the conclusion, because the original judgement was made on a different
tile at 3.75 m/px, so there was never a like-for-like appearance to preserve. **K = 0.15
stands, now for a measured reason rather than an inherited one.**

Two caveats on that calibration, recorded so they are not rediscovered:
  * the `cap_m` clamp binds at every K tested including 0.03, so `max` incision is censored
    and p99 is the only usable tail statistic;
  * incision depth alone cannot rank K — it rises monotonically and without a kink — so the
    judgement rests on the hillshade plus the fraction of the domain left steeper than the
    angle of repose. Beware that the latter, measured as a 2D gradient magnitude, can exceed
    the repose angle by up to sqrt(2) on ridges and corners without violating the per-axis
    rule `thermal.relax` actually enforces.

`tools/calibrate_stream_k.py` is that whole procedure as a file.

numpy only; no scipy, no numba.
"""

from __future__ import annotations

import numpy as np

__all__ = ["stream_power"]

# Slope floor. Flow routing hands back S == 0 on filled pits and flats; without a floor the
# `S**n` term is 0**0.8 == 0 there, which is fine, but the epsilon also keeps the function
# strictly monotone in S at the origin, which is what the monotonicity test asserts.
_SLOPE_EPS = 1e-6

# --- channel initiation -----------------------------------------------------
#
# Without a threshold, `K * A^m * S^n` incises EVERY cell that has any upslope area
# at all, which at 1.875 m/px is every cell in the tile. Measured on real tile
# (-5,3): 77.6 % of the domain incised by more than one voxel at K = 0.03, rising
# to 98.6 % at K = 0.15. That is not a drainage network, it is a slope-dependent
# lowering of the whole surface with the network faintly embedded in it.
#
# Real landscapes have a channel head at a critical contributing area — hillslope
# processes dominate above it, channel incision below — and that area, not K, is
# the primary control on DRAINAGE DENSITY, i.e. on how finely dissected the ground
# reads. Without it K has to do double duty: it sets both how deep channels cut and
# how many there are, and those are not the same question.
#
# 10^4 m^2 is mid-range for the 10^3–10^5 m^2 channel-initiation areas reported for
# humid soil-mantled landscapes; at 1.875 m/px it is ~2840 cells, so the gate is
# resolved by the raster with room to spare rather than sitting at the Nyquist
# limit where it would alias into the cell grid.
A_CRIT_M2 = 1.0e4

# The gate is SOFT on purpose. A hard cutoff at A_crit puts a step in incision
# depth along the contour where contributing area crosses the threshold — which is
# a visible seam along a curve, exactly the failure class this whole project exists
# to remove, just not on the 30 m grid. `A^q / (A^q + A_crit^q)` is C-infinity,
# monotone in A, and preserves the monotonicity contract the tests assert.
# q = 2 makes the transition span roughly a decade of area, which is about how
# sharply real channel heads appear.
GATE_Q = 2.0

# --- the sea-level taper ----------------------------------------------------
#
# Nothing in the bake gated on depth, so priority-flood, MFD, stream power and
# thermal relaxation all ran on the seafloor. Measured on a 100%-ocean tile
# (-7,4), which the model emits as real BATHYMETRY rather than a water plane
# (-4654 to -2405 m, 2.26 km of relief in one tile): 26.6 M cells flagged as
# channel, **39.7% of the tile** against 4.1% on an alpine tile, and 0.87 m mean
# incision against 0.13 m. That is subaerial fluvial erosion at three kilometres
# depth -- dendritic river valleys cut by rain that cannot fall.
#
# It is also the single largest storage lever found: the ocean tile was the
# LARGEST of the three baked, 28.35 MB against 22.62 for alpine, because all
# that invented detail has to be encoded. An ocean-majority world was paying its
# highest per-tile price for drainage no player can reach.
#
# TAPERED, NOT CUT. A hard stop at z=0 would put a step in incision depth along
# the entire coastline -- a seam along the most visually scrutinised curve in the
# world, which is the exact failure class this project exists to remove. The
# taper also keeps the coast itself honest: river mouths, deltas and the incised
# shelf valleys that are real features of a coastline all live in the first
# hundred metres of depth, and they are cut by rivers that DID flow there at
# lower sea level. -200 m is the shelf break, a real physiographic boundary
# rather than a round number, and below it the erosion being suppressed is the
# purely fictional kind.
SEA_TAPER_TOP_M = 0.0
SEA_TAPER_BOTTOM_M = -200.0


def stream_power(acc: np.ndarray, slope: np.ndarray, K: float = 0.15,
                 m: float = 0.45, n: float = 0.8, cap_m: float = 25.0,
                 a_crit_m2: float = A_CRIT_M2, gate_q: float = GATE_Q,
                 elev_m: np.ndarray | None = None,
                 sea_taper_top_m: float = SEA_TAPER_TOP_M,
                 sea_taper_bottom_m: float = SEA_TAPER_BOTTOM_M) -> np.ndarray:
    """Incision depth in **metres**, to be subtracted from the depression-filled surface.

    `acc`   upslope contributing area A, in m^2 (the MFD accumulation field).
    `slope` local slope S, dimensionless rise/run.
    `cap_m` depth cap. Over-carving is its own failure mode — a single cell with an
            enormous catchment and a steep local slope would otherwise punch a shaft
            through the tile — so the depth is clamped rather than left unbounded.
    `a_crit_m2` channel-initiation area. See the module docstring; pass 0 to disable
            and recover the pre-2026-07-29 behaviour exactly.
    `gate_q`  sharpness of the initiation gate.

    Returns float32, non-negative, `<= cap_m` everywhere, and monotone non-decreasing in
    both A and S.
    """
    a = np.asarray(acc, dtype=np.float32)
    s = np.asarray(slope, dtype=np.float32)
    if a.shape != s.shape:
        raise ValueError(f"acc {a.shape} and slope {s.shape} must have the same shape")
    if K < 0.0:
        raise ValueError(f"K must be non-negative, got {K}")
    if cap_m <= 0.0:
        raise ValueError(f"cap_m must be positive, got {cap_m}")
    if a_crit_m2 < 0.0:
        raise ValueError(f"a_crit_m2 must be non-negative, got {a_crit_m2}")
    if gate_q <= 0.0:
        raise ValueError(f"gate_q must be positive, got {gate_q}")

    # Negative area or slope is meaningless and would make the power terms NaN; clamping is
    # cheaper than trusting every upstream producer.
    a = np.clip(a, 0.0, None)
    s = np.clip(s, 0.0, None) + np.float32(_SLOPE_EPS)

    depth = np.float32(K) * np.power(a, np.float32(m), dtype=np.float32) \
        * np.power(s, np.float32(n), dtype=np.float32)

    if a_crit_m2 > 0.0:
        # Computed as aq / (aq + acq) rather than 1 / (1 + (ac/a)^q) so that a == 0
        # is 0 by arithmetic instead of by a division-by-zero special case.
        aq = np.power(a, np.float32(gate_q), dtype=np.float32)
        acq = np.float32(a_crit_m2) ** np.float32(gate_q)
        depth = depth * (aq / (aq + acq))

    if elev_m is not None and sea_taper_bottom_m < sea_taper_top_m:
        z = np.asarray(elev_m, dtype=np.float32)
        if z.shape != a.shape:
            raise ValueError(f"elev_m {z.shape} must match acc {a.shape}")
        # Smoothstep, not a ramp: a linear taper is C0 but not C1, so its
        # DERIVATIVE steps at both ends of the transition. The whole v9 carrier
        # rework exists because a gradient discontinuity is visible under
        # directional light even when the value is continuous, and there is no
        # reason to reintroduce one along the shelf break.
        t = (z - np.float32(sea_taper_bottom_m)) / np.float32(
            sea_taper_top_m - sea_taper_bottom_m)
        t = np.clip(t, 0.0, 1.0)
        depth = depth * (t * t * (np.float32(3.0) - np.float32(2.0) * t))

    return np.minimum(depth, np.float32(cap_m))
