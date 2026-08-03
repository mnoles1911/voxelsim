"""SECTION_BASIN_TABLE, the Python half (watershed plan P1, bake_ver 8).

The C++ half is voxel-core/tests/test_tilestore.cpp; the two meet at
voxel-core/tests/fixtures/vxtl_v2_golden_basins_512.vxtl, which this file also
checks, because a round-trip test in one language only proves that language
agrees with itself.

The refusals are most of this file, deliberately. A basin row is a GAMEPLAY
INSTRUCTION -- "flood-fill from here, up to this level" -- so a row that
decodes into something plausible but wrong puts water where there is none, on
terrain, in front of a player, with no error anywhere. Every field that could
do that is refused rather than clamped.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from terrain_service import tile_codec as tc

FIXTURE = (Path(__file__).resolve().parents[2]
           / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_basins_512.vxtl")


def _entry(basin_id=0, seed=(4, 5), bbox=(2, 3, 8, 9), outlet=(1, 6),
           spill=12_000, surface=9_000, kind=tc.BASIN_KIND_LAKE_TERMINAL):
    return tc.BasinEntry(basin_id=basin_id, seed_px=seed, bbox_px=bbox,
                         outlet_px=outlet, spill_mm=spill, surface_mm=surface,
                         kind=kind)


def _tile(basins, size=32, block_log2=4):
    cp = np.zeros((size, size), np.int16)
    return tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp,
                     block_log2=block_log2, basins=basins)


# --------------------------------------------------------------- the layout


def test_the_row_is_exactly_32_bytes():
    """The size is part of the wire format and the C++ side asserts it too.

    It is also written INTO the table as `entry_bytes`, which is what lets a
    decoder from a different revision refuse rather than misread.
    """
    assert tc.BASIN_ENTRY_BYTES == 32
    assert len(_entry().pack()) == 32
    assert tc._BASIN_TABLE_HEADER.size == 8


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
                                       kind=tc.BASIN_KIND_SALT_FLAT)]
    back = tc.decode_v2(tc.encode_v2(_tile(rows)))
    assert back.basins == rows


def test_negative_elevations_survive():
    """Spill and surface are i32 mm and the sign has to work.

    The bake's registry refuses a basin at or below sea level, so no shipped
    tile will exercise this -- which is exactly why it is tested here and
    carried in the fixture.
    """
    row = _entry(spill=-1, surface=-2_147_000_000)
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
        _entry(spill=1000, surface=2000).pack()
    good = tc.encode_basin_table([_entry(spill=1000, surface=1000)])
    bad = bytearray(good)
    struct.pack_into("<i", bad, tc._BASIN_TABLE_HEADER.size + 22, 5000)
    with pytest.raises(ValueError, match="above its spill"):
        tc.decode_basin_table(bytes(bad))


def test_ids_must_be_zero_to_n_minus_one_in_order():
    """The client INDEXES by id, so a gap lets two processes disagree about
    which basin is "3". The bake orders by (min_y, min_x) of extent precisely
    so the id is a pure function of the surface."""
    with pytest.raises(ValueError, match="0..n-1"):
        tc.encode_basin_table([_entry(basin_id=0), _entry(basin_id=2)])
    good = bytearray(tc.encode_basin_table([_entry(0), _entry(1)]))
    struct.pack_into("<H", good, tc._BASIN_TABLE_HEADER.size + 32, 7)
    with pytest.raises(ValueError, match="not 0..n-1"):
        tc.decode_basin_table(bytes(good))


def test_a_row_size_this_build_does_not_know_is_refused():
    """Refusing beats reading 33-byte records out of a 32-byte stream and
    getting plausible garbage. This is what `entry_bytes` is for."""
    data = bytearray(tc.encode_basin_table([_entry()]))
    struct.pack_into("<H", data, 2, 33)
    with pytest.raises(ValueError, match="entry size"):
        tc.decode_basin_table(bytes(data))
    struct.pack_into("<H", data, 2, tc.BASIN_ENTRY_BYTES)
    struct.pack_into("<H", data, 0, 99)
    with pytest.raises(ValueError, match="version"):
        tc.decode_basin_table(bytes(data))


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
    data = bytearray(tc.encode_basin_table([_entry()]))
    data[tc._BASIN_TABLE_HEADER.size + 30] = 1
    with pytest.raises(ValueError, match="reserved"):
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
