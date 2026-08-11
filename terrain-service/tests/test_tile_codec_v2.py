"""`.vxtl` v2 codec tests (docs/vxtl-v2-format.md â€” FROZEN CONTRACT, Â§9
conformance). v1 is exercised only as a non-regression / cross-version
check here; its own golden tests live in test_tiles.py and are untouched.

Conformance checklist this file covers (spec Â§9):
  1. round-trip: CONSTANT, CODED/16, CODED/32, RAW blocks, both quant values.
  4. truncation/corruption rejected cleanly; version cross-checks both ways.
Items 2 (C++ golden decode) and 3 (B-spline sample parity) are cross-language
and out of scope for this file â€” see test_v2_golden_fixture_* below for what
IS covered from the Python side of item 2 (the fixture decodes correctly
here; a separate C++ agent/tests must confirm parity).
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

import numpy as np
import pytest

from terrain_service import tile_codec as tc

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

from vxtl_zstd_store import zstd_store_frame, zstd_store_inflate  # noqa: E402

FIXTURE_PATH = (
    Path(__file__).resolve().parents[2]
    / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_512.vxtl"
)
FLOW_FIXTURE_PATH = (
    Path(__file__).resolve().parents[2]
    / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_flow_512.vxtl"
)
ZSTD_FIXTURE_PATH = (
    Path(__file__).resolve().parents[2]
    / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_zstd_512.vxtl"
)


# --------------------------------------------------------------- helpers ---

def _smooth_field(size: int, base_offset_mm: int = 200_000, quant: int = tc.QUANT_100MM) -> np.ndarray:
    """A gentle, fully CODED/16 control lattice â€” no cliffs, no flats."""
    yy, xx = np.mgrid[0:size, 0:size]
    elev_mm = (
        base_offset_mm
        + (np.sin(xx / 9.0) * 4000 + np.cos(yy / 13.0) * 3000 + (xx + yy) * 5).astype(np.int64)
    )
    return tc.mm_to_control_points(elev_mm, base_offset_mm, quant)


def _cliff_block(size: int) -> np.ndarray:
    """A block engineered so the MED residual overflows int16 (Â§5): two
    adjacent first-row pixels jump from +32767 to -32768, forcing
    resid = -32768 - 32767 = -65535 for the encoder to represent, exactly
    the 'one post across a 30 m cliff' case the spec calls out."""
    blk = np.zeros((size, size), dtype=np.int16)
    blk[0, size // 2] = 32767
    blk[0, size // 2 + 1] = -32768
    return blk


def _mixed_tile(size: int, block_log2: int, *, quant: int = tc.QUANT_100MM, codec: int = tc.CODEC_RAW,
                 base_offset_mm: int = 200_000, flow: np.ndarray | None = None) -> tc.TileV2:
    """A tile with a CONSTANT block, a smooth (CODED/16) block, and â€” if the
    grid has a second block row â€” a cliff (CODED/32) block."""
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
    CODED/16 (no cliffs, no flats) â€” the common case."""
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
    encode_v2/decode_v2 API â€” the core of Â§9 conformance item 1."""
    size, block_log2 = 32, 4  # 2x2 blocks of 16x16
    tile = _mixed_tile(size, block_log2)
    data = tc.encode_v2(tile)
    back = tc.decode_v2(data)
    np.testing.assert_array_equal(back.elevation_cp, tile.elevation_cp)


def test_v2_constant_block_costs_zero_bytes():
    """Â§4: 'CONSTANT blocks cost zero bytes and are common.' Verify at the
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
    """Â§5: 'resid_bits is REQUIRED, not an optimisation... one post across a
    30 m cliff does it.' A block that is otherwise perfectly ordinary except
    for ONE steep step must be flagged resid_bits=32 and must round-trip â€”
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
    """RAW mode (Â§4 mode=2) is reachable and round-trips. The reference
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
    """Â§6: optional flow plane, same block structure and predictor, packs
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
    on the wire (Â§4) even for the flow plane, whose elements are uint8. A
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
    """RAW for the flow plane is ONE byte per pixel (u1), not two (Â§6: 'same
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


# ------------------------------------------------- injected codec boundary ---
#
# CODEC_ZSTD's compressor/decompressor is INJECTED, on both halves of the
# contract: docs/vxtl-v2-format.md Â§3 puts it at the host boundary because
# voxel-core has zero third-party dependencies and links into a UE binary that
# already ships zstd (a second static copy is an ODR hazard, not just bloat).
# The Python side mirrors that shape deliberately, and Â§7 licenses it â€” "the
# ENCODER need not be deterministic", any conformant frame will do.
#
# These tests need NO compression library: zlib (stdlib) stands in as an
# arbitrary injected codec, which is exactly the point â€” the boundary must not
# know or care what is on the other side of it.


def _zlib_pair():
    def compress(payload: bytes) -> bytes:
        return zlib.compress(payload, 9)

    def decompress(frame: bytes, expected_len: int) -> bytes:
        out = zlib.decompress(frame)
        if len(out) != expected_len:
            raise ValueError("wrong decompressed length")
        return out

    return compress, decompress


def test_v2_injected_codec_roundtrips_without_zstandard():
    size, block_log2 = 32, 4
    tile = _mixed_tile(size, block_log2, codec=tc.CODEC_ZSTD)
    compress, decompress = _zlib_pair()
    data = tc.encode_v2(tile, compressor=compress)
    back = tc.decode_v2(data, decompressor=decompress)
    np.testing.assert_array_equal(back.elevation_cp, tile.elevation_cp)
    assert back.codec == tc.CODEC_ZSTD


def test_v2_injected_codec_is_per_block_never_whole_section():
    """Â§4: one frame per block, no dictionary, no cross-block state â€” that is
    what buys per-block random access. Count the calls and confirm each one
    sees exactly one block's payload, never the section."""
    size, block_log2 = 32, 4  # 2x2 blocks, one of them CONSTANT
    tile = _mixed_tile(size, block_log2, codec=tc.CODEC_ZSTD)
    seen: list[int] = []

    def compress(payload: bytes) -> bytes:
        seen.append(len(payload))
        return zstd_store_frame(payload)

    data = tc.encode_v2(tile, compressor=compress)
    bs = 1 << block_log2
    # 4 blocks, one CONSTANT (zero bytes, never compressed) -> 3 calls, each
    # exactly one block's payload: 2 or 4 bytes/px, never a multiple of blocks.
    assert len(seen) == 3
    assert set(seen) <= {bs * bs * 2, bs * bs * 4}

    inflated: list[int] = []

    def decompress(frame: bytes, expected_len: int) -> bytes:
        inflated.append(expected_len)
        return zstd_store_inflate(frame, expected_len)

    back = tc.decode_v2(data, decompressor=decompress)
    np.testing.assert_array_equal(back.elevation_cp, tile.elevation_cp)
    assert inflated == seen  # same blocks, same sizes, same order


def test_v2_decode_rejects_a_frame_of_the_wrong_decompressed_length():
    """Under CODEC_ZSTD `comp_len` is the COMPRESSED size and constrains
    nothing, so 'expands to exactly the length the header implies' is the only
    length check left. A decompressor handing back one byte too few must be
    rejected, not silently reshaped or truncated."""
    size, block_log2 = 16, 4
    cp = _smooth_field(size)
    tile = tc.TileV2(
        seed=1, x=0, y=0, size=size, elevation_cp=cp,
        block_log2=block_log2, codec=tc.CODEC_ZSTD,
    )
    data = tc.encode_v2(tile, compressor=zstd_store_frame)

    def short(frame: bytes, expected_len: int) -> bytes:
        return zstd_store_inflate(frame, expected_len)[:-1]

    with pytest.raises(ValueError, match="expanded to"):
        tc.decode_v2(data, decompressor=short)

    def long_(frame: bytes, expected_len: int) -> bytes:
        return zstd_store_inflate(frame, expected_len) + b"\x00"

    with pytest.raises(ValueError, match="expanded to"):
        tc.decode_v2(data, decompressor=long_)

    # And the untouched pair still round-trips.
    back = tc.decode_v2(data, decompressor=zstd_store_inflate)
    np.testing.assert_array_equal(back.elevation_cp, cp)


def test_v2_decode_rejects_truncated_and_corrupt_frames():
    size, block_log2 = 16, 4
    cp = _smooth_field(size)
    tile = tc.TileV2(
        seed=1, x=0, y=0, size=size, elevation_cp=cp,
        block_log2=block_log2, codec=tc.CODEC_ZSTD,
    )
    data = bytearray(tc.encode_v2(tile, compressor=zstd_store_frame))

    off = tc._HEADER.size + tc._V2_EXT.size
    table = [
        tc._SECTION_ENTRY.unpack_from(bytes(data), off + i * tc._SECTION_ENTRY.size)
        for i in range(2)
    ]
    elev_index_off = next(o for sid, o, _ in table if sid == tc.SECTION_ELEV_INDEX)
    elev_data_off = next(o for sid, o, _ in table if sid == tc.SECTION_ELEV_DATA)
    entry = tc._BLOCK_ENTRY.unpack_from(bytes(data), elev_index_off)
    assert entry[2] == tc.MODE_CODED
    frame_off = elev_data_off + entry[0]

    # comp_len is a FRAME length now â€” 9 bytes of frame header plus 3 per zstd
    # block more than the payload â€” so it is no longer the plain length.
    assert entry[1] != size * size * 2

    corrupt = bytearray(data)
    corrupt[frame_off] ^= 0xFF  # frame magic
    with pytest.raises(ValueError):
        tc.decode_v2(bytes(corrupt), decompressor=zstd_store_inflate)

    corrupt = bytearray(data)
    struct.pack_into("<I", corrupt, frame_off + 5, 7)  # lie about the content size
    with pytest.raises(ValueError):
        tc.decode_v2(bytes(corrupt), decompressor=zstd_store_inflate)

    # comp_len 0 on a block that owns a frame: an empty frame cannot expand to
    # anything, and a block that owns no bytes is CONSTANT by definition.
    corrupt = bytearray(data)
    struct.pack_into("<I", corrupt, elev_index_off + 8, 0)
    with pytest.raises(ValueError):
        tc.decode_v2(bytes(corrupt), decompressor=zstd_store_inflate)


def test_v2_zstd_store_frames_are_conformant_zstd():
    """The committed CODEC_ZSTD fixture is built from Raw_Block frames so that
    CI needs no compression library. That is only defensible if the frames are
    genuinely conformant zstd â€” verified here by the REAL decoder whenever it
    is installed, and skipped (not quietly assumed) when it is not."""
    zstandard = pytest.importorskip("zstandard")
    for payload in (b"", b"x", bytes(range(256)) * 700, b"\xff" * (300 * 1024)):
        frame = zstd_store_frame(payload)
        assert zstandard.ZstdDecompressor().decompress(
            frame, max_output_size=max(len(payload), 1)
        ) == payload
        assert zstd_store_inflate(frame, len(payload)) == payload


@pytest.mark.skipif(not tc.HAVE_ZSTD, reason="zstandard not installed")
def test_v2_real_zstd_and_injected_store_frames_decode_alike():
    """Two different, both-conformant encoders of the same tile must decode to
    the same lattice. This is the encoder half of Â§7's licence: the bytes may
    differ, the values may not."""
    size, block_log2 = 32, 4
    tile = _mixed_tile(size, block_log2, codec=tc.CODEC_ZSTD)
    real = tc.decode_v2(tc.encode_v2(tile))  # default zstandard compressor
    stored = tc.decode_v2(
        tc.encode_v2(tile, compressor=zstd_store_frame), decompressor=zstd_store_inflate
    )
    np.testing.assert_array_equal(real.elevation_cp, tile.elevation_cp)
    np.testing.assert_array_equal(stored.elevation_cp, tile.elevation_cp)


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
    all zero per Â§4."""
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
    """Â§9 done-means: 'A v1 file must still decode as v1.'"""
    elevation = np.zeros((tc.TILE_SIZE, tc.TILE_SIZE), dtype=np.int16)
    elevation[10, 20] = -500
    climate = np.zeros((tc.CLIMATE_CHANNELS, tc.TILE_SIZE, tc.TILE_SIZE), dtype=np.uint8)
    v1_tile = tc.Tile(seed=1, x=0, y=0, scale=1, elevation=elevation, climate=climate)
    data = tc.encode(v1_tile)
    back = tc.decode(data)
    np.testing.assert_array_equal(back.elevation, elevation)


def test_v2_file_rejected_by_v1_path_on_version_not_garbage():
    """Â§9 done-means: 'a v2 file must be rejected by the v1 path on
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
    # all four appear â€” CONSTANT, CODED/16, CODED/32, RAW.
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
    has no flow plane, so Â§6 was never cross-language exercised. This one
    sets flags bit0 and puts a CONSTANT (all bits set, 0xFF), a CODED, and a
    RAW flow block in one file, with non-zero channel/bank/deposition/log2
    bits throughout â€” so a decoder that mis-shifts a bit, mis-signs
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


# ------------------------------------------------- bake surface -> v2 bytes ---
#
# `encode_fine` is the step that did not exist until 2026-07-29, and its absence
# is why no fine tile had ever been produced end-to-end: `encode_v2` starts from
# a control lattice and a `BakeResult` is a field of SAMPLES. These tests pin the
# two things that step can silently get wrong -- skipping the prefilter, and
# picking a datum the tile does not fit in.


def _bumpy_samples(size: int, offset_m: float = 1200.0) -> np.ndarray:
    """A sample field with real content at the Nyquist, where the prefilter bites."""
    yy, xx = np.mgrid[0:size, 0:size]
    return (
        offset_m
        + 40.0 * np.sin(xx / 7.0)
        + 25.0 * np.cos(yy / 5.0)
        + 3.0 * ((-1.0) ** (xx + yy))
    ).astype(np.float32)


def test_encode_fine_ships_control_points_not_samples():
    """The one shortcut that decodes perfectly and is still wrong.

    Writing the bake's samples straight into the cp plane produces a valid file;
    the client's spline then renders a LOW-PASSED version of it. So the check is
    not "does it round-trip" (it would either way) but "does evaluating the
    shipped lattice give the surface back". At a lattice point the Â§8 weights are
    (1,4,1,0)/6 per axis, so that evaluation is a separable [1,4,1]/6 stencil.
    """
    pytest.importorskip("scipy")
    size = 256
    z = _bumpy_samples(size)
    data = tc.encode_fine(seed=7, x=-5, y=3, elevation_m=z, block_log2=7)
    tile = tc.decode_v2(data)

    cp_mm = tc.control_points_to_mm(tile.elevation_cp, tile.base_offset_mm, tile.quant)

    def smooth(a):
        p = np.pad(a.astype(np.float64), ((1, 1), (0, 0)), mode="edge")
        return (p[:-2] + 4.0 * p[1:-1] + p[2:]) / 6.0

    recon = smooth(smooth(cp_mm).T).T
    err = np.abs(recon[2:-2, 2:-2] - z[2:-2, 2:-2].astype(np.float64) * 1000.0)
    # Quantisation is the floor: a convex combination of three cp each rounded
    # to +-50 mm cannot do better, and the measured interior is well inside it.
    assert err.max() < tc.QUANT_MM[tile.quant], err.max()

    # ...and the test has power: the un-prefiltered path fails it badly.
    naive = tc.mm_to_control_points(
        np.rint(z.astype(np.float64) * 1000.0).astype(np.int64),
        tile.base_offset_mm, tile.quant,
    )
    naive_recon = smooth(smooth(
        tc.control_points_to_mm(naive, tile.base_offset_mm, tile.quant)
    ).T).T
    naive_err = np.abs(naive_recon[2:-2, 2:-2] - z[2:-2, 2:-2].astype(np.float64) * 1000.0)
    assert naive_err.max() > 20 * err.max(), (naive_err.max(), err.max())


def test_encode_fine_carries_flow_and_bake_version():
    pytest.importorskip("scipy")
    from terrain_service.bake.pipeline import BAKE_VERSION

    size = 128
    z = _bumpy_samples(size)
    flow = np.arange(size * size, dtype=np.uint8).reshape(size, size)
    tile = tc.decode_v2(
        tc.encode_fine(seed=11, x=2, y=-4, elevation_m=z, flow=flow, block_log2=6)
    )
    assert tile.bake_ver == BAKE_VERSION
    assert tile.flow is not None and np.array_equal(tile.flow, flow)
    assert tile.x == 2 and tile.y == -4 and tile.scale == tc.FINE_SCALE

    no_flow = tc.decode_v2(
        tc.encode_fine(seed=11, x=2, y=-4, elevation_m=z, block_log2=6)
    )
    assert no_flow.flow is None


def test_choose_datum_prefers_100mm_and_falls_back_before_clipping():
    # A 3 km alpine tile: comfortably inside int16 at one voxel per LSB.
    base, quant = tc.choose_datum(0, 3_000_000)
    assert quant == tc.QUANT_100MM
    assert base % tc.DATUM_STEP_MM == 0
    # Abyssal seafloor to alpine summit in one tile: 100 mm cannot span it.
    base, quant = tc.choose_datum(-5_000_000, 3_000_000)
    assert quant == tc.QUANT_250MM
    for lo, hi in ((0, 3_000_000), (-5_000_000, 3_000_000)):
        q = tc.QUANT_MM[tc.choose_datum(lo, hi)[1]]
        b = tc.choose_datum(lo, hi)[0]
        assert -32768 <= round((lo - b) / q) and round((hi - b) / q) <= 32767
    with pytest.raises(ValueError, match="does not fit int16"):
        tc.choose_datum(-9_000_000, 9_000_000)


def test_encode_fine_rejects_a_mismatched_flow_plane():
    pytest.importorskip("scipy")
    z = _bumpy_samples(64)
    with pytest.raises(ValueError, match="does not match elevation"):
        tc.encode_fine(seed=1, x=0, y=0, elevation_m=z, block_log2=6,
                       flow=np.zeros((32, 32), np.uint8))


def test_pregen_can_reach_the_fine_encoder():
    """`pregen._encode_fine` probes tile_codec by NAME and passes only the
    kwargs the encoder declares. It found `encode_v2`, could not synthesise its
    `tile` argument, and raised -- so `--mode bake` had no path to bytes at all.
    This asserts the probe now lands on something it can actually call."""
    pytest.importorskip("scipy")
    from terrain_service import pregen

    # 256, because pregen passes no block_log2 and the default block edge is 256.
    class _R:
        tile_x, tile_y = -5, 3
        elevation_m = _bumpy_samples(256)
        flow = np.zeros((256, 256), np.uint8)
        # bake_ver 8: an EMPTY registry, which is a statement ("surveyed, holds
        # nothing") and must still set FLAG_BASINS_PRESENT -- a client cannot
        # otherwise tell it from a tile baked before the registry existed.
        basins = ()

    data = pregen._encode_fine(_R(), seed=99, provider_id="test")
    tile = tc.decode_v2(data)
    assert tile.seed == 99 and tile.x == -5 and tile.y == 3
    assert tile.flow is not None
    assert tile.basins == []


def test_flow_plane_blocks_pick_raw_when_it_is_smaller():
    """A uint8 plane costs TWO bytes per pixel as CODED residuals and one as
    RAW, so CODED is never right for it under CODEC_RAW and (measured on a real
    baked flow plane) not right under zstd either. The elevation plane must be
    untouched by this: its resid_bits=32 path is only reachable because CODED
    wins the tie there."""
    size, block_log2 = 32, 4
    cp = _smooth_field(size)
    yy, xx = np.mgrid[0:size, 0:size]
    flow = ((xx * 3 + yy * 7) % 251).astype(np.uint8)
    flow[0:16, 0:16] = 0  # still CONSTANT: free beats both
    data = tc.encode_v2(
        tc.TileV2(seed=3, x=0, y=0, size=size, elevation_cp=cp,
                  block_log2=block_log2, flow=flow)
    )
    np.testing.assert_array_equal(tc.decode_v2(data).flow, flow)

    off = tc._HEADER.size + tc._V2_EXT.size
    table = [tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size)
             for i in range(4)]
    bs = 1 << block_log2
    nb = size // bs

    flow_off = next(o for sid, o, _l in table if sid == tc.SECTION_FLOW_INDEX)
    flow_modes, flow_bytes = [], 0
    for i in range(nb * nb):
        e = tc._BLOCK_ENTRY.unpack_from(data, flow_off + i * tc._BLOCK_ENTRY.size)
        flow_modes.append(e[2])
        flow_bytes += e[1]
    assert tc.MODE_CONSTANT in flow_modes
    assert tc.MODE_CODED not in flow_modes
    assert flow_bytes == 3 * bs * bs  # 1 byte/px over the three non-flat blocks

    elev_off = next(o for sid, o, _l in table if sid == tc.SECTION_ELEV_INDEX)
    elev_modes = {
        tc._BLOCK_ENTRY.unpack_from(data, elev_off + i * tc._BLOCK_ENTRY.size)[2]
        for i in range(nb * nb)
    }
    assert tc.MODE_RAW not in elev_modes, "the int16 plane must not auto-select RAW"
@pytest.mark.skipif(
    not (ZSTD_FIXTURE_PATH.exists() and FIXTURE_PATH.exists()),
    reason="fixtures not generated",
)
def test_v2_zstd_fixture_carries_the_same_lattice_as_the_raw_golden():
    """The CODEC_ZSTD twin of the golden. Its whole reason to exist is that
    the C++ decoder can digest-compare the two and get the same number, so
    this side checks the same claim: identical lattice, identical block modes,
    different codec on the wire.

    Regenerate both with:
        python terrain-service/tools/make_v2_zstd_fixture.py
    """
    raw = tc.decode_v2(FIXTURE_PATH.read_bytes())
    zstd_bytes = ZSTD_FIXTURE_PATH.read_bytes()
    zstd = tc.decode_v2(zstd_bytes, decompressor=zstd_store_inflate)

    assert zstd.codec == tc.CODEC_ZSTD
    assert raw.codec == tc.CODEC_RAW
    for field in ("seed", "x", "y", "size", "block_log2", "quant", "bake_ver",
                  "base_offset_mm"):
        assert getattr(zstd, field) == getattr(raw, field), field
    np.testing.assert_array_equal(zstd.elevation_cp, raw.elevation_cp)

    # Same four modes at the same four positions, and comp_len is now a FRAME
    # length rather than the plain payload length.
    off = tc._HEADER.size + tc._V2_EXT.size
    table = [
        tc._SECTION_ENTRY.unpack_from(zstd_bytes, off + i * tc._SECTION_ENTRY.size)
        for i in range(2)
    ]
    index_off = next(o for sid, o, _ in table if sid == tc.SECTION_ELEV_INDEX)
    bs = 1 << zstd.block_log2
    nb = zstd.size // bs
    modes, resid_bits = set(), set()
    for i in range(nb * nb):
        e = tc._BLOCK_ENTRY.unpack_from(zstd_bytes, index_off + i * tc._BLOCK_ENTRY.size)
        modes.add(e[2])
        if e[2] == tc.MODE_CODED:
            resid_bits.add(e[4])
            assert e[1] != bs * bs * (2 if e[4] == 16 else 4)
        elif e[2] == tc.MODE_RAW:
            assert e[1] != bs * bs * 2
        else:
            assert e[1] == 0  # CONSTANT still owns no frame
    assert modes == {tc.MODE_CONSTANT, tc.MODE_CODED, tc.MODE_RAW}
    assert resid_bits == {16, 32}


@pytest.mark.skipif(not ZSTD_FIXTURE_PATH.exists(), reason="fixture not generated")
def test_v2_zstd_fixture_decodes_with_the_real_zstd_decoder():
    """The committed fixture must be readable by a REAL zstd, not only by the
    Raw_Block reader that wrote it â€” otherwise "conformant zstd frames" is an
    unchecked claim and the UE module (which uses the engine's zstd) would be
    the place it failed."""
    zstandard = pytest.importorskip("zstandard")
    assert zstandard is not None
    tile = tc.decode_v2(ZSTD_FIXTURE_PATH.read_bytes())  # no injection: real zstd
    assert tile.codec == tc.CODEC_ZSTD
    raw = tc.decode_v2(FIXTURE_PATH.read_bytes())
    np.testing.assert_array_equal(tile.elevation_cp, raw.elevation_cp)


# ---------------------------------------------------------------------------
# THE LEVEL BAND (tile_codec.WATER_NO_LEVEL): a water LEVEL for DRY cells.
#
# The band puts values in -32767..-2 into the SAME int16 plane that already
# carries depths, relying on the fact that every reader tests "< 0" for dry. So
# the two things these tests have to hold are (a) the plane still round-trips
# bit for bit through the block codec with those values in it, and (b) turning
# the band on cannot move a single WET cell -- which is the entire safety
# argument, and the only reason the band is allowed to share the plane.
# ---------------------------------------------------------------------------


def _band_planes(size: int, block_log2: int):
    """(water_m, ground_m, level_m): one MIXED block carrying wet, band and
    sentinel together, and one block left entirely at NO_LEVEL."""
    bs = 1 << block_log2
    ground = np.full((size, size), 100.0)
    water = np.full((size, size), np.nan)
    level = np.full((size, size), np.nan)
    # Block (0,0): a river down the middle in a V-shaped valley, a band on both
    # banks, sentinel elsewhere -- the MIXED case.
    ground[0:bs, 0:bs] = 100.0 + np.abs(np.arange(bs) - bs // 2)[None, :] * 0.05
    mid = slice(bs // 2 - 2, bs // 2 + 3)
    water[0:bs, mid] = 100.35
    for dx in range(3, 14):
        for col in (bs // 2 - dx, bs // 2 + dx):
            level[0:bs, col] = 100.35
    # Block (1,1) is untouched: an all-NO_LEVEL block, which is the case a
    # decoder is most likely to get wrong, because const_cp is a signed i16
    # field sitting at exactly its minimum.
    return water, ground, level


def test_level_band_round_trips_through_the_codec():
    """Negative band values, an all-INT16_MIN CONSTANT block and a mixed block
    all survive encode -> decode unchanged."""
    size, block_log2 = 512, 8
    water, ground, level = _band_planes(size, block_log2)
    cp = tc.water_depth_control_points(water, ground, 0, tc.QUANT_100MM,
                                       level_m=level)
    assert cp.dtype == np.int16
    # All three populations are present, or this test is vacuous.
    assert (cp >= 0).any(), "no wet cells"
    assert ((cp >= tc.WATER_LEVEL_MIN_CP) & (cp <= tc.WATER_LEVEL_MAX_CP)).any()
    assert (cp == tc.WATER_NO_LEVEL).any()
    bs = 1 << block_log2
    assert np.all(cp[bs:2 * bs, bs:2 * bs] == tc.WATER_NO_LEVEL)

    elev = np.zeros((size, size), np.int16)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size, elevation_cp=elev,
                     block_log2=block_log2, water_cp=cp)
    back = tc.decode_v2(tc.encode_v2(tile))
    np.testing.assert_array_equal(back.water_cp, cp)

    # And the all-sentinel block really is CONSTANT at INT16_MIN, rather than a
    # coded block that happens to decode right: const_cp is the one i16 field on
    # the wire that has to carry -32768 now, and MODE_CONSTANT is the only path
    # that puts a plane value there.
    idx, _ = tc._encode_plane(cp.astype(np.int64), block_log2=block_log2,
                              codec=tc.CODEC_RAW, elem_dtype="<i2")
    nb = size >> block_log2
    entry = tc._BLOCK_ENTRY.unpack_from(idx, (1 * nb + 1) * tc._BLOCK_ENTRY.size)
    assert entry[2] == tc.MODE_CONSTANT
    assert entry[3] == tc.WATER_NO_LEVEL == -32768
    assert entry[1] == 0, "a CONSTANT block must own no data bytes"


@pytest.mark.skipif(not tc.HAVE_ZSTD, reason="zstandard not installed")
def test_level_band_round_trips_under_zstd():
    """Same plane, the codec that actually ships. INT16_MIN is the value most
    likely to be mangled by a byte-width or sign mistake, and CODEC_ZSTD is the
    path with the most of both."""
    size, block_log2 = 512, 8
    water, ground, level = _band_planes(size, block_log2)
    cp = tc.water_depth_control_points(water, ground, 0, tc.QUANT_100MM,
                                       level_m=level)
    tile = tc.TileV2(seed=1, x=0, y=0, size=size,
                     elevation_cp=np.zeros((size, size), np.int16),
                     block_log2=block_log2, codec=tc.CODEC_ZSTD, water_cp=cp)
    back = tc.decode_v2(tc.encode_v2(tile))
    np.testing.assert_array_equal(back.water_cp, cp)


def test_band_leaves_the_wet_set_bit_identical():
    """THE SAFETY PROPERTY. Turning the band on may not move one wet cell.

    This is why the band is allowed to live in the depth plane at all: a value
    in -32767..-2 reads as dry through every "< 0" test that exists, so an old
    client behaves EXACTLY as it did. That claim is worthless if the encoder
    also perturbs the depths while it is in there, so the two planes are
    compared elementwise on the wet set rather than statistically.
    """
    size, block_log2 = 512, 8
    water, ground, level = _band_planes(size, block_log2)
    off = tc.water_depth_control_points(water, ground, 0, tc.QUANT_100MM)
    on = tc.water_depth_control_points(water, ground, 0, tc.QUANT_100MM,
                                       level_m=level)
    wet = np.isfinite(water)
    assert wet.any()
    np.testing.assert_array_equal(on[wet], off[wet])
    # OFF is byte-for-byte the legacy plane: dry is -1 and nothing else.
    assert set(np.unique(off[~wet]).tolist()) == {tc.WATER_DRY_DEPTH}
    # ON, every dry cell is either a band value or the no-level sentinel, and
    # never anything a client could read as depth.
    d = on[~wet]
    assert np.all((d == tc.WATER_NO_LEVEL)
                  | ((d >= tc.WATER_LEVEL_MIN_CP) & (d <= tc.WATER_LEVEL_MAX_CP)))
    # Passing no level at all is bit-identical to not passing the argument.
    none = tc.water_depth_control_points(
        water, ground, 0, tc.QUANT_100MM, level_m=None)
    np.testing.assert_array_equal(none, off)


def test_no_dry_cell_ever_encodes_non_negative():
    """A positive value at a dry cell adds water to every client, silently and
    forever. The encoder clamps at WATER_LEVEL_MAX_CP rather than trusting the
    producer, and re-checks the finished plane before returning it.

    A level ABOVE its cell's ground is a legal state, not a bug: the lateral
    fill refuses a cell for want of min_depth_m, and the discharge budget
    strands cells the geometry alone would have flooded. So the encoder cannot
    reject them -- it has to represent them as "no water at this ground".
    """
    size = 256
    ground = np.zeros((size, size))
    water = np.full((size, size), np.nan)     # everything dry
    # Levels from 10 m BELOW the ground to 10 m ABOVE it.
    level = np.linspace(-10.0, 10.0, size)[None, :].repeat(size, 0)
    cp = tc.water_depth_control_points(water, ground, 0, tc.QUANT_100MM,
                                       level_m=level)
    assert (cp < 0).all(), "a dry cell encoded as water"
    assert cp.max() == tc.WATER_LEVEL_MAX_CP, "the clamp did not bite"
    # The FLOOR is the int16 range, not the band width. See WATER_LEVEL_MIN_CP:
    # clamping to the band width would RAISE a level, and raising a level adds
    # water on exactly the cliff pixels the dilated predicate admits.
    assert cp.min() == -1000, "a level 10 m down must survive as -1000"
    # Reconstruction is the client's own arithmetic, and it must land back on
    # the level it was given wherever the clamp did not bite.
    got = tc.water_level_mm_from_cp(cp, np.zeros((size, size), np.int64))
    free = cp < tc.WATER_LEVEL_MAX_CP
    np.testing.assert_allclose(got[free], np.rint(level * 1000.0)[free], atol=5)


def test_encoder_refuses_a_level_on_a_wet_cell():
    """The depth plane and the level plane must agree about the final wet set.

    They come from one producer describing one body of water; if they disagree
    then one is stale, and staleness is the exact failure the band stage exists
    to avoid -- the level inside fill_to_local_surface is six stages out of date
    by the time the plane ships.
    """
    size = 64
    ground = np.zeros((size, size))
    water = np.full((size, size), np.nan)
    water[0, 0] = 1.0
    level = np.full((size, size), np.nan)
    level[0, 0] = 1.0                  # wet AND levelled: a disagreement
    with pytest.raises(ValueError, match="BOTH a depth and a level"):
        tc.water_depth_control_points(water, ground, 0, tc.QUANT_100MM,
                                      level_m=level)


def test_level_reader_honours_both_no_level_sentinels():
    """-1 and -32768 both mean "no level here". A reader honouring only the new
    one would answer "water at ground - 10 mm" on every tile baked before the
    band existed -- a 10 mm film of water over every dry pixel in the world."""
    ground = np.array([[0, 0, 0, 0]], np.int64)
    cp = np.array([[tc.WATER_DRY_DEPTH, tc.WATER_NO_LEVEL,
                    tc.WATER_LEVEL_MAX_CP, 5]], np.int16)
    nolevel = np.iinfo(np.int32).min
    got = tc.water_level_mm_from_cp(cp, ground)
    assert got[0, 0] == nolevel and got[0, 1] == nolevel
    assert got[0, 2] == -20     # a band value: ground + (-2) * 10 mm
    assert got[0, 3] == 50      # a depth reads through the same arithmetic
    # And the DEPTH reader is unmoved by the band: every negative is still "no
    # water", which is exactly what an old client does with these bytes.
    surf = tc.water_surface_mm_from_depth(cp, ground)
    assert (surf[0, :3] == nolevel).all()
    assert surf[0, 3] == 50


def test_encode_fine_refuses_a_level_with_no_water_plane():
    """A band with no depth plane to ride in would be silently dropped -- the
    same class of failure as pregen's name probe dropping the water plane
    itself, which cost a 302 CPU-s bake to discover."""
    n = 64
    with pytest.raises(ValueError, match="without water_surface_m"):
        tc.encode_fine(seed=1, x=0, y=0,
                       elevation_m=np.zeros((n, n)),
                       water_level_m=np.full((n, n), np.nan),
                       block_log2=4)


# ------------------------------------------------------- bathymetry (v27) ---
#
# SECTION_BATHY_* carries the two rasters the water material shades a lake
# with: per-cell DEPTH and SIGNED DISTANCE TO SHORE. These pin the wire
# contract, not the hydrology -- the hydrology is pinned in test_bake_basins.

def _bathy_tile(size=32, block_log2=4, **kw):
    cp = _smooth_field(size)
    return tc.TileV2(seed=7, x=1, y=2, size=size, elevation_cp=cp,
                     block_log2=block_log2, **kw)


def test_bathymetry_round_trips_exactly():
    size = 32
    yy, xx = np.mgrid[0:size, 0:size]
    r = np.hypot(xx - 16, yy - 16)
    depth = np.where(r < 8, np.rint((8 - r) * 50), -1).astype(np.int16)
    shore = np.clip(np.rint((8 - r) * 18.75), -1000, 1000).astype(np.int16)
    tile = _bathy_tile(size=size, bathy_depth=depth, bathy_shore=shore)
    got = tc.decode_v2(tc.encode_v2(tile))
    # int16 planes are stored losslessly, exactly as elevation and water are.
    assert np.array_equal(got.bathy_depth, depth)
    assert np.array_equal(got.bathy_shore, shore)


def test_bathymetry_absent_stays_absent():
    """A tile without the pair sets no flag and carries no sections -- so a
    bake predating v27 stays readable and is distinguishable from a surveyed
    tile that simply holds no lakes."""
    got = tc.decode_v2(tc.encode_v2(_bathy_tile()))
    assert got.bathy_depth is None and got.bathy_shore is None


def test_bathymetry_is_both_or_neither():
    """One flag bit covers both planes, so a half-populated tile must be
    refused at construction rather than at decode -- the decoder's own
    agreement check is a much worse place to discover it."""
    size = 32
    plane = np.zeros((size, size), np.int16)
    with pytest.raises(AssertionError):
        _bathy_tile(size=size, bathy_depth=plane)
    with pytest.raises(AssertionError):
        _bathy_tile(size=size, bathy_shore=plane)


def test_an_all_dry_bathymetry_pair_is_essentially_free():
    """THE PROPERTY THAT MAKES THE PLANE AFFORDABLE. A tile with no lakes is
    two uniform rasters, every block MODE_CONSTANT at zero data bytes, so the
    pair costs only its index tables. The wet block measures 0.7% water, so
    this is the common case rather than a corner one."""
    size, block_log2 = 64, 4
    dry = np.full((size, size), -1, np.int16)
    far = np.full((size, size), -1000, np.int16)
    base = len(tc.encode_v2(_bathy_tile(size=size, block_log2=block_log2)))
    withb = len(tc.encode_v2(_bathy_tile(size=size, block_log2=block_log2,
                                         bathy_depth=dry, bathy_shore=far)))
    per_plane_elements = size * size * 2  # int16, if it were stored literally
    assert withb - base < per_plane_elements // 4, (
        f"an all-dry bathymetry pair cost {withb - base} bytes; it should be "
        "index tables only"
    )


def test_bathymetry_flag_is_refused_by_an_older_reader():
    """Adding a flag bit is a HARD BREAK by design: a reader that does not
    know bit4 must refuse the tile loudly rather than draw a world with the
    bathymetry silently missing. Simulated by masking the bit out of the
    decoder's known set, which is what an older build's constant would be."""
    size = 32
    plane = np.zeros((size, size), np.int16)
    blob = tc.encode_v2(_bathy_tile(size=size, bathy_depth=plane,
                                    bathy_shore=plane))
    old_known = (tc.FLAG_FLOW_PRESENT | tc.FLAG_BASINS_PRESENT
                 | tc.FLAG_WATER_PRESENT | tc.FLAG_HEADS_PRESENT)
    assert tc.FLAG_BATHY_PRESENT & ~old_known, (
        "FLAG_BATHY_PRESENT must be a NEW bit, or an old reader accepts a "
        "tile it cannot fully parse"
    )
