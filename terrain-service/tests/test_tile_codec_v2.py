"""`.vxtl` v2 codec tests (docs/vxtl-v2-format.md — FROZEN CONTRACT, §9
conformance). v1 is exercised only as a non-regression / cross-version
check here; its own golden tests live in test_tiles.py and are untouched.

Conformance checklist this file covers (spec §9):
  1. round-trip: CONSTANT, CODED/16, CODED/32, RAW blocks, both quant values.
  4. truncation/corruption rejected cleanly; version cross-checks both ways.
Items 2 (C++ golden decode) and 3 (B-spline sample parity) are cross-language
and out of scope for this file — see test_v2_golden_fixture_* below for what
IS covered from the Python side of item 2 (the fixture decodes correctly
here; a separate C++ agent/tests must confirm parity).
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from terrain_service import tile_codec as tc

FIXTURE_PATH = (
    Path(__file__).resolve().parents[2]
    / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_512.vxtl"
)
FLOW_FIXTURE_PATH = (
    Path(__file__).resolve().parents[2]
    / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_flow_512.vxtl"
)


# --------------------------------------------------------------- helpers ---

def _smooth_field(size: int, base_offset_mm: int = 200_000, quant: int = tc.QUANT_100MM) -> np.ndarray:
    """A gentle, fully CODED/16 control lattice — no cliffs, no flats."""
    yy, xx = np.mgrid[0:size, 0:size]
    elev_mm = (
        base_offset_mm
        + (np.sin(xx / 9.0) * 4000 + np.cos(yy / 13.0) * 3000 + (xx + yy) * 5).astype(np.int64)
    )
    return tc.mm_to_control_points(elev_mm, base_offset_mm, quant)


def _cliff_block(size: int) -> np.ndarray:
    """A block engineered so the MED residual overflows int16 (§5): two
    adjacent first-row pixels jump from +32767 to -32768, forcing
    resid = -32768 - 32767 = -65535 for the encoder to represent, exactly
    the 'one post across a 30 m cliff' case the spec calls out."""
    blk = np.zeros((size, size), dtype=np.int16)
    blk[0, size // 2] = 32767
    blk[0, size // 2 + 1] = -32768
    return blk


def _mixed_tile(size: int, block_log2: int, *, quant: int = tc.QUANT_100MM, codec: int = tc.CODEC_RAW,
                 base_offset_mm: int = 200_000, flow: np.ndarray | None = None) -> tc.TileV2:
    """A tile with a CONSTANT block, a smooth (CODED/16) block, and — if the
    grid has a second block row — a cliff (CODED/32) block."""
    bs = 1 << block_log2
    assert size % bs == 0 and size // bs >= 1
    cp = _smooth_field(size, base_offset_mm, quant)
    nb = size // bs
    # Top-left block: flat (CONSTANT).
    cp[0:bs, 0:bs] = 12345
    if nb >= 2:
        # Block (bx=1, by=0): cliff -> CODED/32.
        cp[0:bs, bs:2 * bs] = _cliff_block(bs)
    return tc.TileV2(
        seed=20260729, x=5, y=-3, size=size, elevation_cp=cp,
        base_offset_mm=base_offset_mm, quant=quant, codec=codec,
        bake_ver=1, block_log2=block_log2, flow=flow,
    )


# ------------------------------------------------------------- round trip ---

@pytest.mark.parametrize("quant", [tc.QUANT_100MM, tc.QUANT_250MM])
def test_v2_roundtrip_smooth_coded16(quant):
    """A typical smooth tile round-trips exactly and lands entirely in
    CODED/16 (no cliffs, no flats) — the common case."""
    size, block_log2 = 32, 4
    cp = _smooth_field(size, quant=quant)
    tile = tc.TileV2(
        seed=1, x=0, y=0, size=size, elevation_cp=cp,
        base_offset_mm=200_000, quant=quant, block_log2=block_log2,
    )
    data = tc.encode_v2(tile)
    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, cp)
    assert back.quant == quant
    assert back.base_offset_mm == 200_000


def test_v2_roundtrip_mixed_modes():
    """CONSTANT + CODED/16 + CODED/32 in one tile, all through the public
    encode_v2/decode_v2 API — the core of §9 conformance item 1."""
    size, block_log2 = 32, 4  # 2x2 blocks of 16x16
    tile = _mixed_tile(size, block_log2)
    data = tc.encode_v2(tile)
    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, tile.elevation_cp)


def test_v2_constant_block_costs_zero_bytes():
    """§4: 'CONSTANT blocks cost zero bytes and are common.' Verify at the
    wire level, not just that decode is correct."""
    size, block_log2 = 16, 4  # exactly one block
    cp = np.full((size, size), -777, dtype=np.int16)
    index_bytes, data_bytes = tc._encode_plane(
        cp.astype(np.int64), block_log2=block_log2, codec=tc.CODEC_RAW, elem_dtype="<i2"
    )
    assert data_bytes == b""
    offset, comp_len, mode, const_cp, resid_bits, pad = tc._BLOCK_ENTRY.unpack_from(index_bytes, 0)
    assert mode == tc.MODE_CONSTANT
    assert comp_len == 0
    assert const_cp == -777

    # And the full tile round-trips with an all-zero ELEV_DATA section.
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2)
    data = tc.encode_v2(tile)
    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, cp)


def test_v2_resid_bits_32_required_for_a_single_cliff():
    """§5: 'resid_bits is REQUIRED, not an optimisation... one post across a
    30 m cliff does it.' A block that is otherwise perfectly ordinary except
    for ONE steep step must be flagged resid_bits=32 and must round-trip —
    an encoder that assumes 16 bits would silently corrupt this."""
    bs = 16
    blk = _cliff_block(bs)
    resid = tc._med_residual(blk.astype(np.int64))
    assert resid.min() < -32768 or resid.max() > 32767, "test fixture must actually overflow int16"

    index_bytes, data_bytes = tc._encode_plane(
        blk.astype(np.int64), block_log2=int(np.log2(bs)), codec=tc.CODEC_RAW, elem_dtype="<i2"
    )
    offset, comp_len, mode, const_cp, resid_bits, pad = tc._BLOCK_ENTRY.unpack_from(index_bytes, 0)
    assert mode == tc.MODE_CODED
    assert resid_bits == 32
    assert comp_len == bs * bs * 4

    decoded = tc._decode_plane(
        index_bytes, data_bytes, size=bs, block_log2=int(np.log2(bs)),
        codec=tc.CODEC_RAW, elem_dtype="<i2", out_dtype=np.int16,
    )
    np.testing.assert_array_equal(decoded, blk)


def test_v2_resid_bits_32_full_tile_roundtrip():
    """Same overflow case, but exercised through the public encode_v2 /
    decode_v2 API on a full multi-block tile (not just the block helper)."""
    size, block_log2 = 32, 4
    tile = _mixed_tile(size, block_log2)
    data = tc.encode_v2(tile)
    # Confirm the cliff block really did land as CODED/32 in the wire bytes,
    # not e.g. silently truncated to 16 bits.
    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, tile.elevation_cp)
    bs = 1 << block_log2
    cliff_region = tile.elevation_cp[0:bs, bs:2 * bs]
    assert cliff_region.min() == -32768 and cliff_region.max() == 32767


def test_v2_raw_block_forced_and_roundtrips():
    """RAW mode (§4 mode=2) is reachable and round-trips. The reference
    encoder never selects RAW automatically (see _encode_plane's docstring
    for why); it's exercised here via the explicit raw_blocks override,
    which is the documented, supported way to reach it."""
    size, block_log2 = 32, 4
    cp = _smooth_field(size)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2)
    data = tc.encode_v2(tile, raw_blocks={(1, 0)})  # block (bx=1, by=0)

    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, cp)

    # Confirm the forced block is actually wire-encoded as MODE_RAW.
    off = tc._HEADER.size + tc._V2_EXT.size
    n_sections = 2
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size) for i in range(n_sections)]
    elev_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_ELEV_INDEX)
    bs = 1 << block_log2
    nb = size // bs
    block_i = 0 * nb + 1  # by=0, bx=1
    entry = tc._BLOCK_ENTRY.unpack_from(data, elev_index_off + block_i * tc._BLOCK_ENTRY.size)
    assert entry[2] == tc.MODE_RAW


def test_v2_flow_plane_roundtrip():
    """§6: optional flow plane, same block structure and predictor, packs
    channel/bank/deposition flag bits plus the log2 accumulation field."""
    size, block_log2 = 32, 4
    cp = _smooth_field(size)
    yy, xx = np.mgrid[0:size, 0:size]
    flow = ((xx + yy) % 20).astype(np.uint8)  # varying log2 field, bits 0-4
    flow[0:16, 0:16] = 0  # one flat block -> CONSTANT on the flow plane too
    # Flag bits set OUTSIDE the flattened block, so they survive it.
    flow[3, 20] |= tc.FLOW_BIT_CHANNEL
    flow[3, 21] |= tc.FLOW_BIT_BANK | tc.FLOW_BIT_DEPOSITION

    tile = tc.TileV2(seed=9, x=1, y=1, size=size, elevation_cp=cp, block_log2=block_log2, flow=flow)
    data = tc.encode_v2(tile)

    # flags bit0 must be set on the wire.
    _, _, _, _, _, flags = struct.unpack_from(
        "<BBBBHH", data, tc._HEADER.size
    )
    assert flags & tc.FLAG_FLOW_PRESENT

    back = tc.decode_v2(data)
    assert back.flow is not None
    np.testing.assert_array_equal(back.flow, flow)
    assert back.flow[3, 20] & tc.FLOW_BIT_CHANNEL
    assert back.flow[3, 21] & tc.FLOW_BIT_BANK
    assert back.flow[3, 21] & tc.FLOW_BIT_DEPOSITION


def test_v2_flow_constant_block_carries_full_u8_range():
    """The C++ decoder flagged this as unverified: `const_cp` is a signed i16
    on the wire (§4) even for the flow plane, whose elements are uint8. A
    constant flow block with every bit set (255 = log2 31 | channel | bank |
    deposition) must decode back to exactly 255, not wrap or sign-extend."""
    size, block_log2 = 16, 4  # single block
    cp = _smooth_field(size)
    flow = np.full((size, size), 0xFF, dtype=np.uint8)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2, flow=flow)
    data = tc.encode_v2(tile)

    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size) for i in range(4)]
    flow_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_FLOW_INDEX)
    offset, comp_len, mode, const_cp, resid_bits, pad = tc._BLOCK_ENTRY.unpack_from(data, flow_index_off)
    assert mode == tc.MODE_CONSTANT
    assert const_cp == 255  # NOT -1: must be read/written as an unsigned u8 value

    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.flow, flow)


def test_v2_decode_rejects_out_of_range_flow_const_cp():
    """A corrupted CONSTANT flow block whose const_cp doesn't fit uint8 must
    be rejected, not silently wrapped by the uint8 output assignment."""
    size, block_log2 = 16, 4
    cp = _smooth_field(size)
    flow = np.zeros((size, size), dtype=np.uint8)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2, flow=flow)
    data = bytearray(tc.encode_v2(tile))

    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(bytes(data), off + i * tc._SECTION_ENTRY.size) for i in range(4)]
    flow_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_FLOW_INDEX)
    struct.pack_into("<h", data, flow_index_off + 13, -1)  # const_cp field, out of u8 range
    with pytest.raises(ValueError):
        tc.decode_v2(bytes(data))


def test_v2_flow_raw_block_forced_and_roundtrips():
    """RAW for the flow plane is ONE byte per pixel (u1), not two (§6: 'same
    block structure' means same mechanics, not the elevation plane's element
    width). Force a flow block to RAW and confirm both the wire byte count
    and the round-tripped values."""
    size, block_log2 = 32, 4  # 2x2 blocks of 16x16
    cp = _smooth_field(size)
    yy, xx = np.mgrid[0:size, 0:size]
    flow = ((xx * 3 + yy * 7) % 251).astype(np.uint8)
    flow[5, 5] |= tc.FLOW_BIT_CHANNEL
    flow[5, 6] |= tc.FLOW_BIT_BANK | tc.FLOW_BIT_DEPOSITION

    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2, flow=flow)
    data = tc.encode_v2(tile, flow_raw_blocks={(0, 0)})

    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size) for i in range(4)]
    flow_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_FLOW_INDEX)
    entry = tc._BLOCK_ENTRY.unpack_from(data, flow_index_off)  # block (bx=0, by=0)
    assert entry[2] == tc.MODE_RAW
    bs = 1 << block_log2
    assert entry[1] == bs * bs * 1  # comp_len: 1 byte/px under CODEC_RAW, not 2
    assert entry[4] == 0  # resid_bits meaningless for RAW, written 0

    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.flow, flow)


def test_v2_no_flow_plane_by_default():
    size, block_log2 = 16, 4
    cp = _smooth_field(size)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2)
    data = tc.encode_v2(tile)
    back = tc.decode_v2(data)
    assert back.flow is None


def test_v2_control_point_conversion_helpers():
    for quant in (tc.QUANT_100MM, tc.QUANT_250MM):
        base = 123_400
        cp = np.array([[-32768, 0, 32767], [1, -1, 5000]], dtype=np.int16)
        mm = tc.control_points_to_mm(cp, base, quant)
        back = tc.mm_to_control_points(mm, base, quant)
        np.testing.assert_array_equal(back, cp)


def test_v2_mm_to_control_points_rejects_out_of_range():
    elev_mm = np.array([[10_000_000]], dtype=np.int64)  # way past +-3276.7 m about base
    with pytest.raises(ValueError):
        tc.mm_to_control_points(elev_mm, base_offset_mm=0, quant=tc.QUANT_100MM)


# ------------------------------------------------------------------ codec ---

@pytest.mark.skipif(not tc.HAVE_ZSTD, reason="zstandard not installed")
def test_v2_codec_zstd_roundtrip():
    size, block_log2 = 32, 4
    tile = _mixed_tile(size, block_log2, codec=tc.CODEC_ZSTD)
    data = tc.encode_v2(tile)
    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, tile.elevation_cp)
    assert back.codec == tc.CODEC_ZSTD


@pytest.mark.skipif(tc.HAVE_ZSTD, reason="only meaningful without zstandard installed")
def test_v2_codec_zstd_unavailable_fails_clearly_not_on_import():
    """The constraint: 'zstandard is NOT installed in CI... anything zstd
    must degrade gracefully... rather than failing to import.' The module
    must already have imported fine (this test file imported tile_codec at
    collection time); encoding with CODEC_ZSTD must raise a clear,
    catchable error instead."""
    size, block_log2 = 16, 4
    cp = _smooth_field(size)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2, codec=tc.CODEC_ZSTD)
    with pytest.raises(RuntimeError):
        tc.encode_v2(tile)


def test_v2_codec_raw_needs_no_compression_dependency():
    """CODEC_RAW must work with zero compression dependency, always."""
    size, block_log2 = 16, 4
    cp = _smooth_field(size)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2, codec=tc.CODEC_RAW)
    data = tc.encode_v2(tile)
    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, cp)


# ---------------------------------------------------- truncation/corruption

def _good_v2_bytes() -> bytes:
    size, block_log2 = 32, 4
    tile = _mixed_tile(size, block_log2)
    return tc.encode_v2(tile)


def test_v2_rejects_bad_magic():
    with pytest.raises(ValueError):
        tc.decode_v2(b"NOPE" + b"\0" * 100)


def test_v2_rejects_wrong_version():
    data = bytearray(_good_v2_bytes())
    struct.pack_into("<H", data, 4, 99)  # version field, offset 4
    with pytest.raises(ValueError, match="version"):
        tc.decode_v2(bytes(data))


def test_v2_rejects_trailing_bytes():
    good = _good_v2_bytes()
    with pytest.raises(ValueError):
        tc.decode_v2(good + b"\0")


def test_v2_rejects_truncated_header():
    with pytest.raises(ValueError):
        tc.decode_v2(b"VXTL" + b"\x02\x00")  # magic + version only, 6 bytes


def test_v2_rejects_truncated_extension():
    good = _good_v2_bytes()
    with pytest.raises(ValueError):
        tc.decode_v2(good[: tc._HEADER.size + 3])


def test_v2_rejects_truncated_section_table():
    good = _good_v2_bytes()
    cut = tc._HEADER.size + tc._V2_EXT.size + 5  # partway through the table
    with pytest.raises(ValueError):
        tc.decode_v2(good[:cut])


def test_v2_rejects_truncated_section_data():
    good = _good_v2_bytes()
    with pytest.raises(ValueError):
        tc.decode_v2(good[: len(good) - 4])


def test_v2_rejects_nonzero_reserved_bytes():
    data = bytearray(_good_v2_bytes())
    reserved_off = tc._HEADER.size + 1 + 1 + 1 + 1 + 2 + 2 + 4 + 1  # up to `reserved`
    data[reserved_off] = 0xFF
    with pytest.raises(ValueError):
        tc.decode_v2(bytes(data))


def test_v2_rejects_corrupted_block_pad():
    """Flip a byte inside a block index entry's `pad` field, which must be
    all zero per §4."""
    size, block_log2 = 16, 4  # single block, forced RAW so pad sits right after a known header
    cp = _smooth_field(size)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2)
    data = bytearray(tc.encode_v2(tile, raw_blocks={(0, 0)}))

    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(bytes(data), off + i * tc._SECTION_ENTRY.size) for i in range(2)]
    elev_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_ELEV_INDEX)
    pad_off = elev_index_off + tc._BLOCK_ENTRY.size - 4  # pad is the trailing 4 bytes
    data[pad_off] = 0xFF
    with pytest.raises(ValueError):
        tc.decode_v2(bytes(data))


def test_v2_rejects_out_of_bounds_block_offset():
    """A corrupted comp_len/offset that would read past the data section
    must raise, not silently return a short slice."""
    size, block_log2 = 16, 4
    cp = _smooth_field(size)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=cp, block_log2=block_log2)
    data = bytearray(tc.encode_v2(tile))

    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(bytes(data), off + i * tc._SECTION_ENTRY.size) for i in range(2)]
    elev_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_ELEV_INDEX)
    # comp_len field is bytes 8..12 of the entry.
    struct.pack_into("<I", data, elev_index_off + 8, 0xFFFFFFFF)
    with pytest.raises(ValueError):
        tc.decode_v2(bytes(data))


# ----------------------------------------------------------- version cross

def test_v1_file_still_decodes_as_v1():
    """§9 done-means: 'A v1 file must still decode as v1.'"""
    elevation = np.zeros((tc.TILE_SIZE, tc.TILE_SIZE), dtype=np.int16)
    elevation[10, 20] = -500
    climate = np.zeros((tc.CLIMATE_CHANNELS, tc.TILE_SIZE, tc.TILE_SIZE), dtype=np.uint8)
    v1_tile = tc.Tile(seed=1, x=0, y=0, scale=1, elevation=elevation, climate=climate)
    data = tc.encode(v1_tile)
    back = tc.decode(data)
    np.testing.assert_array_equal(back.elevation, elevation)


def test_v2_file_rejected_by_v1_path_on_version_not_garbage():
    """§9 done-means: 'a v2 file must be rejected by the v1 path on
    `version`, not on garbage.' The v1 `decode()` must reach its version
    check (i.e. magic parses fine) and fail there specifically."""
    v2_data = _good_v2_bytes()
    with pytest.raises(ValueError, match="version"):
        tc.decode(v2_data)


def test_v1_file_rejected_by_v2_path_on_version_not_garbage():
    """Symmetric check: decode_v2() fed a genuine v1 file must fail on
    `version`, not blow up trying to parse v1 bytes as a v2 header/section
    table."""
    elevation = np.zeros((tc.TILE_SIZE, tc.TILE_SIZE), dtype=np.int16)
    climate = np.zeros((tc.CLIMATE_CHANNELS, tc.TILE_SIZE, tc.TILE_SIZE), dtype=np.uint8)
    v1_tile = tc.Tile(seed=1, x=0, y=0, scale=1, elevation=elevation, climate=climate)
    v1_data = tc.encode(v1_tile)
    with pytest.raises(ValueError, match="version"):
        tc.decode_v2(v1_data)


# --------------------------------------------------------- golden fixture ---

@pytest.mark.skipif(not FIXTURE_PATH.exists(), reason=f"fixture not generated: {FIXTURE_PATH}")
def test_v2_golden_fixture_decodes():
    """Regression + cross-language handoff: decodes the fixture handed to
    the C++ agent for parity testing (see the delivery report for the exact
    parameters). Confirms our own encoder/decoder still agree with what was
    committed, and exercises all four block modes in one real file."""
    data = FIXTURE_PATH.read_bytes()
    tile = tc.decode_v2(data)
    assert tile.size == 512
    assert tile.block_log2 == 8
    assert tile.quant == tc.QUANT_100MM
    assert tile.codec == tc.CODEC_RAW

    bs = 1 << tile.block_log2
    nb = tile.size // bs
    # Re-derive each block's mode straight from the wire bytes and confirm
    # all four appear — CONSTANT, CODED/16, CODED/32, RAW.
    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size) for i in range(2)]
    elev_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_ELEV_INDEX)
    modes_seen = set()
    resid_bits_seen = set()
    for i in range(nb * nb):
        entry = tc._BLOCK_ENTRY.unpack_from(data, elev_index_off + i * tc._BLOCK_ENTRY.size)
        modes_seen.add(entry[2])
        if entry[2] == tc.MODE_CODED:
            resid_bits_seen.add(entry[4])
    assert modes_seen == {tc.MODE_CONSTANT, tc.MODE_CODED, tc.MODE_RAW}
    assert resid_bits_seen == {16, 32}


@pytest.mark.skipif(
    not FLOW_FIXTURE_PATH.exists(), reason=f"fixture not generated: {FLOW_FIXTURE_PATH}"
)
def test_v2_golden_flow_fixture_decodes():
    """Companion to test_v2_golden_fixture_decodes: the first golden fixture
    has no flow plane, so §6 was never cross-language exercised. This one
    sets flags bit0 and puts a CONSTANT (all bits set, 0xFF), a CODED, and a
    RAW flow block in one file, with non-zero channel/bank/deposition/log2
    bits throughout — so a decoder that mis-shifts a bit, mis-signs
    const_cp, or reads RAW as 2 bytes/px instead of 1 fails here, not by
    coincidence passing on all-zero data."""
    data = FLOW_FIXTURE_PATH.read_bytes()
    tile = tc.decode_v2(data)
    assert tile.size == 512
    assert tile.block_log2 == 8
    assert tile.flow is not None

    _, _, _, _, _, flags = struct.unpack_from("<BBBBHH", data, tc._HEADER.size)
    assert flags & tc.FLAG_FLOW_PRESENT

    bs = 1 << tile.block_log2
    nb = tile.size // bs
    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size) for i in range(4)]
    flow_index_off = next(o for sid, o, ln in table if sid == tc.SECTION_FLOW_INDEX)

    modes_seen = {}
    for i in range(nb * nb):
        by, bx = divmod(i, nb)
        entry = tc._BLOCK_ENTRY.unpack_from(data, flow_index_off + i * tc._BLOCK_ENTRY.size)
        modes_seen[(bx, by)] = entry
    kinds = {e[2] for e in modes_seen.values()}
    assert kinds == {tc.MODE_CONSTANT, tc.MODE_CODED, tc.MODE_RAW}

    # The CONSTANT block: const_cp must be the unsigned byte 255, not -1.
    constant_entries = [e for e in modes_seen.values() if e[2] == tc.MODE_CONSTANT]
    assert any(e[3] == 255 for e in constant_entries)

    # The RAW block: comp_len must be bs*bs*1 (one byte/px), never bs*bs*2.
    raw_entries = [e for e in modes_seen.values() if e[2] == tc.MODE_RAW]
    assert raw_entries and all(e[1] == bs * bs for e in raw_entries)

    # Every flag bit actually appears somewhere in the decoded plane.
    assert (tile.flow & tc.FLOW_BIT_CHANNEL).any()
    assert (tile.flow & tc.FLOW_BIT_BANK).any()
    assert (tile.flow & tc.FLOW_BIT_DEPOSITION).any()
    assert (tile.flow == 0xFF).any()
    assert (tile.flow & tc.FLOW_LOG2_MASK).max() <= 31
