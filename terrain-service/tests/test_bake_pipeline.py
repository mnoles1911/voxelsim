"""Tests for the bake ORCHESTRATION (terrain_service/bake/pipeline.py).

What is and is not tested here, deliberately:

* The numerics (``flow``/``noise``/``incise``/``thermal``) are a separate
  workstream and need the terrain-diffusion venv (numba + scipy). CI has
  neither. Every test that touches them is ``importorskip``-guarded, and this
  MODULE never imports them at import time -- a test module that fails to
  import takes the whole CI job down, which has already happened once here.

* Everything this file owns -- apron geometry, interior cropping, the
  world-anchoring contract, the hydrology pyramid's bookkeeping, the flow-plane
  bit packing, the identity payload -- is tested on a bare numpy box, using
  REFERENCE KERNELS defined in this module and injected via ``BakeKernels``.
  They live here, not in the package, so they cannot shadow the real
  implementations at integration. They are not models of the physics; each is
  the simplest function with the right signature and the right *locality*,
  which is the property the apron argument actually depends on.

Run: ``cd terrain-service && python -m pytest tests/test_bake_pipeline.py``
"""

from __future__ import annotations

import dataclasses

import numpy as np
import pytest

pipeline = pytest.importorskip("terrain_service.bake.pipeline")

BakeConstants = pipeline.BakeConstants
BakeGeometry = pipeline.BakeGeometry
BakeKernels = pipeline.BakeKernels


# ---------------------------------------------------------------------------
# Reference kernels. Signatures match the frozen interfaces exactly.
# ---------------------------------------------------------------------------

#: Small enough to bake a 3x3 world in a CI runner, same SHAPE as production:
#: apron/tile ratio 0.5 here vs 0.0625 in production, so the apron is if
#: anything over-generous relative to the reference kernels' reach.
TEST_GEOM = BakeGeometry(coarse_tile_px=8, coarse_pixel_m=30.0, scale=4, apron_coarse_px=4)
#: Pinned to the bake_ver-3 OFF-path (depth incision, no constructional term)
#: on purpose: these tests exercise ORCHESTRATION -- aprons, cropping, seeds,
#: inflow -- against locality-reference kernels that deliberately implement
#: only the five-argument roughness form and no profile solve. The profile
#: path and the constructional term have their own dedicated tests that
#: replace these fields explicitly.
TEST_CONSTS = BakeConstants(thermal_iters=3, superblock_tiles=2, superblock_max_level=0,
                            incision_mode="depth", b1_constructional_amp=0.0,
                            profile_regional_p=0.0)


def ref_carrier(coarse, scale):
    """Exactly local upsample (each coarse cell -> scale x scale block).

    Not a B-spline: the point of the reference set is locality, and a nearest
    upsample has an influence radius of zero, which isolates whether the
    PIPELINE cropped correctly from whether the interpolator is well behaved.
    """
    return np.kron(np.asarray(coarse, np.float32), np.ones((scale, scale), np.float32))


def _hash01(seed, ix, iy):
    """Deterministic uniform [0,1) from an int seed and integer lattice coords."""
    h = np.uint64(seed) ^ (ix.astype(np.int64).astype(np.uint64) * np.uint64(0x9E3779B97F4A7C15))
    h ^= iy.astype(np.int64).astype(np.uint64) * np.uint64(0xBF58476D1CE4E5B9)
    h ^= h >> np.uint64(29)
    h *= np.uint64(0x94D049BB133111EB)
    h ^= h >> np.uint64(32)
    return (h >> np.uint64(11)).astype(np.float64) / float(1 << 53)


def ref_roughness_world(carrier_z, cell_m, slope, seed, src_nyquist_m=30.0,
                        origin_cells=(0, 0)):
    """WORLD-anchored roughness: a pure function of world lattice position.

    ``origin_cells`` is **(row0, col0)** -- row-major, matching the real
    ``noise.roughness`` and the arrays themselves. A test double that got the
    axis order backwards would still look world-anchored on a square domain and
    would be wrong everywhere off the diagonal, so the ordering is asserted
    directly in ``test_padded_origin_cells_are_row_major``.

    Counter-hashed per lattice cell (no RNG walk) and with a fixed amplitude
    (no domain-wide ``std()`` normaliser) -- the two properties that make the
    overlap bit-exact rather than merely close.
    """
    h, w = carrier_z.shape
    row0, col0 = origin_cells
    ix = (np.arange(w, dtype=np.int64) + col0)[None, :] * np.ones((h, 1), np.int64)
    iy = (np.arange(h, dtype=np.int64) + row0)[:, None] * np.ones((1, w), np.int64)
    return ((_hash01(seed, ix // 2, iy // 2) - 0.5) * 2.0).astype(np.float32) * np.float32(
        np.clip(slope, 0.0, 1.0)
    )


def ref_roughness_ignores_origin(carrier_z, cell_m, slope, seed,
                                 src_nyquist_m=30.0, origin_cells=(0, 0)):
    """ARRAY-coordinate roughness -- the prototype's defect, kept as a CONTROL.

    It ACCEPTS ``origin_cells`` and ignores it, so it passes the pipeline's
    structural guard and fails only where it matters. Without a control like
    this a passing seam test proves nothing: it could be passing because the
    harness cannot see a seam at all.
    """
    h, w = carrier_z.shape
    ix = np.arange(w, dtype=np.int64)[None, :] * np.ones((h, 1), np.int64)
    iy = np.arange(h, dtype=np.int64)[:, None] * np.ones((1, w), np.int64)
    return ((_hash01(seed, ix // 2, iy // 2) - 0.5) * 2.0).astype(np.float32)


def ref_roughness_no_origin(carrier_z, cell_m, slope, seed, src_nyquist_m=30.0):
    """The frozen five-argument form, i.e. a kernel that CANNOT be anchored."""
    return np.zeros(np.shape(carrier_z), np.float32)


def ref_fill(z, *, flat_eps=None):
    """Identity fill. Accepts ``flat_eps`` because the real one does -- the
    pipeline forwards it only when the constant is not None, and a double that
    rejected it would hide a wiring break."""
    return np.asarray(z, np.float32).copy()


_D8 = ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1))


def ref_d8(z, cell_m):
    """Steepest-descent receiver + slope. Radius 1, i.e. exactly local."""
    z = np.asarray(z, np.float32)
    h, w = z.shape
    best = np.zeros((h, w), np.float32)
    rec = np.full((h, w), -1, np.int64)
    flat = np.arange(h * w, dtype=np.int64).reshape(h, w)
    for dy, dx in _D8:
        shifted = np.full((h, w), np.inf, np.float32)
        sidx = np.full((h, w), -1, np.int64)
        ys = slice(max(0, -dy), h - max(0, dy))
        xs = slice(max(0, -dx), w - max(0, dx))
        yd = slice(max(0, dy), h - max(0, -dy))
        xd = slice(max(0, dx), w - max(0, -dx))
        shifted[ys, xs] = z[yd, xd]
        sidx[ys, xs] = flat[yd, xd]
        dist = cell_m * (1.4142135 if dy and dx else 1.0)
        s = (z - shifted) / dist
        take = s > best
        best = np.where(take, s, best)
        rec = np.where(take, sidx, rec)
    return rec, best.astype(np.float32)


def ref_accumulate(z, cell_m, p=1.1, inflow=None, *, source=None,
                   return_order=False):
    """Own cell area plus any injected inflow. NO routing, on purpose.

    ``source`` REPLACES the cell-area seed, exactly as the real kernel's does
    (bake_ver 9: it is what turns the sweep from an area field into a discharge
    field). Mirrored here rather than left to raise, because B6 always passes
    it and a double that refused would make every orchestration test in this
    file unrunnable -- while a double that silently IGNORED it would report a
    discharge equal to the catchment area and quietly validate nothing.

    ``return_order`` is accepted because the real kernel hands B2d the
    ascending-elevation order it had to sort anyway; this double has no sweep,
    so it sorts only when asked, which is what keeps the wiring under test.

    Routing is unbounded by nature (that is why the hydrology pyramid exists),
    so a routing reference would make the apron test measure the reference
    instead of the pipeline. What is under test here is that the pipeline
    hands the right ``inflow`` to the right cells.

    Returns float64 m^2, matching the real ``accumulate_mfd``.
    """
    if source is None:
        a = np.full(np.shape(z), cell_m * cell_m, np.float64)
    else:
        a = np.array(source, np.float64, copy=True)
    if inflow is not None:
        a = a + np.asarray(inflow, np.float64)
    if return_order:
        return a, np.argsort(np.asarray(z), axis=None).astype(np.int32)
    return a


def ref_stream_power(acc, slope, K=0.15, m=0.45, n=0.8, cap_m=25.0,
                     a_crit_m2=1.0e4, gate_q=2.0, elev_m=None,
                     sea_taper_top_m=0.0, sea_taper_bottom_m=-200.0):
    a = np.asarray(acc, np.float32)
    d = K * np.power(a, m) * np.power(np.asarray(slope, np.float32) + 1e-6, n)
    if a_crit_m2 > 0.0:
        aq = np.power(a, gate_q)
        d = d * (aq / (aq + a_crit_m2 ** gate_q))
    if elev_m is not None and sea_taper_bottom_m < sea_taper_top_m:
        t = np.clip((np.asarray(elev_m, np.float32) - sea_taper_bottom_m)
                    / (sea_taper_top_m - sea_taper_bottom_m), 0.0, 1.0)
        d = d * (t * t * (3.0 - 2.0 * t))
    return np.minimum(d, cap_m).astype(np.float32)


def ref_profile_incision(filled, receivers, acc, cell_m, K_dt=1.5, m=0.45, n=0.8,
                         cap_m=25.0, a_crit_m2=1.0e4, gate_q=2.0,
                         regional_slope=None, regional_s_ref=0.2,
                         regional_scale=1, field_scale=1, erodibility=None,
                         sea_taper_top_m=0.0, sea_taper_bottom_m=-200.0,
                         order=None):
    """LOCAL (radius-1) reference for the profile solve, on purpose.

    ``regional_scale``, ``field_scale`` and ``order`` mirror the real kernel's
    memory-saving parameters: the pipeline hands over COARSE fields plus their
    coarsening factor (the regional slope, and at bake_ver 7 the four landform-
    province parameter fields), plus the ascending-elevation order B2c already
    sorted. Expanded/ignored here respectively -- what these tests check is that
    the pipeline hands over the right field, not how the solve consumes it.

    The real ``incise.profile_incision`` propagates the solved receiver
    elevation upstream along the whole D8 tree -- unbounded by nature, the
    same class as flow accumulation, and the same reasoning as
    ``ref_accumulate`` applies: a tree-walking reference would make the apron
    tests measure the reference instead of the pipeline. This double is one
    EXPLICIT step against the receiver's *input* elevation (radius 1), which
    exercises the wiring -- receivers, gate, taper, cap, the eroded-vs-depth
    orientation -- with an influence radius the apron provably covers. The
    real kernel's own invariants are tested in test_bake_geomorph.py.
    """
    z = np.asarray(filled, np.float64)
    h, w = z.shape

    def _expand(v):
        """A scalar stays a scalar; a coarse province field is gathered to (h, w)."""
        arr = np.asarray(v, np.float64)
        if arr.ndim == 0:
            return float(arr)
        f = int(field_scale)
        ys = np.minimum(np.arange(h) // f, arr.shape[0] - 1)
        xs = np.minimum(np.arange(w) // f, arr.shape[1] - 1)
        return arr[ys][:, xs]

    K_dt, m, a_crit_m2, gate_q = (_expand(K_dt), _expand(m),
                                  _expand(a_crit_m2), _expand(gate_q))
    # `a_crit_m2 > 0` below must stay a plain bool once a_crit can be an array.
    a_crit_on = bool(np.all(np.asarray(a_crit_m2) > 0.0))
    rec = np.asarray(receivers, np.int64).ravel()
    a = np.clip(np.asarray(acc, np.float64), 0.0, None)
    idx = np.arange(rec.size, dtype=np.int64)
    tgt = np.where(rec >= 0, rec, idx)
    zr = z.ravel()[tgt]
    diag = (np.abs(idx // w - tgt // w) > 0) & (np.abs(idx % w - tgt % w) > 0)
    dist = np.where(diag, cell_m * 1.4142135623730951, cell_m)
    s = np.clip((z.ravel() - zr) / dist, 0.0, None)
    kfac = (np.asarray(K_dt) * np.power(a, m)).ravel()
    if erodibility is not None:
        # bake_ver 6 material-strength hook: multiplied where the gates are,
        # exactly as the real kernel does.
        kfac = kfac * np.asarray(erodibility, np.float64).ravel()
    if regional_slope is not None and regional_s_ref > 0.0:
        sreg = np.asarray(regional_slope, np.float64)
        if regional_scale > 1:
            f = int(regional_scale)
            ys = np.minimum(np.arange(h) // f, sreg.shape[0] - 1)
            xs = np.minimum(np.arange(w) // f, sreg.shape[1] - 1)
            sreg = sreg[ys][:, xs]
        kfac = kfac * np.minimum(
            1.0, np.clip(sreg, 0.0, None) / regional_s_ref).ravel() ** n
    if a_crit_on:
        aq = np.power(a, gate_q).ravel()
        acq = np.asarray(np.power(a_crit_m2, gate_q)).ravel()
        kfac = kfac * (aq / (aq + acq))
    if sea_taper_bottom_m < sea_taper_top_m:
        t = np.clip((z.ravel() - sea_taper_bottom_m)
                    / (sea_taper_top_m - sea_taper_bottom_m), 0.0, 1.0)
        kfac = kfac * (t * t * (3.0 - 2.0 * t))
    d = np.minimum(kfac * s ** n, cap_m if cap_m > 0.0 else np.inf)
    out = np.maximum(z.ravel() - d, zr)  # never below the receiver
    out = np.where(rec >= 0, out, z.ravel())
    return out.reshape(h, w).astype(np.float32)


def ref_relax(z, cell_m, repose_deg=36.0, iters=48, rate=0.4):
    """`iters` 3x3 box passes: influence radius exactly `iters` cells.

    With TEST_CONSTS.thermal_iters = 3 against a 16-fine-px apron that is a 5x
    margin, mirroring production's 48 cells against 512.
    """
    z = np.asarray(z, np.float32).copy()
    for _ in range(int(iters)):
        p = np.pad(z, 1, mode="edge")
        acc = np.zeros_like(z)
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                acc += p[dy : dy + z.shape[0], dx : dx + z.shape[1]]
        z = (z * (1 - rate) + (acc / 9.0) * rate).astype(np.float32)
    return z


def kernels(roughness=ref_roughness_world):
    return BakeKernels(
        carrier=ref_carrier,
        roughness=roughness,
        fill_depressions=ref_fill,
        d8_receivers=ref_d8,
        accumulate_mfd=ref_accumulate,
        stream_power=ref_stream_power,
        relax=ref_relax,
        profile_incision=ref_profile_incision,
    )


@pytest.fixture(scope="module")
def _real_kernels():
    """The REAL fill/D8/MFD, for the few tests that need actual routing.

    ``ref_accumulate`` deliberately does no routing at all -- it is a locality
    reference for the apron argument -- so a test about water ARRIVING
    somewhere downstream cannot use it. Those tests need numba, which CI does
    not have, hence the skip rather than a module-level import.
    """
    pytest.importorskip("numba")
    pytest.importorskip("scipy")
    return pipeline.load_kernels()


def ramp_world(tiles=range(-3, 5), geom=TEST_GEOM):
    """A monotone ramp rising to the south-east, so ALL flow runs north-west.

    Used where a test needs to know which way water crosses a boundary.
    ``synth_world`` deliberately has a short-wavelength component that
    dominates its gradient, which makes drainage direction local and
    unpredictable -- fine for a seam test, useless for an inflow one.
    """
    n = geom.coarse_tile_px
    out = {}
    for ty in tiles:
        for tx in tiles:
            gx = np.arange(n)[None, :] + tx * n
            gy = np.arange(n)[:, None] + ty * n
            out[(tx, ty)] = (2.0 * (gx + gy) + 500.0).astype(np.float32)
    return out


def synth_world(seed=7, tiles=range(-3, 5), geom=TEST_GEOM):
    """A deterministic coarse world: {(x, y): elevation}. Smooth + a ridge."""
    n = geom.coarse_tile_px
    out = {}
    for ty in tiles:
        for tx in tiles:
            gx = np.arange(n)[None, :] + tx * n
            gy = np.arange(n)[:, None] + ty * n
            z = (
                40.0 * np.sin(gx / 5.0)
                + 30.0 * np.cos(gy / 7.0)
                + 0.9 * (gx + gy)
                + 3.0 * ((gx * 37 + gy * 17 + seed) % 11)
            )
            out[(tx, ty)] = z.astype(np.float32)
    return out


# ---------------------------------------------------------------------------
# Geometry / the measured apron.
# ---------------------------------------------------------------------------


def test_production_geometry_matches_the_frozen_format():
    g = pipeline.PRODUCTION
    assert (g.fine_tile_px, g.padded_fine_px) == (8192, 9216)
    assert g.fine_pixel_m == 1.875
    assert g.apron_m == 960.0
    assert g.apron_fine_px == 512
    assert g.interior() == slice(512, 8704)
    assert g.tile_span_m == 15360.0
    g.assert_production()


def test_shrinking_the_apron_is_refused_by_assert_production():
    """The 960 m apron is a MEASUREMENT (0.1 mm error; 120 m gave 6 m, 30 m
    gave 9.8 m and a 40 cm join step). A production shrink must be loud."""
    with pytest.raises(ValueError, match="MEASURED"):
        dataclasses.replace(pipeline.PRODUCTION, apron_coarse_px=4).assert_production()
    with pytest.raises(ValueError, match="3x3 coarse ring"):
        BakeGeometry(coarse_tile_px=512, apron_coarse_px=513)


def test_estimate_peak_bytes_is_the_number_the_pod_must_size_for():
    # Counted, not timed: contention cannot touch it.
    assert pipeline.estimate_peak_bytes() > 3_000_000_000


# ---------------------------------------------------------------------------
# B1 world anchoring.
# ---------------------------------------------------------------------------


def test_padded_origins_are_congruent_modulo_the_tile_pitch():
    """pipeline.NOISE_ANCHORING piece 3.

    Every padded domain starts at t*8192 - 512, so any two bakes that see the
    same world point address it at integer lattice indices differing by an
    exact multiple of one tile -- never a fractional phase, which is what lets
    origin_cells be an integer at all.
    """
    g = pipeline.PRODUCTION
    for t in (-3, -1, 0, 5, 1024):
        row0, col0 = g.padded_origin_cells(t, t + 1)
        assert (row0 + g.apron_fine_px) % g.fine_tile_px == 0
        assert (col0 + g.apron_fine_px) % g.fine_tile_px == 0
    a = g.padded_origin_cells(4, 9)
    b = g.padded_origin_cells(5, 9)
    assert b[1] - a[1] == g.fine_tile_px and b[0] == a[0]


def test_padded_origin_cells_are_row_major_and_include_the_apron():
    """Two ways to get this silently wrong, both invisible on a square domain
    with a diagonal-symmetric test world: transposing (row, col), and passing
    the tile interior's origin instead of the padded array's. The second would
    offset the noise by exactly the 960 m apron."""
    g = pipeline.PRODUCTION
    row0, col0 = g.padded_origin_cells(tile_x=3, tile_y=7)
    assert (row0, col0) == (7 * 8192 - 512, 3 * 8192 - 512)
    # ...and the (x, y) helper is its mirror, used only for the metric maths.
    assert g.padded_origin_fine_px(3, 7) == (col0, row0)
    assert g.padded_origin_m(3, 7) == (col0 * 1.875, row0 * 1.875)


def test_roughness_seed_carries_no_per_tile_entropy():
    """The other half: per-tile entropy is exactly what breaks an apron.

    A seed derived from the tile being baked would guarantee that T's apron and
    T+1's interior disagree, which is the one thing the seed must not do.
    """
    g = pipeline.PRODUCTION
    seeds = {
        pipeline.roughness_seed(20260719, g.padded_origin_cells(x, y))
        for x in (-1, 0, 1, 77)
        for y in (-1, 0, 1, 77)
    }
    assert len(seeds) == 1

    # ...but the world and the bake version must still separate worlds.
    base = pipeline.roughness_seed(20260719, (0, 0))
    assert pipeline.roughness_seed(20260720, (0, 0)) != base
    assert pipeline.roughness_seed(20260719, (0, 0), bake_version=99) != base


def test_positive_anchor_pitch_reanchors_and_is_therefore_off_by_default():
    """A literal 'seed from world position' works, and costs a seam per pitch."""
    assert pipeline.CONSTANTS.noise_anchor_pitch_fine_px == 0
    a = pipeline.roughness_seed(1, (0, 0), anchor_pitch_fine_px=8192)
    b = pipeline.roughness_seed(1, (8192, 0), anchor_pitch_fine_px=8192)
    c = pipeline.roughness_seed(1, (8191, 0), anchor_pitch_fine_px=8192)
    assert a != b and a == c


def test_roughness_origin_kwarg_is_detected():
    assert pipeline.roughness_origin_kwarg(ref_roughness_world) == "origin_cells"
    assert pipeline.roughness_origin_kwarg(ref_roughness_ignores_origin) == "origin_cells"
    assert pipeline.roughness_origin_kwarg(ref_roughness_no_origin) is None


def test_bake_refuses_a_roughness_that_cannot_be_world_anchored():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """Array-anchored noise is a correctness bug, not a tolerance: no apron
    size fixes it. The pipeline must not quietly bake a seam."""
    world = synth_world()
    with pytest.raises(RuntimeError, match="ARRAY"):
        pipeline.bake_tile(
            world_seed=1,
            tile_x=0,
            tile_y=0,
            coarse_fetch=lambda x, y: world.get((x, y)),
            kernels=kernels(ref_roughness_no_origin),
            geom=TEST_GEOM,
            consts=TEST_CONSTS,
        )


def test_thermal_rate_above_one_half_is_refused():
    """thermal.relax rejects it too; catching it in the constants means a
    misconfigured bake fails at construction rather than after B2."""
    with pytest.raises(ValueError, match="thermal_rate"):
        BakeConstants(thermal_rate=0.6)
    BakeConstants(thermal_rate=0.5)


# ---------------------------------------------------------------------------
# Coarse assembly.
# ---------------------------------------------------------------------------


def test_assemble_padded_coarse_lays_the_ring_out_correctly():
    world = synth_world()
    dom, missing = pipeline.assemble_padded_coarse(
        lambda x, y: world.get((x, y)), 0, 0, TEST_GEOM
    )
    assert missing == []
    n, a = TEST_GEOM.coarse_tile_px, TEST_GEOM.apron_coarse_px
    assert dom.shape == (n + 2 * a, n + 2 * a)
    # The interior is the tile itself...
    assert np.array_equal(dom[a : a + n, a : a + n], world[(0, 0)])
    # ...the left apron is the east edge of the west neighbour...
    assert np.array_equal(dom[a : a + n, :a], world[(-1, 0)][:, n - a :])
    # ...and the top-left corner is the south-east corner of (-1,-1).
    assert np.array_equal(dom[:a, :a], world[(-1, -1)][n - a :, n - a :])


def test_missing_ring_tiles_are_reported_and_filled_with_sea_level():
    """Conservative by construction: an absent neighbour becomes a sink that
    absorbs flow, never a ridge that invents upstream area."""
    world = synth_world()
    world.pop((1, 0))
    dom, missing = pipeline.assemble_padded_coarse(
        lambda x, y: world.get((x, y)), 0, 0, TEST_GEOM
    )
    assert missing == [(1, 0)]
    n, a = TEST_GEOM.coarse_tile_px, TEST_GEOM.apron_coarse_px
    assert np.all(dom[a : a + n, a + n :] == pipeline.MISSING_ELEVATION_M)


def test_assemble_rejects_a_wrongly_shaped_coarse_tile():
    with pytest.raises(ValueError, match="expected"):
        pipeline.assemble_padded_coarse(lambda x, y: np.zeros((4, 4), np.float32),
                                        0, 0, TEST_GEOM)


# ---------------------------------------------------------------------------
# The apron: the property the whole design rests on.
# ---------------------------------------------------------------------------


def _bake(world, tx, ty, roughness=ref_roughness_world, geom=TEST_GEOM):
    return pipeline.bake_tile(
        world_seed=20260719,
        tile_x=tx,
        tile_y=ty,
        coarse_fetch=lambda x, y: world.get((x, y)),
        kernels=kernels(roughness),
        geom=geom,
        consts=TEST_CONSTS,
    )


def test_neighbouring_bakes_agree_exactly_when_the_noise_is_world_anchored():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """Per-tile bakes reproduce a single-domain bake -- the apron claim.

    This is the harness-scale analogue of tools/bake_seam_check.py's measured
    result (0.1 mm at 960 m). Here the reference kernels' influence radius is
    known exactly (3 cells vs a 16-cell apron), so the answer is not "small
    error" but EXACT equality, and any inequality is a cropping or anchoring
    bug rather than a numerical one.
    """
    world = synth_world()
    a = _bake(world, 0, 0)
    b = _bake(world, 1, 0)

    # "Truth": one domain spanning both tiles, with the same apron.
    g2 = dataclasses.replace(TEST_GEOM, coarse_tile_px=TEST_GEOM.coarse_tile_px * 2)
    n = TEST_GEOM.coarse_tile_px
    wide = {}
    for ty in (-1, 0, 1):
        for tx in (-1, 0, 1):
            wide[(tx, ty)] = np.block(
                [
                    [world[(2 * tx + i, 2 * ty + j)] for i in (0, 1)]
                    for j in (0, 1)
                ]
            ).astype(np.float32)
    truth = pipeline.bake_tile(
        world_seed=20260719,
        tile_x=0,
        tile_y=0,
        coarse_fetch=lambda x, y: wide.get((x, y)),
        kernels=kernels(),
        geom=g2,
        consts=TEST_CONSTS,
    )
    f = TEST_GEOM.fine_tile_px
    assert truth.elevation_m.shape == (2 * f, 2 * f)
    np.testing.assert_array_equal(a.elevation_m, truth.elevation_m[:f, :f])
    np.testing.assert_array_equal(b.elevation_m, truth.elevation_m[:f, f : 2 * f])
    assert n == TEST_GEOM.coarse_tile_px  # the block() layout above assumed it


def test_array_coordinate_noise_breaks_the_seam_even_with_the_apron():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """CONTROL. Without this, the test above could be passing because the
    harness cannot see a seam at all.

    This is the prototype's actual defect: fBm in array coordinates makes two
    overlapping domains disagree in their overlap for reasons that have nothing
    to do with aprons, so no apron size fixes it.
    """
    world = synth_world()
    a = _bake(world, 0, 0, roughness=ref_roughness_ignores_origin)
    b = _bake(world, 1, 0, roughness=ref_roughness_ignores_origin)
    # Adjacent columns across the join should differ by roughly the terrain's
    # own gradient; with array-anchored noise they differ by the noise instead.
    join = np.abs(a.elevation_m[:, -1] - b.elevation_m[:, 0])
    interior = np.abs(a.elevation_m[:, -2] - a.elevation_m[:, -1])
    assert join.mean() > 2.0 * max(interior.mean(), 1e-6)


def test_flat_eps_is_forwarded_only_when_pinned():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """None means 'use the module's auto epsilon'. Pinning 0 would reproduce
    the plain fill that stranded 69.2% of land area on a real tile, so the
    constant exists to make that choice explicit and hashed, not easy."""
    seen = []

    def spy_fill(z, **kw):
        seen.append(kw)
        return np.asarray(z, np.float32).copy()

    world = synth_world()
    k = dataclasses.replace(kernels(), fill_depressions=spy_fill)
    pipeline.bake_tile(world_seed=1, tile_x=0, tile_y=0,
                       coarse_fetch=lambda x, y: world.get((x, y)),
                       kernels=k, geom=TEST_GEOM, consts=TEST_CONSTS)
    # TWO fills since B4b (the post-meso refill), BOTH on the auto epsilon --
    # refill_eps_m is the descent-enforcement bound, and using it as a flood
    # epsilon was measured to dome every wide flat it refloods.
    assert seen == [{}, {}]
    seen.clear()
    pipeline.bake_tile(world_seed=1, tile_x=0, tile_y=0,
                       coarse_fetch=lambda x, y: world.get((x, y)),
                       kernels=k, geom=TEST_GEOM,
                       consts=dataclasses.replace(TEST_CONSTS, flat_eps=1e-6))
    assert seen == [{"flat_eps": 1e-6}, {"flat_eps": 1e-6}]


def test_interior_dead_ends_are_counted():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """After an epsilon fill a receiver of -1 can only mean 'border cell
    draining out of the domain'; an interior one is a routing bug, and pregen
    refuses to ship a tile that has any."""
    world = ramp_world()

    def routed_d8(z, cell_m):
        """Stands in for a correctly epsilon-filled field: every interior cell
        has a receiver. (ref_carrier is a nearest upsample, so the reference
        surface is full of one-block plateaus and would otherwise report
        hundreds of them -- an artifact of the double, not of the pipeline.)"""
        rec, slope = ref_d8(z, cell_m)
        flat = np.arange(z.size, dtype=np.int64).reshape(z.shape)
        return np.where(rec < 0, flat, rec), slope

    def pitted_d8(z, cell_m):
        rec, slope = routed_d8(z, cell_m)
        rec[z.shape[0] // 2, z.shape[1] // 2] = -1
        return rec, slope

    def run(d8):
        return pipeline.bake_tile(
            world_seed=1, tile_x=0, tile_y=0,
            coarse_fetch=lambda x, y: world.get((x, y)),
            kernels=dataclasses.replace(kernels(), d8_receivers=d8),
            geom=TEST_GEOM, consts=TEST_CONSTS,
        ).stats["interior_dead_ends"]

    assert run(routed_d8) == 0.0
    assert run(pitted_d8) == 1.0


def test_basin_width_detector_flags_flats_wider_than_the_apron():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """The apron's blind spot is cheap to DETECT even though it is not cheap
    to fix -- see pipeline.APRON_BLIND_SPOT."""
    assert pipeline._max_axis_run(np.zeros((4, 4), bool)) == 0
    m = np.zeros((6, 6), bool)
    m[2, 1:5] = True
    assert pipeline._max_axis_run(m) == 4
    m[:, 0] = True
    assert pipeline._max_axis_run(m) == 6

    world = ramp_world()

    def sink_fill(z, **kw):
        # A flat the full width of the domain: wider than any apron.
        return np.asarray(z, np.float32) + np.float32(1.0)

    k = dataclasses.replace(kernels(), fill_depressions=sink_fill)
    r = pipeline.bake_tile(world_seed=1, tile_x=0, tile_y=0,
                           coarse_fetch=lambda x, y: world.get((x, y)),
                           kernels=k, geom=TEST_GEOM, consts=TEST_CONSTS)
    assert r.stats["basin_cells_frac"] == 1.0
    assert r.stats["max_basin_run_m"] == TEST_GEOM.fine_tile_px * TEST_GEOM.fine_pixel_m
    assert r.stats["basin_exceeds_apron"] == 1.0
    assert _bake(world, 0, 0).stats["basin_exceeds_apron"] == 0.0


def test_border_detector_separates_a_contained_flat_from_a_spilling_one():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """``max_basin_run_m`` measures a LENGTH; the seam depends on a REACH.

    A filled flat that lies wholly inside the padded domain is entered by
    priority-flood through the terrain, at its own spill point, so its epsilon
    staircase is a function of the terrain and two neighbouring bakes cannot
    cross it differently -- however long it is. Only a flat that reaches the
    padded border is entered FROM the border, which is the one thing a
    neighbour sees differently. So the two statistics must be able to
    disagree, and they must disagree in this direction.
    """
    world = ramp_world()
    f = TEST_GEOM.fine_tile_px
    a = TEST_GEOM.apron_fine_px

    def contained_flat(z, **kw):
        # A long flat down the middle of the INTERIOR, apron untouched. Its
        # run is the full tile width, far over the apron.
        out = np.asarray(z, np.float32).copy()
        mid = a + f // 2
        out[mid - 1 : mid + 1, a : a + f] += np.float32(50.0)
        return out

    r = pipeline.bake_tile(
        world_seed=1, tile_x=0, tile_y=0,
        coarse_fetch=lambda x, y: world.get((x, y)),
        kernels=dataclasses.replace(kernels(), fill_depressions=contained_flat),
        geom=TEST_GEOM, consts=TEST_CONSTS,
    )
    assert r.stats["max_basin_run_m"] > TEST_GEOM.apron_m
    assert r.stats["basin_exceeds_apron"] == 1.0        # the noisy one fires
    assert r.stats["basin_reaches_padded_border"] == 0.0  # the sound one does not
    assert r.stats["padded_border_basin_cells"] == 0.0

    def spilling_flat(z, **kw):
        return np.asarray(z, np.float32) + np.float32(1.0)

    s = pipeline.bake_tile(
        world_seed=1, tile_x=0, tile_y=0,
        coarse_fetch=lambda x, y: world.get((x, y)),
        kernels=dataclasses.replace(kernels(), fill_depressions=spilling_flat),
        geom=TEST_GEOM, consts=TEST_CONSTS,
    )
    assert s.stats["basin_reaches_padded_border"] == 1.0
    assert s.stats["padded_border_basin_frac"] == 1.0

    clean = _bake(world, 0, 0)
    assert clean.stats["basin_reaches_padded_border"] == 0.0


def test_bake_writes_only_the_interior():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    world = synth_world()
    r = _bake(world, 0, 0)
    f = TEST_GEOM.fine_tile_px
    assert r.elevation_m.shape == (f, f)
    assert r.accumulation_m2.shape == (f, f)
    assert r.flow.shape == (f, f) and r.flow.dtype == np.uint8
    assert set(r.cpu_seconds) == set(pipeline.STAGE_ORDER) | set(
        pipeline.PRODUCT_STAGE_ORDER
    )
    assert all(v >= 0.0 for v in r.cpu_seconds.values())
    assert r.missing_coarse == ()
    assert r.stats["relief_m"] > 0.0


def test_stage_sink_observes_every_sub_stage_and_changes_nothing():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """The sink exists so 'which stage made it wrong' is answerable
    (tools/dump_stage_heightfields.py --stages). Three contracts: every
    STAGE_SINK_FIELDS entry arrives, interior-shaped, in pipeline order; the
    last surface it sees IS the shipped one; and observing is free -- a bake
    with a sink is byte-identical to one without."""
    world = synth_world()
    seen: dict[str, np.ndarray] = {}

    def run(sink):
        return pipeline.bake_tile(
            world_seed=20260719, tile_x=0, tile_y=0,
            coarse_fetch=lambda x, y: world.get((x, y)),
            kernels=kernels(), geom=TEST_GEOM, consts=TEST_CONSTS,
            stage_sink=sink,
        )

    r = run(lambda name, arr: seen.__setitem__(name, np.array(arr)))
    assert list(seen) == [n for n, _ in pipeline.STAGE_SINK_FIELDS]
    f = TEST_GEOM.fine_tile_px
    assert all(a.shape == (f, f) for a in seen.values())
    np.testing.assert_array_equal(seen["B3.relaxed"], r.elevation_m)
    # The sub-stages must be consistent with each other, or a dump would be
    # describing a bake that never ran: incised == filled - incision depth.
    np.testing.assert_allclose(
        seen["B2d.incised"],
        seen["B2a.filled"] - seen["B2d.incision_depth_m"],
        rtol=0.0, atol=1e-5,
    )
    # The preserved lake-bed survey (docs/water-system-architecture.md item 0):
    # basin depth IS filled minus carrier+roughness, exactly, and the fill
    # only ever raises, so it is non-negative everywhere. Its scalar
    # reductions must agree with the stats the tile already ships, or the
    # raster and the stats would describe two different bakes.
    np.testing.assert_array_equal(
        seen["B2a.basin_depth"],
        seen["B2a.filled"] - seen["B0B1.carrier_rough"],
    )
    assert float(seen["B2a.basin_depth"].min()) >= 0.0
    assert r.stats["basin_max_depth_m"] == float(seen["B2a.basin_depth"].max())
    assert r.stats["basin_cells_frac"] == float(
        (seen["B2a.basin_depth"] > 0.0).mean()
    )
    r2 = run(None)
    np.testing.assert_array_equal(r.elevation_m, r2.elevation_m)
    np.testing.assert_array_equal(r.flow, r2.flow)


def test_profile_mode_wires_the_profile_kernel_and_depth_is_the_default():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """incision_mode='profile' must reach the injected profile kernel (with a
    regional-slope field when the constant enables one), change the surface
    relative to depth mode, and refuse a kernel set that cannot provide it.
    Depth mode is the default and must not touch the profile kernel at all."""
    world = synth_world()
    calls = []

    def spy_profile(filled, receivers, acc, cell_m, **kw):
        calls.append(kw)
        return ref_profile_incision(filled, receivers, acc, cell_m, **kw)

    def bake(consts, profile_kernel):
        return pipeline.bake_tile(
            world_seed=1, tile_x=0, tile_y=0,
            coarse_fetch=lambda x, y: world.get((x, y)),
            kernels=dataclasses.replace(kernels(), profile_incision=profile_kernel),
            geom=TEST_GEOM, consts=consts,
        )

    # TEST_CONSTS pins depth mode (bake_ver-4 defaults to "profile"; the
    # reference kernel set has no profile solve, see the TEST_CONSTS note).
    assert TEST_CONSTS.incision_mode == "depth"
    base = bake(TEST_CONSTS, spy_profile)
    assert calls == [], "depth mode must not call the profile kernel"

    prof_consts = dataclasses.replace(TEST_CONSTS, incision_mode="profile")
    prof = bake(prof_consts, spy_profile)
    assert len(calls) == 1
    # bake_ver 7: K_dt reaches the kernel as a COARSE province FIELD, not a
    # scalar (see bake.province). Its shape and coarsening factor are asserted
    # with the other province kwargs below; here it only has to be the right
    # constant, which it is exactly where the province mix is pure FLUVIAL.
    assert calls[0]["field_scale"] == TEST_GEOM.scale
    for key, base_const in (("K_dt", prof_consts.profile_K_dt),
                            ("m", prof_consts.stream_m),
                            ("a_crit_m2", prof_consts.channel_init_area_m2),
                            ("gate_q", prof_consts.channel_init_q)):
        fld = calls[0][key]
        assert fld.shape == (TEST_GEOM.padded_coarse_px,
                             TEST_GEOM.padded_coarse_px), key
        # FLUVIAL's multipliers are all 1.0 and it is the only province with a
        # constant baseline weight, so the constant must lie inside the field's
        # range wherever the mix is not pure -- and the field must never be a
        # different number everywhere.
        assert float(fld.min()) <= base_const + 1e-6, key
    assert calls[0]["regional_slope"] is not None, \
        "profile_regional_s_ref > 0 must hand the kernel a regional slope field"
    # COARSE plus its coarsening factor, not a 16x16-replicated full-resolution
    # copy: expanding it here cost 340 MB inside the bake's peak stage. The
    # pair must still cover the padded domain exactly.
    f = TEST_GEOM.scale
    assert calls[0]["regional_scale"] == f
    assert calls[0]["regional_slope"].shape == (
        TEST_GEOM.padded_fine_px // f, TEST_GEOM.padded_fine_px // f)
    assert "order" in calls[0], \
        "B2c's ascending-elevation order must reach the profile solve"
    assert not np.array_equal(prof.elevation_m, base.elevation_m), \
        "the two formulations must actually produce different surfaces"

    with pytest.raises(RuntimeError, match="profile_incision"):
        bake(prof_consts, None)


# ---------------------------------------------------------------------------
# Hydrology pyramid.
# ---------------------------------------------------------------------------


def test_superblock_grid_is_world_anchored_not_tile_centred():
    """Two tiles sharing an edge must read the SAME superblock bytes, or each
    side of that edge gets its own answer for how much water crosses it."""
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    assert lv.tiles_per_side == 2
    assert pipeline.superblock_index(0, 0, lv) == (0, 0)
    assert pipeline.superblock_index(1, 1, lv) == (0, 0)
    assert pipeline.superblock_index(2, 0, lv) == (1, 0)
    # Floor division, so the grid does not fold at the origin.
    assert pipeline.superblock_index(-1, -1, lv) == (-1, -1)
    assert pipeline.superblock_index(-2, -2, lv) == (-1, -1)
    assert pipeline.superblock_index(-3, -3, lv) == (-2, -2)


def test_pyramid_levels_keep_the_raster_size_constant():
    """Each level covers 4x the ground per axis for the same priority-flood."""
    c = pipeline.CONSTANTS
    sizes = {
        pipeline.FlowLevel(level=lv).size_px for lv in range(0, 3)
    }
    assert len(sizes) == 1
    assert pipeline.FlowLevel(level=0).cell_m == 30.0
    assert pipeline.FlowLevel(level=1).cell_m == 30.0 * c.superblock_tiles
    assert pipeline.FlowLevel(level=0).span_m == 4 * 15360.0
    assert pipeline.FlowLevel(level=1).span_m == 16 * 15360.0


def test_flow_superblock_roundtrips_through_the_cache_container():
    rng = np.random.default_rng(3)
    sb = pipeline.FlowSuperblock(
        level=1,
        sx=-4,
        sy=9,
        tiles_per_side=16,
        cell_m=120.0,
        origin_m=(-983040.0, 2211840.0),
        acc=rng.random((8, 8), np.float32) * 1e7,
        filled=rng.random((8, 8), np.float32) * 1000.0,
        missing_tiles=((-1, 2), (3, 4)),
    )
    back, seed = pipeline.decode_flow_superblock(
        pipeline.encode_flow_superblock(sb, 20260719)
    )
    assert seed == 20260719
    assert (back.level, back.sx, back.sy) == (1, -4, 9)
    assert back.cell_m == 120.0 and back.origin_m == sb.origin_m
    assert back.missing_tiles == sb.missing_tiles
    np.testing.assert_array_equal(back.acc, sb.acc)
    np.testing.assert_array_equal(back.filled, sb.filled)

    with pytest.raises(ValueError, match="trailing bytes"):
        pipeline.decode_flow_superblock(
            pipeline.encode_flow_superblock(sb, 1) + b"\x00"
        )
    with pytest.raises(ValueError, match="magic"):
        pipeline.decode_flow_superblock(
            b"XXXX" + pipeline.encode_flow_superblock(sb, 1)[4:]
        )


def test_build_flow_superblock_downsamples_by_mean_and_records_gaps():
    world = synth_world()
    world.pop((3, 3))
    world.pop((2, 1))
    lv = pipeline.FlowLevel(level=1, geom=TEST_GEOM, consts=TEST_CONSTS)
    assert lv.tiles_per_side == 4 and lv.downsample == 2
    sb = pipeline.build_flow_superblock(
        lambda x, y: world.get((x, y)), 0, 0, lv, kernels()
    )
    assert sb.acc.shape == (lv.size_px, lv.size_px)
    assert set(sb.missing_tiles) == {(3, 3), (2, 1)}
    # Mean-pooled, not min-pooled: min biases every cell to its channel and
    # routes flow over an elevation field no real cell has.
    expect = world[(0, 0)][:2, :2].mean()
    assert sb.filled[0, 0] == pytest.approx(expect, rel=1e-5)


# ---------------------------------------------------------------------------
# Order dependence: HYDROLOGY_RESIDUALS #1, made visible.
# ---------------------------------------------------------------------------


def test_exploration_order_changes_the_hydrology_and_the_fingerprint_says_so():
    """The residual itself, demonstrated, then caught.

    A superblock built while one of its coarse tiles is missing routes water
    differently from the same superblock built once that tile exists -- and
    because a shipped tile is never regenerated, an on-demand frontier freezes
    whichever it happened to build. This asserts BOTH halves: that the two
    superblocks really do differ numerically (otherwise the check would be
    guarding nothing), and that their input fingerprints differ so the
    difference is attributable rather than mysterious.
    """
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    full = synth_world()
    early = dict(full)
    early.pop((1, 1))  # the frontier has not reached this tile yet

    sb_early = pipeline.build_flow_superblock(
        lambda x, y: early.get((x, y)), 0, 0, lv, kernels()
    )
    sb_late = pipeline.build_flow_superblock(
        lambda x, y: full.get((x, y)), 0, 0, lv, kernels()
    )

    # (a) the hydrology genuinely differs -- this is the residual, not a nit.
    assert sb_early.missing_tiles == ((1, 1),)
    assert sb_late.missing_tiles == ()
    assert not np.array_equal(sb_early.filled, sb_late.filled)
    assert not sb_early.complete
    assert sb_late.complete

    # (b) ...and it is now visible without diffing the rasters.
    assert len(sb_early.inputs_fingerprint) == pipeline.FLOW_FINGERPRINT_BYTES
    assert sb_early.inputs_fingerprint != sb_late.inputs_fingerprint


def test_the_fingerprint_hashes_inputs_not_fetch_order():
    """A digest that moved with iteration order would flag everything.

    ``coarse_fetch`` is a callback; nothing stops two callers from walking
    their tiles in different orders. The fingerprint is computed over WORLD
    coordinates precisely so that cannot matter -- only which tiles exist and
    what is in them.
    """
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    world = synth_world()
    seen: list[tuple[int, int]] = []

    def spy(x, y):
        seen.append((x, y))
        return world.get((x, y))

    a = pipeline.superblock_inputs_fingerprint(spy, 0, 0, lv)
    order_a = list(seen)
    seen.clear()
    # Same tiles, reversed delivery order, via a shuffled dict.
    shuffled = dict(reversed(list(world.items())))
    b = pipeline.superblock_inputs_fingerprint(
        lambda x, y: shuffled.get((x, y)), 0, 0, lv
    )
    assert a == b
    assert order_a  # the spy really was called
    # A different superblock index is a different digest even over the same
    # world, so the tag identifies a PLACE as well as a content.
    assert pipeline.superblock_inputs_fingerprint(spy, 1, 0, lv) != a


def test_the_fingerprint_chains_the_parent_level():
    """Level 1 feeds level 0; a change up there must change the tag down here.

    Otherwise a level-0 block could carry a fingerprint that says "unchanged"
    while the inflow it received came from a parent built against a different
    world -- exactly the silent case this is for.
    """
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    world = synth_world()
    fetch = lambda x, y: world.get((x, y))  # noqa: E731
    bare = pipeline.superblock_inputs_fingerprint(fetch, 0, 0, lv)
    with_p = pipeline.superblock_inputs_fingerprint(
        fetch, 0, 0, lv, parent_fingerprint=b"\x11" * 16
    )
    other_p = pipeline.superblock_inputs_fingerprint(
        fetch, 0, 0, lv, parent_fingerprint=b"\x22" * 16
    )
    assert len({bare, with_p, other_p}) == 3
    # Length-prefixed, so an all-zero parent digest is not "no parent".
    assert pipeline.superblock_inputs_fingerprint(
        fetch, 0, 0, lv, parent_fingerprint=b"\x00" * 16
    ) != bare


def test_a_baked_tile_records_which_hydrology_it_used():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """Provenance has to reach the BakeResult or it cannot be logged per tile."""
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    world = synth_world()
    world.pop((1, 1))
    sb = pipeline.build_flow_superblock(
        lambda x, y: world.get((x, y)), 0, 0, lv, kernels()
    )
    res = pipeline.bake_tile(
        world_seed=5,
        tile_x=0,
        tile_y=0,
        coarse_fetch=lambda x, y: world.get((x, y)),
        kernels=kernels(),
        geom=TEST_GEOM,
        consts=TEST_CONSTS,
        inflow_source=sb,
    )
    assert res.superblock_fingerprint == sb.fingerprint_hex
    assert res.stats["superblock_missing_tiles"] == 1.0
    assert res.stats["superblock_complete"] == 0.0

    # No superblock at all is a STRONGER statement than "an incomplete one",
    # so it must not read back as zero missing tiles.
    bare = pipeline.bake_tile(
        world_seed=5,
        tile_x=0,
        tile_y=0,
        coarse_fetch=lambda x, y: world.get((x, y)),
        kernels=kernels(),
        geom=TEST_GEOM,
        consts=TEST_CONSTS,
    )
    assert bare.superblock_fingerprint == ""
    assert bare.stats["superblock_missing_tiles"] == -1.0
    assert bare.stats["superblock_complete"] == 0.0


def test_a_stale_superblock_is_caught_by_recomputing_the_fingerprint():
    """The check pregen performs, at the level it performs it.

    Build against an early world, cache it, let the world grow, then ask the
    question a frontier bake asks: does this cached artifact still describe the
    world we are in?
    """
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    full = synth_world()
    early = dict(full)
    early.pop((1, 1))
    cached = pipeline.build_flow_superblock(
        lambda x, y: early.get((x, y)), 0, 0, lv, kernels()
    )
    blob = pipeline.encode_flow_superblock(cached, 99)
    back, _ = pipeline.decode_flow_superblock(blob)
    assert back.inputs_fingerprint == cached.inputs_fingerprint

    now = pipeline.superblock_inputs_fingerprint(
        lambda x, y: full.get((x, y)), 0, 0, lv
    )
    assert now != back.inputs_fingerprint  # stale, and detectably so
    unchanged = pipeline.superblock_inputs_fingerprint(
        lambda x, y: early.get((x, y)), 0, 0, lv
    )
    assert unchanged == back.inputs_fingerprint  # ...and no false positive


def test_a_superblock_from_the_old_container_version_is_refused():
    """A v1 blob has no fingerprint; reading it as zero would read as
    'provenance unknown' everywhere and silently defeat the check."""
    sb = pipeline.FlowSuperblock(
        level=0, sx=0, sy=0, tiles_per_side=4, cell_m=30.0,
        origin_m=(0.0, 0.0),
        acc=np.zeros((4, 4), np.float32),
        filled=np.zeros((4, 4), np.float32),
    )
    blob = bytearray(pipeline.encode_flow_superblock(sb, 1))
    blob[4:6] = (1).to_bytes(2, "little")  # claim version 1
    with pytest.raises(ValueError, match="unsupported flow superblock version 1"):
        pipeline.decode_flow_superblock(bytes(blob))


def test_inject_edge_inflow_is_conservative_and_finds_the_thalweg():
    """Single-receiver D8 crossings => every path counted exactly once."""
    # Parent: 4x4 at 40 m, a west-to-east staircase so everything drains east.
    src = pipeline.FlowSuperblock(
        level=0,
        sx=0,
        sy=0,
        tiles_per_side=1,
        cell_m=40.0,
        origin_m=(0.0, 0.0),
        acc=np.array(
            [[1.0, 2.0, 3.0, 4.0]] * 4, np.float32
        ) * 1000.0,
        filled=np.tile(np.array([40.0, 30.0, 20.0, 10.0], np.float32), (4, 1)),
        missing_tiles=(),
    )
    # Child covers parent columns 2..3 (x in [80, 160)), all four rows.
    child = np.zeros((8, 4), np.float32)
    child[:, :] = 5.0
    child[3, 0] = 1.0  # the thalweg inside parent cell (row 1, col 2)
    inflow = pipeline.inject_edge_inflow(
        child_z=child,
        child_origin_m=(80.0, 0.0),
        child_cell_m=20.0,
        src=src,
        d8_fn=ref_d8,
    )
    # One entry per parent row: column 1 (outside) drains into column 2
    # (inside). Column 0's receiver is column 1, i.e. outside, so it is NOT
    # counted -- its area is already inside column 1's accumulation.
    assert inflow.sum() == pytest.approx(4 * 2000.0)
    assert (inflow > 0).sum() == 4
    # Row 1 of the parent spans child rows 2..3; the minimum sits at row 3.
    assert inflow[3, 0] == pytest.approx(2000.0)


def test_inject_edge_inflow_returns_zero_when_the_domains_do_not_overlap():
    src = pipeline.FlowSuperblock(
        level=0, sx=0, sy=0, tiles_per_side=1, cell_m=40.0, origin_m=(0.0, 0.0),
        acc=np.ones((4, 4), np.float32), filled=np.ones((4, 4), np.float32),
    )
    out = pipeline.inject_edge_inflow(
        child_z=np.zeros((4, 4), np.float32),
        child_origin_m=(10_000.0, 10_000.0),
        child_cell_m=20.0,
        src=src,
        d8_fn=ref_d8,
    )
    assert out.sum() == 0.0


# ---------------------------------------------------------------------------
# Task #49: the pyramid carries DISCHARGE, not just area.
# See pipeline.CARRIED_DISCHARGE.
# ---------------------------------------------------------------------------


def climate_world(precip_for_tile, temp_c=15.0, tiles=range(-3, 5),
                  geom=TEST_GEOM):
    """{(tx, ty): (4, n, n) uint8} at the requested physical values.

    ``precip_for_tile(tx, ty) -> mm/yr``. Quantised exactly as the wire does,
    so a test that reads runoff back gets the same rounding a real tile has.
    """
    province = pipeline._province
    n = geom.coarse_tile_px
    out = {}
    for ty in tiles:
        for tx in tiles:
            planes = np.zeros((len(province.CLIMATE_ORDER), n, n), np.uint8)
            vals = {"temperature": temp_c,
                    "precipitation": float(precip_for_tile(tx, ty))}
            for i, name in enumerate(province.CLIMATE_ORDER):
                lo, hi = province.CLIMATE_RANGES[name]
                v = vals.get(name, lo)
                planes[i] = np.clip(round((v - lo) / (hi - lo) * 255.0), 0, 255)
            out[(tx, ty)] = planes
    return out


def test_flow_superblock_roundtrips_the_discharge_raster():
    """Both states survive the container, and they stay DISTINGUISHABLE.

    ``q is None`` means "this block cannot say how much water" and ``q`` all
    zeros means "none". Collapsing the two on disk would make a block built
    without climate read as a dry continent.
    """
    rng = np.random.default_rng(11)
    base = dict(
        level=0, sx=1, sy=-2, tiles_per_side=4, cell_m=30.0,
        origin_m=(61440.0, -122880.0),
        acc=rng.random((8, 8), np.float32) * 1e7,
        filled=rng.random((8, 8), np.float32) * 1000.0,
        missing_tiles=(),
    )
    q = rng.random((8, 8)) * 1e9
    with_q, _ = pipeline.decode_flow_superblock(
        pipeline.encode_flow_superblock(
            pipeline.FlowSuperblock(**base, q=q), 20260719)
    )
    assert with_q.carries_discharge
    # float64 on the wire: the width law reads Q^0.4 from a headwater trickle
    # to a continental mouth, and the sweep that made it is float64 throughout.
    np.testing.assert_array_equal(with_q.q, q)

    without, _ = pipeline.decode_flow_superblock(
        pipeline.encode_flow_superblock(pipeline.FlowSuperblock(**base), 1)
    )
    assert without.q is None
    assert not without.carries_discharge


def test_a_superblock_that_predates_carried_discharge_is_refused():
    """v2 blobs are rebuilt, not read as "no discharge".

    Decoding one as v3 would come back with ``q = None`` -- the correct
    reading -- and B6 would then quietly fall back to the local-runoff proxy in
    a bake that had been told the pyramid carries Q. A loud rebuild is cheap
    (a superblock is derived); a silent downgrade is the defect returning.
    """
    sb = pipeline.FlowSuperblock(
        level=0, sx=0, sy=0, tiles_per_side=4, cell_m=30.0, origin_m=(0.0, 0.0),
        acc=np.zeros((4, 4), np.float32), filled=np.zeros((4, 4), np.float32),
    )
    blob = bytearray(pipeline.encode_flow_superblock(sb, 1))
    blob[4:6] = (2).to_bytes(2, "little")
    with pytest.raises(ValueError, match="unsupported flow superblock version 2"):
        pipeline.decode_flow_superblock(bytes(blob))


def test_superblock_runoff_is_none_without_climate_and_zero_where_ungenerated():
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    assert pipeline.superblock_runoff_mm_yr(None, 0, 0, lv) is None

    cl = climate_world(lambda tx, ty: 1500.0)
    cl.pop((1, 1))  # the frontier has not reached this tile
    runoff = pipeline.superblock_runoff_mm_yr(lambda x, y: cl.get((x, y)),
                                              0, 0, lv)
    sub = TEST_GEOM.coarse_tile_px
    # An ungenerated tile delivers no water -- the same statement
    # MISSING_ELEVATION_M already makes about the routing.
    assert runoff[sub:, sub:].max() == 0.0
    assert runoff[:sub, :sub].min() > 0.0
    # ...and the hole does NOT bleed a fabricated cold desert into its
    # neighbours: the smooth is coverage-normalised, so the generated quadrant
    # is uniform to within quantisation rather than dished toward the gap.
    live = runoff[:sub, :sub]
    assert live.max() - live.min() < 1e-6 * live.max() + 1e-9


def test_the_pyramid_carries_discharge_across_a_climate_boundary(_real_kernels):
    """THE TASK #49 TEST. A river that leaves its climate zone.

    Built as the failure was found: a wet upstream and an arid downstream, with
    the mouth in the dry half. The proxy the bake used before this -- upstream
    AREA times the LOCAL runoff at the cell the area arrives in -- reads
    essentially nothing there, because the local runoff IS essentially nothing.
    That is what baked three of the showcase corridor's five tiles, including
    its mouth, at 0.000% wet while the headwaters ran.

    Asserted here as a RATIO between the two constructions rather than an
    absolute, so the test says what the defect was rather than restating a
    tuning constant.
    """
    # Ground rises to the south-east, so every drop runs north-west.
    world = ramp_world()
    # Climate follows the ground the OTHER way: wet uplands, arid lowlands.
    # The level-0 block at (0,0) covers tiles (0..1, 0..1) under TEST_CONSTS,
    # whose largest tx+ty is 2 -- so the >= 3 cut puts EVERY wet tile strictly
    # upstream of the block and leaves the destination uniformly arid. (At >= 2
    # one wet tile falls inside the block, which raises the "local" runoff the
    # proxy gets to use and understates the defect by three orders of
    # magnitude: 6.5x instead of 15,000x. The corridor this models is arid the
    # whole way down.)
    cl = climate_world(lambda tx, ty: 2400.0 if (tx + ty) >= 3 else 30.0)
    fetch_z = lambda x, y: world.get((x, y))          # noqa: E731
    fetch_c = lambda x, y: cl.get((x, y))             # noqa: E731

    up = pipeline.FlowLevel(level=1, geom=TEST_GEOM, consts=TEST_CONSTS)
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    parent = pipeline.build_flow_superblock(
        fetch_z, 0, 0, up, _real_kernels, climate_fetch=fetch_c
    )
    child = pipeline.build_flow_superblock(
        fetch_z, 0, 0, lv, _real_kernels, parent=parent, climate_fetch=fetch_c
    )
    assert parent.carries_discharge and child.carries_discharge

    # What the pyramid delivered at this block's edge, in both currencies.
    entries = pipeline._edge_entries(
        child_z=child.filled, child_origin_m=child.origin_m,
        child_cell_m=child.cell_m, src=parent,
        d8_fn=_real_kernels.d8_receivers,
    )
    area_in = pipeline._scatter_entries(entries, parent.acc, child.filled.shape)
    q_in = pipeline._scatter_entries(entries, parent.q, child.filled.shape,
                                     dtype=np.float64)
    assert area_in.sum() > 0.0, "no crossing at all -- the fixture drains wrong"

    # (a) THE DEFECT, reproduced. The proxy converts the arriving area with the
    #     runoff of the arid cell it arrives in.
    local = pipeline.superblock_runoff_mm_yr(fetch_c, 0, 0, lv) / 1000.0
    q_proxy = float((area_in * local).sum())
    q_true = float(q_in.sum())
    # Measured on this fixture: 9.85e5 carried against 64.5 from the proxy,
    # i.e. 15,000x. The bound is loose because the RATIO is the statement.
    assert q_true > 1000.0 * q_proxy, (
        f"carried Q {q_true:.4g} is not meaningfully above the proxy's "
        f"{q_proxy:.4g}; the fixture no longer crosses a climate boundary"
    )

    # (b) ...and the carried number is the RIGHT one: the implied
    #     catchment-mean runoff behind the imported water lands between the
    #     arid destination (0.074 mm/yr here) and the wet source (2,400), not
    #     at either end. Measured: 1,131 mm/yr.
    implied_mm = 1000.0 * q_true / float(area_in.sum())
    assert 1000.0 * float(local.max()) < implied_mm < 2400.0

    # (c) Area is UNTOUCHED by any of this -- it is what incision reads, and
    #     the whole argument for BAKE_VERSION rather than TERRAIN_VERSION is
    #     that the ground does not move.
    no_climate = pipeline.build_flow_superblock(
        fetch_z, 0, 0, lv, _real_kernels, parent=parent
    )
    np.testing.assert_array_equal(no_climate.acc, child.acc)
    np.testing.assert_array_equal(no_climate.filled, child.filled)
    assert no_climate.q is None


def test_discharge_source_refuses_both_currencies_at_once():
    """The same water entering twice is a caller error, not a blend."""
    runoff = np.full((2, 2), 500.0)
    with pytest.raises(ValueError, match="not both"):
        pipeline._water.discharge_source(
            runoff, (4, 4), 2, 10.0,
            inflow_area_m2=np.ones((4, 4)),
            inflow_q_m3_yr=np.ones((4, 4)),
        )


def test_discharge_source_adds_a_carried_q_without_reweighting_it():
    """A carried Q is already m^3/yr. Multiplying it by the local runoff again
    is precisely the bug; assert the arithmetic, not the intent."""
    runoff = np.zeros((2, 2))          # arid: local contribution is exactly 0
    q_in = np.zeros((4, 4))
    q_in[1, 1] = 5.0e7
    src = pipeline._water.discharge_source(
        runoff, (4, 4), 2, 10.0, inflow_q_m3_yr=q_in)
    assert src[1, 1] == pytest.approx(5.0e7)
    assert src.sum() == pytest.approx(5.0e7)
    # The same boundary condition spelled as an AREA vanishes here, which is
    # the whole defect in two lines.
    proxy = pipeline._water.discharge_source(
        runoff, (4, 4), 2, 10.0, inflow_area_m2=q_in)
    assert proxy.sum() == 0.0


def test_the_climate_planes_are_in_the_superblock_fingerprint():
    """Since the block carries a discharge, climate decides its bytes.

    Both halves matter: a block built from a different climate must get a
    different digest, and a block built with NO climate must get exactly the
    digest it always got -- otherwise every cached block in every world would
    read as stale for an input none of them used.
    """
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    world = synth_world()
    fetch_z = lambda x, y: world.get((x, y))          # noqa: E731
    wet = climate_world(lambda tx, ty: 2000.0)
    dry = climate_world(lambda tx, ty: 40.0)

    bare = pipeline.superblock_inputs_fingerprint(fetch_z, 0, 0, lv)
    fp_wet = pipeline.superblock_inputs_fingerprint(
        fetch_z, 0, 0, lv, climate_fetch=lambda x, y: wet.get((x, y)))
    fp_dry = pipeline.superblock_inputs_fingerprint(
        fetch_z, 0, 0, lv, climate_fetch=lambda x, y: dry.get((x, y)))

    assert fp_wet != fp_dry
    assert bare not in (fp_wet, fp_dry)
    # The continuity half: absent climate hashes exactly as before task #49.
    assert bare == pipeline.superblock_inputs_fingerprint(
        fetch_z, 0, 0, lv, climate_fetch=None)


def test_inflow_reaches_accumulate_mfd_through_bake_padded_domain():
    pytest.importorskip("scipy")  # carrier() needs scipy.ndimage; CI has none
    """The wiring, end to end: a superblock's through-flow must show up in the
    fine domain's accumulation."""
    world = ramp_world()
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    sb = pipeline.build_flow_superblock(
        lambda x, y: world.get((x, y)), 0, 0, lv, kernels()
    )
    plain = _bake(world, 0, 0)
    withflow = pipeline.bake_tile(
        world_seed=20260719,
        tile_x=0,
        tile_y=0,
        coarse_fetch=lambda x, y: world.get((x, y)),
        kernels=kernels(),
        geom=TEST_GEOM,
        consts=TEST_CONSTS,
        inflow_source=sb,
    )
    assert withflow.stats["injected_inflow_km2"] > 0.0
    assert withflow.accumulation_m2.sum() >= plain.accumulation_m2.sum()


# ---------------------------------------------------------------------------
# Flow plane (.vxtl v2 section 6).
# ---------------------------------------------------------------------------


def test_flow_plane_bit_layout():
    acc = np.array([[1.0, 256.0], [1e6, 1e6]], np.float32)
    inc = np.zeros((2, 2), np.float32)
    gain = np.array([[0.0, 0.0], [0.0, 1.0]], np.float32)
    p = pipeline.flow_plane(acc, inc, gain)
    assert p[0, 0] & 0x1F == 0            # log2(1) = 0
    assert p[0, 1] & 0x1F == 8            # log2(256) = 8
    assert p[1, 0] & 0x20                 # channel: 1e6 m2 >= 1e5
    assert p[0, 0] & 0x40                 # bank: touches a channel
    assert not p[1, 0] & 0x40             # ...but a channel is not its own bank
    assert p[1, 1] & 0x80                 # deposition


def test_flow_plane_bank_does_not_wrap_around_the_domain():
    """np.roll would paint a bank on the north edge from a channel on the
    south one -- a 15 km-long artifact from a one-line convenience."""
    acc = np.zeros((5, 5), np.float32)
    acc[4, 4] = 1e7
    p = pipeline.flow_plane(acc, np.zeros((5, 5), np.float32), np.zeros((5, 5), np.float32))
    assert not (p[0, :] & 0x40).any()
    assert not (p[:, 0] & 0x40).any()
    assert p[3, 3] & 0x40


def test_flow_plane_clamps_the_log_field():
    acc = np.full((2, 2), 1e30, np.float32)
    p = pipeline.flow_plane(acc, np.zeros((2, 2), np.float32), np.zeros((2, 2), np.float32))
    assert (p & 0x1F).max() == 31


def test_the_magnitude_gate_zeroes_hillslopes_and_only_hillslopes():
    """The plane is 'mostly zeros' only if hillslope accumulation is not in it.

    At 1.875 m/px every land cell has some upslope area, so bits 0-4 were a
    dense field over the whole tile -- 5.75 MB of it on the alpine tile
    against the format's '~5-10 KB' (FLOW_PLANE_SIZE). The gate drops the
    magnitude below the channel-initiation area, and nowhere else: a channel
    or its bank keeps its magnitude however small its own accumulation is,
    because that is the number the consumer named in the format actually
    wants.
    """
    c = pipeline.CONSTANTS
    assert c.flow_mag_min_area_m2 > 0.0
    # A lone hillslope cell, far from any channel.
    acc = np.full((7, 7), 100.0, np.float32)
    zero = np.zeros((7, 7), np.float32)
    p = pipeline.flow_plane(acc, zero, zero, c)
    assert (p == 0).all()

    # ...and the same cell once it carries a channel's worth of area.
    acc[3, 3] = 1e7
    p = pipeline.flow_plane(acc, zero, zero, c)
    assert p[3, 3] & 0x20 and (p[3, 3] & 0x1F) == 23  # log2(1e7)
    assert p[2, 2] & 0x40                             # bank
    assert (p[2, 2] & 0x1F) != 0                      # ...keeps its magnitude
    assert p[0, 0] == 0                               # hillslope: still zero


def test_disabling_the_magnitude_gate_reproduces_the_dense_field():
    """0 must be an exact escape hatch, or the constant is not a knob."""
    off = dataclasses.replace(pipeline.CONSTANTS, flow_mag_min_area_m2=0.0)
    acc = np.full((5, 5), 100.0, np.float32)
    zero = np.zeros((5, 5), np.float32)
    p = pipeline.flow_plane(acc, zero, zero, off)
    assert (p & 0x1F == 6).all()  # log2(100) = 6.64 -> 6, everywhere


def test_the_channel_flag_tapers_with_the_same_smoothstep_as_incision():
    """A flag the incision disagrees with describes a river not in the ground.

    ``incise.stream_power`` fades incision out between sea level and the shelf
    break; the flow plane must fade the channel flag over the same interval
    and with the same curve, or a 3 km-deep seafloor keeps being labelled as
    dendritic drainage that nothing cut.
    """
    incise = pytest.importorskip("terrain_service.bake.incise")
    c = pipeline.CONSTANTS
    z = np.linspace(-400.0, 200.0, 61, dtype=np.float32).reshape(1, -1)
    acc = np.full(z.shape, 1e7, np.float32)
    slope = np.full(z.shape, 0.2, np.float32)

    ours = pipeline._sea_taper(z, c)
    # Same taper, read off the real kernel: with the gate saturated and the cap
    # slack, incision is exactly proportional to the taper.
    theirs = incise.stream_power(
        acc, slope, K=c.stream_K, m=c.stream_m, n=c.stream_n,
        cap_m=1e9, a_crit_m2=0.0, elev_m=z,
        sea_taper_top_m=c.sea_taper_top_m, sea_taper_bottom_m=c.sea_taper_bottom_m,
    )
    ref = incise.stream_power(
        acc, slope, K=c.stream_K, m=c.stream_m, n=c.stream_n,
        cap_m=1e9, a_crit_m2=0.0, elev_m=None,
    )
    np.testing.assert_allclose(theirs / ref, ours, rtol=1e-5, atol=1e-6)

    # And the flag really does switch off with depth rather than at a step.
    zero = np.zeros(z.shape, np.float32)
    p = pipeline.flow_plane(acc, zero, zero, c, elev_m=z)
    chan = (p & 0x20) != 0
    assert chan[0, -1]                     # above sea level: a channel
    assert not chan[0, 0]                  # below the shelf break: not one
    # Monotone in depth -- no isolated on/off band along the coast.
    assert (np.diff(chan[0].astype(np.int8)) >= 0).all()


def test_the_sea_taper_on_flags_is_off_without_an_elevation_field():
    """flow_plane must not silently invent a taper from nothing."""
    c = pipeline.CONSTANTS
    acc = np.full((4, 4), 1e7, np.float32)
    zero = np.zeros((4, 4), np.float32)
    assert ((pipeline.flow_plane(acc, zero, zero, c) & 0x20) != 0).all()
    deep = np.full((4, 4), -3000.0, np.float32)
    assert not ((pipeline.flow_plane(acc, zero, zero, c, elev_m=deep) & 0x20) != 0).any()


# ---------------------------------------------------------------------------
# Identity.
# ---------------------------------------------------------------------------


def test_constants_partition_is_exhaustive():
    """Every BakeConstants field lands in EXACTLY ONE of the two identity halves.

    ``as_payload`` (terrain) and ``product_payload`` (product) are both
    whitelists, which is what lets a product constant be added without rolling
    the terrain identity of every world -- and which is exactly why a field can
    silently land in NEITHER and then decide baked bytes while rolling no
    identity at all. That is the drift this asserts away.

    Failing here means one of two things, both of which want a human:
      * a new constant was added and not classified -- put it in ``as_payload``
        if it can move a height, in ``PRODUCT_FIELDS`` if it can only change a
        product;
      * a constant is in both, so a product retune would re-roll the world.
    """
    fields = {f.name for f in dataclasses.fields(BakeConstants)}
    terrain = set(pipeline.CONSTANTS.as_payload())
    product = set(pipeline.CONSTANTS.product_payload())
    assert not (terrain & product), (
        f"constants in BOTH identity halves: {sorted(terrain & product)}"
    )
    assert not (terrain | product) - fields, (
        f"identity names that are not fields: {sorted((terrain | product) - fields)}"
    )
    assert not fields - terrain - product, (
        f"constants in NEITHER identity half: {sorted(fields - terrain - product)}"
    )


def test_product_constants_roll_only_the_product_fingerprint():
    """A water constant must re-key the namespace WITHOUT re-rolling the world.

    Both halves of that, because either alone is a bug. If the product half did
    not move, a tile whose water bytes differ would share an id with one whose
    do not. If the terrain half DID move, adding a section would re-roll the
    roughness seed -- the fused-counter behaviour the split exists to end, and
    the one that would invalidate the 256-tile lake survey, the bank probe's 25
    tiles and every spawn site the owner holds.
    """
    terrain_base = pipeline.bake_fingerprint()
    product_base = pipeline.product_fingerprint()
    # An enumerated constant has no "+1"; its alternative has to be another
    # member, and it has to be a DIFFERENT member or this test would pass
    # vacuously on the one field whose values are not numbers.
    named_alt = {
        "water_extent_mode": next(
            m for m in BakeConstants.EXTENT_MODES
            if m != pipeline.CONSTANTS.water_extent_mode
        ),
    }
    for name in BakeConstants.PRODUCT_FIELDS:
        cur = getattr(pipeline.CONSTANTS, name)
        if name in named_alt:
            alt = named_alt[name]
        else:
            alt = (not cur) if isinstance(cur, bool) else cur + 1.0
        assert alt != cur
        alt_consts = dataclasses.replace(pipeline.CONSTANTS, **{name: alt})
        assert pipeline.product_fingerprint(alt_consts) != product_base, (
            f"{name} does not roll the product fingerprint"
        )
        assert pipeline.bake_fingerprint(consts=alt_consts) == terrain_base, (
            f"{name} re-rolls the TERRAIN fingerprint; it would move the ground"
        )


def test_terrain_version_bump_rerolls_the_world_and_product_does_not():
    """The two counters do what they say. Executable, because the whole split
    rests on it: TERRAIN_VERSION reaches the roughness seed, BAKE_VERSION must
    not."""
    seed8 = pipeline.roughness_seed(20260719, (0, 0), bake_version=8)
    seed9 = pipeline.roughness_seed(20260719, (0, 0), bake_version=9)
    assert seed8 != seed9, "TERRAIN_VERSION must reach the roughness seed"
    # The shipped world's seed, pinned. If this moves, every measurement,
    # screenshot and spawn site taken on seed 20260719 describes other ground.
    assert seed8 == 0x7E1EC856567C4FB5
    assert pipeline.TERRAIN_VERSION == 8
    assert pipeline.bake_identity_payload()["bake_version"] == pipeline.TERRAIN_VERSION
    assert pipeline.product_identity_payload()["bake_version"] == pipeline.BAKE_VERSION
    # And the shipped terrain fingerprint, likewise pinned.
    assert pipeline.bake_fingerprint().startswith("fe0275e105cbf77c")


def test_every_bake_constant_rolls_the_fingerprint():
    """Same rule as DiffusionConfig's: a field belongs in the identity iff
    changing it can change generated bytes. All of these can.

    TERRAIN constants only since bake_ver 9 -- the product half is covered by
    ``test_product_constants_roll_only_the_product_fingerprint``, which also
    asserts the direction this one cannot see (that they do NOT roll terrain).
    """
    base = pipeline.bake_fingerprint()
    seen = {base}
    product_fields = set(BakeConstants.PRODUCT_FIELDS)
    # Alternatives that still satisfy BakeConstants' own validation.
    valid_alt = {"thermal_rate": 0.25, "flat_eps": 1e-6, "incision_mode": "depth",
                 # +0.5 would put slope_lo above slope_hi, which validation
                 # (correctly) refuses; use in-range alternatives instead.
                 "b1_constructional_slope_lo": 0.05,
                 "b1_constructional_slope_hi": 0.25,
                 "meso_slope_lo": 0.10,
                 "meso_slope_hi": 0.50,
                 # Same story: +0.5 on a dimensionless slope threshold of 0.03
                 # would put province_relief_lo above province_relief_hi.
                 "province_relief_lo": 0.05}
    for fld in dataclasses.fields(BakeConstants):
        if fld.name in product_fields:
            continue
        cur = getattr(pipeline.CONSTANTS, fld.name)
        if fld.name in valid_alt:
            alt = valid_alt[fld.name]
        else:
            alt = cur + (1 if isinstance(cur, int) and not isinstance(cur, bool) else 0.5)
        seen.add(
            pipeline.bake_fingerprint(
                consts=dataclasses.replace(pipeline.CONSTANTS, **{fld.name: alt})
            )
        )
    for fld in dataclasses.fields(BakeGeometry):
        cur = getattr(pipeline.PRODUCTION, fld.name)
        alt = cur + (1 if isinstance(cur, int) else 0.5)
        seen.add(
            pipeline.bake_fingerprint(
                geom=dataclasses.replace(pipeline.PRODUCTION, **{fld.name: alt})
            )
        )
    n = (len(dataclasses.fields(BakeConstants)) - len(product_fields)
         + len(dataclasses.fields(BakeGeometry)))
    assert len(seen) == n + 1, "a bake constant is not covered by the fingerprint"


def test_bake_identity_payload_is_json_stable():
    import json

    payload = pipeline.bake_identity_payload()
    # TERRAIN_VERSION: the key name is historical and is hashed into every
    # shipped fine_provider_id, so it stays even though the counter it carries
    # was renamed. See the note beside it in pipeline.py.
    assert payload["bake_version"] == pipeline.TERRAIN_VERSION
    assert payload["stage_order"] == list(pipeline.STAGE_ORDER)
    json.dumps(payload, sort_keys=True)  # must not raise

    product = pipeline.product_identity_payload()
    assert product["bake_version"] == pipeline.BAKE_VERSION
    assert product["stage_order"] == list(pipeline.PRODUCT_STAGE_ORDER)
    json.dumps(product, sort_keys=True)  # must not raise


def test_bake_identity_keys_only_the_bake_derived_namespace(monkeypatch):
    """A bake change must yield a NEW world for everything the bake produced,
    and must leave the coarse tiles alone.

    Both halves matter and they used to conflict. provider_id covered the bake,
    which got the first half right and made the second half impossible: a
    bake-only tuning change discarded coarse tiles costing ~22.5 s of GPU each
    and unrecreatable on a CPU-only box. Since the split, fine_provider_id
    carries the bake digest and provider_id does not.
    """
    from terrain_service.providers import diffusion

    coarse_before = diffusion.DiffusionConfig().provider_id()
    fine_before = diffusion.DiffusionConfig().fine_provider_id()

    monkeypatch.setattr(pipeline, "BAKE_VERSION", pipeline.BAKE_VERSION + 1)
    # The half that must still hold: bake-derived artifacts re-key.
    assert diffusion.DiffusionConfig().fine_provider_id() != fine_before
    # The half the split exists to buy: inference output does NOT.
    assert diffusion.DiffusionConfig().provider_id() == coarse_before

    monkeypatch.undo()
    assert diffusion.DiffusionConfig().provider_id() == coarse_before
    assert diffusion.DiffusionConfig().fine_provider_id() == fine_before


def test_fine_namespace_is_the_coarse_one_plus_a_bake_suffix():
    """The fine id must name its own coarse namespace, so a human reading a
    cache root can tell which fine generations belong to which coarse tiles
    without running anything -- and so the UNPINNED / UNVERIFIEDDATA / dryrun
    markers survive into it."""
    from terrain_service.providers import diffusion

    cfg = diffusion.DiffusionConfig()
    coarse, fine = cfg.provider_id(), cfg.fine_provider_id()
    assert fine.startswith(coarse + "-b") and len(fine) == len(coarse) + 10

    # Every provider that pregen can bake from must offer both ids, or the
    # bake writes into whatever namespace happens to be lying around.
    from terrain_service.providers.synthetic import SyntheticProvider

    syn = SyntheticProvider()
    assert syn.fine_provider_id.startswith(syn.provider_id + "-b")

    # A dry-run's fine tier inherits the dry-run tag rather than landing in
    # the real namespace.
    dry = diffusion.DiffusionProvider(dry_run=True)
    assert "-dryrun-" in dry.provider_id
    assert dry.fine_provider_id.startswith(dry.provider_id + "-b")


def test_the_fake_bilinear_scale8_path_is_gone():
    """docs/terrain-amplification-plan.md: the scale-8 tier 'already exists in
    the format and carries zero information today. That is the slot this
    fills.' A silent re-introduction would make a cached sub-30 m tile
    ambiguous between baked data and an interpolator."""
    from terrain_service.providers import diffusion

    assert not hasattr(diffusion.TerrainDiffusionBackend, "_get_terrain_at_scale")
    backend = diffusion.TerrainDiffusionBackend(diffusion.DiffusionConfig())
    with pytest.raises(ValueError, match="BAKED, not upsampled"):
        backend._get_native(object(), 0, 0, 512, 512, 8)


# ---------------------------------------------------------------------------
# Cache layout.
# ---------------------------------------------------------------------------


def test_cache_addresses_the_fine_tier_and_flow_superblocks(tmp_path):
    from terrain_service.cache import FINE_SCALE, TileCache

    c = TileCache(tmp_path)
    assert FINE_SCALE == 16
    p = c.fine_path("prov", 0x135276F, -3, 7)
    assert p.parent.name == "s16" and p.name == "-3_7.vxtl"
    assert p == c.path("prov", 0x135276F, -3, 7, 16)

    c.put_fine("prov", 5, 1, 2, b"fine-bytes")
    assert c.get_fine("prov", 5, 1, 2) == b"fine-bytes"
    assert c.get_fine("prov", 5, 1, 3) is None
    # A coarse tile at the same (x, y) must not collide with its fine tier.
    c.put("prov", 5, 1, 2, 1, b"coarse-bytes")
    assert c.get("prov", 5, 1, 2, 1) == b"coarse-bytes"
    assert c.get_fine("prov", 5, 1, 2) == b"fine-bytes"

    f = c.flow_path("prov", 5, 1, -2, 3)
    assert f.parent.name == "flow1" and f.name == "-2_3.vxfl"
    c.put_flow("prov", 5, 0, -2, 3, b"flow-bytes")
    assert c.get_flow("prov", 5, 0, -2, 3) == b"flow-bytes"
    assert c.get_flow("prov", 5, 1, -2, 3) is None  # levels do not collide


def test_cache_writes_are_atomic_and_leave_no_tmp(tmp_path):
    from terrain_service.cache import TileCache

    c = TileCache(tmp_path)
    c.put_flow("prov", 5, 0, 0, 0, b"x" * 1000)
    d = c.flow_path("prov", 5, 0, 0, 0).parent
    assert [p.name for p in d.iterdir()] == ["0_0.vxfl"]


# ---------------------------------------------------------------------------
# pregen bake mode.
# ---------------------------------------------------------------------------


def test_pregen_bake_mode_refuses_cleanly_with_nothing_to_bake(tmp_path):
    """Never bakes a production-sized tile in CI: with an empty cache and
    --bake-no-coarse-generate it exits before any bake, whether or not the
    numerics are installed."""
    import subprocess
    import sys

    r = subprocess.run(
        [
            sys.executable, "-m", "terrain_service.pregen",
            "--seed", "42", "--radius", "0", "--mode", "bake",
            "--cache-dir", str(tmp_path / "cache"),
            "--provider", "synthetic", "--bake-no-coarse-generate",
        ],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 1
    assert (
        "bake numerics" in r.stderr  # kernels absent (CI)
        or "no coarse tiles available" in r.stderr  # kernels present
    ), r.stderr


def test_pregen_exposes_the_order_dependence_check_and_its_escape_hatch():
    """The check is ON by default -- a residual you have to opt into seeing is
    a residual nobody sees. Only the I/O cost is opt-out."""
    import subprocess
    import sys

    r = subprocess.run(
        [sys.executable, "-m", "terrain_service.pregen", "--help"],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0
    assert "--bake-no-verify-superblocks" in r.stdout
    assert "ORDER_DEPENDENCE" in r.stdout


def test_pregen_coarse_mode_is_unchanged_by_the_new_flag(tmp_path):
    import subprocess
    import sys

    from terrain_service.cache import TileCache

    r = subprocess.run(
        [
            sys.executable, "-m", "terrain_service.pregen",
            "--seed", "9", "--radius", "0",
            "--cache-dir", str(tmp_path / "cache"), "--provider", "synthetic",
        ],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0, r.stderr
    assert TileCache(tmp_path / "cache").get("synthetic-v1", 9, 0, 0, 1) is not None


def test_pregen_rejects_a_scale_the_codec_does_not_know(tmp_path):
    import subprocess
    import sys

    r = subprocess.run(
        [
            sys.executable, "-m", "terrain_service.pregen",
            "--seed", "9", "--radius", "0", "--scale", "3",
            "--cache-dir", str(tmp_path / "cache"), "--provider", "synthetic",
        ],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 1 and "--scale must be one of" in r.stderr


def test_pregen_refuses_to_wrap_a_fine_tier_in_a_v1_container():
    """A fine tier written through tile_codec.encode would be exactly the
    'silent disagreement between the two halves' docs/vxtl-v2-format.md opens
    by forbidding, so the encoder probe fails loudly instead."""
    from terrain_service import pregen, tile_codec

    if any(
        hasattr(tile_codec, n)
        for n in ("encode_fine", "encode_v2", "encode_fine_tile")
    ):
        pytest.skip("tile_codec v2 encoder has landed; probe path no longer taken")
    result = pipeline.BakeResult(
        tile_x=0, tile_y=0,
        elevation_m=np.zeros((4, 4), np.float32),
        accumulation_m2=np.zeros((4, 4), np.float32),
        flow=np.zeros((4, 4), np.uint8),
        cpu_seconds={}, stats={},
    )
    with pytest.raises(NotImplementedError, match="v2 fine-tier encoder"):
        pregen._encode_fine(result, 1, "prov")


# ---------------------------------------------------------------------------
# The real numerics, if they happen to be installed.
# ---------------------------------------------------------------------------


def test_load_kernels_reports_a_legible_error_or_returns_all_seven():
    try:
        k = pipeline.load_kernels()
    except RuntimeError as exc:
        assert "terrain-diffusion venv" in str(exc)
        pytest.skip(f"bake numerics not present: {exc}")
    for fld in dataclasses.fields(BakeKernels):
        assert callable(getattr(k, fld.name))


@pytest.mark.parametrize("mod", ["flow", "noise", "incise", "thermal"])
def test_real_kernels_have_the_frozen_signatures(mod):
    """Contract check against the sibling workstreams. Skips cleanly until
    they land; fails loudly if they land with a different signature, which is
    an integration break worth catching here rather than on the pod."""
    import inspect

    m = pytest.importorskip(f"terrain_service.bake.{mod}")
    expected = {
        "flow": {
            "fill_depressions": ["z"],
            "d8_receivers": ["z", "cell_m"],
            "accumulate_mfd": ["z", "cell_m", "p", "inflow"],
        },
        "noise": {
            "carrier": ["coarse", "scale"],
            "roughness": ["carrier_z", "cell_m", "slope", "seed", "src_nyquist_m"],
        },
        "incise": {"stream_power": ["acc", "slope", "K", "m", "n", "cap_m"]},
        "thermal": {"relax": ["z", "cell_m", "repose_deg", "iters", "rate"]},
    }[mod]
    for fn_name, params in expected.items():
        fn = getattr(m, fn_name, None)
        assert fn is not None, f"terrain_service.bake.{mod}.{fn_name} is missing"
        try:
            got = list(inspect.signature(fn).parameters)
        except (TypeError, ValueError):
            continue  # numba dispatcher without an introspectable signature
        # Extra trailing parameters are fine (e.g. a world origin); the frozen
        # leading ones are not negotiable.
        assert got[: len(params)] == params, (
            f"{mod}.{fn_name}{tuple(got)} does not start with the frozen "
            f"interface {tuple(params)}"
        )


# ---------------------------------------------------------------------------
# The MODEL-BACKED top of the pyramid: HYDROLOGY_RESIDUALS #2.
# ---------------------------------------------------------------------------


def _model_bowl(n=16):
    """A raster that drains to one corner, so 'upstream area' is unambiguous."""
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    return (yy + xx) * 10.0


def test_build_model_superblock_needs_no_tiles_and_is_reproducible():
    """The whole point: a parent constructible from an ARRAY, not from tiles.

    ``inject_edge_inflow`` reads only cell_m / origin_m / acc / filled off a
    parent, so a synthetic one is a legal parent -- which is what lets the top
    level be seeded from the model's coarse map at 7.68 km/px, covering 3,932
    km for zero coarse tiles.
    """
    z = _model_bowl()
    sb = pipeline.build_model_superblock(
        z, origin_m=(-1_966_080.0, 983_040.0), cell_m=7680.0, kernels=kernels(),
        sx=-1, sy=3, consts=TEST_CONSTS,
    )
    assert sb.level == pipeline.MODEL_FLOW_LEVEL
    assert sb.cell_m == 7680.0
    assert sb.acc.shape == z.shape and sb.filled.shape == z.shape
    # No tiles means nothing can be MISSING. This must not be read as "the
    # child is complete" -- see the docstring; the child computes its own.
    assert sb.missing_tiles == () and sb.complete
    assert sb.tiles_per_side == pipeline.MODEL_TILES_PER_SIDE

    # Determinism over the raster: same window in, same accumulation out.
    again = pipeline.build_model_superblock(
        z.copy(), origin_m=(-1_966_080.0, 983_040.0), cell_m=7680.0,
        kernels=kernels(), sx=-1, sy=3, consts=TEST_CONSTS,
    )
    np.testing.assert_array_equal(sb.acc, again.acc)
    assert sb.inputs_fingerprint == again.inputs_fingerprint

    # ... and the fingerprint moves when the window's CONTENT moves, which is
    # what makes "this block was built against a different world" detectable
    # through the child's chained digest.
    moved = pipeline.build_model_superblock(
        z + 1.0, origin_m=(-1_966_080.0, 983_040.0), cell_m=7680.0,
        kernels=kernels(), sx=-1, sy=3, consts=TEST_CONSTS,
    )
    assert moved.inputs_fingerprint != sb.inputs_fingerprint


def test_build_model_superblock_refuses_rasters_it_cannot_route():
    k = kernels()
    with pytest.raises(ValueError, match="square 2-D"):
        pipeline.build_model_superblock(
            np.zeros((4, 8), np.float32), (0.0, 0.0), 7680.0, k)
    with pytest.raises(ValueError, match="non-finite"):
        z = _model_bowl(4)
        z[1, 1] = np.nan
        pipeline.build_model_superblock(z, (0.0, 0.0), 7680.0, k)
    with pytest.raises(ValueError, match="cell_m"):
        pipeline.build_model_superblock(_model_bowl(4), (0.0, 0.0), 0.0, k)


def test_model_parent_carries_area_into_a_child_that_had_none():
    """Conservation at the model parent's 64x64 ratio.

    ``inject_edge_inflow`` claims exact conservation; the tile-backed pyramid
    only ever exercised it at 4x4. Every parent flow path crosses the child
    boundary exactly once (single-receiver D8), so the injected total must
    equal the accumulation of exactly those crossing cells -- no more, and
    counted no more than once.
    """
    k = kernels()
    parent = pipeline.build_model_superblock(
        _model_bowl(16), origin_m=(0.0, 0.0), cell_m=7680.0, kernels=k,
        consts=TEST_CONSTS,
    )
    child_cell_m = 120.0
    child_origin = (4 * 7680.0, 4 * 7680.0)
    child_n = 128
    yy, xx = np.mgrid[0:child_n, 0:child_n].astype(np.float32)
    child_z = (yy + xx) * 0.5 + 800.0

    inflow = pipeline.inject_edge_inflow(
        child_z=child_z, child_origin_m=child_origin,
        child_cell_m=child_cell_m, src=parent, d8_fn=ref_d8,
    )
    assert inflow.sum() > 0.0

    rec, _ = ref_d8(parent.filled, parent.cell_m)
    rec = np.asarray(rec).reshape(-1)
    sh, sw = parent.acc.shape
    cx = parent.origin_m[0] + (np.arange(sw) + 0.5) * parent.cell_m
    cy = parent.origin_m[1] + (np.arange(sh) + 0.5) * parent.cell_m
    inside = (
        ((cy >= child_origin[1])
         & (cy < child_origin[1] + child_n * child_cell_m))[:, None]
        & ((cx >= child_origin[0])
           & (cx < child_origin[0] + child_n * child_cell_m))[None, :]
    ).reshape(-1)
    valid = rec >= 0
    entry = np.zeros(rec.shape, bool)
    entry[valid] = (~inside[valid]) & inside[rec[valid]]
    expect = float(parent.acc.reshape(-1)[entry].sum())
    assert float(inflow.sum()) == pytest.approx(expect, rel=1e-5)


def test_entry_crossing_keeps_the_water_on_the_face_it_came_through():
    """Mitigation 1, and why it is not cosmetic at a 64x64 ratio.

    ENTRY_FOOTPRINT deposits in the lowest child cell ANYWHERE in the
    receiving parent cell's footprint. Here the footprint's global minimum sits
    63 cells from the face the parent's flow actually crosses -- at the
    production ratio that is ~7.6 km inside the domain, in whatever valley
    happens to be there. ENTRY_CROSSING cannot leave the face.
    """
    parent = pipeline.FlowSuperblock(
        level=pipeline.MODEL_FLOW_LEVEL, sx=0, sy=0,
        tiles_per_side=pipeline.MODEL_TILES_PER_SIDE, cell_m=640.0,
        origin_m=(0.0, 0.0),
        # A west-to-east staircase: everything drains east, so the crossing
        # into column 2 is through its WEST face.
        acc=np.tile(np.array([1.0, 2.0, 3.0, 4.0], np.float32), (4, 1)) * 1000.0,
        filled=np.tile(np.array([40.0, 30.0, 20.0, 10.0], np.float32), (4, 1)),
    )
    # Child covers parent columns 2..3 at a 64x ratio.
    child = np.full((256, 128), 5.0, np.float32)
    child[100, 63] = 1.0  # a deep hole at the EAST edge of parent cell (1, 2)

    common = dict(child_origin_m=(1280.0, 0.0), child_cell_m=10.0,
                  src=parent, d8_fn=ref_d8)
    foot = pipeline.inject_edge_inflow(
        child_z=child, entry_mode=pipeline.ENTRY_FOOTPRINT, **common)
    cross = pipeline.inject_edge_inflow(
        child_z=child, entry_mode=pipeline.ENTRY_CROSSING, **common)

    # Conservation is identical -- both pick exactly one cell per crossing.
    assert float(foot.sum()) == pytest.approx(float(cross.sum()))
    # Parent row 1 spans child rows 64..127. The footprint rule follows the
    # hole 63 columns in; the crossing rule stays on column 0, the west face.
    assert foot[100, 63] > 0.0
    assert cross[100, 63] == 0.0
    assert cross[64:128, 0].sum() > 0.0

    with pytest.raises(ValueError, match="entry_mode"):
        pipeline.inject_edge_inflow(
            child_z=child, entry_mode="nearest", **common)


def test_entry_mode_is_in_the_digest_but_only_when_it_is_not_the_default():
    """Where inflow lands changes the hydrology, so it belongs in the digest.

    Emitted CONDITIONALLY: hashing it unconditionally would have rewritten the
    fingerprint of every superblock already cached, for a setting none of them
    used.
    """
    world = synth_world()

    def fetch(x, y):
        return world.get((x, y))

    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    base = pipeline.superblock_inputs_fingerprint(fetch, 0, 0, lv)
    assert pipeline.superblock_inputs_fingerprint(
        fetch, 0, 0, lv, entry_mode=pipeline.ENTRY_FOOTPRINT) == base
    assert pipeline.superblock_inputs_fingerprint(
        fetch, 0, 0, lv, entry_mode=pipeline.ENTRY_CROSSING) != base


def test_a_model_parent_changes_the_child_and_the_child_says_so():
    """End to end through ``build_flow_superblock``: the wiring under test.

    The parent must reach the child's accumulation AND the child's provenance
    digest, or "was this built with a model parent?" is unanswerable from the
    cached artifact.
    """
    world = synth_world()

    def fetch(x, y):
        return world.get((x, y))

    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    span = lv.span_m
    n = 8
    cell = span  # one parent cell per child block, so coverage is generous
    parent = pipeline.build_model_superblock(
        _model_bowl(n), origin_m=(-2 * cell, -2 * cell), cell_m=cell,
        kernels=kernels(), consts=TEST_CONSTS,
    )
    assert pipeline.superblock_covers(parent, (0.0, 0.0), span)

    plain = pipeline.build_flow_superblock(fetch, 0, 0, lv, kernels())
    seeded = pipeline.build_flow_superblock(
        fetch, 0, 0, lv, kernels(), parent=parent)
    assert seeded.inputs_fingerprint != plain.inputs_fingerprint
    assert float(seeded.acc.sum()) > float(plain.acc.sum())


def test_superblock_covers_catches_a_parent_that_only_half_reaches():
    sb = pipeline.build_model_superblock(
        _model_bowl(8), origin_m=(0.0, 0.0), cell_m=7680.0, kernels=kernels(),
        consts=TEST_CONSTS,
    )
    span = 8 * 7680.0
    assert pipeline.superblock_covers(sb, (0.0, 0.0), span)
    assert pipeline.superblock_covers(sb, (7680.0, 7680.0), span - 2 * 7680.0)
    assert not pipeline.superblock_covers(sb, (-7680.0, 0.0), span)
    assert not pipeline.superblock_covers(sb, (7680.0, 0.0), span)


def test_a_model_parent_does_not_make_an_incomplete_child_look_complete():
    """Scope doc test 4, decided deliberately.

    A model-backed parent has no ``missing_tiles`` -- the model is defined
    everywhere -- and it would be easy for that to leak downward and mark a
    child complete. It must not: model backing fixes TRUNCATION (a catchment
    larger than the top level's span) and says nothing about EXPLORATION ORDER
    (a neighbour that has never been generated). Conflating them would
    silently reopen what the publish gate just closed.
    """
    world = synth_world()
    world.pop((1, 1))  # the frontier has not reached this tile yet

    def fetch(x, y):
        return world.get((x, y))

    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    parent = pipeline.build_model_superblock(
        _model_bowl(8), origin_m=(-2 * lv.span_m, -2 * lv.span_m),
        cell_m=lv.span_m, kernels=kernels(), consts=TEST_CONSTS,
    )
    assert parent.complete  # nothing it is built from can be missing

    sb = pipeline.build_flow_superblock(fetch, 0, 0, lv, kernels(), parent=parent)
    assert sb.missing_tiles == ((1, 1),)
    assert not sb.complete


# ---------------------------------------------------------------------------
# bake_ver 12: the pyramid's discharge, and the width law.
# ---------------------------------------------------------------------------


def test_the_pyramid_routes_its_discharge_single_receiver(_real_kernels):
    """THE SEAM WIRING. The block's ``q`` IS the D8 sweep, and ``acc`` is not.

    This is the defect bake_ver 12 fixes, and the test is written against the
    thing that can silently regress -- which half of the pyramid each sweep
    reaches -- rather than against a magnitude. The fine tier routes its
    discharge single-receiver (bake_ver 11) and the pyramid routed its own with
    MFD, so a river arrived at a tile's edge as a FAN while the field computed
    from that edge was a centreline, and the consumer at the far end is a hard
    threshold that a fan cannot clear.

    WHAT THE EFFECT IS NOT. The boundary was never STARVED: measured on the real
    (-11,-5)/(-11,-6) seam, the same 8.6e7 m^3/yr crossed either way (D8/MFD =
    0.99 on the total) and only the spread changed -- MFD's largest single
    crossing held 8.66e6 against D8's 2.02e7. A synthetic block small and smooth
    enough to run in a unit test has no trunk to concentrate and reproduces
    neither number, so the corridor measurement is the evidence for the effect
    and this is the evidence for the wiring. See
    ``docs/measurements/river-seam-and-width-2026-08-04.txt``.
    """
    world = synth_world()
    cl = climate_world(lambda tx, ty: 900.0)
    fetch_z = lambda x, y: world.get((x, y))          # noqa: E731
    fetch_c = lambda x, y: cl.get((x, y))             # noqa: E731

    assert TEST_CONSTS.water_pyramid_single_receiver
    lv_d8 = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    lv_mfd = pipeline.FlowLevel(
        level=0, geom=TEST_GEOM,
        consts=dataclasses.replace(TEST_CONSTS,
                                   water_pyramid_single_receiver=False))

    mfd = pipeline.build_flow_superblock(
        fetch_z, 0, 0, lv_mfd, _real_kernels, climate_fetch=fetch_c)
    d8 = pipeline.build_flow_superblock(
        fetch_z, 0, 0, lv_d8, _real_kernels, climate_fetch=fetch_c)

    # 1. THE AREA FIELD IS BIT-IDENTICAL. It is what stream-power incision reads
    #    (`A^m`), so it decides every height, and this is the whole reason
    #    bake_ver 12 is a product bump and not a terrain one.
    np.testing.assert_array_equal(mfd.acc, d8.acc)
    np.testing.assert_array_equal(mfd.filled, d8.filled)

    # 2. THE DISCHARGE IS THE D8 SWEEP, exactly -- not "something more
    #    concentrated". A test that only asserted a direction would pass on a
    #    larger `mfd_p`, which is the change this deliberately is not.
    src = pipeline.superblock_runoff_mm_yr(fetch_c, 0, 0, lv_d8) / 1000.0
    src = src * (lv_d8.cell_m * lv_d8.cell_m)
    want = _real_kernels.accumulate_d8(d8.filled, lv_d8.cell_m, source=src)
    np.testing.assert_allclose(d8.q, want, rtol=0, atol=0)

    # 3. ...and it is genuinely a different raster from the one that shipped.
    assert not np.array_equal(mfd.q, d8.q)
    assert float(d8.q.max()) >= float(mfd.q.max())

    # 4. CONSERVATION is untouched: after the epsilon fill every terminal cell
    #    is on the border, so the whole seed leaves through the edge -- the same
    #    invariant `accumulate_mfd` states, and the reason the total crossing a
    #    tile boundary did not move.
    rec, _ = _real_kernels.d8_receivers(d8.filled, lv_d8.cell_m)
    term = np.asarray(rec).ravel() < 0
    np.testing.assert_allclose(float(d8.q.ravel()[term].sum()), float(src.sum()),
                               rtol=1e-9)


def test_the_pyramid_routing_rule_is_in_the_superblock_fingerprint():
    """A cached block built with the MFD sweep must not be read as this one.

    ``pregen`` REUSES a cached superblock even when its digest disagrees -- that
    is ORDER_DEPENDENCE and it is deliberate -- so the digest is the only place
    that can record "this block carries a differently-routed discharge". Absent
    means MFD, on the ``entry_mode`` precedent, which is what makes every block
    cached before bake_ver 12 read as what it actually is rather than as stale.
    """
    world = synth_world()
    fetch = lambda x, y: world.get((x, y))            # noqa: E731
    on = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    off = pipeline.FlowLevel(
        level=0, geom=TEST_GEOM,
        consts=dataclasses.replace(TEST_CONSTS,
                                   water_pyramid_single_receiver=False))
    fp_on = pipeline.superblock_inputs_fingerprint(fetch, 0, 0, on)
    fp_off = pipeline.superblock_inputs_fingerprint(fetch, 0, 0, off)
    assert fp_on != fp_off
    # The OFF digest is stable -- turning the flag on must not have rewritten
    # the fingerprint of blocks that never used it.
    assert fp_off == pipeline.superblock_inputs_fingerprint(fetch, 0, 0, off)


def test_the_pyramid_refuses_single_receiver_without_the_kernel():
    """The same refusal B6 makes, for the same reason.

    Falling back to MFD would ship a pyramid that is not the one the constants
    describe, and the only symptom would be a river that stops at a seam --
    which is exactly the bug that took a day to find the first time.
    """
    world = synth_world()
    cl = climate_world(lambda tx, ty: 900.0)
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=TEST_CONSTS)
    k = dataclasses.replace(kernels(), accumulate_d8=None)
    with pytest.raises(RuntimeError, match="no accumulate_d8 kernel"):
        pipeline.build_flow_superblock(
            lambda x, y: world.get((x, y)), 0, 0, lv, k,
            climate_fetch=lambda x, y: cl.get((x, y)),
        )


# --- the width law ---------------------------------------------------------


def _straight_channel(n=64, depth_m=6.0, half_px=40):
    """A V-shaped trench down column n//2 in a plane that falls to the south.

    Deliberately SHALLOW-SIDED: at the 1.5 m stage the test stands water at, the
    ground allows ~20 px of half-width, well past the widest law width in play.
    That is what makes this fixture a test of the LAW -- if the walls bound it
    the test measures the clamp instead, which has its own case below.
    """
    z = np.zeros((n, n), np.float32)
    z += np.arange(n, dtype=np.float32)[:, None] * -0.5
    cx = n // 2
    d = np.abs(np.arange(n) - cx).astype(np.float32)
    z -= np.maximum(depth_m * (1.0 - d / half_px), 0.0)[None, :]
    return z


def test_width_follows_the_discharge_and_stops_at_the_ground():
    """The two bounds, each shown to bite where it should.

    A trench with a constant cross-section and a discharge that grows down it:
    the drawn ribbon must WIDEN downstream (the law is monotone in Q) and must
    never reach a cell whose ground stands above that reach's own water surface
    (the clamp). The second is not decoration -- widening in the ground plane
    without one put 58.2% of a far-field ribbon's edge below drawn ground.
    """
    n = 64
    z = _straight_channel(n)
    cx = n // 2
    water = np.full((n, n), np.nan, np.float32)
    q = np.ones((n, n), np.float64)
    # Q rises four orders of magnitude down the reach while the water stands a
    # fixed 1.5 m over the trench floor, so the CLAMP is constant and any change
    # in the drawn width is the LAW's doing alone.
    for r in range(n):
        q[r, cx] = 3.0e6 * (10.0 ** (2.0 * r / (n - 1)))
        water[r, cx] = z[r, cx] + 1.5

    out, stats = pipeline._water.widen_to_channel_width(water, z, q, cell_m=1.875)
    wet = np.isfinite(out)
    assert stats["width_added_cells"] > 0

    up, down = int(wet[2].sum()), int(wet[n - 3].sum())
    assert down > up, f"the ribbon did not widen downstream: {up} -> {down} px"

    # It tracks the LAW rather than merely growing -- but bounded by the GROUND,
    # which is the second half of this test's own title and which the
    # exaggerated exponents (2026-08-06) made bite for the first time.
    #
    # The trench is a fixed cross-section. Under Earth's exponents the law never
    # asked for more room than it had, so "drawn == law" held everywhere and the
    # clamp was only exercised by the explicit check below. Under width ∝ Q^0.648
    # the law asks for 116 m at the bottom of this reach and the trench offers
    # 88 m, so the drawn width is now VALLEY-LIMITED there. That is the rule
    # working, and it is what the owner asked for -- "drawn width and depth
    # should be a function of how much water the river carries AND the
    # underlying ground terrain at a certain point."
    #
    # So the assertion is drawn == min(law, room), which is a stronger statement
    # than the old one: it says the law is followed wherever there is room and
    # the ground wins where there is not.
    for r in (2, n // 2, n - 3):
        law = float(pipeline._water.channel_width_m(q[r, cx]))
        drawn = int(wet[r].sum()) * 1.875
        room = float(np.isfinite(z[r]).sum()) * 1.875
        expect = min(law, room)
        assert drawn <= law + 1.875 + 1e-9, (
            f"row {r}: drew {drawn:.2f} m, MORE than the law's {law:.2f} m")
        assert abs(drawn - expect) <= 1.875 + 1e-9, (
            f"row {r}: drew {drawn:.2f} m against min(law {law:.2f}, room {room:.2f})")

    # THE CLAMP. Every drawn cell stands under its own water, by at least the
    # representability floor the codec needs.
    assert float((out[wet] - z[wet]).min()) >= pipeline._water.WIDEN_MIN_DEPTH_M - 1e-6
    # The centreline is untouched: widening adds cells, it never moves a level.
    src = np.isfinite(water)
    np.testing.assert_allclose(out[src], water[src])


def test_width_never_leaves_a_trench_it_cannot_fill():
    """A major river in a one-pixel slot with sheer walls stays one pixel wide."""
    n = 48
    z = np.full((n, n), 100.0, np.float32)
    cx = n // 2
    z[:, cx] = 90.0
    q = np.ones((n, n))
    q[:, cx] = 5.0e8
    water = np.full((n, n), np.nan, np.float32)
    water[:, cx] = 92.0
    # The law wants many pixels; the ground offers one.
    assert float(pipeline._water.channel_width_m(5.0e8)) > 8 * 1.875

    out, _ = pipeline._water.widen_to_channel_width(water, z, q, cell_m=1.875)
    wet = np.isfinite(out)
    assert int(wet.sum()) == n, "water escaped a slot whose walls stand 8 m above it"
    assert wet[:, cx].all()


def test_width_does_not_spill_into_a_registered_basin():
    """B5 basins are written dry and must stay dry.

    Their surface is already on the wire in SECTION_BASIN_TABLE and the client
    composes the two samplers; a widened cell inside one would be a second,
    freely-disagreeing copy of a shipped fact -- which is the reason
    ``graded_water_surface`` excludes them in the first place.
    """
    n = 32
    z = np.full((n, n), 50.0, np.float32)
    cx = n // 2
    z[:, cx] = 40.0
    z[:, cx + 1:cx + 4] = 10.0          # a deep re-opened hole beside the reach
    q = np.ones((n, n))
    q[:, cx] = 5.0e8
    water = np.full((n, n), np.nan, np.float32)
    water[:, cx] = 42.0
    exclude = np.zeros((n, n), bool)
    exclude[:, cx + 1:cx + 4] = True

    out, _ = pipeline._water.widen_to_channel_width(
        water, z, q, cell_m=1.875, exclude=exclude)
    assert not np.isfinite(out[exclude]).any()
    # ...and without the exclusion it WOULD have flooded it, so this is
    # measuring the guard rather than a geometry that never reached.
    loose, _ = pipeline._water.widen_to_channel_width(water, z, q, cell_m=1.875)
    assert np.isfinite(loose[:, cx + 1]).any()


def test_width_takes_the_nearest_reach_not_the_highest():
    """Two reaches three pixels apart: the cell between them takes its own.

    Taking the maximum level instead would let a trunk raise the water over a
    tributary's bank from three pixels away, which draws a stamped sheet rather
    than two rivers.
    """
    n = 24
    z = np.full((n, n), 100.0, np.float32)
    a, b = 8, 11
    z[:, a] = 90.0
    z[:, b] = 90.0
    z[:, a + 1:b] = 95.0
    q = np.ones((n, n))
    q[:, a] = 5.0e8            # the trunk, high water
    q[:, b] = 2.0e7            # the tributary, low water
    water = np.full((n, n), np.nan, np.float32)
    water[:, a] = 99.0
    water[:, b] = 96.0

    out, _ = pipeline._water.widen_to_channel_width(water, z, q, cell_m=1.875)
    # The cell adjacent to the tributary takes the TRIBUTARY's level, though the
    # trunk's is higher and its law reaches that far.
    assert np.isfinite(out[:, b - 1]).all()
    np.testing.assert_allclose(out[:, b - 1], 96.0)


# ---------------------------------------------------------------------------


# bake_ver 13: extent from the terrain, not from a formula.
#
# EVERY FIXTURE HERE USES A REALISTIC VALLEY GRADIENT, and that is not
# decoration. The rule anchors a cell's water level on the reach it DRAINS TO,
# so on a floor that falls downstream faster than it falls toward the channel
# the D8 arrow points along the valley and the fill correctly declines to reach.
# The measured corridor falls 389 m over 30,577 m of channel -- 0.024 m per
# 1.875 m pixel -- so a fixture at 0.25 m/px is a 13% grade, is nothing in this
# world, and quietly tests the opposite of what it says. (It was written that
# way first and it failed for exactly this reason.)

_DOWN = 0.02   # m per pixel along the valley, the corridor's own mean gradient


def _flow_forest(z, cell_m=1.875):
    """A D8 receiver forest on a sink-filled copy of `z`, as B6 builds one."""
    from terrain_service.bake import flow

    rec, _ = flow.d8_receivers(
        flow.fill_depressions(np.ascontiguousarray(z, np.float32)), cell_m)
    return rec


def _valley(n, lateral, incision=1.0, depth=1.5, down=_DOWN):
    """A trench down the middle of a floor that falls `down` m/px downstream.

    Returns `(z, water)`. `lateral` is the cross-valley gradient in m/px, which
    is the only thing that differs between the floodplain and the gorge below.
    """
    cx = n // 2
    z = (100.0
         - np.arange(n, dtype=np.float32)[:, None] * down
         + np.abs(np.arange(n) - cx).astype(np.float32)[None, :] * lateral)
    z = np.ascontiguousarray(z, np.float32)
    z[:, cx] -= incision
    water = np.full((n, n), np.nan, np.float32)
    water[:, cx] = z[:, cx] + depth
    return z, water


def test_lateral_fill_widens_on_a_floodplain_and_stays_narrow_in_a_gorge(_real_kernels):
    """THE POINT OF THE CHANGE, as one assertion.

    Two reaches with the SAME incision carrying the SAME depth of water down the
    SAME gradient. One sits in a floor that rises 0.05 m per pixel to either
    side, the other in a slot that rises 4 m. A width law reads only Q and would
    draw them identically, because Q is identical. The terrain says they are not
    the same river, and after this change the bake says so too.
    """
    n = 96
    mid = n // 2
    got = {}
    for name, lateral in (("pan", 0.05), ("slot", 4.0)):
        z, water = _valley(n, lateral)
        out, _ = pipeline._water.fill_to_local_surface(
            water, z, _flow_forest(z), cell_m=1.875)
        got[name] = int(np.isfinite(out[mid]).sum())

    assert got["pan"] > 12, got
    assert got["slot"] <= 3, got
    # And the ratio, so a future change that narrows the pan or floods the slot
    # fails here rather than in a screenshot.
    assert got["pan"] >= 8 * got["slot"], got


def test_lateral_fill_cross_section_is_level_to_the_channels_own_gradient(_real_kernels):
    """THE OWNER'S SECOND TESTABLE PROPERTY, stated as tightly as it is true.

    "If the left bank and right bank of one cross-section sit at different
    heights, the fill is wrong." A bank cell that drains STRAIGHT into the reach
    inherits the reach cell's float exactly -- not to a tolerance, the same
    number. What is not exact is the cell whose steepest descent is DIAGONAL:
    it reaches the channel a row or two downstream and inherits that row's
    surface, which is lower by the channel's own gradient. So the honest bound
    is not zero, it is THE CHANNEL'S OWN FALL over the offset:

        spread across a section  <=  gradient * lateral offset

    and it is asserted here at that bound rather than at a round number. On the
    measured corridor this is worth p90 0.29-0.88 m against the shipped ribbon's
    own p90 of 0.46-2.07 m, i.e. the fill's surface is FLATTER than the one it
    replaces; the adjacent-cell step distribution says the same thing at every
    percentile to p99. See docs/measurements/river-lateral-fill-2026-08-04.txt.

    The bed is deliberately ASYMMETRIC -- the left flank rises twice as fast as
    the right -- so a rule that tapered the surface with distance, or with the
    local bed, is caught here rather than passing on a symmetric fixture.
    """
    n = 64
    cx = n // 2
    x = np.arange(n, dtype=np.float32)[None, :]
    z = np.ascontiguousarray(
        100.0 - np.arange(n, dtype=np.float32)[:, None] * _DOWN
        + np.maximum(cx - x, 0.0) * 0.08 + np.maximum(x - cx, 0.0) * 0.04,
        np.float32)
    z[:, cx] -= 1.0
    water = np.full((n, n), np.nan, np.float32)
    water[:, cx] = z[:, cx] + 1.5

    out, _ = pipeline._water.fill_to_local_surface(
        water, z, _flow_forest(z), cell_m=1.875)
    asym = 0
    rows = range(4, n - 4)
    for r in rows:
        cols = np.flatnonzero(np.isfinite(out[r]))
        assert cols.size >= 3, (r, cols)
        lvl = out[r, cols]
        offset = max(cx - cols.min(), cols.max() - cx)
        assert lvl.max() - lvl.min() <= _DOWN * offset + 1e-4, (
            r, offset, np.unique(lvl))
        # the cells immediately either side of the channel drain straight in,
        # so THEY are exact.
        assert out[r, cx - 1] == out[r, cx] == out[r, cx + 1], r
        asym += (cx - cols.min()) != (cols.max() - cx)
    # The gentler right flank reaches further on most sections. Without this a
    # symmetric fixture would make the assertions above pass for free.
    assert asym > len(list(rows)) // 2, asym


def test_lateral_fill_does_not_run_down_a_hillside(_real_kernels):
    """THE FAILURE THAT KILLED THE FIRST VERSION, kept as a test.

    A channel on a LEDGE with a cliff beside it. Every cell down that cliff is
    below the channel's water surface and 8-connected to it, so a fill anchored
    on the NEAREST channel admits the whole slope -- which is how the first
    implementation turned 317,665 corridor cells into 66,546,420 at a median
    added depth of 7.8 m. Anchored on the flow path instead, the cliff cells
    take the level of what they drain into, which is not this reach.

    The fixture asserts its own premise first: if the cliff were NOT below the
    channel's waterline this test would pass without testing anything.
    """
    n = 48
    ledge = 12
    z = np.zeros((n, n), np.float32)
    z += np.arange(n, dtype=np.float32)[:, None] * -_DOWN
    z[:, ledge + 1:] -= 40.0                                   # a 40 m cliff
    z[:, ledge] -= 1.0                                         # the channel
    z = np.ascontiguousarray(z, np.float32)
    water = np.full((n, n), np.nan, np.float32)
    water[:, ledge] = z[:, ledge] + 0.9

    cliff = np.zeros((n, n), bool)
    cliff[:, ledge + 2:] = True
    assert (z[cliff] < water[:, ledge].min() - 1.0).all(), (
        "premise: the whole cliff must stand below the channel's waterline"
    )

    out, stats = pipeline._water.fill_to_local_surface(
        water, z, _flow_forest(z), cell_m=1.875)
    assert not np.isfinite(out[cliff]).any(), (
        f"{int(np.isfinite(out[cliff]).sum())} cells of cliff face drawn wet"
    )
    # Whatever it did add stands no deeper than the channel's own 0.9 m.
    if stats["width_added_cells"]:
        assert stats["fill_added_depth_max_m"] <= 0.9 + 1e-4


def test_lateral_fill_is_connected_and_never_deeper_than_its_own_reach(_real_kernels):
    """Every wet cell has a wet DESCENDING path to a drawn cell.

    That is what "connected to the channel" has to mean for water, and here it
    is a property of the rule rather than a check the rule performs: a cell and
    its receiver share a level and the receiver is lower. Walked explicitly on
    NOISY ground, because the argument is only as good as the forest it assumes.
    """
    rng = np.random.default_rng(7)
    n = 80
    cx = n // 2
    z, water = _valley(n, 0.05, incision=0.6, depth=1.1)
    z = np.ascontiguousarray(z + rng.normal(0.0, 0.02, (n, n)), np.float32)
    water[:, cx] = z[:, cx] + 1.1
    rec = _flow_forest(z)

    out, _ = pipeline._water.fill_to_local_surface(water, z, rec, cell_m=1.875)
    wet = np.isfinite(out).ravel()
    src = np.isfinite(water).ravel()
    zf = z.ravel()
    of = out.ravel()
    rf = np.asarray(rec, np.int64).ravel()
    walked = 0
    for c in np.flatnonzero(wet & ~src):
        cur = int(c)
        steps = 0
        while not src[cur]:
            nxt = int(rf[cur])
            assert nxt >= 0, f"{c} drains off the domain but is wet"
            assert wet[nxt], f"{c} is wet but its receiver {nxt} is dry"
            assert of[nxt] == of[c], "the path changed level"
            cur = nxt
            steps += 1
            assert steps < n * n
        # and it never stands deeper than the reach it belongs to
        assert of[c] - zf[c] <= of[cur] - zf[cur] + 1e-4
        walked += 1
    assert walked > 100, walked


def test_lateral_fill_leaves_the_centreline_untouched(_real_kernels):
    """The fill ADDS cells. It must not move one that was already drawn.

    The guardrail on this change is that the water surface still never rises
    going downstream, and that property belongs to ``graded_water_surface``.
    This is the assertion that the extent step cannot break it: every finite
    cell of the input is the same float in the output.
    """
    n = 56
    cx = n // 2
    z, water = _valley(n, 0.03, incision=0.8, depth=1.0)
    out, _ = pipeline._water.fill_to_local_surface(
        water, z, _flow_forest(z), cell_m=1.875)
    src = np.isfinite(water)
    np.testing.assert_array_equal(out[src], water[src])
    assert np.isfinite(out).sum() > src.sum()
    assert np.all(np.diff(out[:, cx]) <= 0.0)


def test_lateral_fill_treats_a_registered_basin_as_a_barrier(_real_kernels):
    """A basin is not merely a hole in the output; water may not pass THROUGH it.

    Its surface is already on the wire in SECTION_BASIN_TABLE and the client
    composes the two samplers, so a cell whose water would arrive by way of a
    basin has to be left to the basin's own row -- writing it here would be a
    second, freely-disagreeing copy of a shipped fact.
    """
    n = 40
    z, water = _valley(n, 0.02, incision=1.0, depth=1.5)
    rec = _flow_forest(z)
    excl = np.zeros((n, n), bool)
    excl[20, :] = True                      # a basin straddling the whole reach
    water[excl] = np.nan                    # as graded_water_surface leaves it

    out, _ = pipeline._water.fill_to_local_surface(
        water, z, rec, cell_m=1.875, exclude=excl)
    assert not np.isfinite(out[excl]).any(), "the fill wrote inside a basin"
    free, _ = pipeline._water.fill_to_local_surface(water, z, rec, cell_m=1.875)
    assert np.isfinite(out).sum() < np.isfinite(free).sum(), (
        "excluding a basin removed nothing; the mask is not doing anything"
    )


def test_lateral_fill_is_dry_where_nothing_drains_to_drawn_water(_real_kernels):
    """No local surface, no water -- and the count is reported, not swallowed.

    A cell whose flow path leaves the domain without meeting drawn water has no
    answer to inherit. That is a different thing from "the terrain contained the
    water here" and the two look identical in a wet count, so the stats separate
    them.
    """
    n = 32
    z = np.ascontiguousarray(
        np.broadcast_to(100.0 - np.arange(n, dtype=np.float32)[:, None] * _DOWN,
                        (n, n)), np.float32)
    water = np.full((n, n), np.nan, np.float32)     # nothing drawn at all
    out, stats = pipeline._water.fill_to_local_surface(
        water, z, _flow_forest(z), cell_m=1.875)
    assert not np.isfinite(out).any()
    assert stats["width_added_cells"] == 0.0
    assert "fill_drains_to_channel_frac" not in stats  # no sources, no report

    z2, w2 = _valley(n, 0.02)
    out2, stats2 = pipeline._water.fill_to_local_surface(
        w2, z2, _flow_forest(z2), cell_m=1.875)
    assert 0.0 < stats2["fill_drains_to_channel_frac"] <= 1.0
    assert np.isfinite(out2).sum() > np.isfinite(w2).sum()


def test_extent_mode_is_enumerated_and_a_typo_is_refused():
    """A misspelt mode must be a refusal, not a silent centreline."""
    for mode in BakeConstants.EXTENT_MODES:
        dataclasses.replace(pipeline.CONSTANTS, water_extent_mode=mode)
    with pytest.raises(ValueError, match="water_extent_mode"):
        dataclasses.replace(pipeline.CONSTANTS, water_extent_mode="lateral-fill")


# --------------------------------------------------------------------------
# bake_ver 14 -- face contact. See water.bridge_to_face_contact.
# --------------------------------------------------------------------------

def _diagonal_reach(n=24, drop=0.3, depth=0.2):
    """A one-pixel-wide reach running DIAGONALLY down a plane.

    This is not a contrived fixture: it is what the shipped bv13 corridor does
    on steep ground. The D8 path steps diagonally, the lateral fill adds nothing
    because the bed rises within one pixel to either side, and the drawn water
    is a chain of single pixels touching at their CORNERS.
    """
    r = np.arange(n, dtype=np.float32)[:, None]
    c = np.arange(n, dtype=np.float32)[None, :]
    z = np.ascontiguousarray(100.0 - (r + c) * drop, np.float32)
    water = np.full((n, n), np.nan, np.float32)
    d = np.arange(n)
    water[d, d] = z[d, d] + depth
    return z, water


def test_face_contact_stats_sees_the_break_the_wet_mask_cannot(_real_kernels):
    """THE MEASUREMENT THE OLD PROBES WERE MISSING, as one assertion.

    A diagonal chain is ONE component to any mask labeller -- it is 8-connected
    by construction. The client draws one flat slab per fine pixel and two
    diagonal slabs share a corner, not a face, so what is actually drawn is `n`
    separate objects with air between them. Both numbers come out of the same
    call here so they cannot be quoted apart.
    """
    n = 24
    z, water = _diagonal_reach(n)
    st = pipeline._water.face_contact_stats(water, z)
    assert st["contact_wet_cells"] == n
    assert st["contact_plan8_components"] == 1.0, st
    assert st["contact_face_components"] == float(n), st
    assert st["contact_isolated_frac"] == 1.0, st
    assert st["contact_shatter_ratio"] == float(n), st


def test_face_contact_bridge_connects_a_diagonal_reach(_real_kernels):
    """THE ACCEPTANCE CRITERION: adjacent wet cells along the flow share a FACE.

    Asserted as a connectivity property of the output, not by eye and not as a
    distribution moving in the right direction.
    """
    n = 24
    z, water = _diagonal_reach(n)
    out, stats = pipeline._water.bridge_to_face_contact(water, z, cell_m=1.875)
    st = pipeline._water.face_contact_stats(out, z)
    assert st["contact_face_broken"] == 0.0, st
    assert st["contact_face_components"] == 1.0, st
    assert st["contact_isolated_cells"] == 0.0, st
    # It bought that with ONE cell per diagonal step and nothing else.
    assert stats["bridge_corner_added"] == float(n - 1), stats
    assert stats["bridge_corner_refused"] == 0.0, stats


def test_face_contact_bridge_is_exactly_a_no_op_on_ponded_water(_real_kernels):
    """THE CONSTRAINT THAT MAKES IT SAFE, and it is exact rather than tolerated.

    A still pool is a set of cells that share one level and each stands below
    it. Every wet neighbour's bed is therefore already under this cell's
    surface, so the bridge's max() finds nothing to raise -- not "raises very
    little", nothing, byte for byte. That is what lets the same rule run
    everywhere without a slope threshold, and a threshold is what would put a
    visible seam along the river.

    The bed is TILTED under the pool so this cannot pass by both sides being
    equal; the pool is what makes the surface level, not the ground.
    """
    n = 40
    z = np.ascontiguousarray(
        50.0 - np.arange(n, dtype=np.float32)[:, None] * 0.05
        - np.arange(n, dtype=np.float32)[None, :] * 0.02, np.float32)
    level = float(z.max()) + 0.5
    water = np.full((n, n), np.nan, np.float32)
    pool = np.zeros((n, n), bool)
    pool[6:34, 6:34] = True
    water[pool] = np.float32(level)
    assert (level - z[pool] >= 0.1).all(), "premise: the whole pool is submerged"

    out, stats = pipeline._water.bridge_to_face_contact(water, z, cell_m=1.875)
    assert stats["bridge_raised_cells"] == 0.0, stats
    assert stats["bridge_corner_added"] == 0.0, stats
    np.testing.assert_array_equal(np.isfinite(out), np.isfinite(water))
    np.testing.assert_array_equal(out[pool], water[pool])
    # And the cross-section is still level to the float.
    assert out[20, 6:34].max() == out[20, 6:34].min()


def test_face_contact_bridge_never_lifts_water_above_water_already_beside_it(
        _real_kernels):
    """THE NON-FLOODING BOUND, which is what makes the raise safe to be unbounded.

    A wet cell stands at least ``min_depth`` under its own surface, so raising a
    neighbour to that cell's BED can never lift it above that cell's SURFACE.
    Two consequences, both asserted:

      * the maximum of the water surface over the whole plane does not move at
        all -- not "moves a little", the same float;
      * every raised cell has a cell it TOUCHES whose surface is at least as
        high, so the raise is always anchored on water that was already drawn
        beside it rather than invented.

    Without that bound the rule would be a flood with a connectivity argument
    attached, which is what ``fill_to_local_surface``'s own docstring records
    happening once already on this branch.
    """
    rng = np.random.default_rng(11)
    n = 48
    z, water = _diagonal_reach(n, drop=0.9, depth=0.15)
    z = np.ascontiguousarray(z + rng.normal(0.0, 0.05, (n, n)), np.float32)
    d = np.arange(n)
    water[d, d] = z[d, d] + 0.15
    out, _ = pipeline._water.bridge_to_face_contact(water, z, cell_m=1.875)

    assert np.nanmax(out) == np.nanmax(water)
    nb = np.full((n, n), -np.inf, np.float32)
    got = np.where(np.isfinite(out), out, np.float32(-np.inf))
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        sl_t = (slice(max(dy, 0), n + min(dy, 0)), slice(max(dx, 0), n + min(dx, 0)))
        sl_s = (slice(max(-dy, 0), n + min(-dy, 0)), slice(max(-dx, 0), n + min(-dx, 0)))
        np.maximum(nb[sl_t], got[sl_s], out=nb[sl_t])
    lifted = np.isfinite(out) & np.isfinite(water) & (out > water)
    assert lifted.any(), "premise: this fixture must actually exercise the raise"
    assert (out[lifted] <= nb[lifted] + 1e-5).all(), (
        float((out[lifted] - nb[lifted]).max()))


def test_face_contact_bridge_keeps_the_surface_descending_downstream(_real_kernels):
    """The one property this must not break, on the reach it most affects.

    A straight steep reach: every cell's surface is raised to the bed of the
    cell above it, so the surface becomes bed-parallel -- and a bed-parallel
    surface on a descending bed still descends. Checked cell by cell rather than
    resampled, which is stricter than the shipped 100 m / 250 m gate.
    """
    n = 64
    cx = n // 2
    down = 0.6                                       # a 0.32 gradient at 1.875 m
    z, water = _valley(n, 0.5, incision=0.4, depth=0.25, down=down)
    out, stats = pipeline._water.bridge_to_face_contact(water, z, cell_m=1.875)
    assert stats["bridge_raised_cells"] > 0, stats
    prof = out[:, cx]
    assert np.isfinite(prof).all()
    assert np.all(np.diff(prof) <= 1e-6), np.diff(prof).max()
    # ... and the reach it raised is now continuous, which is the whole point.
    st = pipeline._water.face_contact_stats(out, z)
    assert st["contact_face_broken"] == 0.0, st


def test_face_contact_bridge_treats_a_registered_basin_as_a_barrier(_real_kernels):
    """A corner inside a registered basin is not this function's to wet.

    The basin's surface is already on the wire in SECTION_BASIN_TABLE and the
    client composes the two samplers, so writing a level here would be a second
    copy of a shipped fact, free to disagree with the first -- the same reason
    ``fill_to_local_surface`` refuses to fill through one.
    """
    n = 24
    z, water = _diagonal_reach(n)
    excl = np.zeros((n, n), bool)
    d = np.arange(1, n)
    excl[d, d - 1] = True                    # every lower corner of the chain
    excl[d - 1, d] = True                    # and every upper one
    out, stats = pipeline._water.bridge_to_face_contact(
        water, z, cell_m=1.875, exclude=excl)
    assert not np.isfinite(out[excl]).any(), "the bridge wrote inside a basin"
    assert stats["bridge_corner_added"] == 0.0, stats
    assert stats["bridge_corner_refused"] == float(n - 1), stats


def test_face_contact_bridge_refuses_a_corner_that_stands_above_the_water(
        _real_kernels):
    """WHERE IT STOPS, and it stops rather than dragging the reach up to meet it.

    A diagonal whose two corners are both a rise cannot be bridged without
    putting water on that rise, and the bridge would then raise the whole reach
    to the rise's own bed. It is refused and counted instead, which is the
    residual the acceptance statistic legitimately still sees.
    """
    n = 12
    z, water = _diagonal_reach(n, drop=0.3, depth=0.2)
    d = np.arange(1, n)
    z[d, d - 1] += 5.0
    z[d - 1, d] += 5.0
    out, stats = pipeline._water.bridge_to_face_contact(water, z, cell_m=1.875)
    assert stats["bridge_corner_added"] == 0.0, stats
    assert stats["bridge_corner_refused"] == float(n - 1), stats
    np.testing.assert_array_equal(np.isfinite(out), np.isfinite(water))


def test_face_contact_bridge_is_a_hashed_product_constant_and_can_be_switched_off():
    """bake_ver 13's plane must stay reproducible, and the switch must be visible.

    A constant that claims to reproduce a previous bake has to be able to, and a
    product constant that fed no identity would change written bytes under an
    unchanged namespace -- which is the failure ``product_identity_payload``
    exists to make impossible.
    """
    assert "water_face_contact_bridge" in BakeConstants.PRODUCT_FIELDS
    on = pipeline.product_identity_payload(pipeline.CONSTANTS)
    off = pipeline.product_identity_payload(
        dataclasses.replace(pipeline.CONSTANTS, water_face_contact_bridge=False))
    assert on != off
    # 14 -> 15 on 2026-08-06, when F6 and the two slope terms were flipped on
    # together. The literal is deliberate: it forces a conscious edit at every
    # roll instead of letting the version drift silently, which is why this is
    # pinned rather than read from pipeline.BAKE_VERSION.
    assert on["bake_version"] == 20

    n = 24
    z, water = _diagonal_reach(n)
    st = pipeline._water.face_contact_stats(water, z)
    assert st["contact_face_components"] == float(n)


def _staircase_parent(n=6, cell=40.0):
    """Parent that drains strictly east, with a distinguishable accumulation."""
    return pipeline.FlowSuperblock(
        level=0, sx=0, sy=0, tiles_per_side=1, cell_m=cell, origin_m=(0.0, 0.0),
        acc=np.tile(np.arange(1, n + 1, dtype=np.float32), (n, 1)) * 1000.0,
        filled=np.tile(
            np.arange(n, 0, -1, dtype=np.float32) * 10.0, (n, 1)),
        missing_tiles=(),
        q=np.tile(np.arange(1, n + 1, dtype=np.float64), (n, 1)) * 7.0,
    )


def test_target_slice_none_matches_the_full_slice():
    """The default must be exactly the historical behaviour.

    Not a tautology: `target_slice=None` and `slice(0, n)` travel different
    branches, and if they disagreed every shipped tile would move the day the
    parameter was added.
    """
    src = _staircase_parent()
    child = np.zeros((12, 12), np.float32)
    child[5, 5] = -1.0
    common = dict(child_origin_m=(0.0, 0.0), child_cell_m=20.0, src=src,
                  d8_fn=ref_d8)
    a = pipeline.inject_edge_inflow(child_z=child, **common)
    b = pipeline._scatter_entries(
        pipeline._edge_entries(child_z=child, target_slice=slice(0, 12),
                               **common),
        src.acc, child.shape)
    assert np.array_equal(a, b)


def test_target_slice_delivers_into_the_inner_rect_not_onto_its_edge():
    """HYDROLOGY_RESIDUALS #7: crossings taken against the INTERIOR.

    With the default, an entry cell is one outside the whole child. Restricted
    to the inner rectangle, the crossing moves inward -- which is the entire
    content of the fix, and the reason a stream that only ever runs through the
    apron stops being counted as if it had reached the tile.
    """
    src = _staircase_parent()
    child = np.zeros((12, 12), np.float32)
    # A thalweg inside the footprint of parent cell (row 2, col 2).
    child[5, 4] = -1.0
    inner = slice(4, 8)  # world [80, 160) on both axes
    entries = pipeline._edge_entries(
        child_z=child, child_origin_m=(0.0, 0.0), child_cell_m=20.0,
        src=src, d8_fn=ref_d8, target_slice=inner)
    got = pipeline._scatter_entries(entries, src.acc, child.shape)

    # Parent cells whose centre is inside [80,160) are rows/cols {2,3}. The
    # staircase drains east, so the crossings are (2,1) and (3,1) -- and NOT
    # (2,2)->(2,3), whose receiver's area is already inside.
    assert got.sum() == pytest.approx(src.acc[2, 1] + src.acc[3, 1])
    # EVERY deposit lands inside the target rectangle.
    ys, xs = np.nonzero(got)
    assert ys.min() >= inner.start and ys.max() < inner.stop
    assert xs.min() >= inner.start and xs.max() < inner.stop
    # And it found the thalweg rather than smearing across the footprint.
    assert got[5, 4] == pytest.approx(src.acc[2, 1])


def test_interior_rim_injection_moves_discharge_but_not_area():
    """The constant is water-only by construction; assert it, do not assume it.

    The area currency must keep the padded-rim crossings so `acc`, and
    therefore `A^m` and every shipped height, cannot move.
    """
    src = _staircase_parent()
    child = np.zeros((12, 12), np.float32)
    child[5, 4] = -1.0
    common = dict(child_origin_m=(0.0, 0.0), child_cell_m=20.0, src=src,
                  d8_fn=ref_d8)
    padded = pipeline._edge_entries(child_z=child, **common)
    inner = pipeline._edge_entries(child_z=child, target_slice=slice(4, 8),
                                   **common)
    area_padded = pipeline._scatter_entries(padded, src.acc, child.shape)
    q_padded = pipeline._scatter_entries(inner, src.q, child.shape,
                                         dtype=np.float64)
    # The two currencies now enter at DIFFERENT cells. That is the invariant
    # `water_inject_at_interior_rim` knowingly trades away; pinning it here so
    # the trade stays visible rather than becoming folklore.
    assert np.nonzero(area_padded)[0].tolist() != np.nonzero(q_padded)[0].tolist()
    assert q_padded.sum() == pytest.approx(src.q[2, 1] + src.q[3, 1])


# ---------------------------------------------------------------------------
# F3: SLOPE IN THE DEPTH LAW
# ---------------------------------------------------------------------------
#
# water_depth_m was Leopold & Maddock hydraulic geometry -- depth from discharge
# and nothing else -- which is a fit to lowland rivers at roughly constant
# slope. This world's long profile runs 173 -> 29 m/km on the wet block alone,
# so a law with no S in it puts too much water on steep upper reaches and too
# little on flat lower ones.


def test_slope_term_is_bit_identical_at_the_reference_gradient():
    """The property that keeps every existing measurement valid.

    Architecture §4 records observed depth matching the Q law to three
    significant figures across three decades of discharge. The slope term
    enters as a RATIO against SLOPE_REF_M_PER_M precisely so that agreement is
    preserved exactly rather than approximately -- if this drifts, every depth
    number in the docs silently stops describing the bake.
    """
    import numpy as np
    from terrain_service.bake import water as w

    q = np.array([1e6, 1e7, 1e8, 1e9])
    base = w.water_depth_m(q)
    at_ref = w.water_depth_m(q, slope=np.full(q.shape, w.SLOPE_REF_M_PER_M))
    assert np.array_equal(base, at_ref)


def test_slope_term_makes_steep_reaches_shallower_and_flat_ones_deeper():
    """The whole point of F3, in the direction normal-depth flow requires.

    depth goes as (Q / sqrt(S)) ** (3/5), so S enters as S ** -0.3: steeper is
    shallower. The current law has this term missing entirely, which is why
    bridge_to_face_contact exists as a hand-built substitute for it.
    """
    import numpy as np
    from terrain_service.bake import water as w

    q = np.full(3, 1e7)
    base = w.water_depth_m(q)
    steep = w.water_depth_m(q, slope=np.full(3, 0.173))  # the block's head
    flat = w.water_depth_m(q, slope=np.full(3, 0.029))   # its mouth

    assert np.all(steep < base)
    assert np.all(flat > base)


def test_slope_term_clamps_instead_of_diverging_on_the_epsilon_fill_floor():
    """A near-zero slope must not send depth to infinity.

    58% of river cells sit on the epsilon-fill floor, where the "slope" is not
    terrain at all but the fill's own increment. Unclamped, S ** -0.3 diverges
    there -- on the majority of the network.
    """
    import numpy as np
    from terrain_service.bake import water as w

    q = np.full(4, 1e7)
    base = w.water_depth_m(q)
    at_zero = w.water_depth_m(q, slope=np.zeros(4))

    assert np.all(np.isfinite(at_zero))
    assert np.all(at_zero <= base * (w.SLOPE_RATIO_MIN ** w.SLOPE_DEPTH_EXP) + 1e-9)


def test_slope_to_receiver_measures_a_diagonal_step_as_root_two():
    """Treating every D8 step as one cell would overstate diagonals by 41%.

    The receiver forest is the bake's own, and it is the same one the
    incision's slope term was taken along -- so a depth law reading it is
    reading the geometry the channel was actually cut with.
    """
    import numpy as np
    from terrain_service.bake import water as w

    # 2x2, cell 1 m. Cell 3 (bottom-right) drains diagonally to cell 0, one
    # metre down; cell 1 drains orthogonally to cell 0, also one metre down.
    z = np.array([[0.0, 1.0], [1.0, 1.0]], np.float64)
    rec = np.array([[0, 0], [0, 0]], np.int64)  # everything points at cell 0

    s = w.slope_to_receiver(z, rec, cell_m=1.0)
    assert s[0, 0] == 0.0                       # a pit points at itself
    assert s[0, 1] == pytest.approx(1.0)        # orthogonal: 1 m over 1 m
    assert s[1, 1] == pytest.approx(1.0 / np.sqrt(2.0))  # diagonal: 1 m over sqrt(2) m


def test_the_water_physics_constants_are_on_and_identity_covered():
    """The three that were flipped together at bake_ver 15, 2026-08-06.

    They were each off by default while unproven, and flipping any one of them
    rolls bake_ver and invalidates every baked water plane -- so they were
    decided together and rolled ONCE rather than three times:

      * water_inject_at_interior_rim -- F6. The pyramid delivered discharge onto
        the tile's 960 m apron instead of into the tile. Measured 300x on
        (-7,-5): 0.041 -> 12.40 m3/s.
      * water_slope_in_depth -- F3. Leopold & Maddock depth has no slope term,
        on a profile running 173 -> 29 m/km.
      * water_slope_in_extent -- the same missing physics in the extent rule, so
        water concentrates on steep ground instead of spreading as a sheet.

    Identity coverage is the half that must never regress: a product constant
    that fed no identity would change written bytes under an unchanged
    namespace, which is exactly what product_identity_payload exists to prevent.
    """
    from terrain_service.bake.pipeline import BakeConstants

    c = BakeConstants()
    assert c.water_inject_at_interior_rim is True
    assert c.water_slope_in_depth is True
    assert c.water_slope_in_extent is True
    for name in ("water_inject_at_interior_rim", "water_slope_in_depth",
                 "water_slope_in_extent"):
        assert name in BakeConstants.PRODUCT_FIELDS


def test_channel_exponents_match_the_mirror():
    """water.py and channel.h must agree, and the identity must cover both.

    The laws live in two places on purpose -- Python decides the baked plane,
    channel.h's fixed point decides what the client computes -- and the docs
    say they "agree by construction". They only agree by construction if
    something checks, because nothing in either file can see the other.

    The identity half matters more than the arithmetic half. These were module
    constants outside every payload, so editing them changed written bytes
    under an unchanged fine_provider_id: a namespace holding two mutually
    incompatible waters with no way to tell them apart.
    """
    import re
    from pathlib import Path

    from terrain_service.bake import water as w
    from terrain_service.bake.pipeline import BakeConstants

    c = BakeConstants()
    assert w.CHANNEL_WIDTH_EXP == c.channel_width_exp_q8 / 256.0
    assert w.CHANNEL_DEPTH_EXP == c.channel_depth_exp_q8 / 256.0
    assert "channel_width_exp_q8" in BakeConstants.PRODUCT_FIELDS
    assert "channel_depth_exp_q8" in BakeConstants.PRODUCT_FIELDS

    header = Path(__file__).resolve().parents[2] / "voxel-core/include/voxelcore/channel.h"
    src = header.read_text(encoding="utf-8", errors="ignore")
    wq8 = int(re.search(r"kChannelWidthExpQ8\s*=\s*(\d+)", src).group(1))
    dq8 = int(re.search(r"kChannelDepthExpQ8\s*=\s*(\d+)", src).group(1))
    assert wq8 == c.channel_width_exp_q8, "channel.h width exponent drifted from the bake"
    assert dq8 == c.channel_depth_exp_q8, "channel.h depth exponent drifted from the bake"


def test_slope_in_extent_narrows_a_steep_reach_and_leaves_a_floodplain_alone():
    """Water concentrates on a slope and spreads on the flat, from one rule.

    The owner's report, flying the marker: water "not being placed on a
    downslope where gravity would actually guide and push the water on a path
    of least resistance". That is water failing to CONCENTRATE -- fast water on
    a steep bed occupies less cross-section for the same discharge.

    Asserted as a COMPARISON rather than an absolute count, because the point
    is the difference between steep and flat, not a tuned number.
    """
    import numpy as np
    from terrain_service.bake import water as w

    n = 48
    cx = n // 2

    def run(drop_per_row, slope_on):
        # A V-shaped cross-section descending in +y at drop_per_row metres.
        z = np.zeros((n, n), np.float32)
        for r in range(n):
            for c in range(n):
                z[r, c] = 100.0 - r * drop_per_row + abs(c - cx) * 0.05
        # Receivers must CONVERGE on the channel, or nothing off-centre drains
        # into it and the rule correctly refuses to spread -- the level is
        # anchored on the first drawn cell of a cell's own downstream path, not
        # on the nearest one. A first draft of this fixture pointed every column
        # straight down and measured no fill at all, from either arm.
        rec = np.zeros(n * n, np.int64)
        for r in range(n):
            for c in range(n):
                if r + 1 >= n:
                    rec[r * n + c] = r * n + c
                    continue
                step = 0 if c == cx else (1 if c < cx else -1)
                rec[r * n + c] = (r + 1) * n + (c + step)
        water = np.full((n, n), np.nan, np.float32)
        water[:, cx] = z[:, cx] + 0.6
        slope = w.slope_to_receiver(z, rec.reshape(n, n), cell_m=1.875) if slope_on else None
        out, _ = w.fill_to_local_surface(
            water, z, rec.reshape(n, n), cell_m=1.875, slope=slope)
        return int(np.isfinite(out).sum())

    # Flat-ish reach: the slope term must barely touch it.
    flat_off = run(0.002, False)
    flat_on = run(0.002, True)
    # Steep reach: it must narrow.
    steep_off = run(0.30, False)
    steep_on = run(0.30, True)

    assert steep_on < steep_off, "the slope term did not narrow a steep reach"
    assert flat_on >= steep_on, "a floodplain must not end up narrower than a gorge"
    # And the narrowing must be a slope effect, not a blanket reduction.
    assert (steep_off - steep_on) > (flat_off - flat_on)


def test_neighbour_consistency_removes_uphill_water_without_drying_a_cell():
    """The two properties that make this pass safe to apply unconditionally.

    Measured motivation: on tile (-4,-4) at bake_ver 15, 13.59% of downstream
    steps along traced reaches have the water surface RISING (p90 627 mm
    against a 100 mm wire LSB), and 100% of them are between touching pixels.
    enforce_descent does not cover it -- it guarantees descent along the D8
    RECEIVER FOREST, and two adjacent channel pixels can drain to different
    receivers.
    """
    import numpy as np
    from terrain_service.bake import water as w

    # A ground ramp descending in +x, with one cell holding water ABOVE the
    # water of its uphill neighbour -- the impossible state.
    z = np.zeros((3, 6), np.float32)
    for x in range(6):
        z[:, x] = 10.0 - x
    water = np.full((3, 6), np.nan, np.float32)
    water[1, :] = z[1, :] + 0.5
    water[1, 3] = z[1, 3] + 3.0          # stands above its uphill feeder

    assert water[1, 3] > water[1, 2]     # the defect exists in the fixture
    out, stats = w.enforce_neighbour_consistency(water, z)

    # Property 1: no wet cell stands higher than adjacent water on higher ground.
    for x in range(1, 6):
        if np.isfinite(out[1, x]) and np.isfinite(out[1, x - 1]):
            assert out[1, x] <= out[1, x - 1] + 1e-6

    # Property 2: IT CANNOT DRY A CELL. The replacement value is an upstream
    # neighbour's surface and that neighbour stands on higher ground, so the new
    # surface still clears this cell's own ground. This is what makes the pass
    # safe to run everywhere rather than only on flagged cells.
    before_wet = np.isfinite(water)
    after_wet = np.isfinite(out) & (out > z)
    assert int(after_wet.sum()) == int(before_wet.sum())

    assert stats["level_consistency_sweeps"] == 1.0
    assert stats["level_consistency_lowerings"] > 0


def test_neighbour_consistency_leaves_a_consistent_plane_alone():
    """It must be a no-op where nothing is wrong, or it is a smoothing filter.

    The rule that was tried FIRST -- equalise every connected adjacent pair,
    "water finds its level" -- fails this: it drags a legitimate downstream
    gradient toward the global minimum, measured at a median 925 mm drop on
    essentially every wet cell. A river has a gradient by definition.
    """
    import numpy as np
    from terrain_service.bake import water as w

    z = np.zeros((3, 6), np.float32)
    for x in range(6):
        z[:, x] = 10.0 - x
    water = np.full((3, 6), np.nan, np.float32)
    water[1, :] = z[1, :] + 0.5          # constant depth on a descending bed

    out, stats = w.enforce_neighbour_consistency(water, z)
    np.testing.assert_allclose(out[1, :], water[1, :], atol=1e-6)
    assert stats["level_consistency_lowerings"] == 0.0


def _lateral_fixture_two_banks():
    """A flat reach with a channel and two banks holding DIFFERENT levels.

    The left bank inherited its surface from a channel cell further upstream --
    which is exactly what ``fill_to_local_surface`` does when two side-by-side
    cells drain to different receivers -- so it stands 800 mm above the water on
    the other side of the same river. Both banks are floodplain, not channel.
    """
    import numpy as np

    n = 6
    z = np.full((5, n), 20.0, np.float32)
    z[2, :] = 10.0          # the bed
    z[1, :] = 10.3          # left bank
    z[3, :] = 10.3          # right bank
    rec = np.full((5, n), -1, np.int64)
    for x in range(n):
        rec[1, x] = 2 * n + x          # both banks drain into the channel
        rec[3, x] = 2 * n + x
        if x < n - 1:
            rec[2, x] = 2 * n + x + 1  # the channel runs +x
    drawn = np.zeros((5, n), bool)
    drawn[2, :] = True
    water = np.full((5, n), np.nan, np.float32)
    water[2, :] = 11.0
    water[3, :] = 11.0
    water[1, :] = 11.8                  # perched, from an upstream reach
    return z, rec, drawn, water


def test_lateral_equal_level_levels_two_banks_that_inherited_different_levels():
    """The defect the rule exists for: one bank perched, the other not.

    Measured motivation, on the shipped bv18 plane of tile (-4,-4) in a 2048^2
    window: 20.76% of adjacent wet pairs differ by more than one voxel and 61.4%
    of those pairs drain to DIFFERENT receivers, so no rule enforced along the
    D8 forest -- which is every level rule in the module -- ever compares them.
    """
    import numpy as np
    from terrain_service.bake import water as w

    z, rec, drawn, water = _lateral_fixture_two_banks()
    assert water[1, 2] - water[3, 2] == pytest.approx(0.8)   # the defect exists

    out, stats = w.equalize_lateral_levels(water, z, rec, drawn)

    tol = w.LATERAL_LEVEL_TOL_M
    # The two banks end level with the channel between them, to the tolerance.
    for x in range(6):
        assert np.isfinite(out[1, x])
        assert out[1, x] <= out[2, x] + tol + 1e-5
        assert out[3, x] == pytest.approx(11.0)
    # It LOWERED; it did not raise anything, anywhere.
    assert out[1, 2] < water[1, 2]
    fin = np.isfinite(out) & np.isfinite(water)
    assert np.all(out[fin] <= water[fin] + 1e-6)
    # And it did not dry the bank: 11.1 still clears 10.3 by well over a voxel.
    assert stats["lateral_dried_cells"] == 0.0
    assert stats["lateral_wet_after"] == stats["lateral_wet_before"]
    assert stats["lateral_lowered_cells"] == 6.0
    assert stats["lateral_steps_over_tol_after"] == 0.0
    assert stats["lateral_steps_over_tol_before"] > 0.0


def test_lateral_equal_level_does_not_cross_a_genuine_barrier():
    """Two pools with dry ground between them keep their own levels.

    This is the property that separates the rule from "water finds its level"
    globally. It only ever compares two cells that are BOTH already wet, so a
    bar of dry ground is not something it can reach across -- which is why the
    rule needs no barrier test of its own, and why it cannot join two bodies of
    water the terrain keeps apart.
    """
    import numpy as np
    from terrain_service.bake import water as w

    z = np.full((3, 9), 30.0, np.float32)
    z[1, 1:4] = 10.0        # upper pool floor
    z[1, 4] = 20.0          # THE BAR -- above both water surfaces
    z[1, 5:8] = 5.0         # lower pool floor, 5 m down
    rec = np.full((3, 9), -1, np.int64)
    drawn = np.zeros((3, 9), bool)
    water = np.full((3, 9), np.nan, np.float32)
    water[1, 1:4] = 11.0
    water[1, 5:8] = 6.0
    assert z[1, 4] > max(11.0, 6.0)     # it really is a barrier

    out, stats = w.equalize_lateral_levels(water, z, rec, drawn)

    np.testing.assert_allclose(out[1, 1:4], 11.0, atol=1e-6)
    np.testing.assert_allclose(out[1, 5:8], 6.0, atol=1e-6)
    assert not np.isfinite(out[1, 4])
    assert stats["lateral_lowered_cells"] == 0.0
    assert stats["lateral_steps_over_tol_before"] == 0.0
    assert stats["lateral_wet_after"] == stats["lateral_wet_before"]


def test_lateral_equal_level_leaves_a_descending_river_descending():
    """A reach falling 300 mm per cell -- three voxels -- must not be flattened.

    This is the failure the tolerance exists to prevent, and it is not
    hypothetical: at ``tol = 0`` the same rule took the drawn channel's median
    along-flow step on tile (-4,-4) from 14.5 mm to 0.0 mm. That is
    ``enforce_neighbour_consistency``'s documented "IT IS NOT WATER FINDS ITS
    LEVEL" reproduced exactly -- min-propagation is transitive, so without a
    tolerance it chains the downstream minimum up the whole network.
    """
    import numpy as np
    from terrain_service.bake import water as w

    m = 12
    z = np.full((3, m), 30.0, np.float32)
    z[1, :] = 10.0 - 0.3 * np.arange(m)          # 300 mm per cell
    rec = np.full((3, m), -1, np.int64)
    for x in range(m - 1):
        rec[1, x] = m + x + 1
    drawn = np.zeros((3, m), bool)
    drawn[1, :] = True
    water = np.full((3, m), np.nan, np.float32)
    water[1, :] = z[1, :] + 0.5

    out, stats = w.equalize_lateral_levels(water, z, rec, drawn)

    np.testing.assert_allclose(out[1, :], water[1, :], atol=1e-6)
    assert stats["lateral_lowered_cells"] == 0.0
    assert stats["lateral_dried_cells"] == 0.0
    assert stats["lateral_sweeps"] == 1.0        # nothing to do, one look
    # And the descent itself survives, step by step.
    assert np.all(np.diff(out[1, :]) < 0.0)


def test_lateral_equal_level_exemption_is_the_drawn_channel_not_the_forest():
    """A perched cell that drains into the river is NOT entitled to a gradient.

    The exemption is "both cells carry drawn discharge AND one is the other's
    receiver". Exempting every along-flow pair instead -- the reading the D8
    forest invites -- protects the defect, because a bank cell's receiver IS the
    channel cell beside it. Measured on the crop, that wider exemption leaves
    3.91% of pairs over one voxel against 0.87% and 9,548 cells dry below
    adjacent water against 8,367.
    """
    import numpy as np
    from terrain_service.bake import water as w

    z, rec, drawn, water = _lateral_fixture_two_banks()
    # The perched bank's receiver is the channel cell directly below it, so the
    # pair IS along-flow -- and it is still lowered, because the bank carries no
    # drawn discharge.
    assert rec[1, 2] == 2 * 6 + 2
    out, _ = w.equalize_lateral_levels(water, z, rec, drawn)
    assert out[1, 2] < 11.8, "an along-flow pair off the channel must not be exempt"

    # THE OTHER HALF. A step between two cells that ARE both drawn channel and
    # ARE consecutive on the flow path is a flowing reach's own surface, and the
    # rule must leave it exactly alone however large it is. Same geometry, same
    # 700 mm step, one bit of `drawn` different.
    n = 6
    z2 = np.full((3, n), 30.0, np.float32)
    z2[1, :] = 10.0 - 0.05 * np.arange(n)
    rec2 = np.full((3, n), -1, np.int64)
    for x in range(n - 1):
        rec2[1, x] = n + x + 1
    water2 = np.full((3, n), np.nan, np.float32)
    water2[1, :] = z2[1, :] + 0.5
    water2[1, 2] += 0.7                       # a 700 mm step, both sides
    drawn2 = np.zeros((3, n), bool)

    off, _ = w.equalize_lateral_levels(water2, z2, rec2, drawn2)
    assert off[1, 2] < water2[1, 2] - 0.5, "off the channel it must be levelled"

    drawn2[1, :] = True
    on, stats_on = w.equalize_lateral_levels(water2, z2, rec2, drawn2)
    np.testing.assert_allclose(on[1, :], water2[1, :], atol=1e-6)
    assert stats_on["lateral_lowered_cells"] == 0.0


def test_lateral_equal_level_is_a_product_constant_that_ships_dark():
    """Identity coverage, and a flag that cannot be confused with "did not run".

    Both halves have bitten this module. A water constant outside every payload
    changes written bytes under an unchanged ``fine_provider_id``; and a counter
    that is ABSENT reads as zero in the per-tile log, which is how the monotone
    stage reported "mono=0>0" for an entire session while never executing at
    all. So the pipeline writes ``water_lateral_equal_ran`` in BOTH branches:
    0.0 means off, 1.0 means the rest of the counters are real.
    """
    import numpy as np
    from terrain_service.bake import water as w
    from terrain_service.bake.pipeline import BakeConstants

    c = BakeConstants()
    assert c.water_lateral_equal_level is False, "must ship dark"
    assert "water_lateral_equal_level" in BakeConstants.PRODUCT_FIELDS
    assert "water_lateral_equal_level" not in c.as_payload(), (
        "it cannot move a height, so it belongs in the PRODUCT half only"
    )

    z, rec, drawn, water = _lateral_fixture_two_banks()
    _, stats = w.equalize_lateral_levels(water, z, rec, drawn)
    assert stats["lateral_equal_ran"] == 1.0
    # Every counter that could read as "nothing was wrong" is accompanied by the
    # flag, and the before/after pair is what makes the stage's work visible.
    for key in ("lateral_steps_over_tol_before", "lateral_steps_over_tol_after",
                "lateral_lowered_cells", "lateral_dried_cells",
                "lateral_wet_before", "lateral_wet_after", "lateral_sweeps",
                "lateral_tol_m"):
        assert key in stats


def test_lateral_equal_level_actually_executes_in_the_bake(_real_kernels):
    """The stage must RUN, and the log must be able to tell that it did.

    This is the module's own worst bug, not a hypothetical. The monotone stage
    was guarded by ``locals().get("rec_w") is not None`` with ``del rec_w`` 70
    lines above, so it never executed in any bake -- while the per-tile log
    printed ``mono=0>0`` from the stats dict's default and read as "zero
    violations, nothing to fix". A unit test on the function alone would have
    passed throughout. So this bakes a real tile both ways and checks the flag
    in BOTH branches, plus the one invariant the constant claims: it is a
    PRODUCT constant, so no elevation byte may move.
    """
    import dataclasses

    import numpy as np

    world = ramp_world()
    cl = climate_world(lambda tx, ty: 3000.0)

    def run(**kw):
        # A low perennial threshold, so this small synthetic tile actually
        # carries drawn water; at the production threshold it is bone dry and
        # the test would pass vacuously on an empty plane.
        c = dataclasses.replace(TEST_CONSTS, water_q_perennial_m3_yr=1.0, **kw)
        return pipeline.bake_tile(
            world_seed=20260719, tile_x=0, tile_y=0,
            coarse_fetch=lambda x, y: world.get((x, y)),
            climate_fetch=lambda x, y: cl.get((x, y)),
            kernels=_real_kernels, geom=TEST_GEOM, consts=c)

    off = run()
    on = run(water_lateral_equal_level=True)

    assert off.stats["water_lateral_equal_ran"] == 0.0
    assert on.stats["water_lateral_equal_ran"] == 1.0
    # It did real work on real water -- not a no-op that would make the flag
    # meaningless.
    assert on.stats["water_lateral_steps_over_tol_before"] > 0.0
    assert (on.stats["water_lateral_steps_over_tol_after"]
            < on.stats["water_lateral_steps_over_tol_before"])
    assert on.stats["water_lateral_lowered_cells"] > 0.0
    assert on.stats["water_lateral_wet_after"] <= on.stats["water_lateral_wet_before"]

    # PRODUCT, not payload: the ground is untouched, which is what
    # tools/verify_water_only_change.py checks on real tiles.
    np.testing.assert_array_equal(off.elevation_m, on.elevation_m)
    assert int(np.isfinite(on.water_surface_m).sum()) <= int(
        np.isfinite(off.water_surface_m).sum())


def test_lateral_equal_level_never_wets_a_cell_so_it_cannot_flood():
    """The 209x flood has no way in: the wet set can only shrink.

    ``fill_to_local_surface`` records a lateral rule that took 317,665 wet cells
    to 66,546,420. Every lateral rule in this module is measured against that
    number, and this one is safe for a structural reason rather than a tuned
    one -- it never assigns a level to a dry cell.
    """
    import numpy as np
    from terrain_service.bake import water as w

    rng = np.random.default_rng(20260808)
    z = rng.uniform(0.0, 5.0, (48, 48)).astype(np.float32)
    water = np.where(rng.random((48, 48)) < 0.3,
                     z + rng.uniform(0.2, 3.0, (48, 48)), np.nan).astype(np.float32)
    rec = np.full((48, 48), -1, np.int64)
    drawn = np.zeros((48, 48), bool)

    out, stats = w.equalize_lateral_levels(water, z, rec, drawn)
    wet_in = np.isfinite(water)
    wet_out = np.isfinite(out)
    assert not np.any(wet_out & ~wet_in), "a dry cell was wetted"
    assert stats["lateral_wet_after"] <= stats["lateral_wet_before"]
    fin = wet_out & wet_in
    assert np.all(out[fin] <= water[fin] + 1e-6), "a level was raised"
