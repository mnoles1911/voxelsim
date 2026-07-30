"""B3 — slope-limited thermal relaxation (talus, footslopes, gully-wall weathering).

Runs *after* incision so that gully walls weather and the spoil forms talus cones and fan
aprons. It is also a low-pass on the residual, so it directly improves compression.

Two properties are load-bearing and both were learned by getting them wrong:

1. **It moves material, it does not delete it.** The obvious formulation — subtract each
   cell's over-repose excess — stripped 128 m off cliff tops in 48 iterations on a real
   tile, because a 30 m drop across one 3.75 m post is ~10x the repose limit and nothing
   put the debris anywhere. Mass conservation is asserted to float tolerance in
   `tests/test_bake_geomorph.py`; it is not a nice-to-have.

2. **The shed is scaled by the STEEPEST over-repose pair, not the sum over eight
   neighbours.** Scaling by the sum let a spike with eight 20 m-lower neighbours shed 48 m
   in a single step, overshoot far below them, and diverge the whole field to ~1e23 in 48
   iterations. Capping by the steepest pair means that pair can be driven *to* repose but
   never *through* it for any `rate <= 0.5`.

Structure is two passes (compute excess, then gather) so it stays parallel-safe: no cell
writes to a neighbour, every cell only reads. numba accelerates it when available; the
numpy path is the reference implementation and the two are checked against each other.

**Per-cell repose (2026-07-30).** ``repose_deg`` may be a 2-D array of degrees, one per
cell — a material-strength field. A single global angle is what machines the terrain:
measured on the g35 exemplar (tile -5,2), a third of a 3.8 km mountainside finishes
within ±10% of tan(36°), because every face that thermal touches is driven toward the
SAME slope, and a constant-slope face voxelises to evenly spaced parallel contour
terraces (docs/measurements/contour-crookedness-2026-07-30.txt). With a field, each
face relaxes toward its OWN angle: weak bands ravel back to gentle talus aprons, strong
bands hold near-cliff faces and keep the relief incision carved into them — which is
bench-and-riser structure, the thing a smooth ramp lacks.

The threshold is keyed on the DONOR cell (the higher cell of the pair) in both passes,
which is what keeps the two passes describing the same set of moves and therefore keeps
mass conservation exact: material leaves a cell only by its own strength, and the gather
pass recomputes exactly the donor-keyed excess the shed pass used. The stability bound is
unchanged per pair (a donor can be driven *to its own* repose, never through it, for any
``rate <= 0.5``).
"""

from __future__ import annotations

import math

import numpy as np

__all__ = ["relax", "excess_over_repose", "max_slope"]

# (dy, dx, distance in cells). All eight neighbours, cardinals first.
_NEIGHBOURS = (
    (-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0),
    (-1, -1, math.sqrt(2.0)), (-1, 1, math.sqrt(2.0)),
    (1, -1, math.sqrt(2.0)), (1, 1, math.sqrt(2.0)),
)


def _numba():
    """Return numba's njit/prange, or (None, None) if numba is not installed.

    numba is a bake-pod dependency and deliberately absent from
    terrain-service/requirements.txt, so importing this module must not require it.
    """
    try:
        from numba import njit, prange
    except ImportError:  # pragma: no cover - the CI path
        return None, None
    return njit, prange


def _pair_slices(dy: int, dx: int, shape):
    """Index slices for every in-bounds ordered pair `(c, c+(dy,dx))`.

    Only fully in-bounds pairs participate, in BOTH passes. That symmetry is what makes
    mass conservation exact at the domain border: every gram a cell sheds is gathered by a
    cell that is also inside the array. The prototype updated only `1..h-2` and gathered
    only over the same interior, so material shed towards the border row was silently
    destroyed — small, but it makes the conservation invariant untestable.
    """
    h, w = shape
    cy = slice(max(0, -dy), h - max(0, dy))
    ny = slice(max(0, dy), h - max(0, -dy))
    cx = slice(max(0, -dx), w - max(0, dx))
    nx = slice(max(0, dx), w - max(0, -dx))
    return (cy, cx), (ny, nx)


def _max_drops(cell_m: float, repose_deg: float, dtype) -> tuple:
    """Per-direction repose drop.

    PER-DIRECTION, not one number for all eight. The prototype used a single
    `tan(repose) * cell_m` for cardinals and diagonals alike, which holds a diagonal pair
    to the same *height* drop over a `sqrt(2)` longer run — i.e. to a 41% gentler slope.
    That is the classic eight-neighbour talus artifact: cones come out octagonal, with the
    diagonals visibly flatter than the cardinals. Scaling the drop by the pair distance
    costs nothing and removes it.
    """
    t = math.tan(math.radians(float(repose_deg)))
    return tuple(dtype(t * dist * cell_m) for _, _, dist in _NEIGHBOURS)


def _work_dtype(z: np.ndarray):
    return np.float64 if z.dtype == np.float64 else np.float32


def _repose_tan_field(repose_deg: np.ndarray, cell_m: float, shape, dtype) -> np.ndarray:
    """Per-cell ``tan(repose) * cell_m`` (the CARDINAL drop), validated.

    The per-direction drop for a pair at distance ``dist`` cells is this times
    ``dist`` — same per-direction scaling as ``_max_drops``, so diagonal pairs
    are held to the same *slope*, not the same height drop.
    """
    r = np.asarray(repose_deg)
    if r.shape != shape:
        raise ValueError(
            f"repose_deg field shape {r.shape} does not match z {shape}"
        )
    lo = float(np.min(r))
    hi = float(np.max(r))
    if not (0.0 < lo and hi < 85.0):
        raise ValueError(
            f"repose_deg field range [{lo}, {hi}] outside (0, 85) degrees"
        )
    return (np.tan(np.radians(r.astype(np.float64))) * float(cell_m)).astype(dtype)


def excess_over_repose(z: np.ndarray, cell_m: float, repose_deg: float = 36.0) -> np.ndarray:
    """Per-cell max over-repose drop, in metres. Zero everywhere means "at or below repose"."""
    a = np.asarray(z)
    dt = _work_dtype(a)
    a = a.astype(dt, copy=False)
    md = _max_drops(cell_m, repose_deg, dt)
    mx = np.zeros(a.shape, dtype=dt)
    for (dy, dx, _), drop in zip(_NEIGHBOURS, md):
        c, n = _pair_slices(dy, dx, a.shape)
        d = a[c] - a[n] - drop
        np.clip(d, 0.0, None, out=d)
        np.maximum(mx[c], d, out=mx[c])
    return mx


def max_slope(z: np.ndarray, cell_m: float) -> float:
    """Steepest eight-neighbour slope (rise/run) anywhere in `z`."""
    a = np.asarray(z, dtype=np.float64)
    best = 0.0
    for dy, dx, dist in _NEIGHBOURS:
        c, n = _pair_slices(dy, dx, a.shape)
        best = max(best, float(np.max(np.abs(a[c] - a[n]))) / (dist * cell_m))
    return best


# --------------------------------------------------------------------------------------
# reference implementation
# --------------------------------------------------------------------------------------

def _relax_numpy(z: np.ndarray, max_drops, iters: int, rate) -> np.ndarray:
    dt = z.dtype
    out = z
    shape = z.shape
    for _ in range(iters):
        exc = np.zeros(shape, dtype=dt)     # sum of over-repose drops -> share weights
        mx = np.zeros(shape, dtype=dt)      # steepest over-repose drop -> shed budget
        for (dy, dx, _), drop in zip(_NEIGHBOURS, max_drops):
            c, n = _pair_slices(dy, dx, shape)
            e = out[c] - out[n] - drop
            np.clip(e, 0.0, None, out=e)
            exc[c] += e
            np.maximum(mx[c], e, out=mx[c])

        shed = mx * rate
        gain = np.zeros(shape, dtype=dt)
        for (dy, dx, _), drop in zip(_NEIGHBOURS, max_drops):
            # `n` is the DONOR here (higher), `c` the receiver. Iterating all eight offsets
            # covers every ordered pair exactly once in each direction.
            c, n = _pair_slices(dy, dx, shape)
            e = out[n] - out[c] - drop
            np.clip(e, 0.0, None, out=e)
            en = exc[n]
            share = np.divide(e, en, out=np.zeros_like(e), where=en > 0)
            gain[c] += share * shed[n]

        out = out - shed + gain
    return out


def _relax_numpy_field(z: np.ndarray, tcell: np.ndarray, iters: int, rate) -> np.ndarray:
    """Reference implementation of the per-cell-repose variant.

    Identical structure to ``_relax_numpy``; the only change is that a pair's
    threshold is the DONOR's ``tcell * dist`` instead of a constant. Keying on
    the donor in BOTH passes is what keeps the gather pass reconstructing the
    exact excesses the shed pass summed, hence mass conservation.
    """
    dt = z.dtype
    out = z
    shape = z.shape
    dists = tuple(dt.type(d[2]) for d in _NEIGHBOURS)
    for _ in range(iters):
        exc = np.zeros(shape, dtype=dt)
        mx = np.zeros(shape, dtype=dt)
        for (dy, dx, _), dist in zip(_NEIGHBOURS, dists):
            c, n = _pair_slices(dy, dx, shape)
            e = out[c] - out[n] - tcell[c] * dist
            np.clip(e, 0.0, None, out=e)
            exc[c] += e
            np.maximum(mx[c], e, out=mx[c])

        shed = mx * rate
        gain = np.zeros(shape, dtype=dt)
        for (dy, dx, _), dist in zip(_NEIGHBOURS, dists):
            c, n = _pair_slices(dy, dx, shape)
            e = out[n] - out[c] - tcell[n] * dist
            np.clip(e, 0.0, None, out=e)
            en = exc[n]
            share = np.divide(e, en, out=np.zeros_like(e), where=en > 0)
            gain[c] += share * shed[n]

        out = out - shed + gain
    return out


# --------------------------------------------------------------------------------------
# numba implementation (same algorithm, no full-size temporaries)
# --------------------------------------------------------------------------------------

_njit_step = None


def _build_numba_step():
    global _njit_step
    if _njit_step is not None:
        return _njit_step
    njit, prange = _numba()
    if njit is None:
        return None

    @njit(cache=True, parallel=True, fastmath=False)
    def _step(z, out, exc, mx, dy_a, dx_a, drop_a, rate):
        h, w = z.shape
        for y in prange(h):
            for x in range(w):
                zc = z[y, x]
                s = 0.0
                m = 0.0
                for k in range(8):
                    ny = y + dy_a[k]
                    nx = x + dx_a[k]
                    if ny < 0 or nx < 0 or ny >= h or nx >= w:
                        continue
                    e = zc - z[ny, nx] - drop_a[k]
                    if e > 0.0:
                        s += e
                        if e > m:
                            m = e
                exc[y, x] = s
                mx[y, x] = m
        for y in prange(h):
            for x in range(w):
                zc = z[y, x]
                gain = 0.0
                for k in range(8):
                    ny = y + dy_a[k]
                    nx = x + dx_a[k]
                    if ny < 0 or nx < 0 or ny >= h or nx >= w:
                        continue
                    en = exc[ny, nx]
                    if en <= 0.0:
                        continue
                    e = z[ny, nx] - zc - drop_a[k]
                    if e > 0.0:
                        gain += (e / en) * (mx[ny, nx] * rate)
                out[y, x] = zc - mx[y, x] * rate + gain

    _njit_step = _step
    return _step


_njit_step_field = None


def _build_numba_step_field():
    global _njit_step_field
    if _njit_step_field is not None:
        return _njit_step_field
    njit, prange = _numba()
    if njit is None:
        return None

    @njit(cache=True, parallel=True, fastmath=False)
    def _step(z, out, exc, mx, tcell, dy_a, dx_a, dist_a, rate):
        h, w = z.shape
        for y in prange(h):
            for x in range(w):
                zc = z[y, x]
                tc = tcell[y, x]
                s = 0.0
                m = 0.0
                for k in range(8):
                    ny = y + dy_a[k]
                    nx = x + dx_a[k]
                    if ny < 0 or nx < 0 or ny >= h or nx >= w:
                        continue
                    e = zc - z[ny, nx] - tc * dist_a[k]
                    if e > 0.0:
                        s += e
                        if e > m:
                            m = e
                exc[y, x] = s
                mx[y, x] = m
        for y in prange(h):
            for x in range(w):
                zc = z[y, x]
                gain = 0.0
                for k in range(8):
                    ny = y + dy_a[k]
                    nx = x + dx_a[k]
                    if ny < 0 or nx < 0 or ny >= h or nx >= w:
                        continue
                    en = exc[ny, nx]
                    if en <= 0.0:
                        continue
                    # Donor-keyed threshold: the DONOR's own strength decides
                    # what it sheds, in both passes.
                    e = z[ny, nx] - zc - tcell[ny, nx] * dist_a[k]
                    if e > 0.0:
                        gain += (e / en) * (mx[ny, nx] * rate)
                out[y, x] = zc - mx[y, x] * rate + gain

    _njit_step_field = _step
    return _step


def _relax_numba_field(z: np.ndarray, tcell: np.ndarray, iters: int, rate) -> np.ndarray:
    step = _build_numba_step_field()
    dt = z.dtype
    dy_a = np.array([d[0] for d in _NEIGHBOURS], dtype=np.int64)
    dx_a = np.array([d[1] for d in _NEIGHBOURS], dtype=np.int64)
    dist_a = np.array([d[2] for d in _NEIGHBOURS], dtype=dt)
    a = z.copy()
    b = np.empty_like(a)
    exc = np.empty_like(a)
    mx = np.empty_like(a)
    for _ in range(iters):
        step(a, b, exc, mx, tcell, dy_a, dx_a, dist_a, dt.type(rate))
        a, b = b, a
    return a


def _relax_numba(z: np.ndarray, max_drops, iters: int, rate) -> np.ndarray:
    step = _build_numba_step()
    dt = z.dtype
    dy_a = np.array([d[0] for d in _NEIGHBOURS], dtype=np.int64)
    dx_a = np.array([d[1] for d in _NEIGHBOURS], dtype=np.int64)
    drop_a = np.array(max_drops, dtype=dt)
    a = z.copy()
    b = np.empty_like(a)
    exc = np.empty_like(a)
    mx = np.empty_like(a)
    for _ in range(iters):
        step(a, b, exc, mx, dy_a, dx_a, drop_a, dt.type(rate))
        a, b = b, a
    return a


# --------------------------------------------------------------------------------------

def relax(z: np.ndarray, cell_m: float, repose_deg=36.0, iters: int = 48,
          rate: float = 0.4) -> np.ndarray:
    """Slope-limited, mass-conserving thermal relaxation. Returns a new array.

    ``repose_deg`` is either a scalar (one global angle — the historical
    behaviour, bit-identical code path) or a 2-D array of per-cell degrees (a
    material-strength field; see the module docstring). The field variant keys
    each pair's threshold on the donor cell.

    `rate` must stay `<= 0.5`: the stability argument is that a cell sheds `rate` times its
    *steepest* over-repose drop, so that pair can be driven to repose but not through it.
    Above 0.5 the guarantee is gone and the field can oscillate.

    Note the algorithm bounds what a cell *gives*, not what it *receives*. A cell with many
    over-repose neighbours all shedding into it (an isolated one-cell pit is the extreme
    case) can be overfilled in a single step. See the pit note in
    `tests/test_bake_geomorph.py` — it converges rather than diverges at the default rate,
    but it is the sharp edge of this scheme and worth knowing about before raising `rate`.
    """
    a = np.asarray(z)
    if a.ndim != 2:
        raise ValueError(f"relax expects a 2-D array, got shape {a.shape}")
    if float(cell_m) <= 0.0:
        raise ValueError(f"cell_m must be positive, got {cell_m}")
    if not 0.0 <= float(rate) <= 0.5:
        raise ValueError(
            f"rate must be in [0, 0.5] for the steepest-pair stability bound, got {rate}"
        )
    iters = int(iters)
    if iters < 0:
        raise ValueError(f"iters must be non-negative, got {iters}")

    dt = _work_dtype(a)
    a = a.astype(dt, copy=True)
    if iters == 0:
        return a
    rate_v = dt(rate)
    njit, _ = _numba()

    if np.ndim(repose_deg) == 2:
        tcell = _repose_tan_field(repose_deg, float(cell_m), a.shape, dt)
        if njit is not None and a.size >= 4096:
            return _relax_numba_field(a, tcell, iters, rate_v)
        return _relax_numpy_field(a, tcell, iters, rate_v)
    if np.ndim(repose_deg) != 0:
        raise ValueError(
            f"repose_deg must be a scalar or a 2-D field, got ndim {np.ndim(repose_deg)}"
        )

    max_drops = _max_drops(float(cell_m), float(repose_deg), dt)

    if njit is not None and a.size >= 4096:
        return _relax_numba(a, max_drops, iters, rate_v)
    return _relax_numpy(a, max_drops, iters, rate_v)
