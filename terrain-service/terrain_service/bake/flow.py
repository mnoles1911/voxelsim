"""B2 flow routing: depression fill, D8 receivers, MFD flow accumulation.

Three primitives, in the order the bake uses them::

    filled     = fill_depressions(z)
    rec, slope = d8_receivers(filled, cell_m)          # centrelines, and S for K*A^m*S^n
    area       = accumulate_mfd(filled, cell_m, inflow=upstream)   # the area field, m^2

Target size is 8192x8192 at 1.875 m/px (67.1 M cells) per `docs/vxtl-v2-format.md`,
plus the 256-px apron of `docs/terrain-amplification-plan.md`, so every hot loop is
compiled.

Cost at that size, in **CPU-seconds** (`time.process_time`; wall-clock on this box is
contended and would be a fiction): `fill_depressions` 12.5, `d8_receivers` 2.2 (the
one parallel kernel, so its wall time is lower again), `accumulate_mfd` 15.8, of which
~4.5 is the `argsort`. ~30 CPU-s of the plan's ~165 CPU-s/tile bake budget, and about
2.4 GB of transient arrays. There are no timing assertions in the tests -- a threshold
on this box would be a flaky test, not a gate.

Four things this module exists to get right, each of them learned the hard way in
`terrain-service/tools/bake_prototype.py`:

**MFD for the area field, D8 for centrelines.** Pure D8 -- send everything to the
single steepest neighbour -- can only route along 8 compass directions, and at this
resolution that shows up in a hillshade as dead-straight 45-degree channels tens of
pixels long. It also fragments the network: switching the prototype to MFD tripled
the largest catchment it found (10.2 -> 78 km2). D8 is still exactly right for
tracing a channel centreline and for the stream-power slope term, which is why
`d8_receivers` is a public primitive rather than an implementation detail.

**Priority-flood with a real binary heap.** The reference `d8_flow` /
`flow_accumulation` in `terrain-diffusion/terrain_diffusion/inference/postprocessing.py`
have the right algorithms but a Python `heapq` loop, which does not survive 67 M cells.

**Filled depressions must not be left flat.** A plain fill replaces every pit with a
level lake, and on a level lake no cell has a lower neighbour, so both D8 and MFD
simply stop there: every filled dimple silently swallows its own catchment and
everything upstream of it. That is a correctness bug, not a cosmetic one -- it is the
same fragmentation MFD was adopted to fix, reintroduced by the fill.

Measured on tile `-6_3` of the shipped set, using the prototype's own B0+B1 output at
its own 3.75 m/px: the plain fill raises only 2.0% of cells, but those flats leave
341,368 inland dead-ends and strand **69% of the domain's area** before it reaches an
edge. Largest catchment 69.6 km2 and 13,256 cells above 1 km2; with the epsilon fill
below, on the same surface, **203.1 km2 and 107,496** -- 2.9x and 8.1x. (That also
means the drainage statistics quoted for the prototype are low by about 3x, and that
`incise.py`'s K calibration inherits the same bias through `A^m`.)

`fill_depressions` therefore uses the epsilon variant of priority-flood (Barnes,
Lehman & Mulla 2014, "Priority-Flood"): each newly discovered cell is raised to at
least `spill + flat_eps`, so every cell except the domain border has a strictly lower
neighbour and every drop of water reaches the border. `flat_eps` defaults to two ULPs
of the data's own magnitude -- 2.4e-4 m at 3 km elevation in float32, well under the
100 mm wire LSB; see `_auto_eps` for the bound on the accumulated staircase.

**Accumulate in float64.** Not because float32 is measurably wrong -- on that same
tile, whose largest catchment is 203 km2, a float32 accumulator agrees to within
2 ppm everywhere. It is that the conservation invariant is the cheapest complete
check this module has, and in float64 it holds to 1e-12, which is a test, whereas in
float32 it holds to ~1e-5, which is a tolerance nobody can reason about. The cost is
one 537 MB array at full tile size and no CPU: the weights are still evaluated in the
input's own dtype (`powf` for float32), only the shares and the running totals are
float64.

Numba is imported *inside* `_compile`, never at module scope: CI runs the
terrain-service tests without numba or scipy, and an import-time failure here would
break the whole job instead of skipping a few tests.

Known limitation, for the pipeline/apron owner: the epsilon staircase across a flat
runs outward from whichever domain border the flood reached it from, so a flat wider
than the apron can be *crossed* in different directions by two neighbouring bakes.
The height effect is sub-ULP-per-cell and invisible after quantisation; the routing
effect is not, so genuinely tile-spanning water bodies want a real lake/outlet model
rather than an epsilon ramp.
"""

from __future__ import annotations

import functools

import numpy as np

__all__ = [
    "fill_depressions",
    "d8_receivers",
    "accumulate_mfd",
    "enforce_descent",
]

# Rebound to `numba.prange` by `_compile` before any kernel is compiled; numba
# resolves a jitted function's globals at compile time, which is what lets the
# kernels below stay ordinary module-level source (so `cache=True` works and they
# can be read/debugged in pure Python) while numba stays an optional import.
prange = range

# N, S, W, E, NW, NE, SW, SE.
_DY = (-1, 1, 0, 0, -1, -1, 1, 1)
_DX = (0, 0, -1, 1, -1, 1, -1, 1)
_R2 = 1.4142135623730951
_DIST = (1.0, 1.0, 1.0, 1.0, _R2, _R2, _R2, _R2)

# Heap payloads are int32 flat indices; 8192^2 + apron is 76 M, far inside int32,
# and the halved footprint is 268 MB saved at full size.
_MAX_CELLS = 2**31 - 1


def _compile(pyfunc, options):
    global prange
    import numba

    prange = numba.prange
    return numba.njit(**options)(pyfunc)


def _jit(**options):
    """`numba.njit`, deferred to first call so importing this module needs no numba."""

    def decorate(pyfunc):
        state = {}

        @functools.wraps(pyfunc)
        def dispatch(*args):
            fn = state.get("fn")
            if fn is None:
                fn = state["fn"] = _compile(pyfunc, options)
            return fn(*args)

        dispatch.py_func = pyfunc
        return dispatch

    return decorate


# --------------------------------------------------------------------------- kernels
@_jit(cache=True)
def _priority_flood(z, eps, seed):
    """Barnes et al. priority-flood with an epsilon gradient.

    `seed` is the border cells' flat indices, ascending by elevation -- an ascending
    array is already a valid min-heap, which is why seeding is a straight copy here
    and the sift-up appears only once.

    Sequential by nature (cpu == wall in the prototype's per-stage table); do not
    reach for `parallel=True`.
    """
    h, w = z.shape
    n = h * w
    out = z.copy()
    done = np.zeros(n, np.uint8)
    hz = np.empty(n, z.dtype)  # heap keys ...
    hi = np.empty(n, np.int32)  # ... and payloads, kept in lockstep

    cnt = seed.size
    for i in range(cnt):
        si = seed[i]
        hi[i] = si
        hz[i] = out[si // w, si % w]
        done[si] = 1

    dy = np.array(_DY, np.int64)
    dx = np.array(_DX, np.int64)

    while cnt > 0:
        ez = hz[0]
        ei = hi[0]
        cnt -= 1
        hz[0] = hz[cnt]
        hi[0] = hi[cnt]
        c = 0
        while True:  # sift down
            l = 2 * c + 1
            r = l + 1
            m = c
            if l < cnt and hz[l] < hz[m]:
                m = l
            if r < cnt and hz[r] < hz[m]:
                m = r
            if m == c:
                break
            hz[m], hz[c] = hz[c], hz[m]
            hi[m], hi[c] = hi[c], hi[m]
            c = m

        cy = ei // w
        cx = ei - cy * w
        lim = ez + eps
        for k in range(8):
            ny = cy + dy[k]
            nx = cx + dx[k]
            if ny < 0 or nx < 0 or ny >= h or nx >= w:
                continue
            ni = ny * w + nx
            if done[ni] != 0:
                continue
            done[ni] = 1
            zn = out[ny, nx]
            if zn < lim:  # fill; with eps == 0 this is the plain flat fill
                zn = lim
                out[ny, nx] = zn
            hz[cnt] = zn
            hi[cnt] = ni
            cnt += 1
            c = cnt - 1
            while c > 0:  # sift up
                p = (c - 1) >> 1
                if hz[p] <= hz[c]:
                    break
                hz[p], hz[c] = hz[c], hz[p]
                hi[p], hi[c] = hi[c], hi[p]
                c = p
    return out


@_jit(cache=True, parallel=True)
def _d8_receivers(z, cell_m):
    """Steepest-descent neighbour per cell. Genuinely parallel: rows are independent."""
    h, w = z.shape
    # int32, not int64: `_as_grid` has already refused any domain whose flat
    # indices do not fit, and at the padded production domain this is 340 MB
    # rather than 680 -- live from B2b all the way through B2d, and half the
    # store bandwidth in the one genuinely parallel kernel in the bake.
    rec = np.full((h, w), -1, np.int32)
    slope = np.zeros_like(z)
    dy = np.array(_DY, np.int64)
    dx = np.array(_DX, np.int64)
    dist = np.array(_DIST, np.float64)
    for y in prange(h):
        for x in range(w):
            zc = z[y, x]
            best = 0.0
            bi = -1
            for k in range(8):
                ny = y + dy[k]
                nx = x + dx[k]
                if ny < 0 or nx < 0 or ny >= h or nx >= w:
                    continue
                s = (zc - z[ny, nx]) / (dist[k] * cell_m)
                if s > best:
                    best = s
                    bi = ny * w + nx
            rec[y, x] = bi
            slope[y, x] = best
    return rec, slope


@_jit(cache=True)
def _accumulate_mfd(z, order_asc, acc, inv_dist, p):
    """One descending-elevation sweep, splitting each cell across all lower neighbours.

    `order_asc` is ascending by elevation and is walked backwards, purely so the caller
    can hand over `np.argsort(...)` without materialising a second 537 MB copy of it at
    full tile size.

    Inherently sequential: a cell must be complete before it pays out. `acc` is
    pre-seeded by the caller (cell area + any external inflow) and updated in place.

    The *weights* are computed in `z`'s own dtype (`inv_dist` and `p` arrive already
    cast) because for float32 that is `powf`: measured 5.06 -> 3.97 CPU-s over 16.8 M
    cells. Only the weights: `tot` and the payout stay float64, so the shares still
    sum to one to within a float64 ULP and the conservation invariant is unharmed.
    """
    h, w = z.shape
    dy = np.array(_DY, np.int64)
    dx = np.array(_DX, np.int64)
    wgt = np.empty(8, np.float64)
    for i in range(order_asc.size - 1, -1, -1):
        c = order_asc[i]
        cy = c // w
        cx = c - cy * w
        zc = z[cy, cx]
        tot = 0.0
        for k in range(8):
            wgt[k] = 0.0
            ny = cy + dy[k]
            nx = cx + dx[k]
            if ny < 0 or nx < 0 or ny >= h or nx >= w:
                continue
            drop = zc - z[ny, nx]
            if drop > 0.0:
                wk = (drop * inv_dist[k]) ** p
                wgt[k] = wk
                tot += wk
        if tot <= 0.0:
            continue  # pit, or a border cell draining out of the domain
        a = acc[c]
        for k in range(8):
            if wgt[k] > 0.0:
                acc[(cy + dy[k]) * w + (cx + dx[k])] += a * (wgt[k] / tot)
    return acc


@_jit(cache=True)
def _descent_enforce(rec, z_ref, z, eps):
    """One topological sweep of ``z[c] = max(z[c], z[rec[c]] + min(drop, eps))``.

    ``rec`` is a receiver forest on ``z_ref`` -- every receiver is STRICTLY
    lower than its donor there -- so it is acyclic and each chain ends at a
    ``rec < 0`` root. The stack walks a chain down to the first already-final
    cell and then applies the rule on the way back up, which visits every cell
    exactly once after its own receiver is final. That is the unique least
    fixed point, so the answer does not depend on the order chains are
    started in.

    Arithmetic is float32 throughout on purpose: the Jacobi form this replaces
    computed ``zf[tgt] + drop`` in float32 too, and float32 addition is
    monotone, so every intermediate that form produced was dominated by the
    final one and the two agree bit-for-bit rather than approximately.
    """
    n = z.size
    done = np.zeros(n, np.uint8)
    # Grown by doubling rather than allocated at n: a full-domain int32 stack
    # would be 340 MB for a chain depth that is in practice thousands.
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
                continue  # root: its "drop" is 0, so the rule is a no-op
            d = z_ref[c] - z_ref[t]
            if d > eps:
                d = eps
            need = z[t] + d
            if z[c] < need:
                z[c] = need


# ---------------------------------------------------------------------------- public
def _as_grid(a, name):
    a = np.ascontiguousarray(a)
    if a.ndim != 2:
        raise ValueError(f"{name} must be 2-D, got shape {a.shape}")
    if a.size == 0:
        raise ValueError(f"{name} is empty")
    if a.size > _MAX_CELLS:
        raise ValueError(f"{name} has {a.size} cells; flat indices are int32 here")
    if a.dtype not in (np.float32, np.float64):
        a = a.astype(np.float64)
    if not np.isfinite(a).all():
        # A NaN poisons priority-flood silently -- every comparison against it is
        # False, so it neither fills nor blocks and the result is merely wrong.
        raise ValueError(f"{name} contains NaN or infinity")
    return a


def _auto_eps(z):
    """Two ULPs at the data's own magnitude.

    One ULP of the largest magnitude present is the smallest step that survives
    `spill + eps > spill` for every value in the array, since ULP is monotone in
    magnitude. Two is used so the guarantee also holds if the fill happens to push a
    value across the next binade, where the ULP doubles.

    Size of the resulting staircase, since it is a real if small distortion: a flat
    accumulates one eps per step of flood-tree depth, and depth is bounded by the
    Chebyshev distance to the domain border, ~4096 at 8192^2. So a *tile-spanning*
    flat costs ~1 mm near sea level and ~2 m at 3 km elevation in float32 (ULP
    2.4e-4 m there), and nothing at all in float64 (1.8e-9 m). Terrain that flat over
    thousands of cells does not come out of B1; if a caller ever has some, fill in
    float64.
    """
    mag = max(abs(float(z.min())), abs(float(z.max())), 1.0)
    return 2.0 * float(np.spacing(z.dtype.type(mag)))


def fill_depressions(z: np.ndarray, *, flat_eps: float | None = None) -> np.ndarray:
    """Raise every cell to the lowest elevation reachable from the domain border.

    Returns a new array of the same shape and float dtype; integer input is promoted
    to float64. The border itself is never modified, so a bake on tile + apron and a
    bake on a larger domain agree wherever the apron is adequate.

    Every cell that is not on the border comes out with at least one strictly lower
    neighbour: pits are filled and the resulting flats get an epsilon gradient toward
    their outlet, so downstream routing never dead-ends (see the module docstring).
    Pass ``flat_eps=0.0`` for the plain fill, where a filled pit is exactly level at
    its spill elevation -- useful for asserting the spill level, useless for routing.
    """
    zz = _as_grid(z, "z")
    h, w = zz.shape
    eps = _auto_eps(zz) if flat_eps is None else float(flat_eps)
    if eps < 0.0 or not np.isfinite(eps):
        raise ValueError(f"flat_eps must be finite and >= 0, got {flat_eps!r}")

    border = np.concatenate(
        [
            np.arange(w, dtype=np.int64),
            np.arange((h - 1) * w, h * w, dtype=np.int64),
            np.arange(1, max(h - 1, 1), dtype=np.int64) * w,
            np.arange(1, max(h - 1, 1), dtype=np.int64) * w + (w - 1),
        ]
    )
    border = np.unique(border)  # h or w < 3 makes the four edges overlap
    seed = border[np.argsort(zz.ravel()[border], kind="stable")]
    return _priority_flood(zz, zz.dtype.type(eps), seed)


def d8_receivers(z: np.ndarray, cell_m: float) -> tuple[np.ndarray, np.ndarray]:
    """Single steepest-descent receiver per cell.

    Returns ``(receiver, slope)``, both shaped like `z`:

    * ``receiver`` -- int32 **flat** index (``y * width + x``) of the steepest lower
      neighbour, or **-1** where the cell has none. After `fill_depressions` with a
      non-zero ``flat_eps`` that means exactly "a domain-border cell that drains out
      of the domain"; on a raw surface it also means "pit".
    * ``slope`` -- the corresponding drop over true distance (diagonals over
      ``sqrt(2) * cell_m``), 0.0 at receiver -1, in `z`'s dtype. This is the ``S`` of
      ``K * A^m * S^n``; use it rather than an MFD-weighted slope, which is
      systematically gentler.

    Out-of-domain neighbours are not candidates, so flow leaves the domain by
    terminating at a border cell rather than by being routed off the edge.
    """
    zz = _as_grid(z, "z")
    cell_m = float(cell_m)
    if not np.isfinite(cell_m) or cell_m <= 0.0:
        raise ValueError(f"cell_m must be finite and > 0, got {cell_m!r}")
    return _d8_receivers(zz, cell_m)


def accumulate_mfd(
    z: np.ndarray,
    cell_m: float,
    p: float = 1.1,
    inflow: np.ndarray | None = None,
    *,
    return_order: bool = False,
) -> np.ndarray:
    """Multiple-flow-direction catchment area, in m^2, as float64.

    Each cell pays its whole accumulation out to *all* strictly lower neighbours in
    proportion to ``slope ** p``, sweeping in descending elevation. `z` should already
    be depression-filled; on a raw surface the sweep is still correct, it just parks
    each pit's catchment in the pit.

    ``return_order=True`` additionally hands back the ascending-elevation
    ``argsort`` this sweep had to compute anyway, as **int32 flat indices**.
    ``incise.profile_incision`` walks the same order over the same array, and
    recomputing it there cost a second full-domain sort inside the bake's peak
    stage (13.6 s at 9216^2 with ``kind="stable"``, plus a 680 MB int64
    transient). The order is a plain ascending sort, so callers must not assume
    anything about how EQUAL elevations are arranged -- both sweeps that use it
    only ever read strictly-lower neighbours, which makes ties inert.

    ``inflow`` is per-cell externally supplied upstream area in m^2, added to the
    initial accumulation -- this is how the apron and the hydrology pyramid's boundary
    conditions enter (plan: "inject upstream accumulation as inflow boundary conditions
    at the fine domain edge"). ``None`` means every cell starts with its own area.

    Conservation, which is the invariant to test against: every cell's contribution is
    passed along until it reaches a cell with no strictly lower neighbour, so the sum
    of the result over those terminal cells equals ``z.size * cell_m**2 + inflow.sum()``.
    After an epsilon fill the terminal cells are all on the domain border, i.e. the
    whole budget leaves through the edge.
    """
    zz = _as_grid(z, "z")
    cell_m = float(cell_m)
    if not np.isfinite(cell_m) or cell_m <= 0.0:
        raise ValueError(f"cell_m must be finite and > 0, got {cell_m!r}")
    p = float(p)
    if not np.isfinite(p) or p <= 0.0:
        raise ValueError(f"p must be finite and > 0, got {p!r}")

    acc = np.full(zz.size, cell_m * cell_m, np.float64)
    if inflow is not None:
        inf = np.ascontiguousarray(inflow, dtype=np.float64)
        if inf.shape != zz.shape:
            raise ValueError(f"inflow shape {inf.shape} != z shape {zz.shape}")
        if not np.isfinite(inf).all():
            raise ValueError("inflow contains NaN or infinity")
        if (inf < 0.0).any():
            raise ValueError("inflow is an area in m^2 and must be >= 0")
        acc += inf.ravel()

    inv_dist = (1.0 / (np.array(_DIST, np.float64) * cell_m)).astype(zz.dtype)
    # int32, not argsort's native intp: `_as_grid` has already refused any
    # domain whose flat indices do not fit, this is the array the sweep
    # random-accesses, and at 9216^2 it is 340 MB rather than 680 -- which
    # matters because B2d now holds it too instead of sorting again.
    order = np.argsort(zz, axis=None).astype(np.int32)
    _accumulate_mfd(zz, order, acc, inv_dist, zz.dtype.type(p))
    acc = acc.reshape(zz.shape)
    return (acc, order) if return_order else acc


def enforce_descent(
    receivers: np.ndarray,
    z_ref: np.ndarray,
    z: np.ndarray,
    eps: float,
) -> np.ndarray:
    """Raise `z` until every cell clears its receiver by its own `z_ref` drop.

    Precisely: the least ``z' >= z`` satisfying, for every cell with a
    receiver, ``z'[c] >= z'[rec[c]] + min(z_ref[c] - z_ref[rec[c]], eps)``.

    This is the guarantee behind the bake's post-meso band (B4): the band's
    along-flow gradient is not bounded by the gate that shapes it, so a reach
    whose drop lands under the codec's 100 mm LSB dams on RECONSTRUCTION even
    though no pit exists at float precision. Enforcing the ORIGINAL drop,
    capped at `eps`, keeps every reach that was codec-proof codec-proof and
    leaves gentle reaches exactly as they were.

    `receivers` must be a receiver forest on `z_ref` -- flat ``y*w + x``, -1
    where none, i.e. `d8_receivers(z_ref, ...)[0]`. Because every receiver is
    strictly lower on `z_ref`, the constraint graph is an acyclic forest and
    ONE sweep in receiver-before-donor order is the exact answer; iterating a
    ``max`` to convergence reaches the same fixed point and costs a pass per
    tree level.

    Works IN PLACE when `z` is already a C-contiguous float32 array; the
    result is returned either way, so callers should use the return value.
    """
    rec = np.ascontiguousarray(receivers, dtype=np.int32).ravel()
    zr = np.ascontiguousarray(z_ref, dtype=np.float32).ravel()
    zz = np.ascontiguousarray(z, dtype=np.float32).ravel()
    if rec.size != zz.size or zr.size != zz.size:
        raise ValueError(
            f"receivers {rec.size}, z_ref {zr.size} and z {zz.size} must all "
            "have the same number of cells"
        )
    if zz.size > _MAX_CELLS:
        raise ValueError(f"z has {zz.size} cells; flat indices are int32 here")
    eps = float(eps)
    if not np.isfinite(eps) or eps < 0.0:
        raise ValueError(f"eps must be finite and >= 0, got {eps!r}")
    _descent_enforce(rec, zr, zz, np.float32(eps))
    return zz.reshape(np.shape(z))
