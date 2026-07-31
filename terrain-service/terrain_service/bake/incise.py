"""B2e — stream-power incision.

Two formulations, and the measurement that forced the second:

`stream_power` — the original per-cell depth law, `depth = K * A^m * S^n`, subtracted
from the surface. It is the pass that turns a flow field into landform: without it the
drainage network exists in the accumulation array and nowhere in the ground.

`profile_incision` — the same detachment-limited stream-power law solved IMPLICITLY
along the D8 receiver tree (Braun & Willett 2013), one downstream-to-upstream pass:

    z_new(c)  solves  z_new = z - K_dt * A^m * ((z_new - z_new(rcv)) / dist)^n

**Why the second exists.** The per-cell depth law paints depth onto the carrier; it
cannot re-grade a channel's LONG PROFILE, because each cell's cut is decided by the
carrier's local slope alone. Measured on the three exemplar tiles
(docs/terrain-validation-2026-07.md section 7.1, and the stage dumps of 2026-07-29):
at 30 m, where the carrier already carries real concavity, incision is fine (alpine
theta 0.194 -> 0.207 against the Alps' 0.203); in the sub-30 m band the bake invents,
theta reads 0.028-0.086 against 0.177-0.318 for matched real 1 m DTMs, and the depth
law moves it the WRONG way (B0+B1 0.083 -> B2d 0.040 on alpine). Neither the 25 m cap
(binds only at A >= 1e6 m^2, 0.06-2.5% of channel cells) nor B3 thermal (median carved
depth survives it at ratio 1.00) is the mechanism -- both were measured before this
formulation was written. The implicit solve grades each channel toward its own
steady-state profile, which is the thing the depth law cannot do, and on the same
alpine window moves theta 0.065 -> 0.240 (r^2 0.93) into the real 0.18-0.32 band.

Three properties the solve gives that the depth law could not:

  * **z_new >= z_new(receiver) by construction** -- the carve can never cut below the
    downstream profile, so "a single cell with an enormous catchment punching a shaft
    through the tile" is structurally impossible, not clamped away. (The cap is kept
    anyway: it bounds total lowering where the graded profile is far below the
    carrier, e.g. a trunk crossing a carrier bump, and K_dt = 15 UNCAPPED was measured
    at 679 m of incision -- the failure mode is real.)
  * **No carve-created pits along the network** (the depth law leaves the channel bed
    non-monotone wherever d(depth)/dx < -slope, which the next fill flattens into
    slope-free segments).
  * **The profile information travels upstream along the tree**, which is exactly the
    non-locality the apron cannot bound. That is the same class as flow accumulation
    (HYDROLOGY_RESIDUALS), it is bounded in magnitude by `cap_m`, and it is measured
    rather than assumed -- see the seam numbers in the pipeline's B2d block comment.

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

numpy for `stream_power`; `profile_incision` compiles its tree pass with numba when
available (the bake pod has it) and falls back to a pure-python loop that is fine for
test grids and hopeless for a real tile -- the same trade `flow.py` makes. No scipy.
"""

from __future__ import annotations

import functools

import numpy as np

__all__ = ["stream_power", "profile_incision"]

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


# ---------------------------------------------------------------------------
# The implicit profile solve.
# ---------------------------------------------------------------------------

def _jit(**options):
    """`numba.njit` deferred to first call, exactly `flow._jit`'s pattern.

    Importing this module must not require numba (CI has none); the python
    fallback is the reference implementation and is fine at test sizes.
    """

    def decorate(pyfunc):
        state = {}

        def dispatch(*args):
            fn = state.get("fn")
            if fn is None:
                try:
                    import numba
                    fn = numba.njit(**options)(pyfunc)
                except ImportError:  # pragma: no cover - the CI path
                    fn = pyfunc
                state["fn"] = fn
            return fn(*args)

        functools.update_wrapper(dispatch, pyfunc)
        dispatch.py_func = pyfunc
        return dispatch

    return decorate


@_jit(cache=True)
def _profile_pass(order, rec, z, kfac, dist, n_exp, cap, out):
    """One downstream-to-upstream Newton pass over the D8 tree.

    ``order`` is ascending filled elevation, so a receiver (strictly lower on
    the epsilon-filled surface) is always final before any of its donors is
    visited. Sequential by nature -- a donor reads its receiver's SOLVED
    elevation -- so, like the priority flood, do not reach for parallel=True.

    The arrays are float32/int32 -- at the padded production domain every
    float64 working array is 680 MB against the pod's 8 GiB, and the peak
    already measures 5.5 GiB -- while the Newton itself runs in float64
    scalars. The 1e-4 m tolerance is 0.1 mm, one thousandth of the wire LSB.
    """
    n = order.size
    for k in range(n):
        c = order[k]
        r = rec[c]
        zc = np.float64(z[c])
        if r < 0 or kfac[c] <= 0.0:
            out[c] = z[c]
            continue
        zr = np.float64(out[r])
        if zc <= zr:
            # A flat/inversion the epsilon fill did not resolve; do not erode
            # into it, the receiver is already at or above us.
            out[c] = z[c]
            continue
        # Solve f(x) = x - zc + kfac * ((x - zr) / dist)^n = 0 on (zr, zc].
        # f(zr) = zr - zc < 0 and f(zc) > 0, f' >= 1, so the root exists, is
        # unique, and Newton from x = zc converges monotonically downward;
        # the half-step pullback guards the n < 1 derivative blowup at x = zr.
        x = zc
        dx = np.float64(dist[c])
        kf = np.float64(kfac[c])
        for _ in range(24):
            s = (x - zr) / dx
            if s < 0.0:
                s = 0.0
            f = x - zc + kf * s ** n_exp
            if s > 0.0:
                fp = 1.0 + kf * n_exp * (s ** (n_exp - 1.0)) / dx
            else:
                fp = 1.0
            step = f / fp
            x -= step
            if x < zr:
                x = 0.5 * ((x + step) + zr)
            if abs(step) < 1e-4:
                break
        if x < zr:
            x = zr
        if x > zc:
            x = zc
        if cap > 0.0 and zc - x > cap:
            x = zc - cap
        out[c] = x


def profile_incision(filled, receivers, acc, cell_m, *, K_dt: float = 4.5,
                     m: float = 0.45, n: float = 0.8, cap_m: float = 25.0,
                     a_crit_m2: float = A_CRIT_M2, gate_q: float = GATE_Q,
                     regional_slope=None, regional_s_ref: float = 0.2,
                     regional_p: float = 0.0,
                     erodibility=None,
                     sea_taper_top_m: float = SEA_TAPER_TOP_M,
                     sea_taper_bottom_m: float = SEA_TAPER_BOTTOM_M) -> np.ndarray:
    """Eroded surface in **metres** -- the implicit stream-power step.

    `filled`    the epsilon-filled surface the routing ran on (metres). It is
                both the surface being carved and the elevation the sea taper
                reads, which keeps the taper identical to `stream_power`'s.
    `receivers` D8 receiver as flat index `y*w + x`, -1 where none, i.e. the
                first return of `flow.d8_receivers(filled, cell_m)`.
    `acc`       MFD contributing area, m^2.
    `K_dt`      erosion number: K times the pass's pseudo-time. At small values
                this reproduces `stream_power`'s explicit depth to first order;
                as it grows, each channel approaches the steady-state graded
                profile `S = (relief-rate/K)^(1/n) * A^(-m/n)`. It is a new
                constant rather than `stream_K` because the two are only
                dimensionally comparable at small depths.
    `cap_m`     total-lowering bound, applied AFTER the solve. 0 disables --
                do not: K_dt = 15 uncapped measured 679 m of incision on the
                alpine exemplar. The structural guarantee z >= z_rcv makes a
                shaft impossible but not a canyon.
    `regional_p`  exponent of the regional-energy factor: ``min(1, S_reg /
                regional_s_ref) ** regional_p``. 0 (the default) falls back to
                ``n``, which reproduces the prior behaviour exactly. Values
                above ``n`` SHARPEN the class separation: at n = 0.8 a till
                plain (S_reg ~ 0.03, s_ref 0.2) keeps 22% of the erosion
                energy — enough that a dense channel network trenches it and
                triples its mean slope (measured: a_crit 625 m^2 took the
                plains exemplar 2.03 -> 3.59 deg at 1.875 m against a real
                2.13) — while at p = 2 it keeps 2.3%, and the same dense
                network carves sub-metre swales instead. Steep ground
                (S_reg >= s_ref) is unaffected at any p.
    `erodibility` OPTIONAL per-cell multiplier on the erodibility (same shape
                as `filled`, non-negative). This is the MATERIAL STRENGTH hook
                (bake_ver 6): the same repose field that keys thermal's
                threshold is mapped to a K multiplier, so hard strata resist
                the carve -- a stream crossing a strong band holds its bed
                (a knickpoint) while the weak band downstream cuts a tread.
                It multiplies `kfac` exactly where the gates and tapers do, so
                every structural guarantee of the solve (z >= z_rcv, cap,
                monotone network) is untouched: a multiplier of 0 simply means
                "this cell does not erode", which the a_crit gate already
                exercises everywhere below threshold. None disables and
                reproduces the prior surface bit-for-bit.
    `regional_slope` OPTIONAL 30 m-scale slope of the CARRIER (not the
                per-cell fine slope -- that variant double-counts S and is
                refuted, see the note below). When given, the erodibility is
                scaled by ``min(1, S_reg / regional_s_ref)^n``: erosion energy
                follows regional relief, which is the one piece of
                class-identity information the bake legitimately has. Why it
                is needed, measured on the exemplars: WITHOUT it, the solve
                has no uplift term, so its steady state is a peneplain at
                every A -- big-catchment trunks grade toward base level
                regardless of how gentle the landscape is, and a till plain
                whose whole relief is 200 m grew 12 m median channel trenches
                and 2.5x its real mean slope at every scale rung. WITH it
                (s_ref 0.2) the same K_dt leaves the plains exemplar's mean
                slope within 5-35% of the un-eroded surface (K_dt 1.5-4.5)
                while the alpine window keeps theta 0.146 (r^2 0.90).

    Returns float32, `filled.shape`; everywhere `<= filled`, `>= filled - cap_m`
    (when capped), and monotone along the receiver tree: `out[c] >= out[rec[c]]`
    wherever the receiver is strictly lower, so the carve introduces no new pit
    on its own network.
    """
    z = np.asarray(filled, dtype=np.float32)
    if z.ndim != 2:
        raise ValueError(f"filled must be 2-D, got shape {z.shape}")
    rec = np.asarray(receivers, dtype=np.int64).ravel()
    a = np.asarray(acc, dtype=np.float64)
    if a.shape != z.shape:
        raise ValueError(f"acc {a.shape} must match filled {z.shape}")
    if rec.size != z.size:
        raise ValueError(f"receivers has {rec.size} cells, filled {z.size}")
    if K_dt < 0.0:
        raise ValueError(f"K_dt must be non-negative, got {K_dt}")
    if cap_m < 0.0:
        raise ValueError(f"cap_m must be >= 0 (0 disables), got {cap_m}")
    if a_crit_m2 < 0.0:
        raise ValueError(f"a_crit_m2 must be non-negative, got {a_crit_m2}")
    if gate_q <= 0.0:
        raise ValueError(f"gate_q must be positive, got {gate_q}")
    if float(cell_m) <= 0.0:
        raise ValueError(f"cell_m must be positive, got {cell_m}")

    # K_dt * A^m, with the same channel-initiation gate and sea-level taper as
    # `stream_power` -- multiplied into the erodibility rather than the depth,
    # which for the gate is the same thing and for the taper keeps the solved
    # profile continuous across the shelf break.
    # NOTE deliberately no S^n factor here: the solve evaluates stream power
    # at the SOLVED slope -- `S_new^n` is inside the Newton residual -- so
    # multiplying the erodibility by the pre-carve slope as well would count S
    # twice. That variant was tried (it looked like "keep gentle ground
    # gentle") and measured: it collapses the concavity gain (alpine theta
    # 0.240 -> 0.077 at matched depth) because re-grading a profile is exactly
    # the case where the pre-carve slope is the wrong slope.
    #
    # Working arrays are float32/int32 on purpose -- see _profile_pass.
    af = np.clip(a, 0.0, None)
    kfac = (np.float64(K_dt) * np.power(af, np.float64(m)))
    if regional_p < 0.0:
        raise ValueError(f"regional_p must be >= 0 (0 = use n), got {regional_p}")
    if regional_slope is not None:
        sreg = np.asarray(regional_slope, dtype=np.float64)
        if sreg.shape != z.shape:
            raise ValueError(f"regional_slope {sreg.shape} must match filled {z.shape}")
        if regional_s_ref <= 0.0:
            raise ValueError(f"regional_s_ref must be positive, got {regional_s_ref}")
        p_exp = regional_p if regional_p > 0.0 else n
        kfac *= np.minimum(1.0, np.clip(sreg, 0.0, None) / np.float64(regional_s_ref)
                           ) ** np.float64(p_exp)
    if erodibility is not None:
        ero = np.asarray(erodibility, dtype=np.float64)
        if ero.shape != z.shape:
            raise ValueError(f"erodibility {ero.shape} must match filled {z.shape}")
        if float(ero.min()) < 0.0:
            raise ValueError("erodibility must be non-negative everywhere")
        kfac *= ero
    if a_crit_m2 > 0.0:
        aq = np.power(af, np.float64(gate_q))
        kfac *= aq / (aq + np.float64(a_crit_m2) ** np.float64(gate_q))
    if sea_taper_bottom_m < sea_taper_top_m:
        t = (z - np.float64(sea_taper_bottom_m)) / np.float64(
            sea_taper_top_m - sea_taper_bottom_m)
        t = np.clip(t, 0.0, 1.0)
        kfac *= t * t * (3.0 - 2.0 * t)
    kfac = kfac.astype(np.float32).ravel()

    h, w = z.shape
    if z.size > np.iinfo(np.int32).max:
        raise ValueError(f"domain of {z.size} cells exceeds the int32 order index")
    zf = np.ascontiguousarray(z).ravel()
    order = np.argsort(zf, kind="stable").astype(np.int32)
    rec32 = rec.astype(np.int32)
    idx = np.arange(rec.size, dtype=np.int64)
    tgt = np.where(rec >= 0, rec, idx)
    diag = ((np.abs(idx // w - tgt // w) > 0) & (np.abs(idx % w - tgt % w) > 0))
    dist = np.where(diag, float(cell_m) * _R2_DIST, float(cell_m)).astype(np.float32)
    del idx, tgt, diag, a, af
    # Seeded with the input, not empty: if a caller's fill ever produced a tie
    # in float32, the tied donor would read its receiver's UNSOLVED elevation
    # and take the no-erosion branch -- a safe degradation instead of a read
    # of uninitialised memory.
    out = zf.copy()
    _profile_pass(order, rec32, zf, kfac, dist, float(n), float(cap_m), out)
    return out.reshape(h, w)


_R2_DIST = 1.4142135623730951
