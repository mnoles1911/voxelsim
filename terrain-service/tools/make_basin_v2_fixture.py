"""Generate the BASIN TABLE v2 + SECTION_HEADWATERS conformance fixture.

WHY A SECOND FIXTURE RATHER THAN AN EDIT TO THE FIRST. `make_basin_fixture.py`
writes a v1 table and must keep doing so forever: v1 tiles are on disk in
shipped namespaces, and the claim "this build still reads them" is worth
nothing if the artefact that tests it follows the encoder's current default.
So there are two files, one per layout, and both are parsed by both languages.

WHY A FIXTURE AT ALL. The Python encoder and the C++ decoder are written
independently against the same layout comments, and a round-trip test in either
language proves only that that language agrees with itself. A committed file
both sides parse is the only thing that catches a disagreement -- and §3's
"unknown flag bits are rejected" makes such a disagreement a hard refusal at
load rather than a subtle rendering fault.

The table is chosen to break a lazy v2 decoder, not to be typical:

* **A basin whose FLOOR IS OUTSIDE THIS TILE** (id 1). This is the whole point
  of v2: a tile-spanning basin's deepest cell can be in the neighbour, so a
  decoder that assumes the identity anchor is a local pixel -- or that
  `seedPx` is the floor -- reads a lake in the wrong place. v1 could not
  express this row at all, which is why it dropped such basins.
* **Both span directions**: id 1 crosses the left edge, id 3 the right, and
  their `world_bbox` extends past the tile accordingly while the u16 bbox stays
  clipped inside it. A decoder that validates one against the other with the
  wrong sign fails here.
* **A capacity above u32** (id 3, 8.0e12 litres = 8.0e9 m^3). A decoder that
  reads the field as 32 bits truncates it to something plausible.
* **Capacity exactly 0** (id 0), which is what an overflowing lake has: it is
  already at its spill, so its headroom is nil. A decoder that treats 0 as
  "missing" would make every full lake bottomless.
* **NEGATIVE world coordinates throughout** -- the tile is at (-2,-4), like the
  v1 fixture, because the wet alpine block is entirely in negative tiles and
  the anchor packs its coordinates through two's complement.
* All five kinds, a below-sea-level row, a one-pixel basin, and a basin
  touching the last pixel on both axes -- the v1 fixture's traps, kept.

And the head table beside it:

* **A head at u32 max** (4,294,967,295 m^3/yr). A decoder that reads Q as
  int32 gets a negative faucet rate.
* **A head with Q = 0**, which the format allows and which must not be
  confused with an absent row.
* **Rows in strict (y, x) order**, including one pair that is descending in x
  and ascending in y -- a decoder that sorted by (x, y) refuses this file.

Run:
    python tools/make_basin_v2_fixture.py
It rewrites voxel-core/tests/fixtures/vxtl_v2_golden_basins_v2_512.vxtl.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402
from terrain_service.bake import basins as bs  # noqa: E402

OUT = (Path(__file__).resolve().parents[2]
       / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_basins_v2_512.vxtl")

SIZE = 512
BLOCK_LOG2 = 8
TILE_X, TILE_Y = -2, -4
#: World fine-pixel coordinate of this tile's interior (0, 0) -- exactly what
#: the bake passes as `world_origin_px`, and what the C++ parser recomputes
#: from the header to check the absolute fields against the local ones.
WOX, WOY = TILE_X * SIZE, TILE_Y * SIZE


def _row(basin_id, seed, bbox, outlet, spill, surface, floor, kind,
         floor_world, world_bbox, capacity_l, span=0):
    return tc.BasinEntry(
        basin_id=basin_id, seed_px=seed, bbox_px=bbox, outlet_px=outlet,
        spill_mm=spill, surface_mm=surface, kind=kind,
        # THROUGH THE BAKE'S OWN PACKER, not a re-derivation: a fixture that
        # packed the id its own way would prove the two decoders agree with the
        # FIXTURE rather than with the producer.
        global_id=bs.global_basin_id(*floor_world),
        capacity_l=capacity_l, floor_mm=floor,
        world_bbox_px=world_bbox,
        world_outlet_px=(outlet[0] + WOX, outlet[1] + WOY),
        span_flags=span,
    )


#: Ordered by (min_y, min_x) of extent, exactly as the bake orders them.
BASINS = [
    # id 0: overflowing lake, wholly inside the tile. Surface EXACTLY at the
    # spill, so capacity is 0 -- the "already spilling" case.
    _row(0, seed=(40, 12), bbox=(30, 10, 60, 40), outlet=(29, 25),
         spill=1_234_500, surface=1_234_500, floor=1_210_000,
         kind=tc.BASIN_KIND_LAKE_OVERFLOWING,
         floor_world=(40 + WOX, 12 + WOY),
         world_bbox=(30 + WOX, 10 + WOY, 60 + WOX, 40 + WOY),
         capacity_l=0),
    # id 1: terminal lake CROSSING THE LEFT EDGE, and its deepest cell is 60 px
    # into the neighbour. The local bbox is clipped at x = 0; the world bbox
    # runs 100 px further left. This row is the reason v2 exists.
    _row(1, seed=(3, 130), bbox=(0, 120, 60, 175), outlet=(0, 150),
         spill=880_000, surface=863_250, floor=812_400,
         kind=tc.BASIN_KIND_LAKE_TERMINAL,
         floor_world=(WOX - 60, 140 + WOY),
         world_bbox=(WOX - 100, 120 + WOY, 60 + WOX, 175 + WOY),
         capacity_l=1_950_000_000,
         span=tc.BASIN_SPAN_CROSSES_TILE),
    # id 2: seasonal, and BELOW SEA LEVEL. No bake emits this (the registry
    # refuses a spill at or under the datum), which is precisely why it is
    # here: it is the only exercise the i32 sign gets on spill/surface/floor.
    _row(2, seed=(300, 300), bbox=(300, 300, 300, 300), outlet=(299, 300),
         spill=-2_500, surface=-7_300, floor=-9_000,
         kind=tc.BASIN_KIND_SEASONAL,
         floor_world=(300 + WOX, 300 + WOY),
         world_bbox=(300 + WOX, 300 + WOY, 300 + WOX, 300 + WOY),
         capacity_l=4_294_967_296),   # exactly u32 max + 1: the truncation trap
    # id 3: salt flat -- dry, so the surface sits AT the floor -- crossing the
    # RIGHT edge, and carrying a capacity that only u64 can hold (8.0e9 m^3,
    # i.e. a 174 km^2 pan 46 m deep; the wet block's own worst case is 2.6e8).
    _row(3, seed=(500, 400), bbox=(460, 390, 511, 430), outlet=(511, 410),
         spill=57_000, surface=50_100, floor=50_100,
         kind=tc.BASIN_KIND_SALT_FLAT,
         floor_world=(700 + WOX, 405 + WOY),
         world_bbox=(460 + WOX, 390 + WOY, 900 + WOX, 430 + WOY),
         capacity_l=8_000_000_000_000,
         span=tc.BASIN_SPAN_CROSSES_TILE),
    # id 4: dry playa, reaching the LAST pixel of the tile on both axes.
    _row(4, seed=(500, 500), bbox=(480, 480, 511, 511), outlet=(479, 495),
         spill=2_100_000, surface=2_093_400, floor=2_093_400,
         kind=tc.BASIN_KIND_DRY_PLAYA,
         floor_world=(500 + WOX, 500 + WOY),
         world_bbox=(480 + WOX, 480 + WOY, 511 + WOX, 511 + WOY),
         capacity_l=12_345_678),
]

#: Strictly ascending by (y, x). Note rows 0 and 1: x DESCENDS while y
#: ascends, so a decoder that sorted by (x, y) refuses this file.
HEADS = [
    tc.HeadEntry(px=(400, 5), q_m3_yr=0),
    tc.HeadEntry(px=(11, 9), q_m3_yr=1),
    tc.HeadEntry(px=(300, 300), q_m3_yr=230_000_001),
    tc.HeadEntry(px=(511, 511), q_m3_yr=tc.HEADWATER_Q_MAX),
]


def samples() -> np.ndarray:
    """A gentle, fully deterministic surface. The elevation plane is not what
    this fixture is about -- it exists so the file is a real tile rather than
    a bare table -- so it is smooth enough to compress and integer-exact."""
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    return (100.0 + 0.5 * x + 0.25 * y
            + 20.0 * np.sin(x / 47.0) * np.cos(y / 61.0))


def main() -> int:
    flow = np.zeros((SIZE, SIZE), np.uint8)
    # Non-trivial flow, so the fixture also proves the two new tables coexist
    # with an optional section rather than only with the required ones.
    flow[:, 200:210] = tc.FLOW_BIT_CHANNEL | 17
    flow[300:310, :] = tc.FLOW_BIT_BANK | 9

    data = tc.encode_fine(
        seed=20260719, x=TILE_X, y=TILE_Y,
        elevation_m=samples(),
        flow=flow,
        basins=BASINS,
        heads=HEADS,
        bake_ver=24,
        block_log2=BLOCK_LOG2,
    )
    # Prove it before writing it: a fixture that does not round-trip through
    # its own encoder cannot be evidence about anyone else's decoder.
    back = tc.decode_v2(data)
    assert back.basins is not None and len(back.basins) == len(BASINS)
    for want, got in zip(BASINS, back.basins):
        assert want == got, f"round trip changed basin {want.basin_id}"
    assert back.heads == HEADS
    kinds = {b.kind for b in BASINS}
    assert kinds == set(range(tc.BASIN_KIND_COUNT)), (
        f"the fixture must exercise every kind; it has {sorted(kinds)}")
    assert any(b.capacity_l > 0xFFFFFFFF for b in BASINS), "no u32 capacity trap"
    assert any(b.capacity_l == 0 for b in BASINS), "no overflowing-lake row"
    assert any(b.world_floor_px[0] < WOX for b in BASINS), (
        "no basin whose floor is in the neighbour -- the row v2 exists for")
    assert all(b.world_floor_px[0] < 0 and b.world_floor_px[1] < 0 for b in BASINS), (
        "the negative-world-coordinate exercise was lost")
    # voxelcore/basinledger.h's BasinId contract, checked on the artefact the
    # runtime will actually be handed: bit 63 clear, never 0.
    assert all(0 < b.global_id < (1 << 63) for b in BASINS)
    assert max(h.q_m3_yr for h in HEADS) == tc.HEADWATER_Q_MAX

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print(f"wrote {OUT} ({len(data)} bytes, {len(BASINS)} basins, "
          f"{len(HEADS)} heads)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
