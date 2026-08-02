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


# Rebound to `numba.prange` / a jitted `_profile_cell` by `_bind_kernels` before
# any kernel here is compiled -- flow.py's pattern and for the same reason: numba
# resolves a jitted function's globals at COMPILE time, which is what lets the
# kernels below stay ordinary module-level source (so `cache=True` works and the
# python fallback is still the readable reference) while numba stays optional.
prange = range
_solve_cell = None

# Below this many cells a level is solved sequentially rather than in a parallel
# region. The level histogram on a production tile is a long tail of tiny levels
# (the headwater fringe of the deepest flow paths), and a numba parallel region
# costs a few microseconds to open whatever is inside it. The choice of threshold
# cannot change the RESULT -- every cell in a level is independent of every other
# cell in that level either way -- only how much of the tail is wasted.
_PAR_MIN_LEVEL = 4096


def _profile_cell(c, zr, z, kfac, dist, n_exp, cap, out):
    """Solve one cell against an already-final receiver elevation ``zr``.

    Solve f(x) = x - zc + kfac * ((x - zr) / dist)^n = 0 on (zr, zc].
    f(zr) = zr - zc < 0 and f(zc) > 0, f' >= 1, so the root exists, is
    unique, and Newton from x = zc converges monotonically downward;
    the half-step pullback guards the n < 1 derivative blowup at x = zr.

    Its own function so that the sequential pre-pass and the parallel level
    sweep run literally the same arithmetic rather than two copies of it --
    bit-identity is the whole point of the decomposition and a duplicated
    Newton would be the obvious way to lose it.
    """
    zc = np.float64(z[c])
    if zc <= zr:
        # A flat/inversion the epsilon fill did not resolve; do not erode
        # into it, the receiver is already at or above us.
        out[c] = z[c]
        return
    x = zc
    dx = np.float64(dist[c])
    kf = np.float64(kfac[c])
    for _ in range(24):
        s = (x - zr) / dx
        if s < 0.0:
            s = 0.0
        sn = s ** n_exp
        f = x - zc + kf * sn
        if s > 0.0:
            # `sn / s` in place of the algebraically equal `s ** (n_exp - 1)`,
            # which drops one libm `pow` from every Newton iteration -- worth
            # 1.38x on the parallel sweep at 12 threads, where the pow is the
            # bottleneck once the memory latency is hidden (it is worth nothing
            # single-threaded, where the latency hides the pow instead).
            #
            # It is NOT the same floating-point expression, and the identity is
            # not an accident of one tile: measured over 4 M random Newtons
            # spanning drops of 1e-4..1e2 m and kfac 1e-4..1e4, the two `fp`
            # values differ in 48.8% of cells, the CONVERGED float64 root then
            # differs in 0.002% of them, and by at most 1.4e-14 m -- because
            # the derivative only steers the path to a root that both forms
            # then bracket to full double precision. Against a float32 ULP of
            # ~1.2e-4 m at these elevations that is ten orders of magnitude of
            # headroom, so the stored float32 was identical in 0 of 4 M there
            # and in all 85 M cells of the production tile. If it ever stops
            # holding, the previous form is `n_exp * (s ** (n_exp - 1.0))`.
            fp = 1.0 + kf * n_exp * sn / (s * dx)
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


@_jit(cache=True)
def _profile_levels(order, rec, kfac):
    """Bucket the D8 forest into dependency levels. Cheap, integer, sequential.

    ``level[c] = level[rec[c]] + 1``, computed in ``order`` (ascending filled
    elevation), so the receiver's level is already known when a donor is
    reached. Bucket 0 is reserved for BACKWARD EDGES -- see below -- so the
    roots land in bucket 1 and a cell in bucket b reads only bucket b-1.

    Returns ``(starts, cells)``: ``cells[starts[b]:starts[b+1]]`` are the cells
    of bucket b, and within a bucket they keep their ``order`` position, which
    is what makes bucket 0 reproduce the sequential pass exactly.

    THE BACKWARD EDGE. The tree pass assumes ``rec[c]`` is strictly lower than
    ``c`` on the filled surface, so it precedes ``c`` in ``order``. A float32
    tie breaks that: `argsort(kind="stable")` then orders the pair by index, so
    a donor can be visited BEFORE its receiver. The sequential pass handles it
    silently -- it reads the SEED ``out[r] == z[r]``, which is >= z[c], so the
    cell takes the no-erosion branch -- but that is an ordering-dependent read,
    and it is the one place where a naive level decomposition would silently
    diverge (the receiver would by then be solved, hence LOWER, hence the donor
    would erode). Those cells are therefore parked in bucket 0 and solved
    sequentially in ``order`` position before any other bucket, which restores
    exactly the reads the sequential pass made.
    """
    n = order.size
    level = np.full(n, -1, dtype=np.int32)
    top = 0
    for k in range(n):
        c = order[k]
        r = rec[c]
        if r < 0 or kfac[c] <= 0.0:
            # Reads no receiver at all, so it depends on nothing: bucket 1.
            lv = 1
        else:
            lr = level[r]
            # lr < 0: the receiver has not been visited yet -- a backward edge.
            lv = 0 if lr < 0 else lr + 1
        level[c] = lv
        if lv > top:
            top = lv

    nb = top + 1
    starts = np.zeros(nb + 1, dtype=np.int64)
    for c in range(n):
        starts[level[c] + 1] += 1
    for b in range(nb):
        starts[b + 1] += starts[b]

    cells = np.empty(n, dtype=np.int32)
    cur = starts[:nb].copy()
    for k in range(n):
        c = order[k]
        b = level[c]
        cells[cur[b]] = c
        cur[b] += 1
    return starts, cells


@_jit(cache=True, parallel=True)
def _profile_sweep(starts, cells, rec, z, kfac, dist, n_exp, cap, out):
    """Solve the buckets: parallel WITHIN a level, sequential ACROSS levels.

    Bucket 0 (backward edges) is forced sequential -- its cells read the seed
    ``out[r]`` of a cell that comes LATER in ``order``, so they must run before
    anything is written, and in ``order`` position among themselves.
    """
    nb = starts.size - 1
    for b in range(nb):
        lo = starts[b]
        hi = starts[b + 1]
        if b > 0 and hi - lo >= _PAR_MIN_LEVEL:
            for i in prange(lo, hi):
                c = cells[i]
                r = rec[c]
                if r < 0 or kfac[c] <= 0.0:
                    out[c] = z[c]
                else:
                    _solve_cell(c, np.float64(out[r]), z, kfac, dist,
                                n_exp, cap, out)
        else:
            for i in range(lo, hi):
                c = cells[i]
                r = rec[c]
                if r < 0 or kfac[c] <= 0.0:
                    out[c] = z[c]
                else:
                    _solve_cell(c, np.float64(out[r]), z, kfac, dist,
                                n_exp, cap, out)


def _bind_kernels():
    """Bind the numba-visible globals, before anything below is compiled."""
    global prange, _solve_cell
    if _solve_cell is not None:
        return
    try:
        import numba
    except ImportError:  # pragma: no cover - the CI path
        _solve_cell = _profile_cell
        return
    prange = numba.prange
    _solve_cell = numba.njit(cache=True)(_profile_cell)


def _profile_pass(order, rec, z, kfac, dist, n_exp, cap, out):
    """One downstream-to-upstream Newton pass over the D8 tree.

    ``order`` is ascending filled elevation, so a receiver (strictly lower on
    the epsilon-filled surface) is always final before any of its donors is
    visited. That reads as strictly sequential, and the plain loop over
    ``order`` was -- but the D8 receiver graph is a FOREST, and a forest
    decomposes into dependency LEVELS: a cell's only non-local read is
    ``out[rec[c]]``, whose level is strictly lower and therefore already final.
    So the cells of one level are independent of each other, and the pass is
    parallel within a level and sequential across levels.

    That is BIT-IDENTICAL rather than approximately equal, and deliberately so:
    no cell's arithmetic changes, no reduction is reassociated, every read is
    of the same value the sequential pass read. The two things that could break
    it are handled explicitly -- backward edges (see ``_profile_levels``) and
    the shared Newton body (see ``_profile_cell``) -- and the property is
    checked against the sequential pass rather than argued for.

    Measured on the production tile (9216^2 padded, 85 M cells, 12 threads):
    the level pass plus counting sort costs ~5 s and the solve itself drops
    from ~30 s to ~4 s. It buys that with one extra int32 array of the domain
    (340 MB live through the sweep, 680 MB transiently while the levels are
    still being counted) -- the reason it is a counting sort into a compact
    ``cells`` array rather than a per-level list.

    The arrays are float32/int32 -- at the padded production domain every
    float64 working array is 680 MB against the pod's 8 GiB, and the peak
    already measures 5.5 GiB -- while the Newton itself runs in float64
    scalars. The 1e-4 m tolerance is 0.1 mm, one thousandth of the wire LSB.
    """
    _bind_kernels()
    starts, cells = _profile_levels(order, rec, kfac)
    _profile_sweep(starts, cells, rec, z, kfac, dist, n_exp, cap, out)


def _prepare_param(value, name, *, scale: int, cover, positive: bool = False):
    """Accept a scalar OR a coarse 2-D field for one elementwise parameter.

    Returns ``(scalar_or_None, coarse_field_or_None)``: exactly one is not None.

    THE COARSE FORM IS THE POINT (landform provinces, Tier 1). A per-cell
    parameter materialised at the padded fine domain is 340 MB inside the
    bake's peak stage, per parameter -- the same waste the ``regional``
    ``np.repeat`` was, and it is fixed the same way: keep the field at its
    native coarse pitch and gather ``coarse[y // scale, x // scale]`` in the
    row blocks below. The gather COPIES values rather than computing them, so a
    constant field reproduces the scalar path exactly.
    """
    if value is None:
        raise ValueError(f"{name} must not be None")
    arr = np.asarray(value)
    if arr.ndim == 0:
        v = float(arr)
        if positive and not v > 0.0:
            raise ValueError(f"{name} must be positive, got {v}")
        if not positive and v < 0.0:
            raise ValueError(f"{name} must be non-negative, got {v}")
        return v, None
    if arr.ndim != 2 or min(arr.shape) < 1:
        raise ValueError(
            f"{name} must be a scalar or a non-empty 2-D field, got shape "
            f"{arr.shape}")
    if scale < 1:
        raise ValueError(f"field_scale must be >= 1, got {scale}")
    if arr.shape[0] > cover[0] or arr.shape[1] > cover[1]:
        raise ValueError(
            f"{name} field {arr.shape} at field_scale={scale} is finer than the "
            f"domain allows (at most {cover})")
    if not np.isfinite(arr).all():
        raise ValueError(f"{name} field must be finite everywhere")
    lo = float(arr.min())
    if positive and not lo > 0.0:
        raise ValueError(f"{name} field must be positive everywhere, min was {lo}")
    if not positive and lo < 0.0:
        raise ValueError(
            f"{name} field must be non-negative everywhere, min was {lo}")
    return None, arr.astype(np.float64, copy=False)


def _gather_rows(scalar, coarse, rows, cols):
    """One row block of a parameter: the scalar, or the coarse gather.

    Rows first -- ``(nrows x cw)`` is a few MB -- and only then widened to the
    block's columns, the same order the ``regional_slope`` gather uses and for
    the same reason.
    """
    if coarse is None:
        return np.float64(scalar)
    return coarse[np.minimum(rows, coarse.shape[0] - 1)][:, cols]


def profile_incision(filled, receivers, acc, cell_m, *, K_dt=4.5,
                     m=0.45, n: float = 0.8, cap_m: float = 25.0,
                     a_crit_m2=A_CRIT_M2, gate_q=GATE_Q,
                     regional_slope=None, regional_s_ref: float = 0.2,
                     regional_p: float = 0.0,
                     regional_scale: int = 1,
                     field_scale: int = 1,
                     erodibility=None,
                     sea_taper_top_m: float = SEA_TAPER_TOP_M,
                     sea_taper_bottom_m: float = SEA_TAPER_BOTTOM_M,
                     order=None) -> np.ndarray:
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
    `regional_scale` COARSENING factor of `regional_slope`. 1 (the default)
                means it is a full-resolution field, as before. With ``f > 1``
                the field is read as ``regional_slope[min(y // f, ch - 1),
                min(x // f, cw - 1)]``, which is what expanding a ``ch x cw``
                field with two ``np.repeat``s and then edge-extending any
                remainder used to produce -- bit-for-bit, since the gather
                copies values rather than computing them. The caller's
                regional slope IS a block-constant 30 m-scale quantity, so at
                the padded production domain this keeps a 1.3 MB array instead
                of materialising 340 MB of duplicated float32 inside the
                bake's peak stage.
    `field_scale` COARSENING factor for any of ``K_dt``, ``m``, ``a_crit_m2``
                and ``gate_q`` that is supplied as a 2-D FIELD rather than a
                scalar, with exactly ``regional_scale``'s gather and clamp.
                This is the landform-province hook (bake_ver 7): the whole
                ``kfac`` chain below is elementwise, so making these per-cell
                is arithmetic, not a kernel change -- and it is verified as
                such, since a per-cell ``K_dt`` is identical to folding the
                same field into ``erodibility`` (``tests/test_province.py``).

                ``n`` and ``cap_m`` are deliberately NOT in that list. They are
                scalars *inside* the numba Newton kernel (``_profile_cell``),
                so making them per-cell is a real kernel change rather than an
                array multiply, and it is out of Tier 1's scope.

    `order`     OPTIONAL ascending-elevation ``argsort`` of `filled`, flat
                indices, exactly ``flow.accumulate_mfd(..., return_order=True)``'s
                second return. The MFD sweep in B2c already sorts this same
                array, and sorting it again here cost 13.6 s and a 680 MB
                int64 transient at 9216^2. Ties may be ordered any way: a
                receiver is STRICTLY lower than its donor on the filled
                surface, so a receiver precedes its donors in every ascending
                order, and two equal-elevation cells are never in a
                receiver relationship and never read each other. None
                recomputes it.

    Returns float32, `filled.shape`; everywhere `<= filled`, `>= filled - cap_m`
    (when capped), and monotone along the receiver tree: `out[c] >= out[rec[c]]`
    wherever the receiver is strictly lower, so the carve introduces no new pit
    on its own network.
    """
    z = np.asarray(filled, dtype=np.float32)
    if z.ndim != 2:
        raise ValueError(f"filled must be 2-D, got shape {z.shape}")
    if z.size > np.iinfo(np.int32).max:
        raise ValueError(f"domain of {z.size} cells exceeds the int32 order index")
    # int32 flat indices: the domain is bounded above, `_profile_pass` takes
    # them as int32 anyway, and this is where the extra `.astype(np.int32)`
    # copy of a 680 MB int64 array used to happen.
    rec32 = np.ascontiguousarray(receivers, dtype=np.int32).ravel()
    a = np.asarray(acc, dtype=np.float64)
    if a.shape != z.shape:
        raise ValueError(f"acc {a.shape} must match filled {z.shape}")
    if rec32.size != z.size:
        raise ValueError(f"receivers has {rec32.size} cells, filled {z.size}")
    if cap_m < 0.0:
        raise ValueError(f"cap_m must be >= 0 (0 disables), got {cap_m}")
    if float(cell_m) <= 0.0:
        raise ValueError(f"cell_m must be positive, got {cell_m}")
    fscale = int(field_scale)
    _cover = (-(-z.shape[0] // max(fscale, 1)), -(-z.shape[1] // max(fscale, 1)))
    # Scalar OR coarse field, per parameter. The scalar branches raise exactly
    # what they raised before; the field branches add "everywhere" to the same
    # sentence. a_crit and gate_q are POSITIVE-only as fields (a scalar
    # a_crit_m2 of 0 still means "no gate at all" and skips the block below,
    # but a field of zeros would be a 0/0 at every cell with no upslope area).
    K_dt_s, K_dt_f = _prepare_param(K_dt, "K_dt", scale=fscale, cover=_cover)
    m_s, m_f = _prepare_param(m, "m", scale=fscale, cover=_cover)
    a_crit_s, a_crit_f = _prepare_param(
        a_crit_m2, "a_crit_m2", scale=fscale, cover=_cover,
        positive=a_crit_m2 is not None and np.ndim(a_crit_m2) > 0)
    gate_q_s, gate_q_f = _prepare_param(
        gate_q, "gate_q", scale=fscale, cover=_cover, positive=True)

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
    #
    # CHUNKED over row blocks (2026-07-30, bake pod budget): the unchunked
    # chain held five to seven full-domain float64 temporaries at once, which
    # at a production 9216^2 measured as THE bake's working-set peak
    # (11.4 GiB against the 8 GiB pod, with the pipeline's own live grids
    # underneath). Every op here is elementwise, the arithmetic per cell is
    # unchanged (float64 throughout, one final cast to float32 -- exactly what
    # `.astype(np.float32)` did at the end before), so the result is
    # bit-identical and independent of the block layout.
    if regional_p < 0.0:
        raise ValueError(f"regional_p must be >= 0 (0 = use n), got {regional_p}")
    p_exp = regional_p if regional_p > 0.0 else n
    rscale = int(regional_scale)
    if rscale < 1:
        raise ValueError(f"regional_scale must be >= 1, got {regional_scale}")
    sreg = None
    if regional_slope is not None:
        sreg = np.asarray(regional_slope)
        if rscale == 1:
            if sreg.shape != z.shape:
                raise ValueError(
                    f"regional_slope {sreg.shape} must match filled {z.shape}")
        else:
            if sreg.ndim != 2 or min(sreg.shape) < 1:
                raise ValueError(
                    f"regional_slope must be a non-empty 2-D coarse field, got "
                    f"shape {sreg.shape}")
            cover = (-(-z.shape[0] // rscale), -(-z.shape[1] // rscale))
            if sreg.shape[0] > cover[0] or sreg.shape[1] > cover[1]:
                raise ValueError(
                    f"regional_slope {sreg.shape} at regional_scale={rscale} is "
                    f"finer than filled {z.shape} allows (at most {cover})")
        if regional_s_ref <= 0.0:
            raise ValueError(f"regional_s_ref must be positive, got {regional_s_ref}")
    ero = None
    if erodibility is not None:
        ero = np.asarray(erodibility)
        if ero.shape != z.shape:
            raise ValueError(f"erodibility {ero.shape} must match filled {z.shape}")
        if float(ero.min()) < 0.0:
            raise ValueError("erodibility must be non-negative everywhere")

    h, w = z.shape
    # Column gather for a COARSE regional field, hoisted out of the row loop.
    # Clamped, which is what the caller's old edge-extension of the leftover
    # rows/columns amounted to when the domain was not a multiple of `rscale`.
    sreg_cols = None
    if sreg is not None and rscale > 1:
        sreg_cols = np.minimum(np.arange(w) // rscale, sreg.shape[1] - 1)
    # Same hoist for the province parameter fields (landform provinces, Tier 1).
    # They share `field_scale` by construction, so each needs only its own
    # column clamp against its own width.
    pf_cols = {
        nm: np.minimum(np.arange(w) // fscale, fld.shape[1] - 1)
        for nm, fld in (("K_dt", K_dt_f), ("m", m_f),
                        ("a_crit", a_crit_f), ("gate_q", gate_q_f))
        if fld is not None
    }
    kfac32 = np.empty((h, w), dtype=np.float32)
    for r0 in range(0, h, 512):
        r1 = min(r0 + 512, h)
        af_b = np.clip(a[r0:r1], 0.0, None)
        pf_rows = np.arange(r0, r1) // fscale
        K_b = _gather_rows(K_dt_s, K_dt_f, pf_rows, pf_cols.get("K_dt"))
        m_b = _gather_rows(m_s, m_f, pf_rows, pf_cols.get("m"))
        k_b = K_b * np.power(af_b, m_b)
        del K_b, m_b
        if sreg is not None:
            if sreg_cols is None:
                s_b = sreg[r0:r1]
            else:
                # Rows first: (nrows x cw) is a few MB, and only then widened.
                rows = np.minimum(np.arange(r0, r1) // rscale, sreg.shape[0] - 1)
                s_b = sreg[rows][:, sreg_cols]
            k_b *= np.minimum(1.0, np.clip(s_b.astype(np.float64, copy=False),
                                           0.0, None) / np.float64(regional_s_ref)
                              ) ** np.float64(p_exp)
            del s_b
        if ero is not None:
            k_b *= ero[r0:r1].astype(np.float64, copy=False)
        if a_crit_f is not None or a_crit_s > 0.0:
            q_b = _gather_rows(gate_q_s, gate_q_f, pf_rows, pf_cols.get("gate_q"))
            ac_b = _gather_rows(a_crit_s, a_crit_f, pf_rows, pf_cols.get("a_crit"))
            aq = np.power(af_b, q_b)
            k_b *= aq / (aq + np.power(ac_b, q_b))
            del q_b, ac_b
        if sea_taper_bottom_m < sea_taper_top_m:
            t = (z[r0:r1] - np.float64(sea_taper_bottom_m)) / np.float64(
                sea_taper_top_m - sea_taper_bottom_m)
            t = np.clip(t, 0.0, 1.0)
            k_b *= t * t * (3.0 - 2.0 * t)
        kfac32[r0:r1] = k_b
    kfac = kfac32.ravel()
    del kfac32
    zf = np.ascontiguousarray(z).ravel()
    if order is None:
        order = np.argsort(zf, kind="stable").astype(np.int32)
    else:
        order = np.ascontiguousarray(order, dtype=np.int32).ravel()
        if order.size != zf.size:
            raise ValueError(
                f"order has {order.size} entries, filled {zf.size}")
    # Receiver distances, chunked for the same pod-budget reason as kfac above:
    # the flat idx/tgt/diag form held two full-domain int64 temporaries (1.3 GiB
    # at production scale) for one float32 result. Elementwise, so bit-identical
    # and block-layout independent.
    #
    # And no division: a D8 receiver is one of the eight neighbours, so with
    # d = |rec[c] - c| the step is CARDINAL iff d is 1 or w and DIAGONAL iff it
    # is w-1 or w+1. Those four are distinct for w >= 3, which is the only case
    # the decomposition is unique in, so the `//`/`%` form stays as the general
    # fallback. (The array is only ever READ at cells with a receiver -- the
    # solve takes the no-erosion branch at rec < 0 before it looks -- but the
    # `rec >= 0` mask is kept so the array means what its name says.)
    dist = np.empty(rec32.size, dtype=np.float32)
    _CARD = np.float32(float(cell_m))
    _DIAG = np.float32(float(cell_m) * _R2_DIST)
    for c0 in range(0, rec32.size, 512 * 16384):
        c1 = min(c0 + 512 * 16384, rec32.size)
        if w >= 3:
            d = rec32[c0:c1] - np.arange(c0, c1, dtype=np.int32)
            np.abs(d, out=d)
            diag = (d == w - 1) | (d == w + 1)
            diag &= rec32[c0:c1] >= 0
            dist[c0:c1] = np.where(diag, _DIAG, _CARD)
            del d, diag
        else:
            idx = np.arange(c0, c1, dtype=np.int64)
            tgt = np.where(rec32[c0:c1] >= 0, rec32[c0:c1], idx)
            diag = ((np.abs(idx // w - tgt // w) > 0)
                    & (np.abs(idx % w - tgt % w) > 0))
            dist[c0:c1] = np.where(diag, float(cell_m) * _R2_DIST, float(cell_m))
            del idx, tgt, diag
    del a
    # Seeded with the input, not empty: if a caller's fill ever produced a tie
    # in float32, the tied donor would read its receiver's UNSOLVED elevation
    # and take the no-erosion branch -- a safe degradation instead of a read
    # of uninitialised memory.
    out = zf.copy()
    _profile_pass(order, rec32, zf, kfac, dist, float(n), float(cap_m), out)
    return out.reshape(h, w)


_R2_DIST = 1.4142135623730951
