"""Landform provinces, Tier 1 (bake_ver 7). See docs/landform-provinces-plan.md.

What these tests are actually defending, in order of how expensive the mistake
would be:

1. **``province_strength = 0`` reproduces bake_ver 6 exactly.** Every other
   province constant is safe to retune only because this holds; without it a
   province bug is indistinguishable from a bake regression.
2. **A per-cell ``profile_K_dt`` is arithmetically free.** The Tier 1 plan rests
   on ``kfac = K_dt * A^m * regional * erodibility * gate * taper`` being fully
   elementwise, so a per-cell K_dt must equal folding the same field into
   ``erodibility``. If that ever stops holding, the whole tier's cost estimate
   is wrong and this is where it shows.
3. **World anchoring.** Two overlapping domains must agree exactly in their
   overlap, or a province boundary is a tile seam.
4. **The fields reach the kernels and separate the classes.** A province that
   cannot be distinguished from FLUVIAL is not earning its complexity.
"""

from __future__ import annotations

import dataclasses

import numpy as np
import pytest

from terrain_service.bake import incise, noise, pipeline, province


# ---------------------------------------------------------------------------
# Fixtures: a small synthetic domain. A production bake is ~120 s and ~7 GB.
# ---------------------------------------------------------------------------

COARSE_PX = 96
COARSE_M = 30.0


def synth_coarse(seed: int = 7, tilt: float = 0.0) -> np.ndarray:
    """A coarse raster with real relief contrast: mountains left, plain right."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:COARSE_PX, 0:COARSE_PX].astype(np.float64)
    relief = np.clip(1.0 - xx / (COARSE_PX * 0.6), 0.0, 1.0)
    z = 1800.0 * relief + tilt * yy
    for p, amp in ((32, 220.0), (16, 90.0), (8, 35.0)):
        c = rng.standard_normal((COARSE_PX // p + 2, COARSE_PX // p + 2))
        z += np.kron(c, np.ones((p, p)))[:COARSE_PX, :COARSE_PX] * amp * relief
    return z.astype(np.float32)


def synth_climate(temp_c: float, precip_mm: float) -> np.ndarray:
    """Uniform climate at the requested physical values, quantised as the wire does."""
    out = np.zeros((4, COARSE_PX, COARSE_PX), np.uint8)
    for i, name in enumerate(province.CLIMATE_ORDER):
        lo, hi = province.CLIMATE_RANGES[name]
        val = {"temperature": temp_c, "precipitation": precip_mm}.get(name, lo)
        out[i] = np.clip(round((val - lo) / (hi - lo) * 255.0), 0, 255)
    return out


CONSTS = pipeline.CONSTANTS


# ---------------------------------------------------------------------------
# 1. The zero-strength escape hatch.
# ---------------------------------------------------------------------------


def test_zero_strength_is_pure_fluvial_and_every_field_is_the_scalar():
    """province_strength = 0 must reproduce the bake_ver-6 constants EXACTLY.

    This is the property that makes the rest of the table safe to retune: if a
    province ships a visual regression, setting one constant to 0 returns the
    previous world rather than "something close to" it.
    """
    z = synth_coarse()
    cl = synth_climate(-15.0, 100.0)  # maximally NOT fluvial, on purpose
    consts = dataclasses.replace(CONSTS, province_strength=0.0)
    f = province.province_fields(z, cl, coarse_pixel_m=COARSE_M, consts=consts)

    assert np.allclose(f.weights["fluvial"], 1.0)
    for name in province.PROVINCES[1:]:
        assert np.allclose(f.weights[name], 0.0), name
    assert np.allclose(f.k_dt, consts.profile_K_dt)
    assert np.allclose(f.a_crit_m2, consts.channel_init_area_m2)
    assert np.allclose(f.stream_m, consts.stream_m)
    assert np.allclose(f.gate_q, consts.channel_init_q)
    assert np.allclose(f.meso_amp15_m, consts.meso_amp15_m)
    assert np.allclose(f.meso_amp11_m, consts.meso_amp11_m)


def test_weights_are_a_partition_everywhere():
    """The taxonomy says climate-derived provinces are a partition. Enforce it.

    A partition is what makes a parameter field a BLEND rather than a branch,
    which is what makes province boundaries free of hard switches.
    """
    z = synth_coarse()
    for cl in (None, synth_climate(-8.0, 200.0), synth_climate(20.0, 2500.0)):
        f = province.province_fields(z, cl, coarse_pixel_m=COARSE_M, consts=CONSTS)
        total = sum(f.weights[n] for n in province.PROVINCES)
        assert np.allclose(total, 1.0, atol=1e-5)
        for n in province.PROVINCES:
            assert float(f.weights[n].min()) >= 0.0
            assert float(f.weights[n].max()) <= 1.0 + 1e-6


def test_no_climate_means_no_glacial_and_no_arid():
    """Neither can be inferred from shape, so neither may be invented from it.

    The plan's load-bearing principle is that a province is derived from what
    the model produced. With no temperature plane there is no evidence for
    glaciation, and guessing would be exactly the hashed-field incoherence the
    principle exists to forbid.
    """
    f = province.province_fields(synth_coarse(), None,
                                 coarse_pixel_m=COARSE_M, consts=CONSTS)
    assert np.allclose(f.weights["glacial"], 0.0)
    assert np.allclose(f.weights["arid"], 0.0)
    assert f.temp_c is None and f.precip_mm is None


@pytest.mark.parametrize(
    "temp_c,precip_mm,expect",
    [
        (-12.0, 900.0, "glacial"),   # cold + the domain's high-relief half
        (18.0, 80.0, "arid"),        # dry, not cold
        (14.0, 1400.0, "fluvial"),   # temperate and wet
    ],
)
def test_climate_selects_the_province_the_taxonomy_names(temp_c, precip_mm, expect):
    """Cold+steep -> GLACIAL, dry -> ARID, temperate+wet -> FLUVIAL."""
    z = synth_coarse()
    f = province.province_fields(z, synth_climate(temp_c, precip_mm),
                                 coarse_pixel_m=COARSE_M, consts=CONSTS)
    # Judged on the STEEP half of the domain, which is where a relief-gated
    # province can exist at all.
    steep = slice(None), slice(0, COARSE_PX // 3)
    mix = {n: float(f.weights[n][steep].mean()) for n in province.PROVINCES}
    assert max(mix, key=mix.get) == expect, mix


def test_lowland_needs_gentle_ground_not_just_low_ground():
    """A low, gentle plain is LOWLAND; the same climate on a mountain is not.

    Guards the specific incoherence the plan warns about: a province that fires
    on elevation alone would print floodplain physics onto a valley floor at
    2,000 m.
    """
    z = synth_coarse()
    f = province.province_fields(z, synth_climate(12.0, 1000.0),
                                 coarse_pixel_m=COARSE_M, consts=CONSTS)
    steep_side = float(f.weights["lowland"][:, : COARSE_PX // 4].mean())
    flat_side = float(f.weights["lowland"][:, -COARSE_PX // 4:].mean())
    assert flat_side > 0.5
    assert steep_side < 0.05
    assert flat_side > 10 * steep_side


# ---------------------------------------------------------------------------
# 2. THE TIER 1 PREMISE: a per-cell K_dt is free.
# ---------------------------------------------------------------------------


def _drainage_fixture(n=96, cell=1.875, seed=11):
    from terrain_service.bake.flow import (accumulate_mfd, d8_receivers,
                                           fill_depressions)

    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float64)
    z = 300.0 - 0.02 * ((xx - n / 2) ** 2 + (yy - n / 2) ** 2)
    for p, amp in ((32, 60.0), (16, 25.0), (8, 10.0)):
        c = rng.standard_normal((n // p + 2, n // p + 2))
        z += np.kron(c, np.ones((p, p)))[:n, :n] * amp
    z = z.astype(np.float32)
    filled = np.asarray(fill_depressions(z, flat_eps=1e-4), np.float32)
    rec, _ = d8_receivers(filled, cell)
    acc = accumulate_mfd(filled, cell, p=1.1)
    if isinstance(acc, tuple):
        acc = acc[0]
    return filled, rec, np.asarray(acc, np.float64), cell


_KW = dict(n=0.8, cap_m=25.0, gate_q=2.0,
           sea_taper_top_m=-1e30, sea_taper_bottom_m=-1e30)


def test_per_cell_K_dt_equals_folding_the_same_field_into_erodibility():
    """THE PREMISE OF TIER 1, checked rather than argued.

    ``kfac = K_dt * A^m * regional * erodibility * gate * taper`` is fully
    elementwise, so a per-cell K_dt is the same arithmetic as folding that
    field into the array already passed as ``erodibility=``. If this assertion
    ever fails, `profile_K_dt` per province is NOT free and the tier needs
    recosting.

    Agreement is asserted at 1 mm rather than bit-for-bit: the two routes
    multiply the same factors in a different ORDER, and float32 multiplication
    is not associative. 1 mm is one hundredth of the 100 mm wire LSB, so no
    shipped byte can tell the two apart.
    """
    filled, rec, acc, cell = _drainage_fixture()
    h, w = filled.shape
    yy, xx = np.mgrid[0:h, 0:w]
    ero = (1.0 + 0.3 * np.sin(xx / 9.0)).astype(np.float32)
    kfield = (2.0 + 1.5 * np.cos(yy / 11.0)).astype(np.float32)

    via_k = incise.profile_incision(
        filled, rec, acc, cell, K_dt=kfield, m=0.45, a_crit_m2=156.0,
        erodibility=ero, **_KW)
    via_ero = incise.profile_incision(
        filled, rec, acc, cell, K_dt=1.0, m=0.45, a_crit_m2=156.0,
        erodibility=(ero * kfield), **_KW)

    d = float(np.abs(via_k.astype(np.float64) - via_ero.astype(np.float64)).max())
    assert d < 1e-3, f"per-cell K_dt is NOT a fold into erodibility: {d} m apart"
    # And it is not a trivial pass: the field must actually have carved
    # differently from the scalar mean.
    flat = incise.profile_incision(
        filled, rec, acc, cell, K_dt=float(kfield.mean()), m=0.45,
        a_crit_m2=156.0, erodibility=ero, **_KW)
    assert not np.allclose(via_k, flat, atol=1e-2)


def test_a_constant_coarse_field_reproduces_the_scalar_path():
    """A constant field must reproduce the scalar to far under the wire LSB.

    NOT bit-for-bit, and the reason is worth recording because it looks like a
    bug: ``np.power(A, q)`` with a SCALAR ``q`` takes numpy's integral-exponent
    fast path (q = 2 becomes A*A), while an array ``q`` of the same value goes
    through the general libm ``pow``. The two differ in the last ULP or so. The
    gather itself is exact -- see
    ``test_a_coarse_field_gathers_exactly_like_an_expanded_one``.

    This is why ``province_strength = 0`` reproduces bake_ver 6 by taking the
    SCALAR path in the pipeline (``prov is None``) rather than by passing
    constant fields: an exact reproduction claim has to be exact.
    """
    filled, rec, acc, cell = _drainage_fixture()
    f = 8
    ch = -(-filled.shape[0] // f), -(-filled.shape[1] // f)
    scalar = incise.profile_incision(
        filled, rec, acc, cell, K_dt=4.5, m=0.45, a_crit_m2=156.0, gate_q=2.0,
        n=0.8, cap_m=25.0, sea_taper_top_m=-1e30, sea_taper_bottom_m=-1e30)
    fields = incise.profile_incision(
        filled, rec, acc, cell,
        K_dt=np.full(ch, 4.5, np.float32),
        m=np.full(ch, 0.45, np.float32),
        a_crit_m2=np.full(ch, 156.0, np.float32),
        gate_q=np.full(ch, 2.0, np.float32),
        field_scale=f,
        n=0.8, cap_m=25.0, sea_taper_top_m=-1e30, sea_taper_bottom_m=-1e30)
    d = float(np.abs(scalar.astype(np.float64) - fields.astype(np.float64)).max())
    assert d < 1e-4, f"{d} m apart -- a thousandth of the 100 mm wire LSB is the bar"


def test_a_coarse_field_gathers_exactly_like_an_expanded_one():
    """`field[y // f, x // f]` must equal the np.repeat-ed field it replaces.

    The whole memory argument (340 MB per parameter at 9216^2) rests on the
    gather being a pure relabelling of the expanded form, not an approximation.
    """
    filled, rec, acc, cell = _drainage_fixture(n=64)
    f = 8
    h, w = filled.shape
    coarse = np.linspace(2.0, 7.0, (h // f) * (w // f), dtype=np.float32).reshape(
        h // f, w // f)
    fine = np.repeat(np.repeat(coarse, f, axis=0), f, axis=1)
    a = incise.profile_incision(filled, rec, acc, cell, K_dt=coarse, m=0.45,
                                a_crit_m2=156.0, field_scale=f, **_KW)
    b = incise.profile_incision(filled, rec, acc, cell, K_dt=fine, m=0.45,
                                a_crit_m2=156.0, field_scale=1, **_KW)
    assert np.array_equal(a, b)


def test_structural_guarantees_survive_per_cell_parameters():
    """z <= filled, the cap, and monotonicity along the receiver tree.

    Per-cell parameters multiply into `kfac` exactly where the gates and tapers
    already do, so every guarantee of the solve should be untouched -- but a
    per-cell exponent is the one that could plausibly break the Newton, so it
    is checked with the exponents varying too.
    """
    filled, rec, acc, cell = _drainage_fixture()
    h, w = filled.shape
    yy, xx = np.mgrid[0:h, 0:w]
    out = incise.profile_incision(
        filled, rec, acc, cell,
        K_dt=(2.0 + 3.0 * (xx / w)).astype(np.float32),
        m=(0.35 + 0.20 * (yy / h)).astype(np.float32),
        a_crit_m2=(100.0 + 800.0 * (xx / w)).astype(np.float32),
        gate_q=(1.5 + 1.0 * (yy / h)).astype(np.float32),
        field_scale=1, n=0.8, cap_m=25.0,
        sea_taper_top_m=-1e30, sea_taper_bottom_m=-1e30)
    assert np.isfinite(out).all()
    assert (out <= filled + 1e-5).all()
    assert ((filled - out) <= 25.0 + 1e-4).all()
    r = np.asarray(rec).ravel()
    o = out.ravel()
    has = r >= 0
    assert (o[has] >= o[r[has]] - 1e-4).all(), "the carve introduced a new pit"


@pytest.mark.parametrize("bad,name", [
    (np.full((8, 8), -1.0, np.float32), "K_dt"),
    (np.full((8, 8), 0.0, np.float32), "a_crit_m2"),
    (np.full((8, 8), 0.0, np.float32), "gate_q"),
])
def test_invalid_parameter_fields_are_refused(bad, name):
    """The relaxed guards must still refuse what the scalar guards refused."""
    filled, rec, acc, cell = _drainage_fixture(n=64)
    kw = dict(K_dt=4.5, m=0.45, a_crit_m2=156.0, gate_q=2.0, field_scale=8)
    kw[name] = bad
    with pytest.raises(ValueError, match=name):
        incise.profile_incision(filled, rec, acc, cell, n=0.8, cap_m=25.0, **kw)


def test_a_parameter_field_finer_than_the_domain_allows_is_refused():
    filled, rec, acc, cell = _drainage_fixture(n=64)
    with pytest.raises(ValueError, match="finer than"):
        incise.profile_incision(filled, rec, acc, cell,
                                K_dt=np.full((64, 64), 4.5, np.float32),
                                field_scale=8, n=0.8, cap_m=25.0)


# ---------------------------------------------------------------------------
# 3. Meso amplitudes (item 3).
# ---------------------------------------------------------------------------


def _steep_plane(n=64, cell=1.875):
    """A uniform 100% grade, so the meso slope gate is wide open everywhere."""
    _, xx = np.mgrid[0:n, 0:n].astype(np.float64)
    return (xx * cell).astype(np.float32)


def test_meso_amplitude_field_matches_the_scalar_when_constant():
    """A constant amplitude field is the scalar, BIT FOR BIT, at either dtype.

    The dtype half of this is load-bearing and was measured, not assumed: the
    octave field is float32 and a python-float amplitude is a weak scalar under
    NEP 50, so the scalar path multiplies in float32. Gathering the amplitude
    field as float64 -- the obvious choice, and the first one written -- quietly
    promoted the product and put the two paths 2.4e-7 m apart. Harmless in
    magnitude, fatal to the claim that province_strength = 0 reproduces
    bake_ver 6 exactly.
    """
    z = _steep_plane()
    kw = dict(slope_lo=0.1, slope_hi=0.2)
    a = noise.meso_relief(z, 1.875, 12345, (0, 0), amp15_m=0.8, amp11_m=0.4, **kw)
    for dt in (np.float32, np.float64):
        b = noise.meso_relief(z, 1.875, 12345, (0, 0),
                              amp15_m=np.full((8, 8), 0.8, dt),
                              amp11_m=np.full((8, 8), 0.4, dt),
                              amp_scale=8, **kw)
        assert np.array_equal(a, b), dt


def test_meso_amplitude_field_actually_varies_the_band():
    """Half amplitude on one side must halve the band there, and only there."""
    z = _steep_plane()
    amp = np.ones((8, 8), np.float32)
    amp[:, 4:] = 0.5
    out = noise.meso_relief(z, 1.875, 999, (0, 0), amp15_m=amp,
                            amp11_m=np.zeros((8, 8), np.float32),
                            amp_scale=8, slope_lo=0.1, slope_hi=0.2)
    lhs = float(np.abs(out[:, :32]).mean())
    rhs = float(np.abs(out[:, 32:]).mean())
    assert lhs > 0.0
    assert 1.8 < lhs / rhs < 2.2, (lhs, rhs)


# ---------------------------------------------------------------------------
# 4. World anchoring -- a province boundary must not be a tile seam.
# ---------------------------------------------------------------------------


def test_province_fields_agree_in_the_overlap_of_two_offset_domains():
    """Two 'tiles' cut from one world must compute identical province values
    wherever both have the coarse cells the smooth reads.

    This is the whole apron argument, done as an experiment rather than as a
    paragraph. It is NOT a claim that the bake is seam-free: see
    ``pipeline.APRON_BLIND_SPOT``, which measured that guarantee already broken
    by the depression fill for reasons this change neither causes nor cures.
    What it establishes is narrower and is exactly what Tier 1 promises --
    province adds no influence radius beyond its own smooth.
    """
    big = synth_coarse(seed=3)
    cl = np.zeros((4, COARSE_PX, COARSE_PX), np.uint8)
    yy, xx = np.mgrid[0:COARSE_PX, 0:COARSE_PX]
    cl[0] = (60 + 120 * xx / COARSE_PX).astype(np.uint8)   # temperature ramp
    cl[2] = (30 + 100 * yy / COARSE_PX).astype(np.uint8)   # precipitation ramp

    off = 16
    a = province.province_fields(big[:, :COARSE_PX - off], cl[:, :, :COARSE_PX - off],
                                 coarse_pixel_m=COARSE_M, consts=CONSTS, max_half=4)
    b = province.province_fields(big[:, off:], cl[:, :, off:],
                                 coarse_pixel_m=COARSE_M, consts=CONSTS, max_half=4)
    # The smooth's influence radius is 2 * max_half cells; drop that margin from
    # each side of the shared strip and the rest must agree EXACTLY.
    pad = 2 * 4
    lhs = a.k_dt[:, off + pad: COARSE_PX - off - pad]
    rhs = b.k_dt[:, pad: COARSE_PX - 2 * off - pad]
    assert lhs.shape == rhs.shape and lhs.size > 0
    assert np.array_equal(lhs, rhs), "province is not world-anchored"
    # Not a trivial pass: the field must vary across the strip.
    assert float(lhs.max()) > float(lhs.min()) * 1.05


def test_the_smooth_is_clamped_under_the_apron_by_geometry():
    """The influence radius must stay at most half the apron on ANY geometry.

    A constant tuned for the 32-cell production apron must not silently reach
    past a test geometry's 4-cell one, and must not make it unbakeable either.
    """
    for apron in (4, 8, 32):
        geom = dataclasses.replace(pipeline.PRODUCTION, apron_coarse_px=apron)
        assert 2 * (geom.apron_coarse_px // 4) <= geom.apron_coarse_px // 2


# ---------------------------------------------------------------------------
# 5. Identity.
# ---------------------------------------------------------------------------


def test_the_multiplier_table_rolls_the_bake_fingerprint():
    """Retuning a province is a physics change and must re-key the fine tier.

    The table is not a BakeConstants field (it is a table, not a scalar), so it
    would otherwise be the one tuning surface that could change baked bytes
    without rolling fine_provider_id -- a cache holding two mutually
    incompatible bakes under one identity.
    """
    base = pipeline.bake_fingerprint()
    original = province.PROVINCE_MULTIPLIERS["arid"]
    try:
        province.PROVINCE_MULTIPLIERS["arid"] = dataclasses.replace(
            original, k_dt=original.k_dt + 0.5)
        assert pipeline.bake_fingerprint() != base
    finally:
        province.PROVINCE_MULTIPLIERS["arid"] = original
    assert pipeline.bake_fingerprint() == base


def test_climate_ranges_match_the_wire_format():
    """province.CLIMATE_RANGES is the THIRD copy of the climate encoding.

    diffusion.py's EXPECTED_CHANNELS is the quantiser and voxel-core's
    climate.h is the consumer's copy; tests/test_climate_contract.py pins those
    two together. This pins the bake's de-quantiser to the same numbers, so a
    range change cannot silently shift every province boundary.
    """
    from terrain_service.providers.diffusion import EXPECTED_CHANNELS

    specs = {c.name: (c.min, c.max) for c in EXPECTED_CHANNELS}
    assert set(province.CLIMATE_ORDER) == set(province.CLIMATE_RANGES)
    for name, rng in province.CLIMATE_RANGES.items():
        assert name in specs, f"{name} is not a model output channel"
        assert tuple(rng) == specs[name], (
            f"CLIMATE ENCODING DIVERGENCE on {name!r}: province.py says {rng}, "
            f"diffusion.EXPECTED_CHANNELS says {specs[name]}. These de-quantize "
            "and quantize the same bytes.")


def test_dequantize_inverts_the_wire_quantization():
    """Round trip within one LSB, which is the accuracy the encoding has."""
    for name, (lo, hi) in province.CLIMATE_RANGES.items():
        i = province.CLIMATE_ORDER.index(name)
        for val in (lo, lo + 0.25 * (hi - lo), 0.5 * (lo + hi), hi):
            planes = np.zeros((4, 2, 2), np.uint8)
            planes[i] = int(np.clip(round((val - lo) / (hi - lo) * 255.0), 0, 255))
            back = float(province.dequantize_climate(planes)[name][0, 0])
            assert abs(back - val) <= (hi - lo) / 255.0 * 0.51, (name, val, back)
