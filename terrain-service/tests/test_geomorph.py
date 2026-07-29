"""Tests for `terrain_service.geomorph`.

Two kinds of test here, and the split is deliberate.

**Analytic fixtures** pin the things that have exactly one right answer: a plane's slope,
a paraboloid's curvature and its *sign*, a bowl's pit count, a plane's flow length. These
are the tests that catch a missing factor of ``cell_m``, an inverted curvature convention
or a sweep running the wrong way down the flow tree -- all three of which happened while
this package was being written, and two of which produced perfectly plausible numbers.

**Discrimination fixtures** pin the claims the package makes about itself: that curvature
skew is ~0 on a Gaussian field and not on real terrain, that the variogram cannot tell a
spectrum-matched surrogate from the field it was made from, that the resolution guard
fires. These are regression tests for the *conclusions*, so that a later change which
quietly destroys a metric's discriminating power fails here rather than in a report six
months later.

**Neither CI nor the bake pod has every dependency.** This module imports only numpy at
the top level; numba is needed by `bake.flow` and therefore by every hydrological metric,
so those tests `importorskip` it individually rather than at module scope -- a module
that raises on import fails the whole job instead of skipping a few tests, which has
already happened in this repo once.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from terrain_service import geomorph as G
from terrain_service.geomorph import _grid, controls, report

CELL = 10.0


def _numba_or_skip():
    pytest.importorskip("numba", reason="bake.flow's kernels are numba-compiled")


# ======================================================================================
# Input validation and the resolution guard
# ======================================================================================
def test_as_field_rejects_the_four_ways_a_caller_gets_it_wrong():
    with pytest.raises(ValueError, match="2-D"):
        _grid.as_field(np.zeros(10))
    with pytest.raises(ValueError, match="at least 3x3"):
        _grid.as_field(np.zeros((2, 9)))
    with pytest.raises(ValueError, match="NaN"):
        _grid.as_field(np.array([[0.0, np.nan, 0], [0, 0, 0], [0, 0, 0]]))
    with pytest.raises(ValueError, match="cell_m"):
        _grid.check_cell_m(0.0)


def test_comparing_two_resolutions_raises_rather_than_returning_a_number():
    """The failure mode this API is shaped to prevent, asserted directly."""
    z = controls.fbm((64, 64), 10.0, hurst=0.7, seed=0, rms_m=20.0)
    a = G.slope_statistics(z, 10.0)
    b = G.slope_statistics(z, 1.875)
    assert _grid.require_same_resolution([a, a]) == 10.0
    with pytest.raises(G.ResolutionMismatch, match="different cell sizes"):
        _grid.require_same_resolution([a, b])


def test_the_discrimination_table_refuses_mixed_resolution_columns():
    z = controls.fbm((64, 64), 10.0, hurst=0.7, seed=0, rms_m=20.0)
    hi = {"cell_m": 10.0, "mean_deg": 1.0}
    lo = {"cell_m": 1.875, "mean_deg": 1.0}
    assert "mean_deg" in report.discrimination_table({"a": hi, "b": dict(hi)})
    with pytest.raises(G.ResolutionMismatch):
        report.discrimination_table({"a": hi, "b": lo})
    del z


def test_a_flow_context_cannot_be_reused_at_another_cell_size():
    _numba_or_skip()
    z = controls.fbm((64, 64), CELL, hurst=0.7, seed=1, rms_m=20.0)
    ctx = G.flow_context(z, CELL)
    with pytest.raises(G.ResolutionMismatch):
        G.pit_statistics(z, CELL * 2, ctx=ctx)


# ======================================================================================
# Slope -- analytic
# ======================================================================================
def test_slope_of_a_plane_is_exact_in_the_interior():
    """Horn's operator is exact on a plane; anything else is a cell_m error."""
    for s in (0.05, 0.2, 1.0):
        for az in (0.0, 30.0, 90.0):
            z = controls.inclined_plane((64, 64), CELL, slope=s, azimuth_deg=az)
            f = G.slope_field(z, CELL)
            assert np.allclose(f[2:-2, 2:-2], s, rtol=1e-12)


def test_slope_scales_with_cell_size_which_is_why_cell_m_is_mandatory():
    z = controls.inclined_plane((64, 64), 1.0, slope=0.2)   # 0.2 per 1 m of x
    assert G.slope_field(z, 1.0)[10, 10] == pytest.approx(0.2)
    assert G.slope_field(z, 2.0)[10, 10] == pytest.approx(0.1)


def test_repose_fraction_is_all_or_nothing_on_a_uniform_plane():
    """Everything except the replicated border ring, whose slope is halved by design."""
    steep = controls.inclined_plane((512, 512), CELL, slope=math.tan(math.radians(50)))
    gentle = controls.inclined_plane((512, 512), CELL, slope=math.tan(math.radians(10)))
    assert G.slope_statistics(steep, CELL).frac_above_repose > 0.99
    assert G.slope_statistics(gentle, CELL).frac_above_repose == pytest.approx(0.0)


def test_slope_pdf_integrates_to_one():
    z = controls.fbm((128, 128), CELL, hurst=0.7, seed=2, rms_m=50.0)
    s = G.slope_statistics(z, CELL)
    width = np.diff(s.pdf_edges_deg)
    assert float((s.pdf * width).sum()) == pytest.approx(1.0, abs=1e-12)
    assert 0.0 <= s.median_deg <= s.p95_deg <= s.p99_deg <= s.max_deg


# ======================================================================================
# Curvature -- analytic, and the sign convention
# ======================================================================================
def test_a_plane_has_exactly_zero_curvature():
    """To float64 round-off. The tolerance is 1e-12 /m, eight orders below any real
    curvature on a 10 m grid (a hillcrest runs 1e-4 to 1e-3 /m)."""
    z = controls.inclined_plane((64, 64), CELL, slope=0.3, azimuth_deg=20.0)
    f = G.curvature_fields(z, CELL)
    assert np.nanmax(np.abs(f.profile[2:-2, 2:-2])) < 1e-12
    assert np.nanmax(np.abs(f.planform[2:-2, 2:-2])) < 1e-12
    assert np.max(np.abs(f.laplacian[1:-1, 1:-1])) < 1e-12


@pytest.mark.parametrize("sign,expect", [(-1.0, +1.0), (+1.0, -1.0)])
def test_convex_is_positive_and_concave_is_negative(sign, expect):
    """The convention the whole skewness argument rests on. Dome positive, bowl negative.

    ``paraboloid(sign=-1)`` is a dome, whose Zevenbergen-Thorne second derivatives are
    both ``-k``; in this package's convention that must come back as ``+k``.
    """
    k = 1e-4
    z = controls.paraboloid((97, 97), CELL, k=k, sign=sign)
    f = G.curvature_fields(z, CELL, min_slope=1e-6)
    assert f.laplacian[1:-1, 1:-1].mean() == pytest.approx(expect * 2.0 * k, rel=1e-9)
    prof = f.profile[8:-8, 8:-8]
    assert np.nanmedian(prof) == pytest.approx(expect * k, rel=0.02)


def test_curvature_is_undefined_on_a_flat_and_says_so():
    z = np.zeros((64, 64))
    f = G.curvature_fields(z, CELL, min_slope=0.01)
    assert not f.valid.any()
    s = G.curvature_statistics(z, CELL)
    assert s.frac_masked == pytest.approx(1.0)
    assert math.isnan(s.profile_quantile_skew)


def test_a_gaussian_field_has_symmetric_curvature():
    """The claim `curvature`'s discriminating power rests on: noise has skew ~ 0.

    fBm is a Gaussian random field, so ``z`` and ``-z`` are equally likely draws and
    every odd moment of curvature must vanish up to sampling error. If this test starts
    failing, curvature skew has stopped meaning what the module says it means.
    """
    for seed in range(4):
        z = controls.fbm((256, 256), CELL, hurst=0.75, seed=seed, rms_m=100.0)
        s = G.curvature_statistics(z, CELL)
        assert abs(s.profile_quantile_skew) < 0.02
        assert abs(s.laplacian_quantile_skew) < 0.02
        assert s.convex_frac == pytest.approx(0.5, abs=0.02)
        assert s.tail_asymmetry == pytest.approx(1.0, abs=0.10)


def test_curvature_skew_flips_sign_when_the_landscape_is_turned_upside_down():
    """The asymmetry is real, not an artefact of the estimator.

    An eroded landscape is not symmetric under ``z -> -z``; a curvature statistic that
    did not flip sign under that operation would be measuring something else.
    """
    z = controls.fbm((192, 192), CELL, hurst=0.8, seed=5, rms_m=60.0)
    # Manufacture an asymmetry: sharpen the lows, exactly what water does.
    zz = z - 0.6 * np.abs(z - z.mean())
    up = G.curvature_statistics(zz, CELL)
    down = G.curvature_statistics(-zz, CELL)
    assert abs(up.laplacian_quantile_skew) > 0.05
    assert up.laplacian_quantile_skew == pytest.approx(
        -down.laplacian_quantile_skew, rel=0.05)


# ======================================================================================
# Geomorphons
# ======================================================================================
def test_the_lookup_table_matches_the_grass_source_at_its_corners():
    """Ported from ``r.geomorphon``'s ``geom.c``; these four entries pin the axes.

    Getting the two axes the wrong way round swaps peak with pit and ridge with valley
    -- a mistake that leaves the histogram looking entirely reasonable.
    """
    from terrain_service.geomorph.geomorphon import _FORMS, CLASS_CODES

    assert _FORMS[0, 8] == CLASS_CODES["pit"]      # nothing lower, everything higher
    assert _FORMS[8, 0] == CLASS_CODES["peak"]
    assert _FORMS[0, 0] == CLASS_CODES["flat"]
    assert _FORMS[2, 2] == CLASS_CODES["slope"]
    assert _FORMS[4, 2] == CLASS_CODES["spur"]
    assert _FORMS[2, 4] == CLASS_CODES["hollow"]
    # Every reachable (minus, plus) pair -- they cannot sum above 8 -- is a real class.
    for m in range(9):
        for p in range(9 - m):
            assert 1 <= _FORMS[m, p] <= 10


def test_a_plane_is_entirely_slope():
    z = controls.inclined_plane((128, 128), CELL, slope=0.3, azimuth_deg=15.0)
    g = G.geomorphon_histogram(z, CELL, search_m=200.0)
    assert g.fractions["slope"] == pytest.approx(1.0)


def test_a_hill_is_convex_and_a_bowl_is_concave():
    hill = controls.cone((129, 129), CELL, slope=0.3, sign=-1.0)
    bowl = controls.cone((129, 129), CELL, slope=0.3, sign=+1.0)
    gh = G.geomorphon_histogram(hill, CELL, search_m=200.0)
    gb = G.geomorphon_histogram(bowl, CELL, search_m=200.0)
    assert gh.frac("spur", "ridge", "peak") > 0.4
    assert gh.frac("hollow", "valley", "pit") < 0.01
    assert gb.frac("hollow", "valley", "pit") > 0.4
    assert gb.frac("spur", "ridge", "peak") < 0.01


def test_a_flat_is_flat_and_the_threshold_is_what_decides_that():
    """`flat_deg` sets what counts as flat, which is why it is a required part of the
    answer rather than a tuning constant: the same gentle ramp is 100% SLOPE at a
    0.5 deg threshold and 100% FLAT at 3 deg."""
    z = controls.inclined_plane((128, 128), CELL, slope=math.tan(math.radians(1.5)))
    assert G.geomorphon_histogram(z, CELL, search_m=200.0,
                                  flat_deg=0.5).fractions["slope"] == pytest.approx(1.0)
    assert G.geomorphon_histogram(z, CELL, search_m=200.0,
                                  flat_deg=3.0).fractions["flat"] == pytest.approx(1.0)


def test_the_border_is_masked_not_guessed():
    z = controls.fbm((128, 128), CELL, hurst=0.7, seed=6, rms_m=40.0)
    f = G.geomorphon_field(z, CELL, search_m=100.0)   # radius 10 cells
    assert (f[:10, :] == 0).all() and (f[-10:, :] == 0).all()
    assert (f[10:-10, 10:-10] > 0).all()
    trunc = G.geomorphon_field(z, CELL, search_m=100.0, border_mode="truncate")
    assert (trunc > 0).all()


def test_geomorphon_rejects_a_search_radius_that_does_not_fit():
    z = np.zeros((32, 32))
    with pytest.raises(ValueError, match="does not fit"):
        G.geomorphon_histogram(z, CELL, search_m=200.0)
    with pytest.raises(ValueError, match="under one cell"):
        G.geomorphon_histogram(z, CELL, search_m=1.0)


# ======================================================================================
# Hypsometry
# ======================================================================================
def test_the_integral_equals_the_elevation_relief_ratio():
    """Pike & Wilson's identity. The module's own arithmetic check."""
    for seed in range(3):
        z = controls.fbm((128, 128), CELL, hurst=0.7, seed=seed, rms_m=100.0)
        h = G.hypsometry(z, CELL, n_points=4001)
        assert h.hypsometric_integral == pytest.approx(h.elevation_relief_ratio,
                                                       abs=2e-4)


def test_a_plane_has_a_straight_hypsometric_curve():
    z = controls.inclined_plane((128, 128), CELL, slope=0.2)
    h = G.hypsometry(z, CELL)
    assert h.hypsometric_integral == pytest.approx(0.5, abs=0.01)


def test_hypsometry_cannot_see_structure_at_all_and_the_test_says_so():
    """Shuffling every pixel leaves the hypsometric curve *identical*.

    This is the documented limitation, asserted rather than merely stated: HI is a
    statistic of the elevation histogram, so it can never be a realism check.
    """
    z = controls.fbm((128, 128), CELL, hurst=0.8, seed=9, rms_m=100.0)
    a = G.hypsometry(z, CELL)
    b = G.hypsometry(controls.shuffled(z, CELL, seed=1), CELL)
    assert b.hypsometric_integral == pytest.approx(a.hypsometric_integral, abs=1e-12)
    assert np.allclose(a.relative_height, b.relative_height)


def test_a_flat_field_has_no_hypsometric_curve():
    h = G.hypsometry(np.full((32, 32), 7.5), CELL)
    assert math.isnan(h.hypsometric_integral)
    assert h.relief_m == 0.0


# ======================================================================================
# Variogram
# ======================================================================================
def test_the_variogram_recovers_the_hurst_exponent_it_was_given():
    """On a non-periodic window -- which is the only kind a real DEM comes in.

    The crop matters: `controls.fbm` synthesises on a torus, and a *periodic* field's
    variogram flattens at long lags, which biases the fitted exponent towards the middle
    of the range. See the test below, which pins that artefact so nobody rediscovers it
    as a bug in the estimator.
    """
    for target in (0.3, 0.6, 0.9):
        big = controls.fbm((2048, 2048), 1.0, hurst=target, seed=3, rms_m=50.0)
        crop = np.ascontiguousarray(big[768:1280, 768:1280])
        vg = G.variogram(crop, 1.0)
        assert vg.hurst_overall == pytest.approx(target, abs=0.08)
        assert vg.hurst_overall_r2 > 0.99


def test_a_periodic_synthetic_reports_a_compressed_hurst_exponent():
    """The control's artefact, not the metric's, asserted so the table can be read.

    Every synthetic control in `controls` is built on a torus, so its variogram cannot
    keep growing past half the window and its fitted H is pulled towards ~0.55. A real
    DEM window has no such wrap. This is why an H of 0.67 for `fbm(hurst=0.75)` in the
    discrimination table is the control being measured honestly rather than the
    generator being wrong.
    """
    lo = G.variogram(controls.fbm((512, 512), 1.0, hurst=0.3, seed=3, rms_m=50.0),
                     1.0).hurst_overall
    hi = G.variogram(controls.fbm((512, 512), 1.0, hurst=0.9, seed=3, rms_m=50.0),
                     1.0).hurst_overall
    assert lo > 0.3 and hi < 0.9        # both compressed towards the middle
    assert hi - lo > 0.35               # and the ordering survives intact


def test_white_noise_has_a_hurst_exponent_of_zero():
    rng = np.random.default_rng(0)
    vg = G.variogram(rng.standard_normal((256, 256)), 1.0)
    assert abs(vg.hurst_overall) < 0.02


def test_a_plane_has_a_variogram_that_grows_as_the_square_of_the_lag():
    z = controls.inclined_plane((256, 256), 1.0, slope=0.5)
    vg = G.variogram(z, 1.0)
    assert vg.hurst_overall == pytest.approx(1.0, abs=0.05)


def test_tier_continuity_reports_nothing_when_no_boundary_is_inside_the_lags():
    z = controls.fbm((128, 128), 30.0, hurst=0.7, seed=4, rms_m=50.0)
    assert G.tier_continuity(G.variogram(z, 30.0)) == []


def test_tier_continuity_finds_a_manufactured_kink_and_clears_a_clean_field():
    """The metric's stated job: catch a band that was dropped or double-counted.

    A field whose fine octaves have been removed is smooth below the cut, so its local
    Hurst exponent there runs towards 1 while the coarse end keeps the exponent it was
    built with. That step is exactly the signature of a dropped band, and this asserts
    the detector fires on it and not on the same field left alone.
    """
    cell = 0.5
    boundary = 20.0
    z = controls.fbm((512, 512), cell, hurst=0.6, seed=8, rms_m=30.0)
    clean = G.tier_continuity(G.variogram(z, cell), boundaries_m=(boundary,))
    assert len(clean) == 1 and not clean[0]["kink"], clean

    # Drop every band finer than ~33 m: the surface below the boundary is now smooth,
    # so its local Hurst exponent runs towards 1 while the coarse end keeps the 0.6 it
    # was built with.
    F = np.fft.fft2(z)
    f = np.hypot(np.fft.fftfreq(512, cell)[:, None], np.fft.fftfreq(512, cell)[None, :])
    smooth = np.real(np.fft.ifft2(F * np.exp(-(f / 0.03) ** 4)))
    kinked = G.tier_continuity(G.variogram(smooth, cell), boundaries_m=(boundary,))
    assert len(kinked) == 1
    assert kinked[0]["kink"], kinked
    assert kinked[0]["h_below"] > kinked[0]["h_above"]
    assert kinked[0]["delta_h"] < -0.25


def test_the_variogram_cannot_tell_a_surrogate_from_the_real_thing():
    """The documented limitation of the variogram, asserted.

    A spectrum-matched surrogate has no drainage network at all, and the variogram says
    it is the same field. This is why `variogram` is documented as a tier-bookkeeping
    metric and not a realism one, and why the package leads with pit statistics instead.
    """
    z = controls.fbm((256, 256), CELL, hurst=0.7, seed=12, rms_m=80.0)
    s = controls.spectrum_matched_surrogate(z, CELL, seed=13)
    a, b = G.variogram(z, CELL), G.variogram(s, CELL)
    assert b.hurst_overall == pytest.approx(a.hurst_overall, abs=0.06)
    ratio = b.gamma_m2 / a.gamma_m2
    assert 0.5 < float(np.median(ratio)) < 1.6


# ======================================================================================
# Controls -- determinism, and whether they are fair
# ======================================================================================
@pytest.mark.parametrize("gen", ["fbm", "value_noise"])
def test_the_controls_are_deterministic(gen):
    fn = getattr(controls, gen)
    a = fn((64, 64), CELL, seed=17, rms_m=10.0)
    b = fn((64, 64), CELL, seed=17, rms_m=10.0)
    c = fn((64, 64), CELL, seed=18, rms_m=10.0)
    assert np.array_equal(a, b)
    assert not np.allclose(a, c)


def test_the_surrogate_is_deterministic_and_preserves_the_radial_spectrum():
    z = controls.fbm((256, 256), CELL, hurst=0.7, seed=20, rms_m=80.0)
    a = controls.spectrum_matched_surrogate(z, CELL, seed=21)
    assert np.array_equal(a, controls.spectrum_matched_surrogate(z, CELL, seed=21))
    f1, p1 = controls.radial_power_spectrum(z, CELL, n_bins=16)
    f2, p2 = controls.radial_power_spectrum(a, CELL, n_bins=16)
    assert np.allclose(f1, f2)
    ok = np.isfinite(p1) & np.isfinite(p2)
    assert np.median(p2[ok] / p1[ok]) == pytest.approx(1.0, abs=0.35)


def test_the_naive_phase_surrogate_is_the_worse_control_on_a_trended_field():
    """Why `spectrum_matched_surrogate` defaults to ``method='radial'``.

    A field with a strong regional trend is not periodic, so the plain DFT reports the
    wrap discontinuity as broadband roughness and a phase-randomised surrogate realises
    it. Measured here as the ratio of the surrogate's one-lag semivariance to the
    original's: the default stays near 1, the naive method runs away.
    """
    ramp = controls.inclined_plane((256, 256), CELL, slope=0.3)
    z = ramp + controls.fbm((256, 256), CELL, hurst=0.7, seed=22, rms_m=20.0)
    base = G.variogram(z, CELL).gamma_m2[0]
    good = G.variogram(controls.spectrum_matched_surrogate(z, CELL, seed=1),
                       CELL).gamma_m2[0]
    naive = G.variogram(
        controls.spectrum_matched_surrogate(z, CELL, seed=1, method="phase"),
        CELL).gamma_m2[0]
    assert good / base < 3.0
    assert naive / base > 3.0 * (good / base)


# ======================================================================================
# Flow: pits, slope-area, drainage density, Hack
# ======================================================================================
def test_a_bowl_has_exactly_one_pit_and_a_hill_has_none():
    _numba_or_skip()
    bowl = controls.cone((97, 97), CELL, slope=0.3, sign=+1.0)
    hill = controls.cone((97, 97), CELL, slope=0.3, sign=-1.0)
    assert G.pit_statistics(bowl, CELL).raw_pits == 1
    hs = G.pit_statistics(hill, CELL)
    assert hs.raw_pits == 0
    assert hs.filled_frac == 0.0
    assert hs.fill_volume_per_area_m == 0.0


def test_pit_statistics_separate_real_structure_from_noise_by_orders_of_magnitude():
    """The package's headline claim, in its cheapest form.

    A smooth surface with a real downhill direction needs almost no filling; a Gaussian
    field of the same relief is a carpet of local minima. This is the separation that
    survives the epsilon fill, and it is the reason the fill's own output is documented
    as descriptive rather than probative.
    """
    _numba_or_skip()
    hill = controls.cone((128, 128), CELL, slope=0.3, sign=-1.0)
    noisy = controls.fbm((128, 128), CELL, hurst=0.7, seed=30, rms_m=40.0)
    a = G.pit_statistics(hill, CELL)
    b = G.pit_statistics(noisy, CELL)
    assert b.pit_density_per_km2 > 100.0 * max(a.pit_density_per_km2, 1e-6)
    assert b.fill_volume_per_area_m > 0.1


def test_flow_length_grows_down_a_plane_instead_of_stopping_after_one_step():
    """Regression: the ascending/descending sweep bug.

    Walking the elevation order the wrong way leaves every cell one step long, which
    fits Hack's law with an exponent of exactly 0.00 and an r2 of 0 -- numbers that look
    like a measurement rather than like a crash.
    """
    _numba_or_skip()
    n = 64
    z = controls.inclined_plane((n, n), CELL, slope=0.2, azimuth_deg=0.0)
    ctx = G.flow_context(z, CELL)
    L = ctx.flow_length_m()
    assert L.max() == pytest.approx((n - 1) * CELL, rel=0.02)
    assert L[:, -1].max() < CELL * 1.5     # the up-slope edge is a divide


def test_the_slope_area_relation_recovers_an_exponent_it_was_handed():
    """The fitting machinery, checked where the answer is known exactly."""
    a = np.logspace(2, 7, 400)
    s = 3.0 * a ** -0.5625
    slope, intercept, r2, _stderr, _rmse = _grid.weighted_loglog_fit(a, s)
    assert -slope == pytest.approx(G.PREDICTED_THETA, abs=1e-9)
    assert 10.0 ** intercept == pytest.approx(3.0, rel=1e-9)
    assert r2 == pytest.approx(1.0, abs=1e-12)


def test_the_bake_prediction_is_the_one_the_bake_actually_makes():
    """`PREDICTED_THETA` must track `bake.incise.stream_power`'s own defaults."""
    import inspect

    from terrain_service.bake import incise

    sig = inspect.signature(incise.stream_power)
    m = sig.parameters["m"].default
    n = sig.parameters["n"].default
    assert (m, n) == (G.slope_area.STREAM_POWER_M, G.slope_area.STREAM_POWER_N)
    assert G.PREDICTED_THETA == pytest.approx(m / n)
    assert G.PREDICTED_THETA == pytest.approx(0.5625)


def test_slope_area_returns_a_usable_curve_and_a_finite_theta():
    _numba_or_skip()
    z = controls.fbm((256, 256), CELL, hurst=0.75, seed=31, rms_m=60.0)
    sa = G.slope_area_relation(z, CELL)
    assert sa.cell_m == CELL
    assert np.isfinite(sa.theta)
    assert sa.count.sum() == sa.n_cells
    assert sa.in_fit.sum() == sa.n_bins_fit >= 3
    # the fitted limb must actually be the falling one
    assert sa.theta > 0.0


def test_slope_area_refuses_a_window_it_cannot_measure():
    _numba_or_skip()
    with pytest.raises(ValueError, match="too small or too flat"):
        G.slope_area_relation(np.zeros((16, 16)), CELL)


def test_drainage_density_flags_the_case_where_it_is_measuring_the_raster():
    _numba_or_skip()
    z = controls.fbm((128, 128), CELL, hurst=0.7, seed=32, rms_m=40.0)
    ctx = G.flow_context(z, CELL)
    coarse = G.drainage_density(ctx=ctx, a_crit_m2=1.0e5)
    fine = G.drainage_density(ctx=ctx, a_crit_m2=2.0 * CELL * CELL)
    assert coarse.dd_per_m < fine.dd_per_m
    assert fine.saturated and not coarse.saturated
    assert fine.dd_per_m <= fine.dd_saturation_per_m * 1.5


def test_drainage_density_counts_diagonal_steps_at_their_true_length():
    """A diagonal channel is sqrt(2) cells long, not one, and terrain with a diagonal
    grain would otherwise measure a systematically lower density than the same terrain
    rotated 45 degrees."""
    _numba_or_skip()
    n = 64
    diag = controls.inclined_plane((n, n), CELL, slope=0.2, azimuth_deg=45.0)
    card = controls.inclined_plane((n, n), CELL, slope=0.2, azimuth_deg=0.0)
    a_crit = 4.0 * CELL * CELL
    d = G.drainage_density(diag, CELL, a_crit_m2=a_crit)
    c = G.drainage_density(card, CELL, a_crit_m2=a_crit)
    assert d.channel_length_m > d.channel_cells * CELL * 1.2
    assert c.channel_length_m == pytest.approx(c.channel_cells * CELL)


def test_hacks_law_returns_a_plausible_exponent_on_a_routed_field():
    _numba_or_skip()
    z = controls.fbm((256, 256), CELL, hurst=0.75, seed=33, rms_m=60.0)
    hk = G.hacks_law(z, CELL, a_crit_m2=50.0 * CELL * CELL)
    assert hk.n_bins_fit >= 3
    assert 0.3 < hk.h < 1.1
    assert hk.r2 > 0.7
    assert hk.earth_h == 0.57


def test_hacks_law_says_nan_rather_than_guessing_on_a_window_too_small():
    _numba_or_skip()
    z = controls.fbm((48, 48), CELL, hurst=0.7, seed=34, rms_m=20.0)
    hk = G.hacks_law(z, CELL, a_crit_m2=1.0e6)
    assert math.isnan(hk.h)
    assert hk.n_bins_fit == 0


# ======================================================================================
# The bundle
# ======================================================================================
def test_describe_produces_the_whole_headline_row_and_carries_its_resolution():
    _numba_or_skip()
    z = controls.fbm((192, 192), CELL, hurst=0.75, seed=40, rms_m=60.0)
    d = report.describe(z, CELL, search_m=100.0)
    assert d["cell_m"] == CELL
    for key, _unit, _doc in report.HEADLINE_METRICS:
        assert key in d, key
    assert isinstance(report.discrimination_table({"a": d}), str)


def test_describe_can_skip_the_expensive_half():
    z = controls.fbm((128, 128), CELL, hurst=0.75, seed=41, rms_m=60.0)
    d = report.describe(z, CELL, search_m=100.0, skip_flow=True)
    assert "theta" not in d
    assert "mean_deg" in d and "hypsometric_integral" in d


def test_the_paired_verdict_needs_more_than_a_big_fold_on_a_tiny_number():
    """Regression on the discrimination table's own arithmetic.

    Geomorphon `frac_flat` reads 0.006 on the Alps and 0.000 on its surrogate -- a fold
    of 1.0 and a difference of nothing. A fold-only rule promoted it to "sees through"
    beside curvature skew, which is a hundredfold separation on a real signal.
    """
    assert report._verdict(ratio=8.0, fold=0.99, threshold=3.0) == "sees through"
    assert report._verdict(ratio=2.9, fold=0.98, threshold=3.0) == "sees through"
    assert report._verdict(ratio=0.68, fold=1.00, threshold=3.0) == "blind"
    assert report._verdict(ratio=2.6, fold=0.44, threshold=3.0) == "marginal"
    assert report._verdict(ratio=0.02, fold=0.002, threshold=3.0) == "blind"
    assert report._verdict(float("nan"), float("nan"), 3.0) == "blind"


def test_the_channel_initiation_default_scales_with_the_grid():
    """At 30 m the bake's fixed 10^4 m^2 is ten cells, which makes the whole raster a
    channel; the floor keeps the network resolved at any cell size."""
    assert report.default_a_crit_m2(1.875) == pytest.approx(G.A_CRIT_M2)
    assert report.default_a_crit_m2(30.87) > 9.0 * G.A_CRIT_M2
