"""The lake/playa water balance and the basin registry (bake/basins.py).

The load-bearing test here is `test_same_bowl_lake_to_playa_across_a_climate_sweep`:
the SAME hole, swept from rainforest to desert, must walk down the five kinds
in order and never jump back up. That is the plan's whole lake rule (§4.3) --
"caused variety, not placed variety" -- reduced to something a machine can
check. Everything else in this file exists to keep the pieces that rule stands
on honest: a monotone PET, a Budyko curve with the right two limits, a
hypsometric curve that is a real CDF, and a registry whose exclusions are
counted rather than silent.

These run on a 200x200 synthetic bowl, not a bake: the point is the decision
function, and a test that needs 5.5 GiB and 150 CPU-seconds to assert a
classification would never be run.
"""

from __future__ import annotations

import numpy as np
import pytest

from terrain_service.bake import basins as bs
from terrain_service.bake import flow

CELL_M = 1.875


# --------------------------------------------------------------------------- climate


@pytest.fixture(scope="module")
def _numba():
    """The bake kernels need numba, which is deliberately NOT a terrain-service
    requirement -- CI runs without it, and `flow.py` is built so importing the
    package never needs it and only CALLING a kernel does. Every test below that
    reaches a kernel takes this fixture so it skips cleanly there instead of
    failing with ModuleNotFoundError. The two tests that do not take it are the
    ones that must keep running on a numba-less box."""
    return pytest.importorskip("numba", reason="bake kernels need numba; not a CI dep")


def test_pet_is_monotone_over_the_whole_world_temperature_range():
    """L(T) must never decrease with temperature, including where it is floored.

    The world's climate planes span -40 to +40 degC (province.CLIMATE_RANGES).
    Turc's L(T) = 300 + 25T + 0.05T^3 goes NEGATIVE below about -11 degC, which
    is outside the range it was fitted on; the floor is what keeps it a
    physically meaningful "a polar basin evaporates a little" rather than a
    negative amount. Monotone across the join is what makes the equilibrium
    solve single-valued.
    """
    t = np.linspace(-40.0, 40.0, 801)
    pet = bs.pet_mm_yr(t)
    assert np.all(np.diff(pet) >= 0.0)
    assert pet.min() == pytest.approx(bs.WaterBalance().pet_floor_mm)
    # And it is in the right ballpark where it matters: a 20 degC catchment.
    assert float(bs.pet_mm_yr(20.0)) == pytest.approx(1200.0)  # 300 + 500 + 400


def test_budyko_has_both_limits_and_never_yields_more_than_it_rains():
    """The two limits are the reason to use a curve rather than max(P-PET, 0)."""
    pet = 1000.0
    # Water-limited: a desert yields nothing, not a negative amount.
    assert float(bs.budyko_runoff_mm_yr(1.0, pet)) < 0.001
    # Energy-limited: a rainforest yields P - PET.
    q = float(bs.budyko_runoff_mm_yr(100000.0, pet))
    assert q == pytest.approx(100000.0 - pet, rel=1e-3)
    # Never outside [0, P], anywhere.
    p = np.geomspace(1.0, 12000.0, 400)
    for pe in (100.0, 500.0, 1500.0, 2000.0):
        qq = bs.budyko_runoff_mm_yr(p, pe)
        assert np.all(qq >= 0.0) and np.all(qq <= p + 1e-9)
        assert np.all(np.diff(qq) > 0.0)  # more rain always means more runoff
    # At P == PET, Earth's data put E/P near 0.7; Turc-Pike gives 1/sqrt(2).
    assert float(bs.budyko_runoff_mm_yr(pet, pet)) / pet == pytest.approx(
        1.0 - 1.0 / np.sqrt(2.0), rel=1e-6)


# --------------------------------------------------------------------------- rasters


def _bowl(n: int = 200, depth_m: float = 20.0, radius: float = 40.0,
          cx: float = 100.0, cy: float = 100.0, ramp: float = 0.05):
    """A paraboloid hollow on a plane that drains west.

    Returns ``(z_final, basin_depth)`` in the shape the bake hands over: the
    FILLED surface plus the depth the fill added, so ``reopened_surface``
    reconstructs the hollow exactly.
    """
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    plane = 100.0 + ramp * x
    r = np.hypot(x - cx, y - cy)
    z_open = plane - np.maximum(0.0, depth_m * (1.0 - (r / radius) ** 2))
    filled = np.asarray(flow.fill_depressions(z_open, flat_eps=0.0), np.float32)
    return filled, np.asarray(filled - z_open, np.float32)


def _climate(shape, *, temp_c, precip_mm, cv=20.0, seasonality=400.0):
    n = max(shape[0] // 16, 1)
    return {
        "temperature": np.full((n, n), temp_c, np.float32),
        "seasonality": np.full((n, n), seasonality, np.float32),
        "precipitation": np.full((n, n), precip_mm, np.float32),
        "precip_variability": np.full((n, n), cv, np.float32),
    }


def test_reopened_surface_is_exact_and_refuses_a_raised_floor(_numba):
    z, d = _bowl()
    reopened = bs.reopened_surface(z, d)
    assert np.allclose(reopened, z - d)
    # basin_depth is filled - carrier and the fill only ever RAISES, so a
    # negative entry means the caller passed the wrong pair of rasters --
    # which would silently push ground UP instead of re-opening a hole.
    with pytest.raises(ValueError):
        bs.reopened_surface(z, -d - 1.0)


def test_depression_is_found_and_its_spill_is_the_rim_not_the_b2a_level(_numba):
    z, d = _bowl(depth_m=20.0)
    z_open = bs.reopened_surface(z, d)
    labels, filled, n = bs.depression_components(z_open)
    assert n == 1
    mask = labels == 1
    # flat_eps=0.0 means the filled depression is EXACTLY level -- that is what
    # makes the spill readable off the raster instead of inferred.
    assert filled[mask].min() == pytest.approx(filled[mask].max())
    # And the level is the rim of the hollow on the surface that ships.
    assert filled[mask].max() == pytest.approx(float(z[mask].max()), abs=1e-3)


def test_hypsometry_is_a_monotone_curve_the_balance_can_be_inverted_on(_numba):
    z, d = _bowl()
    z_open = bs.reopened_surface(z, d)
    labels, filled, _ = bs.depression_components(z_open)
    mask = labels == 1
    spill = float(filled[mask].max())
    levels, areas = bs.hypsometry(z_open[mask], spill, CELL_M ** 2)
    assert np.all(np.diff(levels) > 0.0)
    assert np.all(np.diff(areas) >= 0.0)
    assert areas[-1] == pytest.approx(int(mask.sum()) * CELL_M ** 2)
    # Inverting at a known area returns a level inside the basin.
    h, need = bs.equilibrium_level(levels, areas, inflow_m3_yr=1000.0,
                                   precip_mm=100.0, pet_mm=1100.0)
    assert levels[0] <= h <= levels[-1]
    assert need == pytest.approx(1000.0 / ((1100.0 - 100.0) / 1000.0))
    # P >= PET is the honest infinity, not a clamp: no finite lake area can
    # evaporate the inflow, so the basin fills to its spill whatever it is.
    h2, need2 = bs.equilibrium_level(levels, areas, 1000.0, 1200.0, 1100.0)
    assert h2 == float("inf") and need2 == float("inf")


# --------------------------------------------------------------------------- the rule


def _survey_bowl(temp_c: float, precip_mm: float, *, cv: float = 20.0,
                 catchment_m2: float = 5.0e6, **bowl_kwargs) -> bs.BasinRecord:
    z, d = _bowl(**bowl_kwargs)
    acc = np.full(z.shape, catchment_m2, np.float32)
    s = bs.survey_basins(
        z_final=z, basin_depth=d, accumulation_m2=acc,
        climate=_climate(z.shape, temp_c=temp_c, precip_mm=precip_mm, cv=cv),
        cell_m=CELL_M,
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False))
    assert len(s.basins) == 1, f"expected one bowl, got {len(s.basins)}"
    return s.basins[0]


def test_same_bowl_lake_to_playa_across_a_climate_sweep(_numba):
    """THE RULE. One hole, one climate axis, five outcomes, in order.

    This is what "a wet-climate depression is a lake; the same depression in a
    desert is a salt pan" means operationally, and what makes the variety
    CAUSED -- a player can trace the inflow valley that explains it. If this
    ever passes with the ordering scrambled, the classification has stopped
    being a water balance and become a lookup table.
    """
    precips = [2500.0, 1200.0, 600.0, 300.0, 150.0, 60.0, 20.0, 5.0]
    kinds = [_survey_bowl(25.0, p).kind for p in precips]
    depths = [_survey_bowl(25.0, p).water_depth_m for p in precips]

    # Wettest is an overflowing lake; driest holds no water at all.
    assert kinds[0] == bs.KIND_LAKE_OVERFLOWING
    assert kinds[-1] <= bs.KIND_SALT_FLAT
    # Never wetter as it gets drier, in either the kind or the level.
    assert kinds == sorted(kinds, reverse=True), kinds
    assert all(a >= b - 1e-9 for a, b in zip(depths, depths[1:])), depths
    # Every one of the wet kinds is actually reached somewhere on the sweep --
    # a rule that only ever emits two of its five values is not a rule.
    assert bs.KIND_LAKE_OVERFLOWING in kinds
    assert bs.KIND_LAKE_TERMINAL in kinds


def test_seasonality_separates_a_seasonal_lake_from_a_dry_playa(_numba):
    """Same annual mean, different bio_15: the only thing that can tell them apart.

    A monsoon desert and an even-rainfall desert receive the same millimetres
    and behave completely differently, and bio_15 is already in the tile.
    """
    dry_even = _survey_bowl(25.0, 40.0, cv=10.0)
    dry_monsoon = _survey_bowl(25.0, 40.0, cv=90.0)
    assert dry_even.water_depth_m == pytest.approx(dry_monsoon.water_depth_m)
    assert dry_even.kind != bs.KIND_SEASONAL
    assert dry_monsoon.kind == bs.KIND_SEASONAL


def test_cold_basins_are_lakes_because_pet_falls_not_because_it_rains(_numba):
    """The temperature half of the balance, isolated.

    Held at a genuinely arid 200 mm/yr, the same bowl is dry in the heat and a
    lake in the cold -- because PET, not P, moved. Tundra ponds are the real
    phenomenon, and this is the axis that produces them.
    """
    hot = _survey_bowl(30.0, 200.0)
    cold = _survey_bowl(-10.0, 200.0)
    assert hot.pet_mm > cold.pet_mm
    assert cold.water_depth_m > hot.water_depth_m
    assert cold.kind >= bs.KIND_LAKE_TERMINAL


def test_a_bigger_catchment_makes_a_deeper_lake_in_the_same_hole(_numba):
    """Inflow is the third input, and it is the one the watershed supplies."""
    small = _survey_bowl(25.0, 250.0, catchment_m2=1.0e5)
    big = _survey_bowl(25.0, 250.0, catchment_m2=5.0e7)
    assert big.water_depth_m > small.water_depth_m
    assert big.catchment_m2 > small.catchment_m2


# --------------------------------------------------------------------------- registry


def test_registry_filter_counts_everything_it_removes(_numba):
    """Each exclusion is a NUMBER, which is the point of the whole survey."""
    z, d = _bowl(depth_m=20.0)
    acc = np.full(z.shape, 1.0e6, np.float32)
    tight = bs.survey_basins(
        z_final=z, basin_depth=d, accumulation_m2=acc,
        climate=_climate(z.shape, temp_c=20.0, precip_mm=1500.0),
        cell_m=CELL_M,
        filt=bs.BasinFilter(min_depth_m=50.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False))
    assert tight.basins == []
    assert tight.excluded_shallow == tight.n_components == 1

    huge_area = bs.survey_basins(
        z_final=z, basin_depth=d, accumulation_m2=acc,
        climate=_climate(z.shape, temp_c=20.0, precip_mm=1500.0),
        cell_m=CELL_M,
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=1.0e9,
                            exclude_spanning=False, require_above_sea=False))
    assert huge_area.basins == [] and huge_area.excluded_small == 1


def test_submarine_depressions_are_sea_floor_not_lakes(_numba):
    """A hole whose SPILL is under water is the ocean, and shipping it as a
    lake would put a second water surface beneath the first.

    Not in the plan; added because the survey found one -- tile (-8,-14)'s
    largest basin spills at -433 m.
    """
    z, d = _bowl()
    z = z - 600.0  # sink the whole scene below sea level
    acc = np.full(z.shape, 1.0e6, np.float32)
    kwargs = dict(z_final=z, basin_depth=d, accumulation_m2=acc,
                  climate=_climate(z.shape, temp_c=20.0, precip_mm=1500.0),
                  cell_m=CELL_M)
    refused = bs.survey_basins(
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=True),
        **kwargs)
    assert refused.basins == [] and refused.excluded_submarine == 1
    kept = bs.survey_basins(
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False),
        **kwargs)
    assert len(kept.basins) == 1 and kept.excluded_submarine == 1


def test_tile_spanning_basins_are_excluded_and_their_size_recorded(_numba):
    """§4.2.4's v1 rule, with the cost of it as a measured quantity."""
    # One bowl inside the interior, one straddling its edge.
    z, d = _bowl(n=200, cx=100.0, cy=100.0, radius=20.0)
    z2, d2 = _bowl(n=200, cx=45.0, cy=100.0, radius=20.0)
    z = np.minimum(z, z2)
    d = np.maximum(d, d2)
    acc = np.full(z.shape, 1.0e6, np.float32)
    interior = slice(40, 160)  # a 120-px "tile" inside a 200-px padded domain
    kwargs = dict(z_final=z, basin_depth=d, accumulation_m2=acc,
                  climate=_climate(z.shape, temp_c=20.0, precip_mm=1500.0),
                  cell_m=CELL_M, interior=interior)
    kept = bs.survey_basins(
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False),
        **kwargs)
    excl = bs.survey_basins(
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=True, require_above_sea=False),
        **kwargs)
    assert len(kept.basins) == 2
    assert len(excl.basins) == 1
    assert excl.excluded_spanning == 1
    # The cost is a SIZE, not just a count.
    assert excl.excluded_spanning_area_m2 > 0.0
    assert excl.excluded_spanning_max_depth_m > 0.0
    assert excl.basins[0].interior is True


def test_a_spanning_basin_gets_the_same_global_id_from_both_sides(_numba):
    """THE CROSS-TILE IDENTITY CLAIM, on one surface seen twice.

    Two neighbouring tiles never talk to each other at bake time. What makes a
    lake that straddles their seam ONE lake is that both of them name it after
    the same physical thing: its deepest cell, in absolute world pixels. This
    is that property, exercised the way the bake produces it -- the same
    surface cropped into two overlapping padded domains with different world
    origins, which is exactly what two adjacent bakes see.

    The two windows here are 40 px apart with the bowl in the shared strip, and
    the bowl's own rim is inside BOTH windows -- which is not a convenience,
    it is the condition under which a basin is registered at all: a hollow that
    reaches the padded border drains out of the domain and is not a depression
    (see BasinFilter.border_margin_px). A basin too big for a tile's pad is
    absent from that tile rather than disagreeing with the neighbour.
    """
    # One 200 px world with a bowl straddling world x = 100. Two 80 px tiles
    # meet there -- A owns world x 20..99, B owns 100..179 -- and each pads 20
    # px past its own edge, so each sees the WHOLE bowl and each sees it leave
    # its own interior.
    z, d = _bowl(n=200, cx=100.0, cy=60.0, radius=14.0, depth_m=20.0)
    acc = np.full(z.shape, 1.0e6, np.float32)
    filt = bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                          exclude_spanning=False, require_above_sea=False)

    def survey(x0, world0):
        """One tile: the padded window x in [x0, x0+120), y in [0, 120)."""
        return bs.survey_basins(
            z_final=z[0:120, x0:x0 + 120], basin_depth=d[0:120, x0:x0 + 120],
            accumulation_m2=acc[0:120, x0:x0 + 120],
            # Deliberately DRY enough that the lake settles below its spill
            # (measured on this bowl: 250 mm/yr still overflows, 120 does not):
            # an overflowing lake has capacity 0 on both sides and the capacity
            # agreement below would then be vacuously true.
            climate=_climate((120, 120), temp_c=20.0, precip_mm=120.0),
            cell_m=CELL_M, interior=slice(20, 100), filt=filt,
            world_origin_px=world0)

    a = survey(0, (20, 20))      # interior world x 20..99
    b = survey(80, (100, 20))    # interior world x 100..179
    assert len(a.basins) == 1 and len(b.basins) == 1
    ra, rb = a.basins[0], b.basins[0]

    # 1. THE IDENTITY AGREES, with no communication between the two calls.
    assert ra.global_id == rb.global_id
    assert ra.world_floor_px == rb.world_floor_px == (100, 60)
    assert bs.world_px_from_global_id(ra.global_id) == (100, 60)

    # 2. Both know they are half a lake, and both say so.
    assert not ra.interior and not rb.interior
    assert a.kept_spanning == b.kept_spanning == 1
    assert a.excluded_spanning == b.excluded_spanning == 1  # counted, not dropped

    # 3. The physics agrees too -- the numbers that DECIDE the lake come from
    #    the component, not from the crop, or the two rows could never be
    #    reconciled even with a matching id.
    assert ra.spill_m == pytest.approx(rb.spill_m)
    assert ra.floor_m == pytest.approx(rb.floor_m)
    assert ra.surface_m == pytest.approx(rb.surface_m)
    assert ra.capacity_m3 == pytest.approx(rb.capacity_m3)
    assert ra.capacity_m3 > 0.0, "a terminal lake must have headroom to spill"
    assert ra.area_cells == rb.area_cells

    # 4. THE FALLBACK PATH: the absolute extents OVERLAP (each tile sees into
    #    the other through its apron), so a client can union these two rows
    #    even if the ids ever disagreed. Clipped bboxes could not do this --
    #    they only abut at the seam.
    ax0, ay0, ax1, ay1 = ra.world_bbox_px
    bx0, by0, bx1, by1 = rb.world_bbox_px
    assert (ax0, ay0, ax1, ay1) == (bx0, by0, bx1, by1)
    assert ax0 <= bx1 and bx0 <= ax1 and ay0 <= by1 and by0 <= ay1

    # 5. The LOCAL views differ, which is the whole reason 1-4 are needed: each
    #    tile's seed and clipped bbox address its own pixels only.
    assert ra.seed_px != rb.seed_px
    assert 0 <= ra.seed_px[0] < 80 and 0 <= ra.seed_px[1] < 80
    assert 0 <= rb.seed_px[0] < 80 and 0 <= rb.seed_px[1] < 80
    assert ra.interior_cells > 0 and rb.interior_cells > 0
    assert ra.interior_cells + rb.interior_cells <= ra.area_cells


def test_a_basin_wholly_in_the_apron_is_the_neighbours_and_is_refused(_numba):
    """It has no pixel in this tile: no seed a client could index, and B5 would
    subtract its depth entirely outside the shipped crop.

    Not a corner case at production scale -- the apron adds 62 km^2 around a
    236 km^2 tile, so a quarter again of every tile's padded domain is other
    tiles' basins. Refused unconditionally and counted, because "we found 40
    basins and shipped 31" has to be a number.
    """
    z, d = _bowl(n=200, cx=30.0, cy=30.0, radius=12.0, depth_m=20.0)
    acc = np.full(z.shape, 1.0e6, np.float32)
    s = bs.survey_basins(
        z_final=z, basin_depth=d, accumulation_m2=acc,
        climate=_climate(z.shape, temp_c=20.0, precip_mm=1500.0),
        cell_m=CELL_M, interior=slice(80, 160),
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False))
    assert s.basins == []
    assert s.excluded_outside == 1
    assert s.kept_spanning == 0


def test_capacity_is_the_headroom_to_the_spill_and_zero_when_overflowing(_numba):
    """`capacity_m3` is what Phase 2's spillway rule compares a ledger against.

    Two properties matter and neither is obvious from the name: it is measured
    from the EQUILIBRIUM SURFACE (not the floor), and it is exactly 0 for a
    lake already standing at its spill -- correct, since every extra litre
    leaves by the outlet.
    """
    z, d = _bowl(depth_m=20.0)
    z_open = bs.reopened_surface(z, d)
    labels, filled, _ = bs.depression_components(z_open)
    mask = labels == 1
    spill = float(filled[mask].max())
    floor = float(z_open[mask].min())
    levels, areas = bs.hypsometry(z_open[mask], spill, CELL_M ** 2)

    full = bs.capacity_m3(levels, areas, floor, spill)
    half = bs.capacity_m3(levels, areas, (floor + spill) / 2.0, spill)
    assert bs.capacity_m3(levels, areas, spill, spill) == 0.0
    assert 0.0 < half < full
    # A bowl is wider at the top, so the upper half holds MORE than half the
    # volume -- a monotone A(h) is the property the trapezoid integrates.
    assert half > full / 2.0
    # Bounded by the two rectangles the true volume must sit between.
    assert full <= areas[-1] * (spill - floor)
    assert full >= areas[0] * (spill - floor)


def test_padded_border_flag_is_a_near_miss_test_because_the_exact_one_is_dead(_numba):
    """``fill_depressions`` never raises a border cell, so no depression can
    contain one and a touches-the-border test can NEVER fire.

    ``pipeline.py``'s shipped ``basin_reaches_padded_border`` /
    ``padded_border_basin_frac`` are exactly that test, and are therefore
    structurally zero on every tile ever baked. This asserts the mechanism (so
    the claim is checked, not just written down) and that the replacement --
    a margin -- does fire.
    """
    n = 60
    z = np.full((n, n), 100.0, np.float32)
    z[0:12, 0:12] -= 20.0  # a hollow whose corner IS the domain corner
    filled = np.asarray(flow.fill_depressions(z, flat_eps=0.0), np.float32)
    basin_depth = filled - z
    assert basin_depth[0, :].sum() == 0.0
    assert basin_depth[:, 0].sum() == 0.0
    # ... and in fact it is not a depression at all: it drains out.
    assert (basin_depth > 0).sum() == 0

    # A hollow set back from the edge IS a depression, and the margin sees it.
    z2, d2 = _bowl(n=60, cx=12.0, cy=30.0, radius=8.0, depth_m=10.0)
    acc = np.full(z2.shape, 1.0e6, np.float32)
    s = bs.survey_basins(
        z_final=z2, basin_depth=d2, accumulation_m2=acc,
        climate=_climate(z2.shape, temp_c=20.0, precip_mm=1500.0),
        cell_m=CELL_M,
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False,
                            border_margin_px=6))
    assert len(s.basins) == 1
    assert s.basins[0].near_padded_border is True
    assert s.kept_near_padded_border == 1


def test_basin_ids_are_deterministic_and_ordered_by_extent(_numba):
    """Ids order by (min_y, min_x) of extent, per P1's schema, so a table row
    means the same basin on any box and in any re-run."""
    z_a, d_a = _bowl(n=220, cx=60.0, cy=150.0, radius=18.0)
    z_b, d_b = _bowl(n=220, cx=160.0, cy=40.0, radius=18.0)
    z = np.minimum(z_a, z_b)
    d = np.maximum(d_a, d_b)
    acc = np.full(z.shape, 1.0e6, np.float32)
    kwargs = dict(z_final=z, basin_depth=d, accumulation_m2=acc,
                  climate=_climate(z.shape, temp_c=20.0, precip_mm=1500.0),
                  cell_m=CELL_M,
                  filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                                      exclude_spanning=False,
                                      require_above_sea=False))
    s1 = bs.survey_basins(**kwargs)
    s2 = bs.survey_basins(**kwargs)
    assert len(s1.basins) == 2
    keys = [(b.bbox_px[1], b.bbox_px[0]) for b in s1.basins]
    assert keys == sorted(keys)
    assert [b.as_dict() for b in s1.basins] == [b.as_dict() for b in s2.basins]
    # The northern bowl (smaller min_y) is basin 0.
    assert s1.basins[0].seed_px[1] < s1.basins[1].seed_px[1]


def test_outlet_is_the_lowest_cell_on_the_rim(_numba):
    """``outlet_px`` is the head of the outlet channel, and for an overflowing
    lake it is where the spillway starts -- so it has to be the saddle, not
    just any neighbour."""
    z, d = _bowl(depth_m=20.0, ramp=0.05)
    acc = np.full(z.shape, 1.0e6, np.float32)
    s = bs.survey_basins(
        z_final=z, basin_depth=d, accumulation_m2=acc,
        climate=_climate(z.shape, temp_c=20.0, precip_mm=2500.0),
        cell_m=CELL_M,
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False))
    b = s.basins[0]
    z_open = bs.reopened_surface(z, d)
    ox, oy = b.outlet_px
    labels, _, _ = bs.depression_components(z_open)
    mask = labels == 1
    # The outlet is OUTSIDE the basin -- it is the cell water leaves through,
    # not the last cell it occupies.
    assert not mask[oy, ox]
    # ... and its elevation IS the spill level, which is what makes
    # `spill_m` and `outlet_px` two views of the same fact.
    assert float(z_open[oy, ox]) == pytest.approx(b.spill_m, abs=1e-3)
    # No cell of the basin's 8-neighbourhood rim is lower than the outlet.
    rim = np.zeros_like(mask)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            rim |= np.roll(np.roll(mask, dy, 0), dx, 1)
    rim &= ~mask
    assert float(z_open[oy, ox]) == pytest.approx(float(z_open[rim].min()), abs=1e-4)


# --------------------------------------------------------------------------- extent


def test_lake_extent_is_the_component_that_holds_the_seed_not_the_threshold(_numba):
    """Two bowls under ONE bbox: the fill must return only the seeded one.

    This is the whole reason a lake's footprint is a flood fill rather than
    ``z_open <= surface``. A bbox large enough to contain two hollows -- or a
    hillside that merely happens to lie below the water level -- passes the
    threshold everywhere, and a client that drew that would flood dry ground.
    """
    zl, dl = _bowl(depth_m=20.0, cx=60.0, cy=100.0, radius=25.0, ramp=0.0)
    zr, dr = _bowl(depth_m=20.0, cx=140.0, cy=100.0, radius=25.0, ramp=0.0)
    z_open = np.minimum(bs.reopened_surface(zl, dl), bs.reopened_surface(zr, dr))
    surface = 100.0 - 5.0                       # 5 m of water in each bowl
    both = z_open <= surface
    assert both[100, 60] and both[100, 140]     # the threshold takes both

    m = bs.lake_extent_mask(z_open, (60, 100), surface,
                            (0, 0, z_open.shape[1] - 1, z_open.shape[0] - 1))
    assert m[100, 60]
    assert not m[100, 140]
    assert m.sum() < both.sum()
    # ... and seeding the other bowl returns the other component, disjointly.
    m2 = bs.lake_extent_mask(z_open, (140, 100), surface,
                             (0, 0, z_open.shape[1] - 1, z_open.shape[0] - 1))
    assert not (m & m2).any()
    assert (m | m2).sum() == both.sum()


def test_lake_extent_agrees_with_the_registry_area_it_ships_beside(_numba):
    """A basin filled to its SPILL must cover exactly the area the registry
    recorded for it.

    The two numbers travel together on the wire (``area_m2`` in the survey,
    ``bbox``+``seed``+``surface_mm`` in the table), and a footprint that
    disagrees with its own row is a shoreline that does not close. Both are
    8-connected components of the same surface, so this is exact, not close.
    """
    z, d = _bowl(depth_m=20.0, ramp=0.05)
    acc = np.full(z.shape, 1.0e6, np.float32)
    s = bs.survey_basins(
        z_final=z, basin_depth=d, accumulation_m2=acc,
        climate=_climate(z.shape, temp_c=5.0, precip_mm=3000.0),
        cell_m=CELL_M,
        filt=bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                            exclude_spanning=False, require_above_sea=False))
    b = s.basins[0]
    assert b.kind == bs.KIND_LAKE_OVERFLOWING          # so surface == spill
    m = bs.lake_extent_mask(bs.reopened_surface(z, d), b.seed_px, b.surface_m,
                            b.bbox_px)
    assert int(m.sum()) == b.area_cells
    assert float(m.sum()) * CELL_M ** 2 == pytest.approx(b.area_m2)


def test_lake_extent_of_a_dry_basin_is_empty_rather_than_an_error(_numba):
    """A playa's ``surface_m`` is its floor, so nothing is at or below it minus
    epsilon. That is an answer, not a failure -- the client must be able to ask
    about every registered basin, including the dry ones."""
    z, d = _bowl(depth_m=20.0, ramp=0.05)
    z_open = bs.reopened_surface(z, d)
    fy, fx = np.unravel_index(int(np.argmin(z_open)), z_open.shape)
    floor = float(z_open[fy, fx])
    assert not bs.lake_extent_mask(z_open, (fx, fy), floor - 0.001).any()
    # Exactly AT the floor is one cell, not zero: the datum is inclusive.
    assert bs.lake_extent_mask(z_open, (fx, fy), floor).sum() >= 1


def test_lake_extent_is_clipped_by_the_bbox_it_is_given(_numba):
    """The bbox is not decoration: it bounds the fill, so a decoder cannot be
    walked across the tile by a corrupt seed and a high surface."""
    z, d = _bowl(depth_m=20.0, ramp=0.0)
    z_open = bs.reopened_surface(z, d)
    full = bs.lake_extent_mask(z_open, (100, 100), 95.0)
    clipped = bs.lake_extent_mask(z_open, (100, 100), 95.0, (90, 90, 110, 110))
    assert clipped.sum() < full.sum()
    assert not clipped[:90].any() and not clipped[111:].any()
    with pytest.raises(ValueError):
        bs.lake_extent_mask(z_open, (10, 10), 95.0, (90, 90, 110, 110))
