"""Geomorphology half of the Phase 2 bake: B0 carrier, B1 roughness, B2e incision, B3 thermal.

Every test here is a regression test for a specific way the prototype was wrong, or for a
clause of the frozen `.vxtl` v2 contract. Nothing here depends on timing, because the box
these run on is contended and a wall-clock assertion would be a flaky test that teaches
nothing.

**CI has neither scipy nor numba.** This module therefore imports only numpy at the top
level and reaches for `pytest.importorskip` inside the individual tests that need more.
That is not politeness: a test module which raises on import fails the whole job rather
than one test, and that has already happened in this repo once.
"""

from __future__ import annotations

import importlib
import math
import sys

import numpy as np
import pytest

from terrain_service.bake import incise, noise, thermal

CELL_M = 1.875           # the v2 fine tier's pixel size
REPOSE_DEG = 36.0
TAN_REPOSE = math.tan(math.radians(REPOSE_DEG))


# ======================================================================================
# B0 carrier -- the frozen wire contract
# ======================================================================================

def test_bspline_weights_match_the_normative_spec():
    """`docs/vxtl-v2-format.md` §8, recomputed independently from the published formulas.

    The client evaluates this exact spline on the shipped lattice, so a disagreement here
    is a corrupt world rather than a quality regression. The `sum == 6*1024**3` invariant
    is checkable *exactly* only because `bspline_weights` returns integer numerators.
    """
    t = 1024
    for scale in (1, 2, 4, 8, 16, 32, 64):
        w = noise.bspline_weights(scale)
        assert w.shape == (scale, 4)
        assert w.dtype == np.int64
        for k in range(scale):
            tq = (k * t) // scale
            expect = [
                (t - tq) ** 3,
                3 * tq ** 3 - 6 * tq ** 2 * t + 4 * t ** 3,
                -3 * tq ** 3 + 3 * tq ** 2 * t + 3 * tq * t ** 2 + t ** 3,
                tq ** 3,
            ]
            assert w[k].tolist() == expect, (scale, k)
        assert (w >= 0).all(), "§8 states all four weights are non-negative"
        assert w.sum(axis=1).tolist() == [noise.SPLINE_DEN] * scale


@pytest.mark.parametrize("scale,px_mm", [(8, 30000), (16, 30000)])
def test_weight_phase_matches_the_client_truncdiv_form(scale, px_mm):
    """§8 defines `tq = truncDiv(fx * 1024, pxMm)`; the table is indexed by fine sample.

    Both tiers share one table precisely because that expression turns out not to depend on
    the physical pitch. If someone "optimises" the phase derivation this catches it.
    """
    fine_mm = px_mm // scale
    w = noise.bspline_weights(scale)
    for k in range(scale):
        fx = k * fine_mm
        tq = (fx * 1024) // px_mm          # truncDiv; fx >= 0 so // is truncation here
        assert w[k, 3] == tq ** 3


def test_carrier_reproduces_a_constant_field_exactly():
    pytest.importorskip("scipy")
    for value in (0.0, 137.5, -812.25, 3210.0):
        out = noise.carrier(np.full((16, 12), value), 8)
        assert out.shape == (128, 96)
        assert out.dtype == np.float32
        assert np.all(out == np.float32(value)), value


def test_carrier_interpolates_its_control_points():
    """Fine index `p*scale` must land exactly on coarse index `p`.

    This is the phase convention shared with the client. Anything else puts a half-cell
    shift between the bake and the decoder. The residual is the prefilter's own tolerance
    (scipy's IIR pass is truncated at the boundary) plus float32 output rounding -- both
    are ~1e-7 relative, which is why the assertion is scaled by the field's own magnitude.
    """
    pytest.importorskip("scipy")
    rng = np.random.default_rng(0)
    coarse = rng.normal(size=(24, 20)) * 40.0 + 500.0
    for scale in (4, 8, 16):
        out = noise.carrier(coarse, scale)
        err = np.abs(out[::scale, ::scale].astype(np.float64) - coarse)
        assert err.max() < 1e-5 * np.abs(coarse).max(), (scale, err.max())


def test_carrier_prefilter_preserves_the_fine_band():
    """THE PREFILTER IS THE POINT. Removing it low-passes the source.

    A B-spline *approximates* its control points, so evaluating it on raw samples is a
    smoothing operator: measured on a real tile, detrended H degrades 0.83 -> 1.47 between
    240 m and 30 m without the prefilter, i.e. the carrier ends up smoother than the raster
    it was built from -- in exactly the band the bake exists to extend.

    Here that is reduced to its mechanism: a near-Nyquist checker in the source must
    survive the round trip. The second half of the test drives the *unprefiltered* path
    directly and asserts it is badly attenuated, so this test is known to have power rather
    than merely passing.
    """
    pytest.importorskip("scipy")
    n, scale = 24, 8
    yy, xx = np.mgrid[0:n, 0:n]
    coarse = 10.0 * ((-1.0) ** (xx + yy))          # the worst case: source Nyquist

    kept = noise.carrier(coarse, scale)[::scale, ::scale].astype(np.float64)
    inner = slice(4, n - 4)
    assert np.abs(kept[inner, inner] - coarse[inner, inner]).max() < 1e-3

    # Same evaluation, no prefilter -- what the prototype did first.
    w = noise.bspline_weights(scale).astype(np.float64) / noise.SPLINE_DEN
    tmp = np.empty((n, n * scale))
    noise._upsample_axis1(coarse.astype(np.float64), scale, w, tmp, transposed=False)
    out = np.empty((n * scale, n * scale), dtype=np.float32)
    noise._upsample_axis1(np.ascontiguousarray(tmp.T), scale, w, out, transposed=True)
    raw = out[::scale, ::scale].astype(np.float64)

    retained = np.abs(raw[inner, inner]).max() / np.abs(coarse[inner, inner]).max()
    assert retained < 0.4, (
        f"the unprefiltered path retained {retained:.3f} of the Nyquist amplitude; this "
        "test can no longer tell a prefiltered carrier from a raw one"
    )


def test_carrier_rejects_bad_arguments():
    with pytest.raises(ValueError):
        noise.carrier(np.zeros(8), 8)
    with pytest.raises(ValueError):
        noise.carrier(np.zeros((8, 8)), 0)
    with pytest.raises(ValueError):
        noise.bspline_weights(2.5)


# ======================================================================================
# B1 roughness
# ======================================================================================

def _roughness(shape=(256, 256), slope=0.3, seed=20260729, **kw):
    z = np.zeros(shape, dtype=np.float32)
    s = np.full(shape, slope, dtype=np.float32)
    return noise.roughness(z, CELL_M, s, seed, **kw).astype(np.float64)


def _phase_ratio(field, period):
    """max/min of the axis-aligned |second difference| binned by column phase.

    Box upsampling (`np.kron`) puts *all* of its curvature on the block seams and none
    inside, so one phase bin carries everything and the rest carry zero -> the ratio is
    unbounded. A properly interpolated octave spreads curvature over every phase.
    """
    d2 = np.abs(field[:, 2:] - 2.0 * field[:, 1:-1] + field[:, :-2])
    prof = np.array([d2[:, i::period].mean() for i in range(period)])
    return float(prof.max() / max(prof.min(), 1e-30))


def _d2_median_over_mean(field):
    d2 = np.abs(field[:, 2:] - 2.0 * field[:, 1:-1] + field[:, :-2])
    return float(np.median(d2) / d2.mean())


def test_roughness_has_no_axis_aligned_blocking():
    """LESSON: the pass whose job is natural roughness must not reintroduce the grid.

    The first prototype box-upsampled every octave with `np.kron` -- nearest neighbour --
    and the hillshade came out as a lattice of hard rectangles at every octave scale: the
    exact artifact this project exists to remove. It is invisible in any averaged statistic
    and obvious the instant the field is shaded, so it is checked structurally.

    The `np.kron` control below is not decoration. It is the proof that these two metrics
    can actually see the failure.
    """
    field = _roughness()
    for period in (2, 4, 8, 16, 32):
        assert _phase_ratio(field, period) < 4.0, period
    assert _d2_median_over_mean(field) > 0.3

    rng = np.random.default_rng(1)
    boxy = np.kron(rng.normal(size=(16, 16)), np.ones((16, 16)))
    assert _phase_ratio(boxy, 16) > 100.0, "control failed: metric cannot see box upsampling"
    assert _d2_median_over_mean(boxy) < 0.05, "control failed"


def test_roughness_is_isotropic_between_x_and_y():
    field = _roughness()
    dx = np.abs(field[:, 2:] - 2.0 * field[:, 1:-1] + field[:, :-2]).mean()
    dy = np.abs(field[2:, :] - 2.0 * field[1:-1, :] + field[:-2, :]).mean()
    assert 0.85 < dx / dy < 1.18, dx / dy


def _block_std(field, block):
    n = (field.shape[0] // block) * block
    return float(field[:n, :n].reshape(n // block, block,
                                       n // block, block).mean(axis=(1, 3)).std())


def test_roughness_synthesises_only_octaves_below_the_source_nyquist():
    """Above `src_nyquist_m` the carrier already holds real diffusion-model data.

    Adding noise there fights the model rather than extending it -- the plan's
    "replace, do not layer" rule, the same rule it applies to the client's landform
    octaves. Asserted on the octave set itself, which is the rule, rather than on a band
    statistic: each octave is a B-spline of a random lattice and so is *lowpass*, not
    bandpass, and therefore legitimately carries a 1/sqrt(area) tail above its own
    wavelength. That tail is ordinary fBm behaviour, not a band-ownership violation, and a
    band statistic cannot tell the two apart.
    """
    for cell_m, nyq, expect in (
        (1.875, 30.0, [30.0, 15.0, 7.5]),
        (3.75, 30.0, [30.0, 15.0]),
        (1.875, 60.0, [60.0, 30.0, 15.0, 7.5]),
    ):
        assert noise.octave_wavelengths(cell_m, nyq) == expect, (cell_m, nyq)
    for w in noise.octave_wavelengths(CELL_M, 30.0):
        assert w <= 30.0
        ratio = w / CELL_M
        assert ratio == int(ratio) and int(ratio) & (int(ratio) - 1) == 0, w

    # And the rule binds on the output: letting B1 claim the 240 m band visibly loads the
    # coarse end that the carrier is supposed to own.
    z = np.zeros((512, 512), dtype=np.float32)
    s = np.full((512, 512), 0.3, dtype=np.float32)
    tight = noise.roughness(z, CELL_M, s, 7, src_nyquist_m=30.0).astype(np.float64)
    loose = noise.roughness(z, CELL_M, s, 7, src_nyquist_m=240.0).astype(np.float64)
    block = int(round(120.0 / CELL_M))
    assert _block_std(tight, block) < 0.65 * _block_std(loose, block)


def test_roughness_amplitude_stays_modest():
    """LESSON: B1 is substrate for erosion, not final texture.

    Tuning B1 up until the fine-end spectrum was textbook-correct (H 1.65 -> 0.91) made the
    terrain *worse*: uniform crumpled paper, and the dendritic flow network collapsed into
    disconnected micro-catchments because every noise dimple became its own sink. That took
    ~3.19 m of 30 m-wavelength noise. This bounds the whole field an order of magnitude
    below it, on the steepest ground the slope gain allows.
    """
    typical = _roughness(slope=0.3)
    steep = _roughness(slope=2.0)                     # gain saturates at 2.0
    assert 0.15 < typical.std() < 0.8, typical.std()
    assert steep.std() < 1.2, steep.std()
    assert np.abs(steep).max() < 3.19, "B1 is approaching the amplitude that broke drainage"


def test_roughness_slope_conditioning_is_monotone_and_clipped():
    flat = _roughness(slope=0.0)
    mid = _roughness(slope=0.3)
    steep = _roughness(slope=1.0)
    steeper = _roughness(slope=5.0)
    assert flat.std() < mid.std() < steep.std()
    # Clipped at 0.25 and 2.0 x the reference gain.
    assert np.allclose(flat, 0.25 * mid, rtol=1e-5, atol=1e-6)
    assert np.allclose(steeper, 2.0 * mid, rtol=1e-5, atol=1e-6)


def test_roughness_is_deterministic_and_seed_sensitive():
    a = _roughness(seed=11)
    b = _roughness(seed=11)
    c = _roughness(seed=12)
    assert np.array_equal(a, b)
    assert np.abs(a - c).max() > 0.05


def test_roughness_is_world_anchored():
    """Two overlapping bake domains must agree EXACTLY in their overlap.

    The plan's seam argument is "aprons, not blending": each tile bakes on its domain plus
    a 960 m apron and writes only its interior, and that only works if the noise is a pure
    function of world position. A sequential `default_rng(seed)` walk cannot do this -- its
    values depend on the domain's origin and extent -- which is why the prototype's fBm was
    array-anchored and its seam test had to inject a shared pre-computed field.

    `origin_cells` is optional and defaults to (0, 0), so the four-argument call form is
    unaffected; passing the true offset makes the guarantee below hold.
    """
    big = _roughness(shape=(192, 192), origin_cells=(-64, -64))
    win = _roughness(shape=(96, 96), origin_cells=(-13, 29))
    y0, x0 = -13 - (-64), 29 - (-64)
    assert np.array_equal(big[y0:y0 + 96, x0:x0 + 96], win)


def test_roughness_rejects_bad_arguments():
    z = np.zeros((16, 16), dtype=np.float32)
    with pytest.raises(ValueError):
        noise.roughness(np.zeros(16), CELL_M, np.zeros(16), 1)
    with pytest.raises(ValueError):
        noise.roughness(z, 0.0, np.zeros((16, 16)), 1)


# ======================================================================================
# B2e stream-power incision
# ======================================================================================

def test_stream_power_is_monotone_in_area_and_slope():
    area = np.geomspace(1.0, 1e8, 64)[None, :] * np.ones((4, 1))
    slope = np.full_like(area, 0.1)
    depth = incise.stream_power(area, slope)
    assert np.all(np.diff(depth, axis=1) >= 0.0)

    slope = np.geomspace(1e-4, 2.0, 64)[None, :] * np.ones((4, 1))
    area = np.full_like(slope, 1e5)
    depth = incise.stream_power(area, slope)
    assert np.all(np.diff(depth, axis=1) >= 0.0)


def test_stream_power_respects_the_cap_and_stays_non_negative():
    rng = np.random.default_rng(2)
    area = rng.uniform(0.0, 1e9, size=(64, 64))
    slope = rng.uniform(0.0, 3.0, size=(64, 64))
    for cap in (0.5, 8.0, 25.0):
        depth = incise.stream_power(area, slope, cap_m=cap)
        assert depth.dtype == np.float32
        assert depth.min() >= 0.0
        assert depth.max() <= cap
        assert np.isfinite(depth).all()
    # The cap must actually bind somewhere on this input, or it proves nothing.
    assert incise.stream_power(area, slope, cap_m=8.0).max() == pytest.approx(8.0)


def test_stream_power_handles_degenerate_inputs():
    a = np.array([[0.0, -5.0, 1e6]])
    s = np.array([[0.0, 0.2, -1.0]])
    depth = incise.stream_power(a, s)
    assert np.isfinite(depth).all()
    assert (depth >= 0.0).all()
    assert incise.stream_power(a, s, K=0.0).max() == 0.0
    with pytest.raises(ValueError):
        incise.stream_power(np.zeros((2, 2)), np.zeros((3, 3)))
    with pytest.raises(ValueError):
        incise.stream_power(np.zeros((2, 2)), np.zeros((2, 2)), cap_m=0.0)


def test_channel_initiation_gate_suppresses_hillslopes_not_channels():
    """The gate exists because without it EVERY cell is a channel.

    Measured on real tile (-5,3) at 1.875 m/px before the gate existed: 77.6 % of the
    domain incised by more than one voxel at K = 0.03 and 98.6 % at K = 0.15. Real
    landscapes have a channel head at a critical contributing area, and that area --
    not K -- is what sets drainage density.
    """
    slope = np.full((1, 1), 0.2)
    hill = float(incise.stream_power(np.full((1, 1), 10.0), slope)[0, 0])
    trunk = float(incise.stream_power(np.full((1, 1), 1e7), slope)[0, 0])
    ungated_hill = float(
        incise.stream_power(np.full((1, 1), 10.0), slope, a_crit_m2=0.0)[0, 0])
    ungated_trunk = float(
        incise.stream_power(np.full((1, 1), 1e7), slope, a_crit_m2=0.0)[0, 0])

    # A single-cell hillslope must be suppressed hard...
    assert hill < ungated_hill / 1000.0
    # ...while a trunk channel three decades above A_crit is essentially untouched,
    # so the gate cannot be smuggling in a global reduction of K.
    assert trunk == pytest.approx(ungated_trunk, rel=1e-3)


def test_channel_initiation_gate_is_continuous_and_monotone():
    """A HARD cutoff at A_crit would put a step in incision depth along the contour
    where contributing area crosses it -- a visible seam along a curve, which is the
    same failure class as the 30 m grid seams this project exists to remove."""
    area = np.geomspace(1.0, 1e9, 4096)[None, :]
    slope = np.full_like(area, 0.15)
    depth = incise.stream_power(area, slope, cap_m=1e6)[0]

    assert np.all(np.diff(depth) >= 0.0), "gate must preserve monotonicity in A"
    # No step: the largest jump between adjacent log-spaced samples must stay a small
    # fraction of the full range. A hard cutoff would put ~100 % of the range in one step.
    span = depth.max() - depth.min()
    assert np.diff(depth).max() < 0.01 * span


def test_channel_initiation_gate_defaults_off_reproduce_prior_behaviour():
    """a_crit_m2=0 must recover the pre-gate function EXACTLY, so the parameter can be
    used to reproduce anything measured before it existed."""
    rng = np.random.default_rng(7)
    area = rng.uniform(0.0, 1e8, size=(32, 32))
    slope = rng.uniform(0.0, 1.5, size=(32, 32))
    gated_off = incise.stream_power(area, slope, a_crit_m2=0.0)
    manual = np.minimum(
        np.float32(0.15) * np.power(np.float32(1) * area.astype(np.float32), np.float32(0.45))
        * np.power(slope.astype(np.float32) + np.float32(1e-6), np.float32(0.8)),
        np.float32(25.0))
    assert np.allclose(gated_off, manual, rtol=1e-6, atol=0.0)

    with pytest.raises(ValueError):
        incise.stream_power(area, slope, a_crit_m2=-1.0)
    with pytest.raises(ValueError):
        incise.stream_power(area, slope, gate_q=0.0)


def test_stream_power_default_K_carves_metres_not_millimetres():
    """LESSON: K is the one knob that decides whether the bake reads as terrain.

    None of the summary statistics distinguish a useless K from a good one -- only a
    hillshade does -- so the calibration is pinned here instead. K = 4e-4 gives a p99
    incision of 0.01 m: a drainage network that exists in the flow field and nowhere in the
    ground. K = 0.15 gives mean 1.66 m / p99 7.84 m and reads as a properly dissected
    hillslope. The bounds below bracket that channel-scale case with room for retuning but
    not room for a 400x error.
    """
    area = np.full((4, 4), 1e6)        # 1 km^2 catchment
    slope = np.full((4, 4), 0.1)
    good = float(incise.stream_power(area, slope).max())
    assert 0.5 < good < 12.0, good
    dead = float(incise.stream_power(area, slope, K=4e-4).max())
    assert dead < 0.05, dead


# ======================================================================================
# B3 thermal relaxation
# ======================================================================================

def _spiky_field(n=96, seed=3):
    """Deliberately pathological: tall spikes, deep one-cell pits, and a 90 m cliff.

    The cliff is ~66x the repose drop at this cell size and the pits are the worst case for
    this scheme (a cell that RECEIVES from all eight neighbours at once), so if anything
    can make it diverge it is this.
    """
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float64)
    z = 40.0 * np.sin(xx / 13.0) + 0.35 * yy * CELL_M
    idx = rng.integers(2, n - 2, size=(120, 2))
    for k, (i, j) in enumerate(idx):
        z[i, j] += 120.0 if k % 2 == 0 else -120.0
    z[:, n // 2:] += 90.0
    return z


def test_relax_conserves_mass():
    """LESSON: thermal erosion MOVES material, it does not delete it.

    Subtracting each cell's over-repose excess without depositing it stripped 128 m off
    cliff tops in 48 iterations on a real tile, because a 30 m drop across one post is ~10x
    the repose limit and nothing put the debris anywhere. Conservation is exact by
    construction here -- only fully in-bounds neighbour pairs participate, in both passes --
    so the tolerance is float round-off, not a fudge factor.
    """
    z = _spiky_field()
    before = z.sum()
    for iters in (1, 48, 200):
        out = thermal.relax(z, CELL_M, iters=iters)
        assert out.dtype == np.float64
        rel = abs(out.sum() - before) / abs(before)
        assert rel < 1e-12, (iters, rel)


def test_relax_is_stable_over_200_iterations_on_a_spiky_field():
    """LESSON: scale the shed by the STEEPEST over-repose pair, not the sum over eight.

    Scaling by the sum let a spike with eight 20 m-lower neighbours shed 48 m in one step,
    overshoot far below them, and diverge the whole field to ~1e23 in 48 iterations.
    Capping by the steepest pair means that pair can be driven to repose but never through
    it for any rate <= 0.5.

    Note what this does NOT assert: monotone smoothing. The scheme bounds what a cell
    *gives*, not what it *receives*, so a single step on this field transiently raises the
    max slope (an isolated pit is filled by all eight neighbours at once). It converges
    from there -- that is the property under test.
    """
    z = _spiky_field()
    out = thermal.relax(z, CELL_M, iters=200)
    assert np.isfinite(out).all()
    assert out.min() > z.min() - 1.0
    assert out.max() < z.max() + 1.0
    assert thermal.max_slope(out, CELL_M) < thermal.max_slope(z, CELL_M)
    # The divergence signature was ~1e23; anything near it is a hard failure.
    assert np.abs(out).max() < 10.0 * np.abs(z).max()


def test_relax_does_not_strip_cliff_tops():
    """The direct regression for the delete-instead-of-deposit bug (128 m lost in 48 iters).

    A high plateau above a cliff: after 48 iterations the summit must weather back by
    metres, not by more than the cliff is tall.
    """
    n = 64
    z = np.zeros((n, n))
    z[:, : n // 2] = 200.0
    out = thermal.relax(z, CELL_M, iters=48)
    lost = z[:, : n // 4].max() - out[:, : n // 4].max()
    assert 0.0 <= lost < 5.0, lost
    assert out[:, : n // 4].max() > 190.0


def test_relax_reaches_repose():
    rng = np.random.default_rng(5)
    n = 64
    z = rng.normal(size=(n, n)) * 6.0 + np.mgrid[0:n, 0:n][1] * 0.2
    assert thermal.max_slope(z, CELL_M) > 3.0 * TAN_REPOSE, "the input is not over-steep"
    out = thermal.relax(z, CELL_M, repose_deg=REPOSE_DEG, iters=300)
    assert thermal.max_slope(out, CELL_M) <= TAN_REPOSE * (1.0 + 1e-5)
    assert float(thermal.excess_over_repose(out, CELL_M, REPOSE_DEG).max()) < 1e-3


def test_relax_uses_the_pair_distance_for_the_repose_drop():
    """A plane already at repose along its steepest (diagonal) direction must not move.

    The prototype used one `tan(repose) * cell_m` for cardinals and diagonals alike, which
    holds a diagonal pair to the same height drop over a sqrt(2) longer run -- a 41%
    gentler limit. That is the classic eight-neighbour talus artifact: cones come out
    octagonal with visibly flatter diagonals. Scaling the drop by the pair distance costs
    nothing and removes it, and this plane is the sharpest statement of the difference.
    """
    n = 32
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float64)
    z = TAN_REPOSE * CELL_M * (xx + yy) / math.sqrt(2.0)
    assert float(thermal.excess_over_repose(z, CELL_M, REPOSE_DEG).max()) < 1e-9
    out = thermal.relax(z, CELL_M, repose_deg=REPOSE_DEG, iters=32)
    assert np.abs(out - z).max() < 1e-9
    assert thermal.max_slope(z, CELL_M) == pytest.approx(TAN_REPOSE, rel=1e-9)


def test_relax_rejects_a_rate_outside_the_stability_bound():
    z = _spiky_field(n=32)
    with pytest.raises(ValueError):
        thermal.relax(z, CELL_M, rate=0.75)
    with pytest.raises(ValueError):
        thermal.relax(z, CELL_M, rate=-0.1)
    with pytest.raises(ValueError):
        thermal.relax(z, 0.0)
    with pytest.raises(ValueError):
        thermal.relax(np.zeros(8), CELL_M)


def test_relax_zero_iters_is_a_copy():
    z = _spiky_field(n=32)
    out = thermal.relax(z, CELL_M, iters=0)
    assert np.array_equal(out, z)
    assert out is not z


def test_relax_numba_matches_the_numpy_reference():
    pytest.importorskip("numba")
    z = _spiky_field(n=48)
    drops = thermal._max_drops(CELL_M, REPOSE_DEG, np.float64)
    ref = thermal._relax_numpy(z.copy(), drops, 40, np.float64(0.4))
    fast = thermal._relax_numba(z.copy(), drops, 40, np.float64(0.4))
    assert np.abs(ref - fast).max() < 1e-9


def test_relax_preserves_float32():
    z = _spiky_field(n=48).astype(np.float32)
    out = thermal.relax(z, CELL_M, iters=16)
    assert out.dtype == np.float32
    assert np.isfinite(out).all()
    rel = abs(float(out.sum()) - float(z.sum())) / abs(float(z.sum()))
    assert rel < 1e-4, rel


# ======================================================================================
# CI shape: these modules must import and mostly work with neither scipy nor numba
# ======================================================================================

class _Blocker:
    def __init__(self, names):
        self.names = tuple(names)

    def find_module(self, fullname, path=None):        # pragma: no cover - legacy hook
        return None

    def find_spec(self, fullname, path=None, target=None):
        if fullname.split(".")[0] in self.names:
            raise ImportError(f"blocked for test: {fullname}")
        return None


def test_modules_import_and_run_without_scipy_or_numba():
    """CI has neither. A module that explodes on import fails the whole job, not one test.

    scipy and numba are bake-pod dependencies and are deliberately absent from
    terrain-service/requirements.txt, so every import of them lives inside a function.
    `carrier` is the one entry point that genuinely cannot work without scipy (the
    prefilter is a scipy IIR pass); it must fail with a clear ImportError, not a NameError
    at import time.
    """
    blocker = _Blocker(("scipy", "numba"))
    saved = {k: v for k, v in sys.modules.items()
             if k.startswith(("scipy", "numba", "terrain_service.bake"))}
    for k in saved:
        del sys.modules[k]
    sys.meta_path.insert(0, blocker)
    try:
        n = importlib.import_module("terrain_service.bake.noise")
        t = importlib.import_module("terrain_service.bake.thermal")
        i = importlib.import_module("terrain_service.bake.incise")

        assert i.stream_power(np.full((4, 4), 1e6), np.full((4, 4), 0.1)).max() > 0.0
        rough = n.roughness(np.zeros((32, 32), np.float32), CELL_M,
                            np.full((32, 32), 0.3, np.float32), 7)
        assert np.isfinite(rough).all() and rough.std() > 0.0
        out = t.relax(np.zeros((32, 32)) + np.eye(32) * 50.0, CELL_M, iters=8)
        assert np.isfinite(out).all()

        with pytest.raises(ImportError, match="scipy"):
            n.carrier(np.zeros((8, 8)), 8)
    finally:
        sys.meta_path.remove(blocker)
        for k in list(sys.modules):
            if k.startswith(("scipy", "numba", "terrain_service.bake")):
                del sys.modules[k]
        sys.modules.update(saved)
        importlib.import_module("terrain_service.bake.noise")
        importlib.import_module("terrain_service.bake.thermal")
        importlib.import_module("terrain_service.bake.incise")


def test_no_module_level_heavy_imports():
    """Belt and braces for the above: the guarded imports must stay inside functions."""
    for mod in (noise, thermal, incise):
        src = open(mod.__file__, encoding="utf-8").read()
        for line in src.splitlines():
            if line.startswith(("import ", "from ")):
                assert "scipy" not in line and "numba" not in line, (mod.__name__, line)
