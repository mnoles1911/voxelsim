"""Tests for the Earth reference corpus in ``tools/earth_reference.py``.

Scope, deliberately: everything here runs with NO network and NO rasterio.
rasterio is not a terrain-service dependency (the tool is run with the
diffusion venv), so the raster path cannot be exercised on this box. What CAN
be exercised is the part that fails SILENTLY -- tile naming, metres-per-degree,
and the slope/void arithmetic. A wrong tile name fails loudly as a 404; a
wrong metres-per-degree, an unmasked void, or a degrees-instead-of-metres
gradient produces a plausible number that is simply not true, and that number
would then become the standard the whole validation pipeline is judged against.

The manifest tests treat the committed JSON as the contract: if someone edits
a site by hand, or a rebuild drops a class, these fail.
"""

import json
import math
import sys
from pathlib import Path

import numpy as np
import pytest

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

import earth_reference as er  # noqa: E402


# --------------------------------------------------------------------------
# Copernicus tile naming -- floor, not round
# --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "lat,lon,expect",
    [
        (46.35, 7.55, "Copernicus_DSM_COG_10_N46_00_E007_00_DEM"),
        (46.0, 7.0, "Copernicus_DSM_COG_10_N46_00_E007_00_DEM"),
        (46.999, 7.999, "Copernicus_DSM_COG_10_N46_00_E007_00_DEM"),
        # The one that bites: a western longitude must FLOOR. -112.2 lives in
        # the tile spanning -113..-112, which is named W113, not W112.
        (36.10, -112.20, "Copernicus_DSM_COG_10_N36_00_W113_00_DEM"),
        (35.55, -76.20, "Copernicus_DSM_COG_10_N35_00_W077_00_DEM"),
        (-1.5, -1.5, "Copernicus_DSM_COG_10_S02_00_W002_00_DEM"),
        (20.5, 52.5, "Copernicus_DSM_COG_10_N20_00_E052_00_DEM"),
    ],
)
def test_cop30_tile_id(lat, lon, expect):
    assert er.cop30_tile_id(lat, lon) == expect


def test_cop30_url_repeats_the_tile_id_in_both_positions():
    tid = er.cop30_tile_id(46.35, 7.55)
    assert er.cop30_url(tid) == f"{er.COP30_BUCKET}/{tid}/{tid}.tif"


def test_utm_epsg_nad83():
    assert er.utm_epsg_nad83(43.8, -102.3) == 26913  # zone 13N, Badlands
    assert er.utm_epsg_nad83(35.6, -83.5) == 26917  # zone 17N, Smokies
    assert er.utm_epsg_nad83(36.1, -112.2) == 26912  # zone 12N, Grand Canyon


# --------------------------------------------------------------------------
# Geodesy -- the silent one
# --------------------------------------------------------------------------


def test_meters_per_degree_matches_known_values():
    m_lon, m_lat = er.meters_per_degree(0.0)
    assert m_lon == pytest.approx(111320, abs=60)
    assert m_lat == pytest.approx(110574, abs=60)
    m_lon, m_lat = er.meters_per_degree(45.0)
    assert m_lon == pytest.approx(78847, abs=60)
    assert m_lat == pytest.approx(111132, abs=60)
    m_lon, _ = er.meters_per_degree(90.0)
    assert abs(m_lon) < 1.0  # a degree of longitude is a point at the pole


def test_meters_per_degree_is_symmetric_in_latitude():
    for lat in (12.0, 34.0, 61.0):
        a = er.meters_per_degree(lat)
        b = er.meters_per_degree(-lat)
        assert a[0] == pytest.approx(b[0])
        assert a[1] == pytest.approx(b[1])


def test_arcsecond_pixel_is_not_square_away_from_the_equator():
    """The whole reason the tool reprojects. At lat 46 a 1-arcsec Copernicus
    post is ~21.5 m east-west and ~30.9 m north-south; treating it as square
    is a 1.4x error in every east-west gradient."""
    m_lon, m_lat = er.meters_per_degree(46.0)
    dx = m_lon / 3600.0
    dy = m_lat / 3600.0
    assert dx == pytest.approx(21.5, abs=0.4)
    assert dy == pytest.approx(30.9, abs=0.2)
    assert dy / dx > 1.4


# --------------------------------------------------------------------------
# Slope -- exact on a plane, NaN-safe around voids
# --------------------------------------------------------------------------


def _ramp(rows, cols, dz_per_col):
    return np.tile(np.arange(cols, dtype=float) * dz_per_col, (rows, 1))


def test_horn_slope_is_exact_on_a_planar_ramp():
    # 10 m rise per 30 m run = 18.4349 degrees, everywhere except the border.
    z = _ramp(40, 40, 10.0)
    s = er.horn_slope_deg(z, 30.0, 30.0)
    interior = s[1:-1, 1:-1]
    assert np.all(np.isfinite(interior))
    assert np.allclose(interior, math.degrees(math.atan(10.0 / 30.0)))


def test_horn_slope_is_zero_on_a_flat_surface():
    z = np.full((20, 20), 812.4)
    s = er.horn_slope_deg(z, 30.0, 30.0)
    assert np.allclose(s[1:-1, 1:-1], 0.0)


def test_horn_slope_uses_the_pixel_size_it_is_given():
    """A gradient taken in degrees rather than metres is the headline hazard.
    Same array, pixel size 30 m vs 1 m => the same terrain reads 30x steeper
    in tangent, which must show up here or the tool cannot be trusted."""
    z = _ramp(20, 20, 1.0)
    coarse = er.horn_slope_deg(z, 30.0, 30.0)[5, 5]
    fine = er.horn_slope_deg(z, 1.0, 1.0)[5, 5]
    assert math.tan(math.radians(fine)) == pytest.approx(
        30.0 * math.tan(math.radians(coarse))
    )


def test_horn_slope_accepts_a_per_row_dx():
    """The native-grid cross-check varies dx with cos(lat) down the window."""
    z = _ramp(9, 9, 5.0)
    dx = np.linspace(20.0, 30.0, 9).reshape(-1, 1)
    s = er.horn_slope_deg(z, dx, 30.0)
    interior = s[1:-1, 1:-1]
    # Wider pixels => gentler slope, monotonically down the rows.
    col = interior[:, 3]
    assert np.all(np.diff(col) < 0)


def test_void_cannot_contribute_a_cliff():
    """An unmasked -32767 next to 400 m ground is a 4000% grade and would
    dominate every percentile. NaN must swallow its whole neighbourhood."""
    z = np.full((21, 21), 400.0)
    z[10, 10] = np.nan
    s = er.horn_slope_deg(z, 30.0, 30.0)
    assert np.isnan(s[9:12, 9:12]).all()
    rest = s[1:-1, 1:-1]
    assert np.nanmax(rest) == pytest.approx(0.0)


def test_masking_removes_nodata_and_the_sea_and_counts_them_separately():
    z = np.full((10, 10), 25.0)
    z[0, :] = -32767.0  # declared nodata
    z[1, :] = 0.0  # GLO-30 open water
    masked, stats = er._mask_copernicus(z, -32767.0)
    assert np.isnan(masked[0]).all() and np.isnan(masked[1]).all()
    assert np.isfinite(masked[2:]).all()
    assert stats["nodata_fraction"] == pytest.approx(0.10)
    assert stats["sea_or_zero_fraction"] == pytest.approx(0.10)
    assert stats["masked_fraction"] == pytest.approx(0.20)


def test_usgs_masking_keeps_legitimate_zero_elevation():
    """NAVD88 land at exactly 0.000 is real on the coastal plain, so the
    Copernicus sea rule must NOT be applied to 3DEP."""
    z = np.full((10, 10), 0.0)
    z[0, :] = -3.4028234663852886e38
    masked, stats = er._mask_usgs(z, -3.4028234663852886e38)
    assert np.isnan(masked[0]).all()
    assert np.isfinite(masked[1:]).all()
    assert stats["sea_or_zero_fraction"] == 0.0


def test_void_floor_catches_fill_values_with_no_declared_nodata():
    z = np.full((10, 10), 60.0)
    z[3, 3] = -9999.0
    masked, stats = er._mask_copernicus(z, None)
    assert np.isnan(masked[3, 3])
    assert stats["nodata_fraction"] == pytest.approx(0.01)


# --------------------------------------------------------------------------
# Aggregation
# --------------------------------------------------------------------------


def test_block_mean_averages_and_is_identity_at_factor_one():
    z = np.arange(36, dtype=float).reshape(6, 6)
    assert np.array_equal(er.block_mean(z, 1), z)
    out = er.block_mean(z, 3)
    assert out.shape == (2, 2)
    assert out[0, 0] == pytest.approx(z[0:3, 0:3].mean())


def test_block_mean_rejects_a_block_that_is_mostly_void():
    z = np.full((4, 4), 10.0)
    z[0:2, 0:2] = np.nan
    z[0, 0] = 10.0  # one survivor out of four
    out = er.block_mean(z, 2)
    assert np.isnan(out[0, 0])  # 25% valid < 50% -> rejected, not promoted
    assert out[1, 1] == pytest.approx(10.0)


def test_detrended_rms_ignores_regional_tilt():
    tilt = _ramp(30, 30, 4.0)
    assert er.plane_detrended_rms(tilt) == pytest.approx(0.0, abs=1e-8)
    rng = np.random.default_rng(7)
    bumpy = tilt + rng.normal(0.0, 3.0, tilt.shape)
    assert er.plane_detrended_rms(bumpy) == pytest.approx(3.0, rel=0.15)


def test_describe_grid_reports_the_fields_the_manifest_promises():
    rng = np.random.default_rng(11)
    z = _ramp(200, 200, 2.0) + rng.normal(0, 1.0, (200, 200))
    d = er.describe_grid(z, 30.0, (1, 2))
    for k in ("elevation_m", "relief_m", "slope_deg", "flat_fraction",
              "detrended_rms_m", "slope_by_scale", "valid_fraction"):
        assert k in d, k
    assert set(d["slope_by_scale"]) == {"30m", "60m"}
    assert d["slope_deg"]["p95"] >= d["slope_deg"]["p50"]


# --------------------------------------------------------------------------
# Site table and committed manifest
# --------------------------------------------------------------------------

REQUIRED_CLASSES = {
    "alpine_glaciated",
    "high_relief_fluvial_mountains",
    "rolling_hills_soil_mantled",
    "plains_low_relief",
    "desert_aeolian_dunes",
    "badlands_dissected",
    "coastal_plain_shoreline",
    "plateau_incised_canyon",
}


def test_site_table_covers_every_class_we_promised():
    assert {s["geomorphic_class"] for s in er.SITES} >= REQUIRED_CLASSES
    assert len(er.SITES) >= 8


def test_site_ids_are_unique():
    ids = [s["id"] for s in er.SITES]
    assert len(ids) == len(set(ids))


def test_every_site_justifies_itself_and_declares_its_dsm_bias():
    for s in er.SITES:
        assert len(s["why"]) > 80, s["id"]
        assert len(s["human_description"]) > 30, s["id"]
        assert s["cop30_dsm_bias"] in ("negligible", "moderate", "severe"), s["id"]
        assert s["cop30_dsm_reason"], s["id"]
        # A site without 1 m bare earth must say WHY rather than quietly lack it.
        if not s.get("usgs_3dep"):
            assert s.get("no_3dep_reason"), s["id"]


def test_analysis_window_fits_inside_its_copernicus_tile():
    """A window that crosses a 1x1 deg tile edge reads nodata as terrain. This
    is checked at runtime too, but catching it here means a badly placed site
    fails before anyone downloads 40 MB."""
    half_m = er.COP30_WINDOW_KM * 1000.0 / 2.0 * 1.15
    for s in er.SITES:
        m_lon, m_lat = er.meters_per_degree(s["lat"])
        dlon = half_m / float(m_lon)
        dlat = half_m / float(m_lat)
        assert math.floor(s["lon"]) <= s["lon"] - dlon, s["id"]
        assert s["lon"] + dlon <= math.floor(s["lon"]) + 1, s["id"]
        assert math.floor(s["lat"]) <= s["lat"] - dlat, s["id"]
        assert s["lat"] + dlat <= math.floor(s["lat"]) + 1, s["id"]


@pytest.fixture(scope="module")
def manifest():
    if not er.MANIFEST_PATH.exists():
        pytest.skip("manifest not built")
    return json.loads(er.MANIFEST_PATH.read_text())


def test_manifest_schema_and_method_are_present(manifest):
    assert manifest["schema"] == er.SCHEMA
    m = manifest["method"]
    assert m["projection"]["copernicus_primary_grid"] == "aeqd30"
    assert m["projection"]["usgs_grid"] == "utm_native"
    assert m["voids_and_water"]["void_floor_m"] == er.VOID_FLOOR_M
    assert "401" in manifest["sources"]["opentopography"]["evidence"]


def test_manifest_has_one_entry_per_site_with_stats(manifest):
    assert [s["id"] for s in manifest["sites"]] == [s["id"] for s in er.SITES]
    for s in manifest["sites"]:
        cop = s["cop30"]
        assert "error" not in cop, f"{s['id']}: {cop.get('error')}"
        assert len(cop["sha256"]) == 64
        assert cop["size_bytes"] > 1_000_000
        st = cop["stats"]
        assert st["grid"] == "aeqd30"
        assert st["pixel_size_m"] == 30.0
        assert st["slope_deg"]["mean"] >= 0.0
        assert "voids" in st


def test_manifest_covers_every_required_class(manifest):
    assert set(manifest["coverage"]["classes"]) >= REQUIRED_CLASSES


def test_manifest_names_its_gaps_rather_than_hiding_them(manifest):
    gaps = manifest["coverage"]["gaps"]
    assert gaps and all("gap" in g and "handled" in g for g in gaps)


def test_reprojection_crosscheck_agrees_at_every_site(manifest):
    """Two independent routes to a metric slope. They will not be identical --
    one resamples to 30 m square, the other does not resample at all -- but a
    factor-of-cos(lat) style bug would show up as a ratio far from 1."""
    for s in manifest["sites"]:
        cc = s["cop30"]["stats"]["crosscheck_native_ground"]
        r = cc["mean_slope_ratio_native_over_aeqd30"]
        assert 0.8 <= r <= 1.35, f"{s['id']}: native/aeqd30 mean slope ratio {r}"


def test_every_site_that_can_have_bare_earth_has_it(manifest):
    for s in manifest["sites"]:
        if "no_3dep_reason" in s:
            assert "usgs_3dep_1m" not in s
            continue
        u = s["usgs_3dep_1m"]
        assert "error" not in u, f"{s['id']}: {u.get('error')}"
        assert u["model_type"].startswith("DTM")
        assert u["stats"]["pixel_size_m"] == 1.0
        assert len(u["cache_sha256"]) == 64


def test_no_3dep_window_is_a_flight_line_gap(manifest):
    """A 3DEP tile is a full 10 km raster even where the aircraft did not fly,
    and the gap is -999999, not an absent tile. Two sites landed in one during
    development; a window that is mostly fill would characterise the hole."""
    for s in manifest["sites"]:
        u = s.get("usgs_3dep_1m")
        if not u or "error" in u:
            continue
        assert u["coverage_fraction"] >= er.USGS_MIN_VALID_FRACTION, s["id"]
        assert u["stats"]["valid_fraction"] >= er.USGS_MIN_VALID_FRACTION, s["id"]
        # Any slide away from the nominal site must be declared, not silent --
        # and must stay INSIDE the Copernicus window for the site, or the two
        # sources would be describing different ground.
        assert "offset_from_site_m" in u, s["id"]
        limit = er.COP30_WINDOW_KM * 1000.0 / 2.0 - u["window_m"] / 2.0
        assert u["offset_from_site_m"] <= limit, (
            f"{s['id']}: 3DEP window {u['offset_from_site_m']:.0f} m off-site is not "
            f"contained in the {er.COP30_WINDOW_KM} km Copernicus window"
        )


def test_dsm_bias_is_measured_where_both_sources_exist(manifest):
    for s in manifest["sites"]:
        if "usgs_3dep_1m" not in s:
            continue
        d = s["dsm_vs_dtm_at_30m"]
        assert "error" not in d, f"{s['id']}: {d.get('error')}"
        # Co-located, so the DSM can only add material, never remove it.
        assert d["dsm_over_dtm_mean_slope"] > 0.5, s["id"]


def test_manifest_findings_are_derived_from_the_data_not_asserted(manifest):
    """The findings block must agree with the per-site numbers it summarises,
    so it cannot drift into a claim the corpus does not support."""
    f = manifest["findings"]
    measured = {
        s["id"]: s["dsm_vs_dtm_at_30m"]["dsm_over_dtm_mean_slope"]
        for s in manifest["sites"]
        if "dsm_vs_dtm_at_30m" in s and "error" not in s["dsm_vs_dtm_at_30m"]
    }
    below = f["cop30_is_smoother_than_the_ground_at_its_own_posting"][
        "dsm_over_dtm_mean_slope_below_1"]
    above = f["dsm_bias_shows_up_in_elevation_far_more_than_in_slope"][
        "dsm_over_dtm_mean_slope_at_or_above_1"]
    assert {**below, **above} == measured
    assert all(v < 1.0 for v in below.values())
    assert all(v >= 1.0 for v in above.values())
    assert f["projection_crosscheck"]["ratios"] == {
        s["id"]: s["cop30"]["stats"]["crosscheck_native_ground"][
            "mean_slope_ratio_native_over_aeqd30"]
        for s in manifest["sites"]
    }


def test_manifest_records_masked_fractions_everywhere(manifest):
    for s in manifest["sites"]:
        for key in ("cop30", "usgs_3dep_1m"):
            src = s.get(key)
            if not src or "stats" not in src:
                continue
            v = src["stats"]["voids"]
            assert 0.0 <= v["masked_fraction"] <= 1.0
            assert src["stats"]["valid_fraction"] > 0.5, f"{s['id']}/{key}"


def test_plains_sites_really_are_flatter_than_the_mountains(manifest):
    """A sanity check on the whole pipeline: if the projection or the void
    masking were wrong, this ordering is the first thing that would break."""
    by_id = {s["id"]: s for s in manifest["sites"]}
    plains = by_id["llano_estacado_plains"]["cop30"]["stats"]["slope_deg"]["mean"]
    alpine = by_id["alps_valais_alpine"]["cop30"]["stats"]["slope_deg"]["mean"]
    smokies = by_id["smokies_appalachian"]["cop30"]["stats"]["slope_deg"]["mean"]
    assert plains < 1.5
    assert alpine > 20.0
    assert smokies > 10.0
