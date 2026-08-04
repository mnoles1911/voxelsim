"""Correctness tests for the B2 flow routing kernels.

Deliberately no timing assertions -- the box these run on is contended, so a
wall-clock threshold here would be a flaky test rather than a performance gate.
Everything below is an invariant or a closed-form answer.

The kernels need numba, which is NOT in terrain-service's requirements (CI runs
flask/numpy/pytest only). Every test that compiles a kernel therefore takes the
`_numba` fixture, which skips it cleanly when numba is absent. The two tests that
do not take it are the ones that matter most in that environment: they assert the
package imports at all without numba, which is the failure mode that has already
taken this branch's CI job down once.
"""

import ast
import pathlib

import numpy as np
import pytest

from terrain_service import bake
from terrain_service.bake import flow

CELL_M = 2.0
CELL_AREA = CELL_M * CELL_M
_OFFSETS = [(dy, dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dy, dx) != (0, 0)]


@pytest.fixture(scope="module")
def _numba():
    return pytest.importorskip("numba", reason="bake kernels need numba; not a CI dep")


# ------------------------------------------------------------------------- helpers
def _plane(h, w, gx, gy, z0=1000.0, dtype=np.float64):
    """Strictly descending toward +x / +y, `g` metres of drop per cell."""
    y, x = np.mgrid[0:h, 0:w]
    return (z0 - gx * x - gy * y).astype(dtype)


def _terminal_mask(z):
    """Cells with no strictly lower 8-neighbour -- where the MFD sweep parks water."""
    h, w = z.shape
    pad = np.pad(z.astype(np.float64), 1, constant_values=np.inf)
    lower = np.zeros(z.shape, bool)
    for dy, dx in _OFFSETS:
        lower |= pad[1 + dy : 1 + dy + h, 1 + dx : 1 + dx + w] < z
    return ~lower


def _d8_accumulate(z, rec, cell_area=CELL_AREA):
    """Reference single-flow accumulation, for the MFD-vs-D8 comparison."""
    acc = np.full(z.size, float(cell_area))
    receiver = rec.ravel()
    for c in np.argsort(z, axis=None)[::-1]:
        target = receiver[c]
        if target >= 0:
            acc[target] += acc[c]
    return acc.reshape(z.shape)


def _rough(h=96, w=96, seed=20260729):
    """A surface with real depressions in it: smooth trend plus fine noise."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w]
    trend = 300.0 - 0.35 * x - 0.11 * y + 12.0 * np.sin(x / 9.0) * np.cos(y / 7.0)
    return trend + rng.standard_normal((h, w)) * 3.0


# ------------------------------------------------- import hygiene (must NOT skip)
def test_bake_imports_without_numba():
    """The package must import on a numba-less box; only calls may need it."""
    assert bake.fill_depressions is flow.fill_depressions
    assert bake.d8_receivers is flow.d8_receivers
    assert bake.accumulate_mfd is flow.accumulate_mfd


def test_no_module_scope_numba_or_scipy_import():
    """Guards the lazy-compile contract by reading the source, not by luck.

    In CI numba is absent, so a stray top-level `import numba` would be caught by
    the import above; here numba IS installed, so only the AST can see it.
    """
    tree = ast.parse(pathlib.Path(flow.__file__).read_text(encoding="utf-8"))
    banned = {"numba", "scipy"}

    def scan(nodes):
        for node in nodes:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                continue  # deferred by construction
            if isinstance(node, ast.Import):
                names = [a.name.split(".")[0] for a in node.names]
            elif isinstance(node, ast.ImportFrom):
                names = [(node.module or "").split(".")[0]]
            else:
                scan(ast.iter_child_nodes(node))
                continue
            assert not banned.intersection(names), f"module-scope import of {names}"

    scan(tree.body)


# ------------------------------------------------------------------- fill_depressions
def test_fill_is_identity_on_a_monotone_surface(_numba):
    """No depressions, no ties -> nothing to raise, to the last bit."""
    z = _plane(64, 48, gx=1.0, gy=0.37)
    filled = flow.fill_depressions(z)
    assert filled is not z
    assert np.array_equal(filled, z)


def test_fill_raises_a_pit_to_exactly_its_spill_elevation(_numba):
    z = _plane(64, 64, gx=0.5, gy=0.13)
    pit = (slice(28, 37), slice(20, 29))
    z[pit] -= 50.0

    ring = np.zeros(z.shape, bool)
    ring[27:38, 19:30] = True
    ring[pit] = False
    spill = z[ring].min()

    # flat_eps=0 is the plain fill: a filled pit is exactly level at its spill.
    filled = flow.fill_depressions(z, flat_eps=0.0)
    assert np.array_equal(filled[pit], np.full((9, 9), spill))
    untouched = np.ones(z.shape, bool)
    untouched[pit] = False
    assert np.array_equal(filled[untouched], z[untouched])


def test_fill_leaves_no_interior_minimum(_numba):
    """The routing precondition: every non-border cell has a strictly lower neighbour.

    A plain fill fails this by construction -- it replaces each pit with a level
    lake, on which both D8 and MFD dead-end.
    """
    z = _rough()
    assert _terminal_mask(z)[1:-1, 1:-1].sum() > 0, "test surface has no depressions"

    filled = flow.fill_depressions(z)
    assert (filled >= z).all()
    assert _terminal_mask(filled)[1:-1, 1:-1].sum() == 0

    plain = flow.fill_depressions(z, flat_eps=0.0)
    assert _terminal_mask(plain)[1:-1, 1:-1].sum() > 0, "flat_eps=0 should leave flats"
    # The epsilon staircase is noise against the 100 mm wire LSB.
    assert (filled - plain).max() < 1e-3


def test_fill_drains_a_perfectly_flat_domain(_numba):
    """The pathological flat: routable afterwards, and the staircase stays noise.

    Bound: one epsilon per step of flood-tree depth, and depth <= the Chebyshev
    distance to the border, so 32 steps here.
    """
    z = np.full((64, 64), 1000.0)
    filled = flow.fill_depressions(z)
    assert _terminal_mask(filled)[1:-1, 1:-1].sum() == 0
    assert (filled - z).max() < 64 * 2.0 * np.spacing(1000.0)

    acc = flow.accumulate_mfd(filled, CELL_M)
    assert acc[_terminal_mask(filled)].sum() == pytest.approx(z.size * CELL_AREA, rel=1e-9)


def test_fill_preserves_dtype_and_the_border(_numba):
    for dtype in (np.float32, np.float64):
        z = _rough().astype(dtype)
        filled = flow.fill_depressions(z)
        assert filled.dtype == dtype
        # Border cells are seeds: never raised, so tile+apron and a larger domain
        # agree on the edge they share.
        assert np.array_equal(filled[0], z[0])
        assert np.array_equal(filled[-1], z[-1])
        assert np.array_equal(filled[:, 0], z[:, 0])
        assert np.array_equal(filled[:, -1], z[:, -1])


# ---------------------------------------------------------------------- d8_receivers
def test_d8_receivers_on_a_plane(_numba):
    h, w = 24, 16
    gx = 0.5
    z = _plane(h, w, gx=gx, gy=0.0)
    rec, slope = flow.d8_receivers(z, CELL_M)

    y, x = np.mgrid[0:h, 0:w]
    expect = y * w + (x + 1)
    assert np.array_equal(rec[:, :-1], expect[:, :-1])
    assert np.allclose(slope[:, :-1], gx / CELL_M)
    # Last column is the row minimum: no lower neighbour anywhere.
    assert np.array_equal(rec[:, -1], np.full(h, -1))
    assert np.array_equal(slope[:, -1], np.zeros(h))


def test_d8_receivers_pick_the_true_steepest_including_diagonals(_numba):
    """Equal x and y tilt: the diagonal drops 2g over sqrt(2) cells, so it wins."""
    h, w = 20, 20
    g = 0.5
    z = _plane(h, w, gx=g, gy=g)
    rec, slope = flow.d8_receivers(z, CELL_M)

    y, x = np.mgrid[0:h, 0:w]
    expect = (y + 1) * w + (x + 1)
    assert np.array_equal(rec[:-1, :-1], expect[:-1, :-1])
    assert np.allclose(slope[:-1, :-1], 2.0 * g / (np.sqrt(2.0) * CELL_M))


def test_d8_receivers_mark_sinks_minus_one(_numba):
    z = _plane(40, 40, gx=0.5, gy=0.13)
    z[20, 20] -= 10.0
    rec, slope = flow.d8_receivers(z, CELL_M)
    assert rec[20, 20] == -1
    assert slope[20, 20] == 0.0

    rec2, _ = flow.d8_receivers(flow.fill_depressions(z), CELL_M)
    assert (rec2[1:-1, 1:-1] >= 0).all(), "an epsilon fill leaves no interior sink"


# --------------------------------------------------------------------- accumulate_mfd
def test_mfd_reproduces_d8_on_a_monotone_plane(_numba):
    """Closed form: on a plane tilted purely in +x, column i carries (i+1) cells.

    MFD splits across E/NE/SE while D8 sends everything E, but translation
    invariance in y makes the per-cell totals identical away from the y edges --
    the boundary perturbation spreads one cell per column, hence the margin.
    """
    h, w = 128, 48
    z = _plane(h, w, gx=0.5, gy=0.0)
    mfd = flow.accumulate_mfd(z, CELL_M)
    rec, _ = flow.d8_receivers(z, CELL_M)
    d8 = _d8_accumulate(z, rec)

    cols = 20
    band = (slice(cols + 4, h - cols - 4), slice(0, cols))
    expect = (np.arange(cols) + 1.0) * CELL_AREA
    assert np.allclose(mfd[band], expect, rtol=1e-12)
    assert np.allclose(mfd[band], d8[band], rtol=1e-12)
    # ... and it is genuinely MFD: outside the invariant band the fields differ.
    assert not np.allclose(mfd, d8)


def test_mfd_conserves_area(_numba):
    """Every cell's area is passed along until it reaches a terminal cell."""
    filled = flow.fill_depressions(_rough())
    acc = flow.accumulate_mfd(filled, CELL_M)
    terminal = _terminal_mask(filled)

    assert terminal[1:-1, 1:-1].sum() == 0, "after an epsilon fill, only the edge drains"
    assert acc[terminal].sum() == pytest.approx(filled.size * CELL_AREA, rel=1e-9)
    assert (acc >= CELL_AREA - 1e-9).all()


def test_mfd_conserves_area_with_inflow(_numba):
    rng = np.random.default_rng(7)
    filled = flow.fill_depressions(_rough())
    inflow = rng.random(filled.shape) * 5e4
    acc = flow.accumulate_mfd(filled, CELL_M, inflow=inflow)
    terminal = _terminal_mask(filled)
    budget = filled.size * CELL_AREA + inflow.sum()
    assert acc[terminal].sum() == pytest.approx(budget, rel=1e-9)


def test_inflow_propagates_downstream(_numba):
    """A column of upstream area injected at x=0 appears, undiluted, at every x."""
    h, w = 128, 48
    z = _plane(h, w, gx=0.5, gy=0.0)
    per_cell = 1.0e6
    inflow = np.zeros((h, w))
    inflow[:, 0] = per_cell

    acc = flow.accumulate_mfd(z, CELL_M, inflow=inflow)
    cols = 20
    band = (slice(cols + 4, h - cols - 4), slice(0, cols))
    expect = (np.arange(cols) + 1.0) * CELL_AREA + per_cell
    assert np.allclose(acc[band], expect, rtol=1e-12)

    # And it is strictly additive: the no-inflow field plus the injected area.
    base = flow.accumulate_mfd(z, CELL_M)
    assert np.allclose(acc[band] - base[band], per_cell, rtol=1e-12)


def test_inflow_reaches_only_downstream_cells(_numba):
    """Injected at one cell on a rough surface, it must move down-slope and nowhere else."""
    filled = flow.fill_depressions(_rough())
    base = flow.accumulate_mfd(filled, CELL_M)
    inflow = np.zeros(filled.shape)
    inflow[30, 30] = 1.0e6
    acc = flow.accumulate_mfd(filled, CELL_M, inflow=inflow)

    delta = acc - base
    assert delta[30, 30] == pytest.approx(1.0e6, rel=1e-12)
    assert (delta >= -1e-6).all()
    touched = delta > 1e-6
    assert touched.sum() > 1, "the injected area went nowhere"
    # Nothing above the injection point may have received any of it.
    assert not touched[filled > filled[30, 30]].any()
    assert delta[_terminal_mask(filled)].sum() == pytest.approx(1.0e6, rel=1e-9)


def test_inflow_none_matches_explicit_zero(_numba):
    z = flow.fill_depressions(_rough())
    assert np.array_equal(
        flow.accumulate_mfd(z, CELL_M), flow.accumulate_mfd(z, CELL_M, inflow=np.zeros(z.shape))
    )


# ------------------------------------------------------- the D8 (water) sweep

def test_accumulate_d8_matches_the_reference_sweep(_numba):
    """Against the independent reference already in this file, cell for cell.

    ``_d8_accumulate`` is a plain Python argsort loop written for the MFD-vs-D8
    comparison; the kernel must agree with it EXACTLY, not approximately, since
    every payout is a single float64 add in the same descending order.
    """
    filled = flow.fill_depressions(_rough())
    rec, _ = flow.d8_receivers(filled, CELL_M)
    assert np.array_equal(flow.accumulate_d8(filled, CELL_M),
                          _d8_accumulate(filled, rec))


def test_accumulate_d8_conserves_its_seed(_numba):
    """The MFD conservation invariant, verbatim, in both currencies."""
    rng = np.random.default_rng(11)
    filled = flow.fill_depressions(_rough())
    terminal = _terminal_mask(filled)

    acc = flow.accumulate_d8(filled, CELL_M)
    assert acc[terminal].sum() == pytest.approx(filled.size * CELL_AREA, rel=1e-9)

    src = rng.random(filled.shape) * 4.0
    inf = rng.random(filled.shape) * 5e4
    q = flow.accumulate_d8(filled, CELL_M, source=src, inflow=inf)
    assert q[terminal].sum() == pytest.approx(src.sum() + inf.sum(), rel=1e-9)


def test_accumulate_d8_never_splits(_numba):
    """THE PROPERTY THE WATER PASS IS FOR, stated as the diagnosis stated it.

    A cell's accumulation is "split" when no strictly lower neighbour holds as
    much as it does -- on the measured corridor that was true of 25-33% of
    NETWORK cells under MFD, and each one is a chance for a reach to drop below
    ``q_drawable`` and come back. Under a single-receiver rule the count must be
    exactly 0, and MFD's must be large on the same surface, or this test would
    pass without the kernel doing anything.

    Measured on the network rather than on every cell, which is how the corridor
    number was taken and is the only version that means anything: a hillslope
    cell holding one cell-area has a lower neighbour holding at least as much
    almost by definition, so the whole-domain figure (3.3% here) is dominated by
    ground with no river on it. This surface is 96^2 and smoother than the
    corridor, so it reads 15% where the corridor read 25-33%; the threshold
    below is set against what this surface does, not against the corridor.
    """
    filled = flow.fill_depressions(_rough())
    network = np.zeros(filled.shape, bool)
    network[1:-1, 1:-1] = True

    def split_frac(acc, sel):
        h, w = filled.shape
        pz = np.pad(filled.astype(np.float64), 1, constant_values=np.inf)
        pa = np.pad(acc, 1, constant_values=-np.inf)
        held = np.zeros(filled.shape, bool)
        for dy, dx in _OFFSETS:
            sl = (slice(1 + dy, 1 + dy + h), slice(1 + dx, 1 + dx + w))
            held |= (pz[sl] < filled) & (pa[sl] >= acc * (1.0 - 1e-12))
        return float((~held)[sel].mean())

    mfd = flow.accumulate_mfd(filled, CELL_M)
    d8 = flow.accumulate_d8(filled, CELL_M)
    # "Network" = 50 cell areas of catchment or more, ~900 cells here.
    network &= mfd >= 50.0 * CELL_AREA
    assert network.sum() > 500, "the network selection must not be a handful of cells"

    assert split_frac(d8, network) == 0.0
    assert split_frac(mfd, network) > 0.10


def test_accumulate_d8_beats_high_p_underflow(_numba):
    """WHY THIS IS A KERNEL AND NOT A LARGER ``mfd_p``. Measured, not asserted.

    ``_accumulate_mfd`` evaluates ``(drop * inv_dist) ** p`` in the surface's own
    dtype. On float32 -- which is what the bake carries -- an epsilon-filled
    flat's drop is a couple of ULPs of the domain's magnitude, so the weights
    underflow to zero at a modest ``p``, ``tot`` is 0, and the sweep treats the
    cell as a PIT and DROPS its whole accumulation. Conservation is the detector:
    the budget stops arriving at the terminal cells.

    This is not a hypothetical tuning limit. It bites on exactly the flat,
    filled, near-sea-level ground a river has to cross to reach a coast.
    """
    z = _rough().astype(np.float32) + 3000.0     # a production-scale magnitude
    filled = flow.fill_depressions(z)
    terminal = _terminal_mask(filled)
    budget = filled.size * CELL_AREA

    lost = {}
    for p in (1.1, 8.0, 16.0, 32.0):
        acc = flow.accumulate_mfd(filled, CELL_M, p=p)
        lost[p] = 1.0 - float(acc[terminal].sum()) / budget
    assert lost[1.1] < 1e-9, "MFD at the production p conserves"
    assert lost[32.0] > 0.5, (
        f"expected large-p float32 underflow to swallow the budget, lost "
        f"{lost}"
    )
    # The single-receiver sweep is the limit of that family and loses nothing.
    d8 = flow.accumulate_d8(filled, CELL_M)
    assert d8[terminal].sum() == pytest.approx(budget, rel=1e-9)


def test_accumulate_d8_rejects_a_mismatched_forest(_numba):
    z = _plane(16, 16, gx=0.5, gy=0.13)
    with pytest.raises(ValueError, match="cell_m"):
        flow.accumulate_d8(z, 0.0)
    with pytest.raises(ValueError, match="receivers has"):
        flow.accumulate_d8(z, CELL_M, receivers=np.zeros(9, np.int32))
    with pytest.raises(ValueError, match=">= 0"):
        flow.accumulate_d8(z, CELL_M, source=np.full((16, 16), -1.0))


def test_float32_routing_agrees_with_float64(_numba):
    """The kernels specialise on dtype (float32 weights use powf); the answer must not.

    Conservation in particular has to survive float32 input, since that is what the
    bake actually carries -- the accumulator is float64 for exactly this reason.
    """
    z64 = _rough()
    f64 = flow.accumulate_mfd(flow.fill_depressions(z64), CELL_M)

    z32 = z64.astype(np.float32)
    filled32 = flow.fill_depressions(z32)
    f32 = flow.accumulate_mfd(filled32, CELL_M)
    assert f32.dtype == np.float64

    terminal = _terminal_mask(filled32)
    assert f32[terminal].sum() == pytest.approx(z32.size * CELL_AREA, rel=1e-9)
    # Routing decisions can differ where float32 reorders two near-equal drops, so
    # compare the field's shape rather than cell-by-cell equality.
    assert np.corrcoef(np.log1p(f32.ravel()), np.log1p(f64.ravel()))[0, 1] > 0.999


def test_cone_matches_the_analytic_radial_accumulation(_numba):
    """A cone drains radially, and the answer is known in closed form.

    All area inside radius R crosses the circle at R, spread over 2*pi*R/cell_m
    cells, so a cell there drains a wedge of pi*R^2 / (2*pi*R/cell_m) = R*cell_m/2,
    i.e. `(r + 0.5) * cell_area / 2` for a cell whose centre sits r cells out. This
    is the case that catches an accumulation which is merely plausible: a field that
    under-counts (float32 rounding, D8 fragmentation) or double-counts still looks
    like a cone.

    The tolerance is 10% because the *estimator* is coarse, not the field: a
    one-cell lattice annulus is not a clean flow cross-section, and water that steps
    tangentially within it is counted in two of its cells. That bias falls with
    radius (measured 1.16x at r=10 to 1.06x at r=70) while the growth rate, which is
    the part with physical content, is right to 4%. The exact statements about this
    field are the apex value, the symmetry and the conservation below.
    """
    n = 161
    c = n // 2
    y, x = np.mgrid[0:n, 0:n]
    r = np.hypot(y - c, x - c)
    z = 1000.0 - r  # 1 m of drop per cell of radius

    acc = flow.accumulate_mfd(z, CELL_M)
    assert acc[c, c] == pytest.approx(CELL_AREA), "nothing may drain into the apex"

    radii = np.arange(20.0, 61.0, 2.0)
    means = np.array([acc[np.abs(r - r0) < 0.5].mean() for r0 in radii])
    assert np.allclose(means, (radii + 0.5) * CELL_AREA / 2.0, rtol=0.10)
    growth = np.polyfit(radii, means, 1)[0]
    assert growth == pytest.approx(CELL_AREA / 2.0, rel=0.08)

    assert acc[_terminal_mask(z)].sum() == pytest.approx(z.size * CELL_AREA, rel=1e-9)


def test_cone_accumulation_keeps_the_cone_s_symmetry(_numba):
    """The sweep order is elevation-sorted and arbitrary among ties; the answer is not."""
    n = 129
    c = n // 2
    y, x = np.mgrid[0:n, 0:n]
    z = 1000.0 - np.hypot(y - c, x - c)
    acc = flow.accumulate_mfd(z, CELL_M)
    assert np.allclose(acc, np.rot90(acc), rtol=1e-9)
    assert np.allclose(acc, acc[::-1, :], rtol=1e-9)


def test_valley_axis_carries_its_catchment(_numba):
    """Steep-sided V draining along +y: the axis must collect the whole cross-section."""
    h, w = 128, 65
    axis = w // 2
    y, x = np.mgrid[0:h, 0:w]
    z = 1000.0 - 0.02 * y + 1.0 * np.abs(x - axis)

    acc = flow.accumulate_mfd(z, CELL_M)
    profile = acc[:, axis]
    assert np.all(np.diff(profile) > 0.0), "accumulation must grow downstream"

    outlet = profile[h - 2]
    ideal = (h - 1) * w * CELL_AREA  # everything from row 0 down to this one
    assert outlet > 0.9 * ideal, f"axis carries {outlet / ideal:.2f} of the catchment"
    # Slightly MORE than `ideal` is correct, not a bug: the valley walls in the last
    # row stand above this cell and drain back into it. The domain total is the bound.
    assert outlet < z.size * CELL_AREA
    # The axis is a channel, not a smear: its neighbours carry almost nothing.
    assert profile[h - 2] > 50.0 * acc[h - 2, axis + 3]
    assert acc[_terminal_mask(z)].sum() == pytest.approx(z.size * CELL_AREA, rel=1e-9)


def test_filled_pit_routes_its_catchment_through_the_spill(_numba):
    """The end-to-end reason flats must be resolved: a lake has to drain, not absorb.

    With a plain (flat) fill the pit's own 81 cells never leave it, so the spill
    cell sees only its own hillslope.
    """
    z = _plane(96, 96, gx=0.5, gy=0.13)
    pit = (slice(40, 49), slice(40, 49))
    z[pit] -= 50.0
    pit_area = 81 * CELL_AREA

    ring = np.zeros(z.shape, bool)
    ring[39:50, 39:50] = True
    ring[pit] = False
    spill = np.unravel_index(np.where(ring.ravel(), z.ravel(), np.inf).argmin(), z.shape)

    acc = flow.accumulate_mfd(flow.fill_depressions(z), CELL_M)
    assert acc[spill] > pit_area, f"spill carries {acc[spill]:.0f} m2, pit alone is {pit_area}"

    flat = flow.accumulate_mfd(flow.fill_depressions(z, flat_eps=0.0), CELL_M)
    assert flat[spill] < pit_area, "control: a flat fill strands the lake's own area"


# ------------------------------------------------------------------------ validation
def test_input_validation(_numba):
    z = _plane(16, 16, gx=0.5, gy=0.13)
    with pytest.raises(ValueError, match="2-D"):
        flow.fill_depressions(np.zeros(16))
    with pytest.raises(ValueError, match="NaN"):
        flow.fill_depressions(np.full((8, 8), np.nan))
    with pytest.raises(ValueError, match="flat_eps"):
        flow.fill_depressions(z, flat_eps=-1.0)
    with pytest.raises(ValueError, match="cell_m"):
        flow.d8_receivers(z, 0.0)
    with pytest.raises(ValueError, match="cell_m"):
        flow.accumulate_mfd(z, -1.0)
    with pytest.raises(ValueError, match="p must be"):
        flow.accumulate_mfd(z, CELL_M, p=0.0)
    with pytest.raises(ValueError, match="inflow shape"):
        flow.accumulate_mfd(z, CELL_M, inflow=np.zeros((4, 4)))
    with pytest.raises(ValueError, match="inflow contains"):
        flow.accumulate_mfd(z, CELL_M, inflow=np.full((16, 16), np.nan))
    with pytest.raises(ValueError, match=">= 0"):
        flow.accumulate_mfd(z, CELL_M, inflow=np.full((16, 16), -1.0))


def test_integer_input_is_promoted(_numba):
    z = (_plane(32, 32, gx=2.0, gy=1.0)).astype(np.int32)
    filled = flow.fill_depressions(z)
    assert filled.dtype == np.float64
    assert np.array_equal(filled, z.astype(np.float64))
