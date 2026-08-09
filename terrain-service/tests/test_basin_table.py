"""SECTION_BASIN_TABLE, the Python half (watershed plan P1; v2 at bake_ver 24).

The C++ half is voxel-core/tests/test_tilestore.cpp; the two meet at
voxel-core/tests/fixtures/vxtl_v2_golden_basins_512.vxtl (v1) and
vxtl_v2_golden_basins_v2_512.vxtl (v2 + headwaters), which this file also
checks, because a round-trip test in one language only proves that language
agrees with itself.

The refusals are most of this file, deliberately. A basin row is a GAMEPLAY
INSTRUCTION -- "flood-fill from here, up to this level" -- so a row that
decodes into something plausible but wrong puts water where there is none, on
terrain, in front of a player, with no error anywhere. Every field that could
do that is refused rather than clamped.

BOTH LAYOUTS ARE UNDER TEST AND BOTH MUST STAY READABLE: v1 tiles are on disk
in shipped namespaces and v2 is what the bake writes now. The v1 half of this
file is therefore not legacy decoration -- it is the compatibility claim.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from terrain_service import tile_codec as tc

FIXTURE = (Path(__file__).resolve().parents[2]
           / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_basins_512.vxtl")
FIXTURE_V2 = (Path(__file__).resolve().parents[2]
              / "voxel-core" / "tests" / "fixtures"
              / "vxtl_v2_golden_basins_v2_512.vxtl")

V1 = tc.BASIN_TABLE_VERSION_V1
V2 = tc.BASIN_TABLE_VERSION_V2


def _entry(basin_id=0, seed=(4, 5), bbox=(2, 3, 8, 9), outlet=(1, 6),
           spill=12_000, surface=9_000, kind=tc.BASIN_KIND_LAKE_TERMINAL,
           v2=True, world_origin=(1_000, 2_000), capacity_l=7_500_000,
           floor=4_000, span_flags=0):
    """One row. v2 by default, because that is what the bake writes.

    The v2 half is derived from the v1 half rather than passed in separately,
    so a test that moves the bbox cannot accidentally leave the world bbox
    describing a different basin -- which is exactly the inconsistency `pack`
    refuses, and a helper that could produce it would make that refusal
    untestable by accident.
    """
    wx, wy = world_origin
    extra = {}
    if v2:
        extra = dict(
            # Packed with the codec's own constants rather than by calling the
            # bake -- this file must stay runnable on a box with no bake
            # package -- and pinned to the bake's packer by
            # test_the_global_id_round_trips_through_the_packing_including_negatives.
            global_id=(tc.BASIN_ID_TAG
                       | ((seed[0] + wx + tc.BASIN_ID_AXIS_BIAS) << 31)
                       | (seed[1] + wy + tc.BASIN_ID_AXIS_BIAS)),
            capacity_l=capacity_l,
            floor_mm=floor,
            world_bbox_px=(bbox[0] + wx, bbox[1] + wy, bbox[2] + wx, bbox[3] + wy),
            world_outlet_px=(outlet[0] + wx, outlet[1] + wy),
            span_flags=span_flags,
        )
    return tc.BasinEntry(basin_id=basin_id, seed_px=seed, bbox_px=bbox,
                         outlet_px=outlet, spill_mm=spill, surface_mm=surface,
                         kind=kind, **extra)


def _tile(basins, size=32, block_log2=4, heads=None):
    cp = np.zeros((size, size), np.int16)
    return tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp,
                     block_log2=block_log2, basins=basins, heads=heads)


# --------------------------------------------------------------- the layout


def test_the_row_is_32_bytes_at_v1_and_80_at_v2():
    """The sizes are part of the wire format and the C++ side asserts them too.

    Each is also written INTO the table as `entry_bytes`, which is what lets a
    decoder from a different revision refuse rather than misread -- and it is
    the whole mechanism v2 used to add fields without a second section id.
    """
    assert tc.BASIN_ENTRY_BYTES_V1 == 32
    assert tc.BASIN_ENTRY_BYTES_V2 == 80
    assert tc._BASIN_TABLE_HEADER.size == 8
    assert len(_entry(v2=False).pack(V1)) == 32
    assert len(_entry().pack(V2)) == 80
    # THE PROPERTY THAT MAKES v2 ADDITIVE: the first 32 bytes of a v2 row are
    # a v1 row, byte for byte. A reader that knows only v1's field offsets
    # still lands on the right fields; only `entry_bytes` tells it to stop.
    assert _entry().pack(V2)[:32] == _entry(v2=False).pack(V1)


def test_a_v2_row_cannot_be_written_without_its_v2_fields():
    """Zeros would be a valid-looking identity: every such basin would claim
    the lake whose floor is world pixel (0, 0), and the client's union rule
    would merge all of them into one."""
    with pytest.raises(ValueError, match="without the v2 fields"):
        _entry(v2=False).pack(V2)
    # And the v1 row is still writable, because v1 tiles must stay round-trippable.
    assert len(_entry(v2=False).pack(V1)) == 32


def test_the_identity_anchor_must_lie_inside_the_basins_own_extent():
    """`global_id` and `world_bbox_px` describe ONE component or the union
    rule -- which reads both -- silently merges the wrong pair."""
    row = _entry()
    row.global_id = ((row.world_bbox_px[2] + 5) << 32) | row.world_bbox_px[1]
    with pytest.raises(ValueError, match="outside its own world bbox"):
        row.pack(V2)


def test_the_global_id_round_trips_through_the_packing_including_negatives():
    """It is a bijection, not a hash -- so it reads back as a place.

    Negative world pixels are the ordinary case: the wet alpine block is at
    tiles (-5,-5)..(-3,-4), i.e. every one of its basins has a negative anchor.
    This also pins the codec's copy of the unpacking to the bake's packer --
    two copies exist because tile_codec must import with no bake package.
    """
    basins = pytest.importorskip("terrain_service.bake.basins")
    assert basins.BASIN_ID_AXIS_BIAS == tc.BASIN_ID_AXIS_BIAS
    assert basins.BASIN_ID_TAG == tc.BASIN_ID_TAG
    edge = tc.BASIN_ID_AXIS_BIAS - 1
    for wx, wy in ((0, 0), (7, 9), (-40_960, -32_768), (edge, -edge),
                   (-tc.BASIN_ID_AXIS_BIAS, edge)):
        gid = basins.global_basin_id(wx, wy)
        assert basins.world_px_from_global_id(gid) == (wx, wy)
        assert tc.BasinEntry(basin_id=0, seed_px=(0, 0), bbox_px=(0, 0, 1, 1),
                             outlet_px=(0, 0), spill_mm=0, surface_mm=0,
                             kind=0, global_id=gid).world_floor_px == (wx, wy)
        # voxelcore/basinledger.h's BasinId contract: bit 63 is its tag for
        # tile-local v1 keys and 0 is its "not a basin". An id breaking either
        # is dropped by the runtime ledger with no error anywhere -- so the
        # producer must never mint one.
        assert 0 < gid < (1 << 63)

    # And a coordinate past the range RAISES rather than folding onto another
    # lake's id, which is the failure that looks like data.
    with pytest.raises(ValueError, match="outside the basin id"):
        basins.global_basin_id(tc.BASIN_ID_AXIS_BIAS, 0)


def test_a_global_id_with_the_runtime_tag_bit_is_refused_at_pack():
    """The naive packing -- two's-complement u32 halves -- sets bit 63 for any
    NEGATIVE world x, which is every basin in the wet alpine block. The runtime
    would refuse all of them silently, so the encoder refuses them loudly."""
    row = _entry()
    row.global_id = (1 << 63) | 12345
    with pytest.raises(ValueError, match="bit 63"):
        row.pack(V2)


def test_capacity_is_litres_in_u64_and_the_bound_is_checked_not_assumed():
    """The unit was picked by an overflow bound; this is that bound, run.

    Realistic worst case on the wet alpine block: kept basin coverage peaks at
    2.4% of a 23,593 ha tile (566 ha) against a 46 m deepest spill. The absurd
    one: the whole padded domain at the elevation plane's own int16 span.
    """
    realistic_m3 = 5.66e6 * 46.0            # 2.6e8 m^3
    absurd_m3 = 2.986e8 * 6553.4            # 1.96e12 m^3
    assert realistic_m3 * tc.CAPACITY_L_PER_M3 > 0xFFFFFFFF, (
        "u32 litres would already overflow on a real lake")
    assert absurd_m3 * tc.CAPACITY_L_PER_M3 < tc.CAPACITY_L_MAX / 1000.0, (
        "u64 litres must clear the absurd case with three orders to spare")
    row = _entry(capacity_l=int(absurd_m3 * tc.CAPACITY_L_PER_M3))
    assert tc.decode_basin_table(tc.encode_basin_table([row]))[0].capacity_l \
        == row.capacity_l
    with pytest.raises(ValueError, match="litres is outside"):
        _entry(capacity_l=tc.CAPACITY_L_MAX + 1).pack(V2)


def test_kind_values_agree_with_the_bake():
    """Three copies of this enum exist (here, bake/basins.py, tilestore.h).

    The Python pair is checked here; the C++ one is checked by the shared
    fixture, which carries one row of every kind.
    """
    basins = pytest.importorskip("terrain_service.bake.basins")
    assert tc.BASIN_KIND_DRY_PLAYA == basins.KIND_DRY_PLAYA
    assert tc.BASIN_KIND_SALT_FLAT == basins.KIND_SALT_FLAT
    assert tc.BASIN_KIND_SEASONAL == basins.KIND_SEASONAL
    assert tc.BASIN_KIND_LAKE_TERMINAL == basins.KIND_LAKE_TERMINAL
    assert tc.BASIN_KIND_LAKE_OVERFLOWING == basins.KIND_LAKE_OVERFLOWING
    assert tc.BASIN_KIND_COUNT == len(basins.KIND_NAMES)


def test_round_trip_through_the_whole_tile():
    rows = [_entry(basin_id=0), _entry(basin_id=1, bbox=(10, 11, 20, 21),
                                       seed=(15, 16), outlet=(9, 15),
                                       spill=-500, surface=-2_500,
                                       floor=-9_000,
                                       kind=tc.BASIN_KIND_SALT_FLAT)]
    back = tc.decode_v2(tc.encode_v2(_tile(rows)))
    assert back.basins == rows
    assert all(b.has_v2 for b in back.basins)


def test_a_v1_table_still_decodes_and_says_it_has_no_identity():
    """v1 tiles are on disk in shipped namespaces. They must keep working, and
    a v1 row's MISSING identity must read as missing, not as zero -- `None` is
    "this tile predates global ids", `0` would be a lake at world (0, 0)."""
    rows = [_entry(v2=False)]
    payload = tc.encode_basin_table(rows, V1)
    assert payload[:4] == struct.pack("<HH", V1, 32)
    back = tc.decode_basin_table(payload)
    assert back == rows
    assert back[0].global_id is None and not back[0].has_v2
    assert back[0].capacity_l is None and back[0].floor_mm is None
    assert back[0].world_floor_px is None


def test_negative_elevations_survive():
    """Spill and surface are i32 mm and the sign has to work.

    The bake's registry refuses a basin at or below sea level, so no shipped
    tile will exercise this -- which is exactly why it is tested here and
    carried in the fixture.
    """
    row = _entry(spill=-1, surface=-2_147_000_000, floor=-2_147_000_000)
    back = tc.decode_v2(tc.encode_v2(_tile([row])))
    assert back.basins == [row]


# ------------------------------------------------------- flag/section pairing


def test_an_empty_table_still_sets_the_flag():
    """"Surveyed, holds nothing" is not the same fact as "predates the registry".

    A client that conflated them would put no water in a world that has some
    and never know it. So an empty list sets FLAG_BASINS_PRESENT and writes a
    zero-count table; None writes neither.
    """
    data = tc.encode_v2(_tile([]))
    flags = struct.unpack_from("<H", data, tc._HEADER.size + 6)[0]
    assert flags & tc.FLAG_BASINS_PRESENT
    assert tc.decode_v2(data).basins == []

    none = tc.encode_v2(_tile(None))
    flags = struct.unpack_from("<H", none, tc._HEADER.size + 6)[0]
    assert not (flags & tc.FLAG_BASINS_PRESENT)
    assert tc.decode_v2(none).basins is None


def test_flag_and_section_must_agree_in_both_directions():
    data = bytearray(tc.encode_v2(_tile([_entry()])))
    off = tc._HEADER.size + 6
    flags = struct.unpack_from("<H", data, off)[0]
    struct.pack_into("<H", data, off, flags & ~tc.FLAG_BASINS_PRESENT)
    with pytest.raises(ValueError, match="basin flag is clear"):
        tc.decode_v2(bytes(data))

    plain = bytearray(tc.encode_v2(_tile(None)))
    off = tc._HEADER.size + 6
    flags = struct.unpack_from("<H", plain, off)[0]
    struct.pack_into("<H", plain, off, flags | tc.FLAG_BASINS_PRESENT)
    with pytest.raises(ValueError, match="SECTION_BASIN_TABLE is missing"):
        tc.decode_v2(bytes(plain))


def test_an_unknown_flag_bit_is_refused():
    """The property that lets the two halves of the format move in lockstep.

    An old client must refuse a new tile LOUDLY, not read it as an old one.
    The C++ decoder has always done this; the Python decoder did not check
    flags at all until the basin bit arrived.
    """
    data = bytearray(tc.encode_v2(_tile([_entry()])))
    off = tc._HEADER.size + 6
    struct.pack_into("<H", data, off,
                     struct.unpack_from("<H", data, off)[0] | 0x8000)
    with pytest.raises(ValueError, match="unknown header flag"):
        tc.decode_v2(bytes(data))


# ------------------------------------------------------------- the refusals


def test_water_above_its_own_outlet_is_refused_on_both_sides():
    """The one field error that would FLOOD TERRAIN rather than fail.

    A surface above the spill is not a lake: the outlet would carry the excess
    away. classify() clamps, so a row like this means something bypassed it.
    """
    with pytest.raises(ValueError, match="above"):
        _entry(spill=1000, surface=2000, floor=0).pack()
    good = tc.encode_basin_table([_entry(spill=1000, surface=1000, floor=0)])
    bad = bytearray(good)
    struct.pack_into("<i", bad, tc._BASIN_TABLE_HEADER.size + 22, 5000)
    with pytest.raises(ValueError, match="above its spill"):
        tc.decode_basin_table(bytes(bad))


def test_a_floor_above_its_own_surface_is_refused_on_both_sides():
    """v2's floor is what turns a volume into a level. Above the surface it is
    not a floor, it is two different basins' numbers in one row."""
    with pytest.raises(ValueError, match="above its own surface"):
        _entry(spill=12_000, surface=9_000, floor=10_000).pack(V2)
    good = bytearray(tc.encode_basin_table([_entry(floor=4_000)]))
    struct.pack_into("<i", good, tc._BASIN_TABLE_HEADER.size + 32 + 16, 10_000)
    with pytest.raises(ValueError, match="floor is above its own surface"):
        tc.decode_basin_table(bytes(good))


def test_ids_must_be_zero_to_n_minus_one_in_order():
    """The client INDEXES by id, so a gap lets two processes disagree about
    which basin is "3". The bake orders by (min_y, min_x) of extent precisely
    so the id is a pure function of the surface."""
    with pytest.raises(ValueError, match="0..n-1"):
        tc.encode_basin_table([_entry(basin_id=0), _entry(basin_id=2)])
    good = bytearray(tc.encode_basin_table([_entry(0), _entry(1)]))
    struct.pack_into("<H", good, tc._BASIN_TABLE_HEADER.size
                     + tc.BASIN_ENTRY_BYTES_V2, 7)
    with pytest.raises(ValueError, match="not 0..n-1"):
        tc.decode_basin_table(bytes(good))


def test_a_row_size_this_build_does_not_know_is_refused():
    """Refusing beats reading 81-byte records out of an 80-byte stream and
    getting plausible garbage. This is what `entry_bytes` is for."""
    data = bytearray(tc.encode_basin_table([_entry()]))
    struct.pack_into("<H", data, 2, 81)
    with pytest.raises(ValueError, match="entry size"):
        tc.decode_basin_table(bytes(data))
    struct.pack_into("<H", data, 2, tc.BASIN_ENTRY_BYTES_V2)
    struct.pack_into("<H", data, 0, 99)
    with pytest.raises(ValueError, match="version"):
        tc.decode_basin_table(bytes(data))


def test_the_version_and_the_row_size_are_checked_as_a_PAIR():
    """A v2 label over 32-byte rows is not "an old table, relabelled" -- it is
    bytes neither revision wrote, and reading it either way is a guess."""
    v1 = bytearray(tc.encode_basin_table([_entry(v2=False)], V1))
    struct.pack_into("<H", v1, 0, V2)              # v2 version, 32-byte rows
    with pytest.raises(ValueError, match="entry size 32 != 80"):
        tc.decode_basin_table(bytes(v1))
    v2 = bytearray(tc.encode_basin_table([_entry()], V2))
    struct.pack_into("<H", v2, 0, V1)              # v1 version, 80-byte rows
    with pytest.raises(ValueError, match="entry size 80 != 32"):
        tc.decode_basin_table(bytes(v2))


def test_a_count_that_disagrees_with_the_length_is_refused():
    data = bytearray(tc.encode_basin_table([_entry()]))
    struct.pack_into("<I", data, 4, 2)
    with pytest.raises(ValueError, match="header says"):
        tc.decode_basin_table(bytes(data))


def test_an_unknown_kind_is_refused():
    data = bytearray(tc.encode_basin_table([_entry()]))
    data[tc._BASIN_TABLE_HEADER.size + 26] = tc.BASIN_KIND_COUNT
    with pytest.raises(ValueError, match="unknown basin kind"):
        tc.decode_basin_table(bytes(data))
    with pytest.raises(ValueError, match="kind"):
        _entry(kind=99).pack()


def test_nonzero_reserved_bytes_are_refused():
    """Both reserved runs: v1's five bytes and the v2 tail's three."""
    data = bytearray(tc.encode_basin_table([_entry()]))
    data[tc._BASIN_TABLE_HEADER.size + 30] = 1
    with pytest.raises(ValueError, match="reserved"):
        tc.decode_basin_table(bytes(data))
    tail = bytearray(tc.encode_basin_table([_entry()]))
    tail[tc._BASIN_TABLE_HEADER.size + tc.BASIN_ENTRY_BYTES_V2 - 1] = 1
    with pytest.raises(ValueError, match="reserved bytes in basin entry v2 tail"):
        tc.decode_basin_table(bytes(tail))


def test_an_unknown_span_flag_bit_is_refused():
    """Same rule as the header flags: an undefined bit means the bytes carry
    something this build does not implement."""
    with pytest.raises(ValueError, match="unknown span flag"):
        _entry(span_flags=0x80).pack(V2)
    data = bytearray(tc.encode_basin_table([_entry()]))
    data[tc._BASIN_TABLE_HEADER.size + tc.BASIN_ENTRY_BYTES_V2 - 4] = 0x02
    with pytest.raises(ValueError, match="unknown span flag"):
        tc.decode_basin_table(bytes(data))


def test_an_inside_out_or_out_of_tile_bbox_is_refused():
    with pytest.raises(ValueError, match="inside out"):
        _entry(bbox=(9, 3, 2, 8), seed=(4, 5)).pack()
    with pytest.raises(AssertionError):
        # A bbox outside the grid would send the client's flood fill off the
        # end of the plane.
        _tile([_entry(bbox=(2, 3, 40, 41), seed=(4, 5))], size=32)


def test_a_bbox_outside_the_tile_is_refused_at_decode_too():
    """The encoder's assert is not enough: bytes can arrive from anywhere."""
    data = bytearray(tc.encode_v2(_tile([_entry(bbox=(2, 3, 8, 9))], size=32)))
    # Find the basin section and widen bbox_x1 past the grid edge.
    off = tc._HEADER.size + tc._V2_EXT.size
    n = struct.unpack_from("<H", data, tc._HEADER.size + tc._V2_EXT.size - 2)[0]
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size)
             for i in range(n)]
    soff = next(o for sid, o, _ in table if sid == tc.SECTION_BASIN_TABLE)
    struct.pack_into("<H", data, soff + tc._BASIN_TABLE_HEADER.size + 10, 999)
    with pytest.raises(ValueError, match="outside the tile"):
        tc.decode_v2(bytes(data))


# ------------------------------------------------------------- the fixture


@pytest.mark.skipif(not FIXTURE.exists(), reason=f"fixture absent: {FIXTURE}")
def test_the_committed_fixture_decodes_and_exercises_every_kind():
    """The cross-language handoff artefact, checked from this side too.

    Regenerate with `python tools/make_basin_fixture.py`; its docstring says
    what each row is there to break.
    """
    tile = tc.decode_v2(FIXTURE.read_bytes())
    assert tile.size == 512
    assert tile.bake_ver == 8
    assert tile.flow is not None, "the table must coexist with another optional section"
    assert tile.basins is not None and len(tile.basins) == 5
    assert {b.kind for b in tile.basins} == set(range(tc.BASIN_KIND_COUNT))
    assert [b.basin_id for b in tile.basins] == [0, 1, 2, 3, 4]

    overflowing = tile.basins[0]
    assert overflowing.kind == tc.BASIN_KIND_LAKE_OVERFLOWING
    assert overflowing.surface_mm == overflowing.spill_mm

    terminal = tile.basins[1]
    assert terminal.surface_mm < terminal.spill_mm

    below_sea = tile.basins[2]
    assert below_sea.spill_mm < 0 and below_sea.surface_mm < 0
    assert below_sea.bbox_px[0] == below_sea.bbox_px[2]  # one pixel wide

    # The last row touches the final pixel on both axes: an off-by-one in the
    # bounds check refuses the whole tile rather than clipping quietly.
    assert tile.basins[4].bbox_px[2] == tile.size - 1
    assert tile.basins[4].bbox_px[3] == tile.size - 1

    # Ordering rule: (min_y, min_x) of extent, ascending.
    keys = [(b.bbox_px[1], b.bbox_px[0]) for b in tile.basins]
    assert keys == sorted(keys)

    # v1: NO identity, and that must read as absence rather than as zero.
    assert all(not b.has_v2 for b in tile.basins)


@pytest.mark.skipif(not FIXTURE_V2.exists(), reason=f"fixture absent: {FIXTURE_V2}")
def test_the_committed_v2_fixture_carries_identity_capacity_and_heads():
    """The v2 handoff artefact. Regenerate with
    `python tools/make_basin_v2_fixture.py`; its docstring says what each row
    and each head is there to break."""
    basins_mod = pytest.importorskip("terrain_service.bake.basins")
    tile = tc.decode_v2(FIXTURE_V2.read_bytes())
    assert tile.size == 512
    assert tile.bake_ver == 24
    assert tile.basins is not None and len(tile.basins) == 5
    assert all(b.has_v2 for b in tile.basins)
    assert {b.kind for b in tile.basins} == set(range(tc.BASIN_KIND_COUNT))

    wox, woy = tile.x * tile.size, tile.y * tile.size

    # THE ROW v2 EXISTS FOR: a basin crossing the tile edge whose DEEPEST CELL
    # is in the neighbour. Its clipped bbox stops at the tile; its world bbox
    # does not; and its identity anchor is a pixel this tile does not own.
    spanning = [b for b in tile.basins if b.span_flags & tc.BASIN_SPAN_CROSSES_TILE]
    assert len(spanning) == 2, "both span directions must be exercised"
    outside = [b for b in spanning if not (wox <= b.world_floor_px[0] < wox + tile.size)]
    assert outside, "no basin whose floor is outside the tile -- the v2 case"
    for b in tile.basins:
        # The clipped view is the world view seen through this tile.
        x0, y0, x1, y1 = b.bbox_px
        wx0, wy0, wx1, wy1 = b.world_bbox_px
        assert wx0 <= wox + x0 and wox + x1 <= wx1
        assert wy0 <= woy + y0 and woy + y1 <= wy1
        # The anchor is a cell of this component, and it is legible: it
        # unpacks to the pixel it was built from.
        fx, fy = b.world_floor_px
        assert wx0 <= fx <= wx1 and wy0 <= fy <= wy1
        assert basins_mod.world_px_from_global_id(b.global_id) == (fx, fy)
        # Negative world coordinates throughout (the wet alpine block is
        # entirely in negative tiles, and the anchor packs through two's
        # complement).
        assert fx < 0 and fy < 0
        assert b.floor_mm <= b.surface_mm <= b.spill_mm

    # Capacity spans a real range: 0 for the overflowing lake (already at its
    # spill) and past u32 for the big pan, which is the truncation trap.
    caps = [b.capacity_l for b in tile.basins]
    assert min(caps) == 0 and max(caps) > 0xFFFFFFFF

    assert tile.heads is not None and len(tile.heads) == 4
    ys = [(h.px[1], h.px[0]) for h in tile.heads]
    assert ys == sorted(ys), "heads are ordered by (y, x)"
    # One head carries a trunk discharge: a reach entering the padded domain
    # has no donor inside it and is a head too. That is the case the u32 has
    # to hold, not the trickle at the top of a hillside.
    assert max(h.q_m3_yr for h in tile.heads) > 230_000_000


# ------------------------------------------------------------- headwaters


def _head(x, y, q):
    return tc.HeadEntry(px=(x, y), q_m3_yr=q)


def test_headwaters_round_trip_through_the_whole_tile():
    heads = [_head(1, 2, 0), _head(30, 2, 12_345), _head(0, 31, 2_300_000_00)]
    back = tc.decode_v2(tc.encode_v2(_tile(None, heads=heads)))
    assert back.heads == heads


def test_an_empty_head_table_still_sets_the_flag():
    """Third time, same rule: "surveyed, no reaches start here" is a fact and
    must not read as "baked before headwaters existed"."""
    data = tc.encode_v2(_tile(None, heads=[]))
    flags = struct.unpack_from("<H", data, tc._HEADER.size + 6)[0]
    assert flags & tc.FLAG_HEADS_PRESENT
    assert tc.decode_v2(data).heads == []

    none = tc.encode_v2(_tile(None, heads=None))
    flags = struct.unpack_from("<H", none, tc._HEADER.size + 6)[0]
    assert not (flags & tc.FLAG_HEADS_PRESENT)
    assert tc.decode_v2(none).heads is None


def test_the_head_flag_and_section_must_agree_in_both_directions():
    data = bytearray(tc.encode_v2(_tile(None, heads=[_head(1, 2, 3)])))
    off = tc._HEADER.size + 6
    flags = struct.unpack_from("<H", data, off)[0]
    struct.pack_into("<H", data, off, flags & ~tc.FLAG_HEADS_PRESENT)
    with pytest.raises(ValueError, match="headwater flag is clear"):
        tc.decode_v2(bytes(data))

    plain = bytearray(tc.encode_v2(_tile(None)))
    struct.pack_into("<H", plain, off,
                     struct.unpack_from("<H", plain, off)[0] | tc.FLAG_HEADS_PRESENT)
    with pytest.raises(ValueError, match="SECTION_HEADWATERS is missing"):
        tc.decode_v2(bytes(plain))


def test_a_head_discharge_above_u32_is_refused_not_saturated():
    """A saturated faucet rate is a plausible number that silently understates
    a river; the bake failing loudly is the cheaper outcome. World maximum
    observed is 2.3e8 m^3/yr against a 4.295e9 ceiling -- 18x."""
    assert tc.HEADWATER_Q_MAX == 4_294_967_295
    with pytest.raises(ValueError, match="refuses rather than saturates"):
        _head(1, 1, tc.HEADWATER_Q_MAX + 1).pack()
    with pytest.raises(ValueError, match="refuses rather than saturates"):
        tc.encode_headwater_arrays([1], [1], [float(tc.HEADWATER_Q_MAX) * 2])


def test_heads_must_be_strictly_ordered_so_a_faucet_cannot_be_emitted_twice():
    """Duplicate points are twice the water at one place, and the order is
    free (the producer walks a raster mask), so it is a contract."""
    with pytest.raises(ValueError, match="strictly ordered"):
        tc.encode_headwaters([_head(5, 1, 10), _head(4, 1, 10)])
    with pytest.raises(ValueError, match="strictly ordered"):
        tc.encode_headwaters([_head(5, 1, 10), _head(5, 1, 20)])
    with pytest.raises(ValueError, match="strictly ordered"):
        tc.encode_headwater_arrays([5, 4], [1, 1], [10, 10])
    # Ascending y with a descending x is fine: the key is (y, x).
    tc.encode_headwaters([_head(5, 1, 10), _head(4, 2, 10)])


def test_the_array_and_object_encoders_write_the_same_bytes():
    """The bake uses the array form (thousands of heads, already numpy). Two
    encoders for one section is a divergence waiting to happen, so they are
    pinned to each other."""
    xs, ys, qs = [1, 30, 0], [2, 2, 31], [0.0, 12_345.4, 2.3e8]
    rows = [_head(x, y, int(round(q))) for x, y, q in zip(xs, ys, qs)]
    assert tc.encode_headwater_arrays(xs, ys, qs) == tc.encode_headwaters(rows)


def test_a_head_outside_the_tile_is_refused_at_encode_and_decode():
    with pytest.raises(ValueError, match="outside the 32px tile"):
        tc.encode_v2(_tile(None, heads=[_head(40, 1, 5)], size=32))
    data = bytearray(tc.encode_v2(_tile(None, heads=[_head(4, 1, 5)], size=32)))
    off = tc._HEADER.size + tc._V2_EXT.size
    n = struct.unpack_from("<H", data, tc._HEADER.size + tc._V2_EXT.size - 2)[0]
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size)
             for i in range(n)]
    soff = next(o for sid, o, _ in table if sid == tc.SECTION_HEADWATERS)
    struct.pack_into("<H", data, soff + tc._HEADWATER_TABLE_HEADER.size, 999)
    with pytest.raises(ValueError, match="outside the tile"):
        tc.decode_v2(bytes(data))
