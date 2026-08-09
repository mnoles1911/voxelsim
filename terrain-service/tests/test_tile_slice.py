"""Index-then-range fetching of a `.vxtl` fine tile (task #52).

What these tests are actually defending, in order of how expensive the bug is:

1. **A CONSTANT block must cost zero requests.** It has no data-section entry
   at all, so the `(offset=0, comp_len=0)` in its index row is not a range --
   byte 0 of the data section belongs to some other block. A fetcher that
   planned a request from it would silently decode the wrong terrain.
2. **A sliced block must be bit-identical to the same block out of a full
   `decode_v2`.** Being off by one block still yields plausible ground.
3. **A +x run inside one block-row must be ONE range.** At the measured
   160 ms RTT the whole design is latency-bound; if coalescing quietly stops
   working the bytes still look right and the fetch takes 30x as long.
4. **`/tile` must answer 206.** It answered 200-with-the-whole-body before
   this change, and an HTTP fetcher that tolerated that would download the
   entire 32-56 MB tile once per block while every byte counter it kept looked
   perfectly reasonable.
"""

from __future__ import annotations

import re
from pathlib import Path

import numpy as np
import pytest

from terrain_service import tile_codec as tc
from terrain_service import tile_slice as ts
from terrain_service.app import create_app
from terrain_service.cache import TileCache
from terrain_service.providers.synthetic import SyntheticProvider

REPO = Path(__file__).resolve().parents[2]


# --------------------------------------------------------------- fixtures ---

def _field(size: int) -> np.ndarray:
    yy, xx = np.mgrid[0:size, 0:size]
    mm = 200_000 + (np.sin(xx / 9.0) * 4000 + np.cos(yy / 13.0) * 3000 + (xx + yy) * 5)
    return tc.mm_to_control_points(mm.astype(np.int64), 200_000, tc.QUANT_100MM)


def _tile(size: int = 64, block_log2: int = 4, codec: int = tc.CODEC_RAW) -> tc.TileV2:
    """All three planes, and CONSTANT blocks scattered through each.

    The water plane is deliberately MOSTLY constant, which is what the shipped
    bv12 tiles look like (72-87% CONSTANT) and is the case where getting trap
    #1 wrong does the most damage.
    """
    bs = 1 << block_log2
    nb = size // bs
    cp = _field(size)
    cp[0:bs, 0:bs] = 12345                       # CONSTANT elevation block (0,0)
    cp[bs:2 * bs, 2 * bs:3 * bs] = -7            # CONSTANT elevation block (2,1)

    flow = (np.mgrid[0:size, 0:size][0] % 7).astype(np.uint8)
    flow[0:bs, bs:2 * bs] = 3                    # CONSTANT flow block (1,0)

    water = np.full((size, size), tc.WATER_DRY_DEPTH, dtype=np.int16)
    water[bs:2 * bs, bs:2 * bs] = (_field(size)[0:bs, 0:bs] % 97).astype(np.int16)
    assert nb >= 4
    return tc.TileV2(
        seed=20260804, x=-11, y=-6, size=size, elevation_cp=cp,
        base_offset_mm=200_000, quant=tc.QUANT_100MM, codec=codec, bake_ver=12,
        block_log2=block_log2, flow=flow, water_cp=water, basins=[],
    )


@pytest.fixture()
def blob() -> bytes:
    return tc.encode_v2(_tile())


# ------------------------------------------------------------ the preamble ---

def test_preamble_is_four_disjoint_regions(blob, monkeypatch):
    """encode_v2 puts each plane's index immediately before its own
    multi-megabyte data section, so "the first N bytes" is NOT the preamble."""
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    order = sorted(pre.sections.items(), key=lambda kv: kv[1][0])
    assert [sid for sid, _ in order] == [
        tc.SECTION_ELEV_INDEX, tc.SECTION_ELEV_DATA,
        tc.SECTION_FLOW_INDEX, tc.SECTION_FLOW_DATA,
        tc.SECTION_WATER_INDEX, tc.SECTION_WATER_DATA,
        tc.SECTION_BASIN_TABLE,
    ]
    # ELEV_INDEX abuts the section table; the other three preamble sections are
    # each separated from it by a whole data section, which on a shipped tile
    # is tens of megabytes. Only ELEV_INDEX can ever ride in on the head probe.
    elev_off, elev_len = pre.sections[tc.SECTION_ELEV_INDEX]
    assert elev_off == tc._HEADER.size + tc._V2_EXT.size + 7 * tc._SECTION_ENTRY.size
    for sid in (tc.SECTION_FLOW_INDEX, tc.SECTION_WATER_INDEX, tc.SECTION_BASIN_TABLE):
        assert pre.sections[sid][0] > elev_off + elev_len
    assert pre.preamble_bytes < len(blob)

    # With a probe sized as it is on a real tile -- big enough for the header,
    # the table and ELEV_INDEX, and nothing beyond -- the full preamble is one
    # head probe plus three more ranges.
    monkeypatch.setattr(ts, "HEAD_PROBE_BYTES", elev_off + elev_len)
    src = ts.BytesRangeSource(blob)
    ts.read_preamble(src)
    assert src.requests == 1 + 3


def test_ground_only_preamble_is_one_request(blob, monkeypatch):
    """A mesher that wants ground and nothing else needs the head probe only."""
    pre0 = ts.read_preamble(ts.BytesRangeSource(blob))
    monkeypatch.setattr(ts, "HEAD_PROBE_BYTES",
                        sum(pre0.sections[tc.SECTION_ELEV_INDEX]))
    src = ts.BytesRangeSource(blob)
    pre = ts.read_preamble(src, want_flow=False, want_water=False,
                           want_basins=False, want_heads=False)
    assert src.requests == 1
    assert pre.flow is None and pre.water is None and pre.basins is None
    assert pre.heads is None
    assert pre.elevation is not None


def test_a_bake_ver_24_tile_slices_and_carries_its_headwaters():
    """THE THIRD PARSER. This module has its own copy of the unknown-flag rule,
    so a new flag bit refuses the tile HERE even when tile_codec and tilestore
    both accept it -- which would make every bake_ver 24 tile unsliceable.

    (Not hypothetical for this format: the two-halves-in-lockstep property is
    exactly what FLAG_HEADS_PRESENT trades on, and a third half nobody counted
    is how that property quietly stops holding.)
    """
    tile = _tile()
    tile.bake_ver = 24
    tile.heads = [tc.HeadEntry(px=(3, 1), q_m3_yr=0),
                  tc.HeadEntry(px=(1, 9), q_m3_yr=230_000_001)]
    blob = tc.encode_v2(tile)
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    assert pre.heads == tile.heads
    assert pre.preamble_bytes > 0
    # And a client that does not want them does not pay for them.
    assert ts.read_preamble(ts.BytesRangeSource(blob), want_heads=False).heads is None


def test_preamble_matches_the_header(blob):
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    full = tc.decode_v2(blob)
    assert (pre.seed, pre.x, pre.y, pre.size) == (full.seed, full.x, full.y, full.size)
    assert pre.block_log2 == full.block_log2 and pre.quant == full.quant
    assert pre.codec == full.codec and pre.bake_ver == full.bake_ver
    assert pre.base_offset_mm == full.base_offset_mm
    assert pre.file_size == len(blob)
    assert pre.basins == full.basins


def test_head_probe_too_small_for_the_index_still_works(blob, monkeypatch):
    """The probe is an optimisation, never a correctness assumption: shrink it
    below ELEV_INDEX and read_preamble must fetch the index in round two."""
    monkeypatch.setattr(ts, "HEAD_PROBE_BYTES", 200)
    src = ts.BytesRangeSource(blob)
    pre = ts.read_preamble(src)
    assert src.requests == 1 + 4          # probe missed ELEV_INDEX as well
    assert len(pre.elevation.entries) == pre.blocks_per_axis ** 2
    full = tc.decode_v2(blob)
    bs = pre.block_dim_px
    got, _ = ts.fetch_blocks(src, pre.elevation, [(1, 1)], codec=pre.codec)
    assert np.array_equal(got[(1, 1)], full.elevation_cp[bs:2 * bs, bs:2 * bs])


# ---------------------------------------------------- trap 1: CONSTANT blocks

def test_constant_blocks_have_no_range_and_cost_no_request(blob):
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    assert pre.elevation.file_range(0, 0) is None          # the flat block
    assert pre.elevation.entry(0, 0).const_cp == 12345
    assert pre.flow.file_range(1, 0) is None
    assert pre.water.constant_share() > 0.8

    src = ts.BytesRangeSource(blob)
    got, plans = ts.fetch_blocks(src, pre.elevation, [(0, 0), (2, 1)], codec=pre.codec)
    assert plans == []                                     # nothing to ask for
    assert src.requests == 0
    assert np.all(got[(0, 0)] == 12345) and np.all(got[(2, 1)] == -7)


def test_all_constant_water_plane_needs_no_data_at_all():
    """An entirely dry tile: 1024 CONSTANT entries, zero data bytes. The whole
    water plane comes out of the index."""
    t = _tile()
    t.water_cp = np.full((t.size, t.size), tc.WATER_DRY_DEPTH, dtype=np.int16)
    blob = tc.encode_v2(t)
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    assert pre.water.constant_share() == 1.0
    assert pre.sections[tc.SECTION_WATER_DATA][1] == 0
    src = ts.BytesRangeSource(blob)
    every = [(bx, by) for by in range(pre.blocks_per_axis)
             for bx in range(pre.blocks_per_axis)]
    got, plans = ts.fetch_blocks(src, pre.water, every, codec=pre.codec)
    assert src.requests == 0 and plans == []
    assert all(np.all(a == tc.WATER_DRY_DEPTH) for a in got.values())


def test_constant_index_row_must_be_zeroed():
    """The parser refuses an index that claims a CONSTANT block owns bytes --
    that pairing is exactly what would make a fetcher plan a bogus range."""
    blob = bytearray(tc.encode_v2(_tile()))
    pre = ts.read_preamble(ts.BytesRangeSource(bytes(blob)))
    off = pre.sections[tc.SECTION_ELEV_INDEX][0]           # entry 0 is CONSTANT
    blob[off + 8:off + 12] = (99).to_bytes(4, "little")    # comp_len := 99
    with pytest.raises(ValueError, match="CONSTANT"):
        ts.read_preamble(ts.BytesRangeSource(bytes(blob)))


# ------------------------------------------------------- trap 2: correctness

@pytest.mark.parametrize("codec", [tc.CODEC_RAW, tc.CODEC_ZSTD])
def test_every_block_of_every_plane_matches_a_full_decode(codec):
    if codec == tc.CODEC_ZSTD and not tc.HAVE_ZSTD:
        pytest.skip("zstandard not installed")
    blob = tc.encode_v2(_tile(codec=codec))
    full = tc.decode_v2(blob)
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    bs = pre.block_dim_px
    every = [(bx, by) for by in range(pre.blocks_per_axis)
             for bx in range(pre.blocks_per_axis)]
    for key, plane, want in (
        ("elev", pre.elevation, full.elevation_cp),
        ("flow", pre.flow, full.flow),
        ("water", pre.water, full.water_cp),
    ):
        got, _ = ts.fetch_blocks(ts.BytesRangeSource(blob), plane, every, codec=codec)
        for (bx, by), arr in got.items():
            slab = want[by * bs:(by + 1) * bs, bx * bs:(bx + 1) * bs]
            assert arr.dtype == slab.dtype, key
            assert np.array_equal(arr, slab), f"{key} block ({bx},{by})"


def test_slicing_reads_far_less_than_the_whole_tile(blob):
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    src = ts.BytesRangeSource(blob)
    ts.fetch_blocks(src, pre.elevation, [(1, 1)], codec=pre.codec)
    assert src.bytes_fetched == pre.elevation.entry(1, 1).comp_len
    assert src.bytes_fetched < len(blob) // 4


# ------------------------------------------------------- trap 3: coalescing

def test_a_row_run_is_one_range(blob):
    """The index is row-major, x fastest, and blocks are written in index
    order -- so blocks along +x in one row are contiguous bytes."""
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    plane = pre.elevation
    row = [(bx, 1) for bx in range(pre.blocks_per_axis) if plane.file_range(bx, 1)]
    plans = ts.plan_block_ranges(plane, row, coalesce_gap=0)
    assert len(plans) == 1
    assert plans[0].wasted_bytes == 0
    assert sorted(plans[0].blocks) == sorted(row)


def test_separate_rows_do_not_merge_when_the_gap_is_too_large(blob):
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    plane = pre.elevation
    picked = [(0, 1), (0, 3)]
    tight = ts.plan_block_ranges(plane, picked, coalesce_gap=0)
    assert len(tight) == 2
    loose = ts.plan_block_ranges(plane, picked, coalesce_gap=len(blob))
    assert len(loose) == 1
    assert loose[0].wasted_bytes > 0
    # Whatever the plan, the answer is the same.
    a, _ = ts.fetch_blocks(ts.BytesRangeSource(blob), plane, picked,
                           codec=pre.codec, coalesce_gap=0)
    b, _ = ts.fetch_blocks(ts.BytesRangeSource(blob), plane, picked,
                           codec=pre.codec, coalesce_gap=len(blob))
    for k in a:
        assert np.array_equal(a[k], b[k])


def test_coalescing_skips_constant_holes(blob):
    """A CONSTANT block punches a hole in the (by,bx) sequence but NOT in the
    file, so its neighbours are still adjacent bytes. Planning by file offset
    is what keeps that one range; planning by block coordinate would not."""
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    plane = pre.elevation
    row0 = [(bx, 0) for bx in range(pre.blocks_per_axis)]   # (0,0) is CONSTANT
    plans = ts.plan_block_ranges(plane, row0, coalesce_gap=0)
    assert len(plans) == 1
    assert len(plans[0].blocks) == len(row0) - 1


def test_plan_is_stable_under_input_order(blob):
    pre = ts.read_preamble(ts.BytesRangeSource(blob))
    row = [(bx, 2) for bx in range(pre.blocks_per_axis)]
    a = ts.plan_block_ranges(pre.elevation, row, coalesce_gap=0)
    b = ts.plan_block_ranges(pre.elevation, list(reversed(row)), coalesce_gap=0)
    assert [p.span for p in a] == [p.span for p in b]


# ------------------------------------------------------------ trap 4: HTTP

def _served(tmp_path) -> tuple:
    provider = SyntheticProvider()
    cache = TileCache(tmp_path)
    blob = tc.encode_v2(_tile())
    cache.put_fine(provider.provider_id, 7, -11, -6, blob)
    app = create_app(provider=provider, cache=cache)
    return app.test_client(), blob


def test_tile_endpoint_serves_206_for_a_range(tmp_path):
    client, blob = _served(tmp_path)
    url = "/tile?seed=7&x=-11&y=-6&scale=16"
    whole = client.get(url)
    assert whole.status_code == 200
    assert whole.headers["Accept-Ranges"] == "bytes"
    assert whole.get_data() == blob

    part = client.get(url, headers={"Range": "bytes=0-182"})
    assert part.status_code == 206
    assert part.headers["Content-Range"] == f"bytes 0-182/{len(blob)}"
    assert part.get_data() == blob[:183]


def test_http_range_source_fetches_one_block(tmp_path):
    client, blob = _served(tmp_path)
    src = ts.HttpRangeSource(client.get, "/tile?seed=7&x=-11&y=-6&scale=16")
    pre = ts.read_preamble(src)
    before = src.bytes_fetched
    got, plans = ts.fetch_blocks(src, pre.elevation, [(1, 1), (2, 1), (3, 1)],
                                 codec=pre.codec)
    assert len(plans) == 1                      # a +x run, one HTTP request
    full = tc.decode_v2(blob)
    bs = pre.block_dim_px
    for (bx, by), arr in got.items():
        assert np.array_equal(arr, full.elevation_cp[by * bs:(by + 1) * bs,
                                                     bx * bs:(bx + 1) * bs])
    assert src.bytes_fetched - before < len(blob) // 4


def test_http_head_probe_past_eof_is_clamped_not_rejected(tmp_path):
    """The head probe asks for HEAD_PROBE_BYTES before it knows the file's
    length, so on any tile smaller than the probe the server legitimately
    returns fewer bytes than asked (RFC 9110: clamp, don't fail). Content-Range
    carries the complete length, which is how that is told apart from a
    truncated response -- without it the fetcher rejects every small tile."""
    client, blob = _served(tmp_path)
    assert len(blob) < ts.HEAD_PROBE_BYTES
    src = ts.HttpRangeSource(client.get, "/tile?seed=7&x=-11&y=-6&scale=16")
    (body,) = src.read_ranges([ts.ByteRange(0, ts.HEAD_PROBE_BYTES)])
    assert body == blob


def test_http_source_rejects_a_transport_that_ignores_range():
    """The pre-change behaviour of /tile, and the one failure mode a byte
    counter cannot see: full body, status 200, "saving" reported anyway."""
    class Ignores:
        status_code = 200
        content = b"x" * 5000

    src = ts.HttpRangeSource(lambda url, headers=None: Ignores(), "/tile")
    with pytest.raises(ts.RangeNotSupported, match="ignored the Range header"):
        src.read_ranges([ts.ByteRange(0, 10)])


def test_http_source_rejects_a_short_range():
    class Short:
        status_code = 206
        content = b"xy"

    src = ts.HttpRangeSource(lambda url, headers=None: Short(), "/tile")
    with pytest.raises(ts.RangeNotSupported, match="returned 2 B"):
        src.read_ranges([ts.ByteRange(0, 10)])


# ------------------------------------------------------------- block coverage

def test_carrier_stencil_constants_match_voxel_core():
    """tile_slice duplicates kCarrierStencilLo/Hi so the fetch planner can size
    a footprint without a UE build. tiles.h asserts them literally; if that
    static_assert ever changes, this fails instead of silently under-dilating.
    """
    src = (REPO / "voxel-core" / "include" / "voxelcore" / "tiles.h").read_text(
        encoding="utf-8", errors="replace"
    )
    m = re.search(
        r"kCarrierStencilLo\s*==\s*(-?\d+)\s*&&\s*kCarrierStencilHi\s*==\s*(-?\d+)", src
    )
    assert m, "tiles.h no longer pins the carrier stencil with a static_assert"
    assert (int(m.group(1)), int(m.group(2))) == (
        ts.CARRIER_STENCIL_LO, ts.CARRIER_STENCIL_HI
    )


def test_blocks_covering_rect_clamps_and_orders():
    got = ts.blocks_covering_rect(0, 0, 511, 255, block_dim_px=256, blocks_per_axis=32)
    assert got == [(0, 0), (1, 0)]
    assert ts.blocks_covering_rect(-100, -100, -1, -1, block_dim_px=256,
                                   blocks_per_axis=32) == []
    assert ts.blocks_covering_rect(5, 5, 4, 9, block_dim_px=256,
                                   blocks_per_axis=32) == []
    # Clamped at the far edge rather than running off the index.
    assert ts.blocks_covering_rect(8000, 8000, 99999, 99999, block_dim_px=256,
                                   blocks_per_axis=32) == [(31, 31)]


def test_dilation_grows_one_block_to_its_neighbours():
    """A block-aligned 480 m footprint reaches into all eight neighbours once
    the carrier stencil is applied -- 1 block wanted, 9 blocks read. That is
    the real cost of a correct ground fetch, and it is why the measurement
    reports the undilated figure as a floor and not as the answer."""
    inner = ts.blocks_covering_rect(4096, 4096, 4351, 4351,
                                    block_dim_px=256, blocks_per_axis=32)
    assert inner == [(16, 16)]
    outer = ts.dilated_block_coverage(4096, 4096, 4351, 4351, block_dim_px=256,
                                      blocks_per_axis=32, extra_margin_px=20)
    assert len(outer) == 9
    assert set(outer) == {(bx, by) for by in (15, 16, 17) for bx in (15, 16, 17)}
