"""B5: the bake keeps the hole (docs/water-system-architecture.md item 3, §4.2).

Two claims are worth a test here and the rest is covered by test_basins.py:

1. **The elevation plane changes, and only inside registered basins.** Every
   stage before B5 still runs on the depression-FILLED surface -- that is what
   keeps routing, the flow plane and incision agreeing cell for cell -- so the
   ONLY thing B5 may touch is what elevation ships.

2. **The drainage contract is restated, not broken.** It used to be "the
   carrier drains: 0 sinks". It is now "the carrier drains once every
   registered basin is virtually filled to its recorded spill", and dry
   playas violate the old form ON PURPOSE. A test that only checked the old
   contract would fail; a test that checked nothing would let a real routing
   bug through as "that's a lake".

These use the REAL depression fill (a reference identity fill leaves
``basin_depth`` at zero and B5 with nothing to do) over reference kernels for
everything else, on a coarse world with a crater put there on purpose.
"""

from __future__ import annotations

import dataclasses

import numpy as np
import pytest

pipeline = pytest.importorskip("terrain_service.bake.pipeline")
flow = pytest.importorskip("terrain_service.bake.flow")
basins = pytest.importorskip("terrain_service.bake.basins")

from test_bake_pipeline import (  # noqa: E402
    TEST_CONSTS,
    TEST_GEOM,
    kernels,
    ramp_world,
)


def crater_world(depth_m=40.0, radius=2.0, at=(0, 0), tiles=range(-3, 5),
                 geom=TEST_GEOM):
    """``ramp_world`` with a paraboloid crater punched into one coarse tile.

    A ramp so the surroundings drain unambiguously, and a crater deep enough
    (40 m over ~5 coarse cells against a ramp that climbs 4 m per cell) that
    the fill cannot simply spill it out sideways.
    """
    world = ramp_world(tiles=tiles, geom=geom)
    n = geom.coarse_tile_px
    z = world[at].copy()
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    cx = cy = (n - 1) / 2.0
    r = np.hypot(xx - cx, yy - cy)
    z -= np.maximum(0.0, depth_m * (1.0 - (r / radius) ** 2)).astype(np.float32)
    world[at] = z
    return world


def real_fill_kernels():
    """Reference kernels with the REAL priority-flood swapped in."""
    return dataclasses.replace(kernels(), fill_depressions=flow.fill_depressions)


def _bake_crater(consts=None, **crater_kwargs):
    world = crater_world(**crater_kwargs)
    return world, pipeline.bake_tile(
        world_seed=20260719, tile_x=0, tile_y=0,
        coarse_fetch=lambda x, y: world.get((x, y)),
        kernels=real_fill_kernels(),
        geom=TEST_GEOM,
        consts=consts or TEST_CONSTS,
    )


def _permissive(consts):
    """The same bake with a filter that registers essentially every hole.

    Used where a test needs a basin to exist at the harness's tiny scale --
    the production filter's 2 m / 2500 m2 is sized for 1.875 m pixels over a
    15.36 km tile, and TEST_GEOM's tile is 240 m across.
    """
    return dataclasses.replace(consts, basin_min_depth_m=1.0,
                               basin_min_area_m2=0.0)


def test_the_crater_survives_the_bake_and_is_registered():
    """The headline: a hole that used to be levelled into rock is still a hole.

    Before B5 the bake guaranteed no basin reached the client -- the plan's F1
    -- so there was nowhere for a lake to sit. This is that guarantee being
    deliberately given up, for registered basins only.
    """
    pytest.importorskip("scipy")
    consts = _permissive(TEST_CONSTS)
    _, r = _bake_crater(consts=consts)
    assert r.basins, "the crater was not registered"
    assert r.stats["basins_registered"] == float(len(r.basins))

    b = max(r.basins, key=lambda x: x.depth_m)
    sx, sy = b.seed_px
    # The shipped surface at the basin's deepest cell is BELOW that basin's
    # spill -- i.e. there is a hole there, not a flat.
    assert r.elevation_m[sy, sx] < b.spill_m
    assert b.spill_m - r.elevation_m[sy, sx] == pytest.approx(b.depth_m, abs=1e-3)


def test_b5_changes_the_elevation_plane_only_inside_registered_basins():
    """Everything outside a registered basin must be bit-identical to bake_ver 7.

    The comparison is against the same bake with the registry filter set so
    high that nothing qualifies -- which is exactly the old behaviour, since
    B5's only effect is the subtraction it then never performs.
    """
    pytest.importorskip("scipy")
    consts = _permissive(TEST_CONSTS)
    none_consts = dataclasses.replace(consts, basin_min_depth_m=1e9)
    _, kept = _bake_crater(consts=consts)
    _, filled = _bake_crater(consts=none_consts)
    assert kept.basins and not filled.basins

    changed = kept.elevation_m != filled.elevation_m
    assert changed.any(), "B5 did nothing at all"
    inside = np.zeros_like(changed)
    for b in kept.basins:
        x0, y0, x1, y1 = b.bbox_px
        inside[y0:y1 + 1, x0:x1 + 1] = True
    assert not (changed & ~inside).any(), (
        "B5 lowered ground outside every registered basin's extent")
    # And it only ever LOWERS: re-opening a hole cannot raise terrain.
    assert (kept.elevation_m <= filled.elevation_m + 1e-6).all()


def test_the_flow_plane_is_untouched_by_b5():
    """Routing ran on the filled surface and must keep agreeing with the carve.

    Water flows ACROSS a lake at its surface, so the flow plane belongs on the
    filled surface -- and the incision it describes was computed there too. If
    B5 fed back into the flow plane the two would disagree cell for cell.
    """
    pytest.importorskip("scipy")
    consts = _permissive(TEST_CONSTS)
    none_consts = dataclasses.replace(consts, basin_min_depth_m=1e9)
    _, kept = _bake_crater(consts=consts)
    _, filled = _bake_crater(consts=none_consts)
    assert np.array_equal(kept.flow, filled.flow)
    assert np.array_equal(kept.accumulation_m2, filled.accumulation_m2)


def test_the_restated_drainage_contract_holds():
    """"The carrier drains once every registered basin is filled to its spill."

    The old contract was 0 sinks, full stop. A dry playa breaks it on purpose:
    an endorheic depression that does not drain IS the feature. So the new
    contract clamps each registered basin to its recorded spill and then
    demands 0 sinks -- which still catches a real routing bug, because a sink
    outside the registry has nothing to clamp it.
    """
    pytest.importorskip("scipy")
    consts = _permissive(TEST_CONSTS)
    _, r = _bake_crater(consts=consts)
    assert r.basins

    z = r.elevation_m.astype(np.float32).copy()
    for b in r.basins:
        x0, y0, x1, y1 = b.bbox_px
        sub = z[y0:y1 + 1, x0:x1 + 1]
        np.maximum(sub, np.float32(b.spill_m), out=sub)
    filled = np.asarray(flow.fill_depressions(z, flat_eps=0.0), np.float32)
    # Interior only: the domain border is never raised by the fill, so a
    # sink test that included it would be testing the boundary condition.
    sinks = int(((filled - z)[1:-1, 1:-1] > 1e-4).sum())
    assert sinks == 0, f"{sinks} cells still sink after filling every basin to its spill"


def test_every_exclusion_is_counted_in_the_stats():
    """The plan asks for the cost of the tile-spanning refusal to be a number.

    Not a spot check on one field: each counter has to be present, so a later
    change that drops one is a failure here rather than a silently missing
    line in a survey.
    """
    pytest.importorskip("scipy")
    consts = _permissive(TEST_CONSTS)
    _, r = _bake_crater(consts=consts)
    for key in ("basins_registered", "basins_lake", "basin_components",
                "basins_excluded_shallow", "basins_excluded_small",
                "basins_excluded_spanning", "basins_excluded_spanning_area_m2",
                "basins_excluded_spanning_max_depth_m",
                "basins_excluded_submarine", "basins_near_padded_edge",
                "basin_water_volume_m3"):
        assert key in r.stats, key
    assert r.stats["basin_components"] >= r.stats["basins_registered"]


def test_the_dead_padded_border_stat_is_still_zero():
    """``basin_reaches_padded_border`` cannot fire and this records that.

    ``fill_depressions`` never raises a border cell, so ``filled - carrier``
    is identically 0 along the padded edges and no depression can contain one.
    The stat is kept at zero rather than deleted (the plan and three stat
    files quote it), and this is the test that says so out loud so the next
    reader does not build another exclusion on it.
    """
    pytest.importorskip("scipy")
    consts = _permissive(TEST_CONSTS)
    _, r = _bake_crater(consts=consts)
    assert r.stats["basin_reaches_padded_border"] == 0.0
    assert r.stats["padded_border_basin_cells"] == 0.0


def test_basin_ids_and_the_bake_are_deterministic():
    """Two bakes of one tile must produce the same table, row for row.

    Ids order by (min_y, min_x) of extent, so the id is a pure function of the
    surface -- which is what lets a client index the table by id and a server
    mean the same basin.
    """
    pytest.importorskip("scipy")
    consts = _permissive(TEST_CONSTS)
    _, a = _bake_crater(consts=consts)
    _, b = _bake_crater(consts=consts)
    assert [x.as_dict() for x in a.basins] == [x.as_dict() for x in b.basins]
    assert np.array_equal(a.elevation_m, b.elevation_m)
    keys = [(x.bbox_px[1], x.bbox_px[0]) for x in a.basins]
    assert keys == sorted(keys)
    assert [x.basin_id for x in a.basins] == list(range(len(a.basins)))


def test_the_registry_filter_rides_the_bake_identity():
    """A registry threshold decides SHIPPED BYTES, so it must roll the world.

    B5 re-opens registered holes in the elevation plane. A filter that changed
    without moving the fingerprint would leave two mutually incompatible bakes
    under one identity -- exactly what bake_identity_payload exists to stop.
    """
    base = pipeline.bake_fingerprint(TEST_GEOM, TEST_CONSTS)
    for field, value in (
        ("basin_min_depth_m", 3.0),
        ("basin_min_area_m2", 10000.0),
        ("basin_exclude_spanning", False),
        ("basin_require_above_sea", False),
        ("basin_salt_aridity", 0.4),
        ("basin_budyko_n", 1.5),
        ("basin_min_lake_depth_m", 1.0),
    ):
        moved = pipeline.bake_fingerprint(
            TEST_GEOM, dataclasses.replace(TEST_CONSTS, **{field: value}))
        assert moved != base, f"{field} does not roll the bake fingerprint"
    assert "B5.reopen_basins" in pipeline.STAGE_ORDER


def test_the_bake_hands_the_registry_to_the_encoder():
    """End to end: bake -> table -> bytes -> table, through the shipped path."""
    pytest.importorskip("scipy")
    tc = pytest.importorskip("terrain_service.tile_codec")
    consts = _permissive(TEST_CONSTS)
    _, r = _bake_crater(consts=consts)
    assert r.basins

    data = tc.encode_fine(
        seed=20260719, x=0, y=0,
        elevation_m=r.elevation_m, flow=r.flow, basins=r.basins,
        block_log2=4,
    )
    back = tc.decode_v2(data)
    assert back.basins is not None and len(back.basins) == len(r.basins)
    for rec, row in zip(r.basins, back.basins):
        assert row.basin_id == rec.basin_id
        assert row.kind == rec.kind
        assert row.spill_mm == round(rec.spill_m * 1000)
        assert row.surface_mm == round(rec.surface_m * 1000)
        assert row.seed_px == tuple(rec.seed_px)
    assert back.bake_ver == pipeline.BAKE_VERSION
