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


def ref_accumulate(z, cell_m, p=1.1, inflow=None, *, return_order=False):
    """Own cell area plus any injected inflow. NO routing, on purpose.

    ``return_order`` is accepted because the real kernel hands B2d the
    ascending-elevation order it had to sort anyway; this double has no sweep,
    so it sorts only when asked, which is what keeps the wiring under test.

    Routing is unbounded by nature (that is why the hydrology pyramid exists),
    so a routing reference would make the apron test measure the reference
    instead of the pipeline. What is under test here is that the pipeline
    hands the right ``inflow`` to the right cells.

    Returns float64 m^2, matching the real ``accumulate_mfd``.
    """
    a = np.full(np.shape(z), cell_m * cell_m, np.float64)
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
    assert set(r.cpu_seconds) == set(pipeline.STAGE_ORDER)
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


def test_every_bake_constant_rolls_the_fingerprint():
    """Same rule as DiffusionConfig's: a field belongs in the identity iff
    changing it can change generated bytes. All of these can."""
    base = pipeline.bake_fingerprint()
    seen = {base}
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
    n = len(dataclasses.fields(BakeConstants)) + len(dataclasses.fields(BakeGeometry))
    assert len(seen) == n + 1, "a bake constant is not covered by the fingerprint"


def test_bake_identity_payload_is_json_stable():
    import json

    payload = pipeline.bake_identity_payload()
    assert payload["bake_version"] == pipeline.BAKE_VERSION
    assert payload["stage_order"] == list(pipeline.STAGE_ORDER)
    json.dumps(payload, sort_keys=True)  # must not raise


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
