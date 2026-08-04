"""The gate that keeps the TERRAIN_VERSION / BAKE_VERSION split honest.

WHY THIS FILE EXISTS
--------------------
Until bake_ver 9 one counter did two jobs: it seeded the world (through
``roughness_seed``) and it recorded which products a tile carried. Fusing them
meant every additive change to the wire format cost a NEW WORLD -- measured,
when P2 was scoped, bumping the single counter 8 -> 9 moved
``roughness_seed(20260719)`` from 0x7e1ec856567c4fb5 to 0xc2b0bf0a8b32531f.

P2 adds a water plane and moves no height. The whole argument for splitting the
counter is that its re-bake is an ADDITION: the 256-tile lake survey, the
25-tile bank probe, the vista archive and the owner's spawn sites all keep
describing ground that still exists. That argument is worth exactly as much as
the evidence for it, so it is asserted rather than reasoned about:

    ``test_water_plane_moves_no_height`` -- the fast, synthetic, always-run
    form: the SAME bake with the water plane on and off must produce a
    bit-identical elevation field and byte-identical elevation sections.

    ``tools/verify_terrain_identity.py`` -- the expensive, real form: re-bake a
    RESIDENT production tile and compare its elevation plane byte for byte
    against the .vxtl on disk. That one needs the tile cache and ~265 CPU-s per
    tile, so it is a tool rather than a test, and it is the gate to run before
    spending a re-bake.

The synthetic test is not a weaker version of the real one -- it isolates a
different thing. It proves the water CODE PATH perturbs nothing (no shared
buffer written through, no RNG consumed, no in-place mutation of a surface
another stage still reads). The tool proves the IDENTITY SPLIT is wired right
end to end. Both can pass while the other fails.
"""

from __future__ import annotations

import dataclasses

import numpy as np
import pytest

tile_codec = pytest.importorskip("terrain_service.tile_codec")
pipeline = pytest.importorskip("terrain_service.bake.pipeline")

from test_bake_pipeline import (  # noqa: E402
    TEST_CONSTS,
    TEST_GEOM,
    kernels,
    ramp_world,
    ref_roughness_world,
    synth_world,
)


#: TEST_CONSTS with a perennial threshold scaled to the TEST GEOMETRY, and the
#: scaling is not a fudge -- it is the only way this test can exist.
#:
#: TEST_GEOM is a 0.48 km padded domain at 7.5 m/px. Production's threshold asks
#: for a channel 2 px = 15 m wide, which the width law puts at 1.02e8 m^3/yr;
#: if EVERY cell of the whole toy domain drained to one outlet it would deliver
#: 4.6e5 m^3/yr, 224x short. A 0.23 km^2 catchment cannot make a 15 m river, and
#: it should not: that is the law being right, not the test being awkward.
#:
#: So the CURRENCY is rescaled and the geometry is left alone. What is under
#: test here is the code path -- does the water stage perturb a height, does it
#: write the right sections -- and that is independent of where the threshold
#: sits. The threshold's own calibration is measured on the real world, where
#: the catchments are real; see the head bound in the P2 report (max 2.89 m,
#: p99 2.51 m, against a probe validated over 0.3-10 m).
WATER_CONSTS = dataclasses.replace(TEST_CONSTS, water_q_perennial_m3_yr=30.0)


def _climate(tx, ty, geom=TEST_GEOM):
    """A WET, TEMPERATE climate tile: (4, n, n) uint8 in CLIMATE_ORDER.

    Deliberately wet. The water path is skipped entirely when
    ``padded_climate`` is None, and it draws almost nothing in a desert, so a
    test that fed no climate -- or an arid one -- would compare "no plane"
    against "no plane" and pass while proving nothing. That is precisely the
    vacuous-statistic failure this branch has hit three times, so
    ``_bake2`` asserts the plane it produced is actually non-empty.

    Temperature ~15 C and precipitation ~2000 mm/yr through
    ``province.CLIMATE_RANGES``: 15 C is (15+40)/80*255 = 175, and
    2000 mm/yr of 12000 is 42.
    """
    n = geom.coarse_tile_px
    out = np.zeros((4, n, n), np.uint8)
    out[0] = 175   # temperature -> ~15 C
    out[1] = 85    # seasonality (unread by the water balance)
    out[2] = 42    # precipitation -> ~1976 mm/yr
    out[3] = 60    # precip variability
    return out


def _kernels_that_route():
    """The orchestration doubles, but with the REAL routing kernels.

    ``ref_accumulate`` deliberately does no routing (a routing reference would
    make the apron tests measure the reference instead of the pipeline), and
    without routing no cell can accumulate a catchment, so no reach ever
    reaches ``q_drawable`` and the water plane comes out empty. The
    anti-vacuity assertion below catches that -- it caught it while this file
    was being written -- but the fix is to route for real: fill/D8/MFD are
    exact, cheap over a 64^2 padded domain, and are precisely the part whose
    interaction with the new ``source`` seed is under test.

    Everything else stays a double, so this remains an orchestration test.
    """
    flow = pytest.importorskip("terrain_service.bake.flow")
    k = kernels(ref_roughness_world)
    return dataclasses.replace(
        k,
        fill_depressions=flow.fill_depressions,
        d8_receivers=flow.d8_receivers,
        accumulate_mfd=flow.accumulate_mfd,
    )


def _bake2(world, tx, ty, consts, with_climate=True):
    """`test_bake_pipeline._bake`, but with climate, real routing and a consts."""
    return pipeline.bake_tile(
        world_seed=20260719,
        tile_x=tx,
        tile_y=ty,
        coarse_fetch=lambda x, y: world.get((x, y)),
        climate_fetch=((lambda x, y: _climate(x, y)) if with_climate else None),
        kernels=_kernels_that_route(),
        geom=TEST_GEOM,
        consts=consts,
    )


def _sections(blob: bytes) -> dict[int, bytes]:
    """Split a .vxtl v2 blob into {section_id: bytes} without decoding planes."""
    off = tile_codec._HEADER.size
    (*_, n_sections) = tile_codec._V2_EXT.unpack_from(blob, off)
    off += tile_codec._V2_EXT.size
    out = {}
    for i in range(n_sections):
        sid, soff, slen = tile_codec._SECTION_ENTRY.unpack_from(
            blob, off + i * tile_codec._SECTION_ENTRY.size
        )
        out[sid] = blob[soff:soff + slen]
    return out


@pytest.mark.parametrize("codec", [tile_codec.CODEC_RAW])
def test_water_plane_moves_no_height(codec):
    """Water on vs water off: identical ground, and only the water sections differ.

    THE SINGLE-TERM CONTROL, one world. Not two builds, not two seeds -- the
    same synthetic world baked twice with exactly one constant toggled, which
    is the only form of this claim that has survived on this branch.
    """
    pytest.importorskip("scipy")
    world = synth_world()

    on = dataclasses.replace(WATER_CONSTS, water_plane_enabled=True)
    off = dataclasses.replace(WATER_CONSTS, water_plane_enabled=False)
    r_on = _bake2(world, 0, 0, on)
    r_off = _bake2(world, 0, 0, off)

    # ANTI-VACUITY, first: this test is worthless unless the "on" bake actually
    # drew water. A pass with an empty plane would be comparing nothing to
    # nothing.
    assert r_on.water_surface_m is not None
    assert np.isfinite(r_on.water_surface_m).any(), (
        "the wet climate produced no water at all; this test would pass "
        "vacuously -- check q_drawable against the test geometry's pixel size"
    )

    # 1. The ground itself, bit for bit. `array_equal` on float32 is exact
    #    equality, which is what is wanted: "close" would let a term that
    #    perturbs the surface by an ULP through, and an ULP at 3 km survives
    #    into the 100 mm wire LSB often enough to matter.
    assert np.array_equal(r_on.elevation_m, r_off.elevation_m), (
        "the water plane perturbed the elevation field"
    )
    # 2. Everything else the bake ships beside it.
    assert np.array_equal(r_on.accumulation_m2, r_off.accumulation_m2)
    assert np.array_equal(r_on.flow, r_off.flow)
    assert len(r_on.basins) == len(r_off.basins)

    # 3. The bytes, which is what a client actually reads. Encode both and
    #    compare section by section: the elevation sections must be identical
    #    and the water sections must be the ONLY difference.
    blob_on = tile_codec.encode_fine(
        seed=1, x=0, y=0, elevation_m=r_on.elevation_m, flow=r_on.flow,
        basins=r_on.basins, water_surface_m=r_on.water_surface_m, codec=codec,
        block_log2=5,
    )
    blob_off = tile_codec.encode_fine(
        seed=1, x=0, y=0, elevation_m=r_off.elevation_m, flow=r_off.flow,
        basins=r_off.basins, water_surface_m=None, codec=codec, block_log2=5,
    )
    s_on, s_off = _sections(blob_on), _sections(blob_off)
    for sid in (tile_codec.SECTION_ELEV_INDEX, tile_codec.SECTION_ELEV_DATA,
                tile_codec.SECTION_FLOW_INDEX, tile_codec.SECTION_FLOW_DATA,
                tile_codec.SECTION_BASIN_TABLE):
        assert s_on.get(sid) == s_off.get(sid), f"section {sid} differs"
    assert set(s_on) - set(s_off) <= {tile_codec.SECTION_WATER_INDEX,
                                      tile_codec.SECTION_WATER_DATA}
    assert not set(s_off) - set(s_on)


def test_carried_discharge_changes_the_water_and_nothing_else():
    """TASK #49's HALF OF THE SPLIT. A superblock that carries Q vs one that
    does not: the water plane must change and the GROUND must not.

    This is the claim that decided the version bump. Stream-power incision reads
    ``A^m`` -- an area law -- so carrying a discharge beside the area can only
    reach the water plane, and BAKE_VERSION 9 -> 10 is a re-bake onto identical
    ground rather than a new world. Reasoned about in ``CARRIED_DISCHARGE``;
    asserted here, because the reasoning is worth 67 M control points a tile and
    the same reasoning was available for every leak that ever happened.

    The two arms differ in EXACTLY ONE FIELD of the superblock -- ``q`` --
    which is what makes the elevation comparison a control rather than a
    coincidence.
    """
    pytest.importorskip("scipy")
    # RAMP, not synth_world: the ramp rises to the south-east so every drop runs
    # north-west and the block's flow demonstrably CROSSES tile (0,0)'s
    # boundary. synth_world's short-wavelength component dominates its gradient,
    # which makes drainage local and can leave a tile with no crossing at all --
    # which it did here, and the anti-vacuity assertion below caught it.
    world = ramp_world()
    k = _kernels_that_route()
    consts = dataclasses.replace(WATER_CONSTS, superblock_max_level=0)
    lv = pipeline.FlowLevel(level=0, geom=TEST_GEOM, consts=consts)
    fetch_z = lambda x, y: world.get((x, y))            # noqa: E731
    fetch_c = lambda x, y: _climate(x, y)               # noqa: E731

    with_q = pipeline.build_flow_superblock(
        fetch_z, 0, 0, lv, k, climate_fetch=fetch_c)
    assert with_q.carries_discharge
    # The SAME block with the discharge removed -- the pre-task-#49 state, and
    # what a cached block built without climate still hands over.
    without_q = dataclasses.replace(with_q, q=None)
    assert np.array_equal(with_q.acc, without_q.acc)

    def bake(sb):
        return pipeline.bake_tile(
            world_seed=20260719, tile_x=0, tile_y=0,
            coarse_fetch=fetch_z, climate_fetch=fetch_c,
            kernels=k, geom=TEST_GEOM, consts=consts, inflow_source=sb,
        )

    r_q, r_proxy = bake(with_q), bake(without_q)

    # ANTI-VACUITY. Both arms must actually have imported water at the boundary,
    # or this compares two tiles that never used their superblock at all.
    assert r_q.stats["injected_inflow_km2"] > 0.0
    assert r_q.stats["water_q_inflow_carried"] == 1.0
    assert r_proxy.stats["water_q_inflow_carried"] == 0.0
    assert r_q.stats["water_q_inflow_m3_yr"] > 0.0

    # THE GROUND, bit for bit. Exact equality, not a tolerance: an ULP at 3 km
    # survives into the 100 mm wire LSB often enough to matter.
    assert np.array_equal(r_q.elevation_m, r_proxy.elevation_m), (
        "carrying a discharge moved a height -- it has leaked into the terrain "
        "half and BAKE_VERSION is the wrong counter"
    )
    assert np.array_equal(r_q.accumulation_m2, r_proxy.accumulation_m2)
    assert np.array_equal(r_q.flow, r_proxy.flow)
    assert len(r_q.basins) == len(r_proxy.basins)

    # ...and the DISCHARGE, which is the whole point, is not the same field.
    assert not np.array_equal(r_q.discharge_m3_yr, r_proxy.discharge_m3_yr)


def test_water_disabled_reproduces_a_bake_ver_8_tile_exactly():
    """`water_plane_enabled=False` must leave no trace at all.

    The escape hatch has to be exact, not approximate: it is what the identity
    tool bakes against when it reproduces a resident bake_ver-8 tile, so a
    stray flag bit or an empty section here would make that comparison
    meaningless.
    """
    pytest.importorskip("scipy")
    world = synth_world()
    off = dataclasses.replace(WATER_CONSTS, water_plane_enabled=False)
    r = _bake2(world, 0, 0, off)

    assert r.water_surface_m is None
    assert r.discharge_m3_yr is None
    assert not any(k.startswith("water_") for k in r.stats), (
        f"water stats leaked with the plane disabled: "
        f"{[k for k in r.stats if k.startswith('water_')]}"
    )

    blob = tile_codec.encode_fine(
        seed=1, x=0, y=0, elevation_m=r.elevation_m, flow=r.flow,
        basins=r.basins, water_surface_m=r.water_surface_m,
        codec=tile_codec.CODEC_RAW, block_log2=5,
    )
    tile = tile_codec.decode_v2(blob)
    assert tile.water_cp is None
    sections = _sections(blob)
    assert tile_codec.SECTION_WATER_INDEX not in sections
    assert tile_codec.SECTION_WATER_DATA not in sections


def test_a_tile_without_the_water_flag_is_not_read_as_dry():
    """"No water plane" and "a dry water plane" must not read alike.

    Same argument as FLAG_BASINS_PRESENT on an empty table, and the same
    failure if it is got wrong: a client that conflated them would draw no
    rivers in a world that has them and never be able to tell why. This is the
    reason the plane is written even when it is entirely dry.
    """
    n = 64
    cp = np.zeros((n, n), np.int16)
    dry = np.full((n, n), tile_codec.WATER_DRY_DEPTH, np.int16)

    absent = tile_codec.decode_v2(tile_codec.encode_v2(tile_codec.TileV2(
        seed=1, x=0, y=0, size=n, elevation_cp=cp, water_cp=None,
        block_log2=5, bake_ver=9)))
    present_dry = tile_codec.decode_v2(tile_codec.encode_v2(tile_codec.TileV2(
        seed=1, x=0, y=0, size=n, elevation_cp=cp, water_cp=dry,
        block_log2=5, bake_ver=9)))

    assert absent.water_cp is None, "a tile with no plane must decode as None"
    assert present_dry.water_cp is not None, (
        "an all-dry plane must survive the round trip as a plane, not as None"
    )
    assert np.array_equal(present_dry.water_cp, dry)
