"""Binary tile format (plan §3.1 step 2, §5 task 1).

Tiles are canonical DATA (doctrine §2.3): generated once server-side,
cached forever, distributed to clients. 512x512 pixels per tile:

    elevation: int16 metres, signed, z=0 is sea level
    climate:   uint8 x 4 planes (temperature, seasonality,
               precipitation, precip variability)

Wire format (little-endian throughout):

    magic    4B  b"VXTL"
    version  u16 (1)
    seed     u64
    x, y     i32 tile coords (tile (0,0) covers pixels [0,512) on each axis)
    scale    u8  (1 => 30m/px, 8 => 3.75m/px, terrain-diffusion convention:
                 scale is a SUPERSAMPLE factor, so scale 8 = 30m / 8)
    size     u16 (512)
    elevation int16[size*size], row-major (y outer)
    climate   uint8[4][size*size]
"""

from __future__ import annotations

import struct
from collections.abc import Callable
from dataclasses import dataclass

import numpy as np

MAGIC = b"VXTL"
VERSION = 1
TILE_SIZE = 512
CLIMATE_CHANNELS = 4
_HEADER = struct.Struct("<4sHQiiBH")

#: mm per pixel by scale — must match voxel-core's ITileSampler::pixelSizeMm.
#: MIRROR: keep byte-identical with voxel-core/include/voxelcore/tiles.h and
#: tilestore.h (tilePixelSizeMm). scale is a supersample factor on the pinned
#: 30 m checkpoint, so scale 8 = 30 m / 8 = 3.75 m/px = 3750 mm. (The old 11250
#: was 90 m / 8, from the superseded 90 m model — wrong by 3x for the 30 m
#: checkpoint. Nothing was ever generated at scale 8, so no cached tile is
#: affected; this rolls provider_id via _tile_format_fingerprint, as intended.)
PIXEL_SIZE_MM = {1: 30000, 8: 3750, 16: 1875}


@dataclass
class Tile:
    seed: int
    x: int
    y: int
    scale: int
    elevation: np.ndarray  # (TILE_SIZE, TILE_SIZE) int16
    climate: np.ndarray  # (CLIMATE_CHANNELS, TILE_SIZE, TILE_SIZE) uint8

    def __post_init__(self) -> None:
        assert self.scale in PIXEL_SIZE_MM, f"unsupported scale {self.scale}"
        assert self.elevation.shape == (TILE_SIZE, TILE_SIZE)
        assert self.elevation.dtype == np.int16
        assert self.climate.shape == (CLIMATE_CHANNELS, TILE_SIZE, TILE_SIZE)
        assert self.climate.dtype == np.uint8


def encode(tile: Tile) -> bytes:
    header = _HEADER.pack(
        MAGIC, VERSION, tile.seed, tile.x, tile.y, tile.scale, TILE_SIZE
    )
    return (
        header
        + tile.elevation.astype("<i2").tobytes()
        + tile.climate.tobytes()
    )


def decode(data: bytes) -> Tile:
    magic, version, seed, x, y, scale, size = _HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError("bad magic")
    if version != VERSION:
        raise ValueError(f"unsupported tile version {version}")
    if size != TILE_SIZE:
        raise ValueError(f"unsupported tile size {size}")
    offset = _HEADER.size
    n = size * size
    elevation = np.frombuffer(data, dtype="<i2", count=n, offset=offset)
    offset += 2 * n
    climate = np.frombuffer(
        data, dtype=np.uint8, count=CLIMATE_CHANNELS * n, offset=offset
    )
    if offset + CLIMATE_CHANNELS * n != len(data):
        raise ValueError("trailing bytes")
    return Tile(
        seed=seed,
        x=x,
        y=y,
        scale=scale,
        elevation=elevation.reshape(size, size).astype(np.int16),
        climate=climate.reshape(CLIMATE_CHANNELS, size, size).copy(),
    )


# ---------------------------------------------------------------------------
# v2: baked fine-tier format (docs/vxtl-v2-format.md — FROZEN CONTRACT).
#
# Everything above this line is v1 and is UNTOUCHED (byte-identical, still
# shipped, still golden-tested). This section is purely additive: it shares
# only the constants MAGIC and the v1 header struct `_HEADER`, reused
# verbatim because the spec requires v2's first 25 bytes to be positionally
# identical to v1's, so a v1 parser fails on `version`, not on garbage.
#
# The C++ decoder (voxel-core/src/tilestore.cpp) is being written in
# parallel against the same spec document. Where the spec text left a
# reading open, the choice made here is called out in the comments below
# and repeated in the delivery report — treat any silent divergence from
# docs/vxtl-v2-format.md as a bug in this file, not a design decision.
# ---------------------------------------------------------------------------

VERSION_V2 = 2
FINE_SCALE = 16
DEFAULT_FINE_SIZE = 8192          # 8192x8192 @ 1.875 m/px (scale 16), §1
DEFAULT_BLOCK_LOG2 = 8            # 256x256 px/block -> 32x32 = 1024 blocks, §3

PRED_MED = 1  # the only predictor v2 defines (§5)

QUANT_100MM = 1
QUANT_250MM = 2
QUANT_MM = {QUANT_100MM: 100, QUANT_250MM: 250}  # §3 quant -> LSB mm

CODEC_RAW = 0
CODEC_ZSTD = 1

MODE_CONSTANT = 0
MODE_CODED = 1
# RAW (§4): the block's literal plane elements, row-major, native byte width
# for that plane — <i2 LE for elevation (2 bytes/px), u1 for flow (1 byte/px)
# — with NO prediction and NO zigzag applied. resid_bits is meaningless in
# this mode and is always written 0 (§4/§5). Under CODEC_RAW a RAW elevation
# block and a resid_bits=16 CODED elevation block have the IDENTICAL byte
# length (both 2 bytes/px) — comp_len alone can't distinguish them, only
# `mode` can, so a decoder must branch on mode before doing anything else
# with the payload.
MODE_RAW = 2

SECTION_ELEV_INDEX = 1
SECTION_ELEV_DATA = 2
SECTION_FLOW_INDEX = 3
SECTION_FLOW_DATA = 4
#: The per-tile basin registry (docs/watershed-system-plan.md P1). A tiny
#: flat table, not a plane: tens of rows at 32 B, so a tile grows by ~1 KB
#: against 26.6 MB of compressed elevation.
SECTION_BASIN_TABLE = 5

FLAG_FLOW_PRESENT = 1 << 0
FLAG_BASINS_PRESENT = 1 << 1

# --- SECTION_BASIN_TABLE layout (watershed plan P1) -------------------------
#
# A LENGTH-PREFIXED, VERSIONED table rather than a bare array of structs. The
# section table already gives the payload's byte length, so `count` is
# redundant with it -- and that redundancy is the point: a decoder that
# disagrees with the encoder about the entry size gets a mismatch it can
# refuse, instead of reading 33-byte records out of a 32-byte stream and
# producing plausible garbage. `entry_bytes` is what lets a later revision add
# a field without a third section id.
#
#   u16 table_version, u16 entry_bytes, u32 count, then count*entry_bytes
BASIN_TABLE_VERSION = 1
_BASIN_TABLE_HEADER = struct.Struct("<HHI")

# One basin row, 32 bytes:
#   basin_id u16 | seed_px 2*u16 | bbox_px 4*u16 | outlet_px 2*u16
#   spill_mm i32 | surface_mm i32 | kind u8 | reserved u8[5]
#
# Pixel coordinates are TILE-INTERIOR pixels in the same space as the
# elevation plane, so a client indexes them with no transform. Elevations are
# millimetres, the unit the rest of the format uses, absolute (NOT relative to
# base_offset_mm): a basin table is read by gameplay code that never touches
# the control-point datum, and making it share one would couple a water query
# to the elevation codec's internals.
_BASIN_ENTRY = struct.Struct("<9H2i B5s")
BASIN_ENTRY_BYTES = 32
_BASIN_RESERVED5 = bytes(5)

#: `kind` values, mirroring bake/basins.py's KIND_* and voxelcore/tilestore.h's
#: BasinKind. Three copies, one meaning; tests/test_basin_table.py asserts the
#: Python pair agree and the C++ one is checked by the shared fixture.
BASIN_KIND_DRY_PLAYA = 0
BASIN_KIND_SALT_FLAT = 1
BASIN_KIND_SEASONAL = 2
BASIN_KIND_LAKE_TERMINAL = 3
BASIN_KIND_LAKE_OVERFLOWING = 4
BASIN_KIND_COUNT = 5

# Flow byte layout (§6): bits 0-4 log2(flow accumulation m^2) clamped 0-31,
# bit 5 channel, bit 6 bank, bit 7 deposition.
FLOW_LOG2_MASK = 0x1F
FLOW_BIT_CHANNEL = 1 << 5
FLOW_BIT_BANK = 1 << 6
FLOW_BIT_DEPOSITION = 1 << 7

try:
    import zstandard  # type: ignore
except ImportError:  # pragma: no cover - deliberately not installed in CI (§3)
    zstandard = None

#: True when the optional zstd dependency is importable. CODEC_RAW never
#: depends on this. CODEC_ZSTD raises a clear RuntimeError (not an
#: ImportError, and not at module import time) when this is False, so
#: callers/tests can skip cleanly instead of the whole module failing to
#: import — see _compress/_decompress.
HAVE_ZSTD = zstandard is not None

# v2 header extension, immediately following the 25 v1-identical bytes that
# `_HEADER` (defined above, for v1) already covers (§3):
#   block_log2 u8, predictor u8, quant u8, codec u8, bake_ver u16, flags u16,
#   base_offset_mm i32, parent_scale u8, reserved u8[3], n_sections u16
_V2_EXT = struct.Struct("<BBBBHHiB3sH")

# Section table entry (§3): id u32, offset u64 (from file start), length u64.
_SECTION_ENTRY = struct.Struct("<IQQ")

# Block index entry (§4): offset u64 (into ELEV_DATA/FLOW_DATA), comp_len u32,
# mode u8, const_cp i16, resid_bits u8, pad u8[4].
_BLOCK_ENTRY = struct.Struct("<QIBhB4s")

_ZERO_PAD4 = b"\x00\x00\x00\x00"
_ZERO_RESERVED3 = b"\x00\x00\x00"


@dataclass
class TileV2:
    """A decoded/to-be-encoded v2 fine tile.

    `elevation_cp` holds ABSOLUTE prefiltered B-spline control points (§2):
    int16, LSB = `quant` mm, relative to `base_offset_mm`. NOT samples, and
    NOT residuals against the coarse s1 tile — see §2 for why.
    `flow`, if given, is one uint8 per fine pixel (§6) and sets flags bit0.
    `basins`, if given, is the P1 registry and sets flags bit1. An EMPTY list
    is not the same as None: a tile with no basins still says so (the flag is
    set and the table has count 0), because "this tile has been surveyed and
    holds nothing" and "this tile predates the registry" must not read alike.
    """

    seed: int
    x: int
    y: int
    size: int
    elevation_cp: np.ndarray          # (size, size) int16
    base_offset_mm: int = 0
    quant: int = QUANT_100MM
    codec: int = CODEC_RAW
    bake_ver: int = 0
    block_log2: int = DEFAULT_BLOCK_LOG2
    flow: np.ndarray | None = None    # (size, size) uint8, optional
    scale: int = FINE_SCALE
    predictor: int = PRED_MED
    parent_scale: int = 0             # reserved (§3) — must stay 0
    basins: "list[BasinEntry] | None" = None   # P1 registry, optional

    def __post_init__(self) -> None:
        assert self.scale == FINE_SCALE, f"v2 scale must be {FINE_SCALE}"
        assert self.parent_scale == 0, "parent_scale is reserved, must be 0"
        assert 0 < self.size <= 0xFFFF
        bs = 1 << self.block_log2
        assert self.size % bs == 0, (
            f"size {self.size} not a multiple of the block edge {bs} "
            f"(block_log2={self.block_log2})"
        )
        assert self.elevation_cp.shape == (self.size, self.size)
        assert self.elevation_cp.dtype == np.int16
        assert self.quant in QUANT_MM, f"unsupported quant {self.quant}"
        assert self.codec in (CODEC_RAW, CODEC_ZSTD), f"unsupported codec {self.codec}"
        if self.flow is not None:
            assert self.flow.shape == (self.size, self.size)
            assert self.flow.dtype == np.uint8
        if self.basins is not None:
            for i, b in enumerate(self.basins):
                assert b.basin_id == i, "basin ids must be 0..n-1 in order"
                assert 0 <= b.bbox_px[0] <= b.bbox_px[2] < self.size
                assert 0 <= b.bbox_px[1] <= b.bbox_px[3] < self.size


def mm_to_control_points(elev_mm: np.ndarray, base_offset_mm: int, quant: int) -> np.ndarray:
    """Invert elevation_mm(i,j) = base_offset_mm + int32(cp)*quant_mm (§2).
    Raises ValueError if a value can't round-trip in the int16 cp range."""
    q = QUANT_MM[quant]
    cp = np.round((elev_mm.astype(np.int64) - base_offset_mm) / q).astype(np.int64)
    if cp.min() < -32768 or cp.max() > 32767:
        raise ValueError(
            "elevation out of int16 control-point range for this base_offset_mm/quant"
        )
    return cp.astype(np.int16)


def control_points_to_mm(cp: np.ndarray, base_offset_mm: int, quant: int) -> np.ndarray:
    """elevation_mm(i,j) = base_offset_mm + int32(cp)*quant_mm (§2)."""
    q = QUANT_MM[quant]
    return base_offset_mm + cp.astype(np.int64) * q


# --------------------------------------------------------------- predictor (§5)

def _med_residual(cp: np.ndarray) -> np.ndarray:
    """Vectorised MED/LOCO-I residual, per §5. `cp` is (H, W) int64.

    Every predictor input (W, N, NW) is an already-known neighbour, so
    vectorising the forward pass changes nothing about the values it reads —
    it must still agree pixel-for-pixel with _med_reconstruct's sequential
    inverse, which the round-trip tests check directly."""
    w = np.zeros_like(cp)
    w[:, 1:] = cp[:, :-1]
    n = np.zeros_like(cp)
    n[1:, :] = cp[:-1, :]
    nw = np.zeros_like(cp)
    nw[1:, 1:] = cp[:-1, :-1]
    mx, mn = np.maximum(w, n), np.minimum(w, n)
    pred = np.where(nw >= mx, mn, np.where(nw <= mn, mx, w + n - nw))
    pred[0, 0] = 0
    pred[0, 1:] = cp[0, :-1]   # first row: pred = W
    pred[1:, 0] = cp[:-1, 0]   # first column: pred = N
    return cp - pred


def _med_reconstruct(resid: np.ndarray) -> np.ndarray:
    """Sequential MED inverse, per §5. Must run in raster order: the
    predictor for (x, y) reads the already-reconstructed W/N/NW neighbours,
    which is exactly the data dependency that makes blocks independent of
    each other (§4) but pixels within a block NOT independently decodable."""
    h, w = resid.shape
    cp = np.zeros((h, w), dtype=np.int64)
    for y in range(h):
        cp_row = cp[y]
        prev_row = cp[y - 1] if y > 0 else None
        for x in range(w):
            if x == 0 and y == 0:
                pred = 0
            elif y == 0:
                pred = cp_row[x - 1]
            elif x == 0:
                pred = prev_row[0]
            else:
                wv = cp_row[x - 1]
                nv = prev_row[x]
                nwv = prev_row[x - 1]
                mx, mn = max(wv, nv), min(wv, nv)
                if nwv >= mx:
                    pred = mn
                elif nwv <= mn:
                    pred = mx
                else:
                    pred = wv + nv - nwv
            cp_row[x] = resid[y, x] + pred
    return cp


def _zigzag(r: np.ndarray) -> np.ndarray:
    """(r << 1) ^ (r >> 31), per §5 — written as plain arithmetic instead of
    a literal shift so it doesn't depend on numpy's right-shift being
    arithmetic on a particular width. Equivalent for any r that fits in
    int32 (true here: a MED residual over int16 control points fits with
    enormous headroom, see the resid_bits comment below)."""
    r = r.astype(np.int64)
    return np.where(r >= 0, r * 2, (-r) * 2 - 1)


def _unzigzag(z: np.ndarray) -> np.ndarray:
    z = z.astype(np.int64)
    return np.where(z % 2 == 0, z // 2, -(z // 2) - 1)


def _pack_residuals(resid: np.ndarray, resid_bits: int) -> bytes:
    zz = _zigzag(resid.ravel())
    dtype = "<u2" if resid_bits == 16 else "<u4"
    return zz.astype(dtype).tobytes()


def _unpack_residuals(buf: bytes, n: int, resid_bits: int) -> np.ndarray:
    dtype = "<u2" if resid_bits == 16 else "<u4"
    zz = np.frombuffer(buf, dtype=dtype, count=n).astype(np.int64)
    return _unzigzag(zz)


# ------------------------------------------------------------------ codec (§3)

#: A CODEC_ZSTD frame compressor: plain block payload in, one frame out.
#: Blocks are independent (§4) — no dictionary, no shared context, no state
#: across calls — so this is called once per block and must be stateless.
Compressor = Callable[[bytes], bytes]

#: A CODEC_ZSTD frame decompressor: (frame, expected_plain_len) -> plain bytes.
#: `expected_plain_len` is derived from the HEADER (block pixels x element or
#: residual width), never from `comp_len`, and returning anything other than
#: exactly that many bytes is an error. This mirrors voxel-core's injected
#: FineDecompressor byte for byte (voxelcore/tilestore.h), deliberately: the
#: two halves are built independently against docs/vxtl-v2-format.md, so the
#: closer the two boundaries look, the fewer ways they have to disagree.
Decompressor = Callable[[bytes, int], bytes]


def _compress(payload: bytes, codec: int, compressor: Compressor | None = None) -> bytes:
    if codec == CODEC_RAW:
        # CODEC_RAW must never depend on a compression library, injected or
        # otherwise (§3) — an injected compressor is simply ignored here.
        return payload
    if codec == CODEC_ZSTD:
        if compressor is not None:
            return compressor(payload)
        if not HAVE_ZSTD:
            raise RuntimeError(
                "zstandard is not installed and no compressor was injected; "
                "encode with codec=CODEC_RAW instead"
            )
        return zstandard.ZstdCompressor().compress(payload)
    raise ValueError(f"unsupported codec {codec}")


def _decompress(
    payload: bytes,
    codec: int,
    expected_len: int,
    decompressor: Decompressor | None = None,
) -> bytes:
    """Expand one block's stored bytes to exactly `expected_len` bytes.

    The length check is the point, and it is the ONLY length check that
    survives compression: under CODEC_ZSTD `comp_len` is the compressed size
    and says nothing about the contents, so a truncated, padded or simply
    wrong frame is caught here or not at all.
    """
    if codec == CODEC_RAW:
        out = payload
    elif codec == CODEC_ZSTD:
        if not payload:
            # A block that owns zero data bytes is CONSTANT by definition;
            # an empty frame cannot expand to anything.
            raise ValueError("empty CODEC_ZSTD frame")
        if decompressor is not None:
            out = decompressor(payload, expected_len)
        elif HAVE_ZSTD:
            out = zstandard.ZstdDecompressor().decompress(
                payload, max_output_size=expected_len
            )
        else:
            raise RuntimeError(
                "zstandard is not installed and no decompressor was injected; "
                "cannot decode CODEC_ZSTD"
            )
    else:
        raise ValueError(f"unsupported codec {codec}")
    if len(out) != expected_len:
        raise ValueError(
            f"block payload expanded to {len(out)} bytes, header implies {expected_len}"
        )
    return out


# ------------------------------------------------------------- block plane (§4)

def _encode_plane(
    values: np.ndarray,
    *,
    block_log2: int,
    codec: int,
    elem_dtype: str,
    force_raw_blocks: set[tuple[int, int]] | None = None,
    compressor: Compressor | None = None,
) -> tuple[bytes, bytes]:
    """Encode one plane (elevation or flow) into (index_bytes, data_bytes).

    `values` is (size, size), already in control-point / flow-byte space,
    int64 for headroom. Blocks are independent (§4): each is predicted,
    zigzagged, and compressed on its own, with no state carried across a
    block boundary.

    `force_raw_blocks` is a set of (bx, by) block coordinates to force to
    MODE_RAW regardless of size.

    RAW IS AUTO-SELECTED FOR 1-BYTE PLANES ONLY, i.e. the §6 flow plane, and
    only when it is actually smaller. The argument for never auto-selecting it
    was written about the ELEVATION plane and is correct there: an i2 element
    ties CODED at resid_bits=16 and only beats it at resid_bits=32, so a
    size-minimising selector would make resid_bits=32 unreachable through the
    public encoder — exactly the code path the spec says must be tested (§5,
    §9 item 1). None of that transfers to a u1 element, where a MED residual is
    zigzagged into TWO bytes per pixel against RAW's one and resid_bits can
    never be 32 (a uint8 difference fits int16 with room to spare). Measured on
    the real baked flow plane of tile (-40,24): 2.10 -> 1.05 MB uncoded, and
    0.144 -> 0.108 MB after zstd-19, i.e. CODED was costing 33% of the flow
    plane's compressed size for nothing.

    The choice is made on the FINAL payload lengths, after `codec`, so it stays
    correct for CODEC_ZSTD as well as CODEC_RAW; ties go to CODED so a plane
    that gains nothing keeps the predicted form.
    """
    size = values.shape[0]
    bs = 1 << block_log2
    assert values.shape == (size, size)
    assert size % bs == 0
    nb = size // bs
    force_raw_blocks = force_raw_blocks or set()
    # See the docstring: only a 1-byte element can be smaller RAW than CODED
    # without taking resid_bits=32 out of the encoder's reach.
    auto_raw = np.dtype(elem_dtype).itemsize == 1

    entries = []
    chunks = []
    data_offset = 0
    for by in range(nb):
        for bx in range(nb):
            blk = values[by * bs:(by + 1) * bs, bx * bs:(bx + 1) * bs]
            const_val = blk[0, 0]
            if np.all(blk == const_val):
                # CONSTANT blocks cost zero bytes (§4) — no data-section entry.
                entries.append((0, 0, MODE_CONSTANT, int(const_val), 0, _ZERO_PAD4))
                continue
            if (bx, by) in force_raw_blocks:
                payload = _compress(blk.astype(elem_dtype).tobytes(), codec, compressor)
                entries.append((data_offset, len(payload), MODE_RAW, 0, 0, _ZERO_PAD4))
            else:
                resid = _med_residual(blk)
                # resid_bits is REQUIRED, not an optimisation (§5): a single
                # steep step forces this, not just pathological input.
                resid_bits = 16 if (resid.min() >= -32768 and resid.max() <= 32767) else 32
                # Both branches take the injected compressor: the auto-RAW choice
                # is decided on COMPRESSED lengths, so comparing a compressed
                # residual against an uncompressed literal would pick RAW almost
                # never, and the flow plane's 26% saving would silently vanish.
                payload = _compress(_pack_residuals(resid, resid_bits), codec, compressor)
                raw_payload = (
                    _compress(blk.astype(elem_dtype).tobytes(), codec, compressor)
                    if auto_raw else None
                )
                if raw_payload is not None and len(raw_payload) < len(payload):
                    payload = raw_payload
                    entries.append(
                        (data_offset, len(payload), MODE_RAW, 0, 0, _ZERO_PAD4)
                    )
                else:
                    entries.append(
                        (data_offset, len(payload), MODE_CODED, 0, resid_bits, _ZERO_PAD4)
                    )
            chunks.append(payload)
            data_offset += len(payload)

    index_bytes = b"".join(_BLOCK_ENTRY.pack(*e) for e in entries)
    return index_bytes, b"".join(chunks)


def _decode_plane(
    index_bytes: bytes,
    data_bytes: bytes,
    *,
    size: int,
    block_log2: int,
    codec: int,
    elem_dtype: str,
    out_dtype,
    decompressor: Decompressor | None = None,
) -> np.ndarray:
    bs = 1 << block_log2
    if size % bs != 0:
        raise ValueError("size not a multiple of the block edge")
    nb = size // bs
    n_blocks = nb * nb
    if len(index_bytes) != n_blocks * _BLOCK_ENTRY.size:
        raise ValueError("block index has the wrong length")

    out = np.zeros((size, size), dtype=out_dtype)
    elem_size = np.dtype(elem_dtype).itemsize
    for i in range(n_blocks):
        by, bx = divmod(i, nb)  # §4: row-major, x fastest
        offset, comp_len, mode, const_cp, resid_bits, pad = _BLOCK_ENTRY.unpack_from(
            index_bytes, i * _BLOCK_ENTRY.size
        )
        if pad != _ZERO_PAD4:
            raise ValueError("nonzero reserved pad bytes in block index")
        y0, x0 = by * bs, bx * bs

        if mode == MODE_CONSTANT:
            if comp_len != 0:
                raise ValueError("CONSTANT block must have comp_len 0")
            # const_cp is a signed i16 on the wire (§4) regardless of plane;
            # for the flow plane (uint8 elements) a corrupted/out-of-range
            # value must be rejected here rather than silently wrapping on
            # assignment into the uint8 output array below.
            lo, hi = np.iinfo(out_dtype).min, np.iinfo(out_dtype).max
            if const_cp < lo or const_cp > hi:
                raise ValueError(
                    f"const_cp {const_cp} out of range [{lo}, {hi}] for this plane"
                )
            out[y0:y0 + bs, x0:x0 + bs] = const_cp
            continue

        if offset + comp_len > len(data_bytes):
            raise ValueError("block payload extends past end of data section")

        # The expected PLAIN length comes from the header (mode, resid_bits,
        # element width) and never from comp_len — under CODEC_ZSTD comp_len
        # is the compressed size, so it constrains nothing. _decompress
        # enforces the expansion is exactly this many bytes.
        n = bs * bs
        if mode == MODE_CODED:
            if resid_bits not in (16, 32):
                raise ValueError(f"bad resid_bits {resid_bits}")
            expected = n * (2 if resid_bits == 16 else 4)
        elif mode == MODE_RAW:
            expected = n * elem_size
        else:
            raise ValueError(f"bad block mode {mode}")

        raw = _decompress(
            data_bytes[offset:offset + comp_len], codec, expected, decompressor
        )

        if mode == MODE_CODED:
            resid = _unpack_residuals(raw, n, resid_bits).reshape(bs, bs)
            out[y0:y0 + bs, x0:x0 + bs] = _med_reconstruct(resid).astype(out_dtype)
        else:
            # MODE_RAW. Note this branch is chosen by `mode` and by nothing
            # else: at resid_bits=16 a RAW int16 plane and a CODED residual
            # plane have IDENTICAL lengths (§4), so `expected` above matching
            # is not evidence the payload was read as the right thing.
            out[y0:y0 + bs, x0:x0 + bs] = np.frombuffer(
                raw, dtype=elem_dtype, count=n
            ).reshape(bs, bs)
    return out


# ---------------------------------------------------------------- top level

@dataclass
class BasinEntry:
    """One row of SECTION_BASIN_TABLE (watershed plan P1).

    The wire form of `bake.basins.BasinRecord`, reduced to what a client needs
    to put water in a hole: where to start the flood fill, how far it can
    possibly reach, how high the water stands, and what kind of place it is.
    Everything the bake used to DECIDE that -- hypsometry, climate, catchment
    -- stays in the survey and never ships.
    """

    basin_id: int
    #: Deepest cell, the client's flood-fill seed. Tile-interior pixels.
    seed_px: tuple[int, int]
    #: (x0, y0, x1, y1) inclusive, tile-interior pixels. The flood fill is
    #: bounded by this, which is what makes the client cost per basin O(bbox)
    #: rather than O(tile).
    bbox_px: tuple[int, int, int, int]
    #: Spill cell -- the head of the outlet channel. Meaningful for
    #: KIND_LAKE_OVERFLOWING; carried for every row because it is also the
    #: diagnostic that says which way a dry basin WOULD drain.
    outlet_px: tuple[int, int]
    #: Fill level of the basin on the FINAL surface, absolute mm.
    spill_mm: int
    #: Equilibrium water surface, absolute mm; == spill_mm when overflowing,
    #: == the floor when dry.
    surface_mm: int
    kind: int

    def pack(self) -> bytes:
        for name, v, lo, hi in (
            ("basin_id", self.basin_id, 0, 0xFFFF),
            ("seed x", self.seed_px[0], 0, 0xFFFF),
            ("seed y", self.seed_px[1], 0, 0xFFFF),
            ("bbox x0", self.bbox_px[0], 0, 0xFFFF),
            ("bbox y0", self.bbox_px[1], 0, 0xFFFF),
            ("bbox x1", self.bbox_px[2], 0, 0xFFFF),
            ("bbox y1", self.bbox_px[3], 0, 0xFFFF),
            ("outlet x", self.outlet_px[0], 0, 0xFFFF),
            ("outlet y", self.outlet_px[1], 0, 0xFFFF),
            ("kind", self.kind, 0, BASIN_KIND_COUNT - 1),
        ):
            if not (lo <= int(v) <= hi):
                raise ValueError(
                    f"basin {self.basin_id}: {name} = {v} is outside [{lo}, {hi}]"
                )
        if self.bbox_px[0] > self.bbox_px[2] or self.bbox_px[1] > self.bbox_px[3]:
            raise ValueError(f"basin {self.basin_id}: bbox is inside out")
        if self.surface_mm > self.spill_mm:
            # A water surface above its own outlet is not a lake, it is a bug:
            # the outlet would carry the excess away. classify() clamps, so
            # reaching here means something bypassed it.
            raise ValueError(
                f"basin {self.basin_id}: surface {self.surface_mm} mm is above "
                f"spill {self.spill_mm} mm"
            )
        return _BASIN_ENTRY.pack(
            self.basin_id, self.seed_px[0], self.seed_px[1],
            self.bbox_px[0], self.bbox_px[1], self.bbox_px[2], self.bbox_px[3],
            self.outlet_px[0], self.outlet_px[1],
            self.spill_mm, self.surface_mm, self.kind, _BASIN_RESERVED5,
        )

    @classmethod
    def from_record(cls, rec) -> "BasinEntry":
        """Wire row from a `bake.basins.BasinRecord`.

        Duck-typed rather than imported: this module must stay importable on a
        box with no numba, and the bake package is only ever reached lazily
        from here (see `elevation_control_points`).

        Metres become millimetres by rounding, which is monotone, so a record
        whose surface is at or below its spill cannot round into one that is
        above it -- the invariant `pack` enforces.
        """
        return cls(
            basin_id=int(rec.basin_id),
            seed_px=(int(rec.seed_px[0]), int(rec.seed_px[1])),
            bbox_px=tuple(int(v) for v in rec.bbox_px),  # type: ignore[arg-type]
            outlet_px=(int(rec.outlet_px[0]), int(rec.outlet_px[1])),
            spill_mm=int(round(float(rec.spill_m) * 1000.0)),
            surface_mm=int(round(float(rec.surface_m) * 1000.0)),
            kind=int(rec.kind),
        )

    @classmethod
    def unpack(cls, buf: bytes, offset: int = 0) -> "BasinEntry":
        (bid, sx, sy, x0, y0, x1, y1, ox, oy,
         spill, surface, kind, reserved) = _BASIN_ENTRY.unpack_from(buf, offset)
        if reserved != _BASIN_RESERVED5:
            raise ValueError("nonzero reserved bytes in basin entry")
        if kind >= BASIN_KIND_COUNT:
            raise ValueError(f"unknown basin kind {kind}")
        if x0 > x1 or y0 > y1:
            raise ValueError("basin bbox is inside out")
        if surface > spill:
            raise ValueError("basin surface is above its spill")
        return cls(basin_id=bid, seed_px=(sx, sy), bbox_px=(x0, y0, x1, y1),
                   outlet_px=(ox, oy), spill_mm=spill, surface_mm=surface,
                   kind=kind)


def encode_basin_table(basins: "list[BasinEntry]") -> bytes:
    """Serialise SECTION_BASIN_TABLE's payload.

    Ids must be 0..n-1 in order. That is not decoration: the client indexes
    the table by id, and the bake orders basins by (min_y, min_x) of extent so
    the id is a deterministic function of the surface. A gap or a repeat means
    two different tiles could disagree about which basin is "3".
    """
    for i, b in enumerate(basins):
        if b.basin_id != i:
            raise ValueError(
                f"basin ids must be 0..n-1 in order; entry {i} says {b.basin_id}"
            )
    if len(basins) > 0xFFFF:
        raise ValueError(f"{len(basins)} basins exceeds the u16 id space")
    head = _BASIN_TABLE_HEADER.pack(BASIN_TABLE_VERSION, BASIN_ENTRY_BYTES,
                                    len(basins))
    return head + b"".join(b.pack() for b in basins)


def decode_basin_table(payload: bytes) -> "list[BasinEntry]":
    """Parse SECTION_BASIN_TABLE, refusing anything it cannot read exactly."""
    if len(payload) < _BASIN_TABLE_HEADER.size:
        raise ValueError("truncated basin table header")
    version, entry_bytes, count = _BASIN_TABLE_HEADER.unpack_from(payload, 0)
    if version != BASIN_TABLE_VERSION:
        raise ValueError(f"unsupported basin table version {version}")
    if entry_bytes != BASIN_ENTRY_BYTES:
        raise ValueError(
            f"basin entry size {entry_bytes} != {BASIN_ENTRY_BYTES}; these "
            "bytes were written by a different revision of the table"
        )
    want = _BASIN_TABLE_HEADER.size + count * entry_bytes
    if len(payload) != want:
        raise ValueError(
            f"basin table is {len(payload)} bytes, header says {want}"
        )
    out = [
        BasinEntry.unpack(payload, _BASIN_TABLE_HEADER.size + i * entry_bytes)
        for i in range(count)
    ]
    for i, b in enumerate(out):
        if b.basin_id != i:
            raise ValueError(f"basin ids are not 0..n-1 (entry {i} says {b.basin_id})")
    return out


def encode_v2(
    tile: TileV2,
    *,
    raw_blocks: set[tuple[int, int]] | None = None,
    flow_raw_blocks: set[tuple[int, int]] | None = None,
    compressor: Compressor | None = None,
) -> bytes:
    """Encode a v2 fine tile (docs/vxtl-v2-format.md §3-§6).

    `raw_blocks` / `flow_raw_blocks` force the named (bx, by) elevation /
    flow blocks to MODE_RAW — see _encode_plane's docstring for why this is
    an explicit opt-in rather than a size-minimising heuristic. They are
    independent: forcing an elevation block RAW says nothing about the flow
    block at the same coordinate, and vice versa.

    `compressor` replaces the default zstandard frame compressor for
    CODEC_ZSTD. It exists for the same reason the C++ decoder takes an
    injected decompressor (§3, and voxelcore/tilestore.h): compression lives
    at the boundary, not in the codec. §7 also licenses it — "the ENCODER
    need not be deterministic", any conformant zstd frame will do — which is
    what lets the committed conformance fixture be built without the optional
    zstandard dependency, since CI deliberately does not install it.
    """
    flags = FLAG_FLOW_PRESENT if tile.flow is not None else 0
    if tile.basins is not None:
        flags |= FLAG_BASINS_PRESENT

    elev_index, elev_data = _encode_plane(
        tile.elevation_cp.astype(np.int64),
        block_log2=tile.block_log2,
        codec=tile.codec,
        elem_dtype="<i2",
        force_raw_blocks=raw_blocks,
        compressor=compressor,
    )
    sections = [(SECTION_ELEV_INDEX, elev_index), (SECTION_ELEV_DATA, elev_data)]

    if tile.flow is not None:
        flow_index, flow_data = _encode_plane(
            tile.flow.astype(np.int64),
            block_log2=tile.block_log2,
            codec=tile.codec,
            elem_dtype="u1",
            force_raw_blocks=flow_raw_blocks,
            compressor=compressor,
        )
        sections += [(SECTION_FLOW_INDEX, flow_index), (SECTION_FLOW_DATA, flow_data)]

    if tile.basins is not None:
        # LAST, and uncompressed whatever `codec` says. It is a kilobyte of
        # already-dense integers -- compressing it would buy nothing and would
        # make the one section a client must read before it can put water
        # anywhere depend on the injected decompressor.
        sections.append((SECTION_BASIN_TABLE, encode_basin_table(tile.basins)))

    header = _HEADER.pack(MAGIC, VERSION_V2, tile.seed, tile.x, tile.y, tile.scale, tile.size)
    ext = _V2_EXT.pack(
        tile.block_log2,
        tile.predictor,
        tile.quant,
        tile.codec,
        tile.bake_ver,
        flags,
        tile.base_offset_mm,
        tile.parent_scale,
        _ZERO_RESERVED3,
        len(sections),
    )
    table_start = len(header) + len(ext)
    table_len = len(sections) * _SECTION_ENTRY.size
    data_start = table_start + table_len

    table = bytearray()
    offset = data_start
    for sid, sbytes in sections:
        table += _SECTION_ENTRY.pack(sid, offset, len(sbytes))
        offset += len(sbytes)

    return header + ext + bytes(table) + b"".join(s for _, s in sections)


def decode_v2(data: bytes, *, decompressor: Decompressor | None = None) -> TileV2:
    """Decode a v2 fine tile. Raises ValueError on any truncation, magic/
    version/field mismatch, or internal inconsistency — never lets a raw
    struct.error or an out-of-bounds slice through silently (§9 item 4).

    `decompressor` is the injected CODEC_ZSTD reader; None falls back to
    zstandard when it is importable and raises a clear RuntimeError when it
    is not. CODEC_RAW never consults it."""
    if len(data) < _HEADER.size:
        raise ValueError("truncated header")
    magic, version, seed, x, y, scale, size = _HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError("bad magic")
    if version != VERSION_V2:
        raise ValueError(f"unsupported tile version {version}")
    if scale != FINE_SCALE:
        raise ValueError(f"unsupported v2 scale {scale}")

    off = _HEADER.size
    if len(data) < off + _V2_EXT.size:
        raise ValueError("truncated v2 header extension")
    (
        block_log2, predictor, quant, codec, bake_ver, flags,
        base_offset_mm, parent_scale, reserved, n_sections,
    ) = _V2_EXT.unpack_from(data, off)
    off += _V2_EXT.size

    if reserved != _ZERO_RESERVED3:
        raise ValueError("nonzero reserved header bytes")
    if predictor != PRED_MED:
        raise ValueError(f"unsupported predictor {predictor}")
    if quant not in QUANT_MM:
        raise ValueError(f"unsupported quant {quant}")
    if codec not in (CODEC_RAW, CODEC_ZSTD):
        raise ValueError(f"unsupported codec {codec}")
    if parent_scale != 0:
        # Reserved for a future residual ladder (§1, §3); this decoder only
        # implements the absolute reading this spec defines.
        raise ValueError(
            "parent_scale != 0 (residual ladder) is not supported by this decoder"
        )
    bs = 1 << block_log2
    if size == 0 or size % bs != 0:
        raise ValueError(f"size {size} not a multiple of the block edge {bs}")

    table_len = n_sections * _SECTION_ENTRY.size
    if len(data) < off + table_len:
        raise ValueError("truncated section table")
    section_table = [
        _SECTION_ENTRY.unpack_from(data, off + i * _SECTION_ENTRY.size)
        for i in range(n_sections)
    ]
    off += table_len

    sections: dict[int, bytes] = {}
    max_end = off
    for sid, soff, slen in section_table:
        end = soff + slen
        if end > len(data):
            raise ValueError("section extends past end of file")
        sections[sid] = data[soff:end]
        max_end = max(max_end, end)
    if max_end != len(data):
        raise ValueError("trailing bytes")

    for required in (SECTION_ELEV_INDEX, SECTION_ELEV_DATA):
        if required not in sections:
            raise ValueError(f"missing required section {required}")

    elevation_cp = _decode_plane(
        sections[SECTION_ELEV_INDEX], sections[SECTION_ELEV_DATA],
        size=size, block_log2=block_log2, codec=codec,
        elem_dtype="<i2", out_dtype=np.int16, decompressor=decompressor,
    )

    if flags & ~(FLAG_FLOW_PRESENT | FLAG_BASINS_PRESENT):
        # Same posture as the C++ parser: an undefined flag bit can only mean
        # the bytes came from something this decoder does not understand, and
        # reading them anyway is how a client silently mis-renders a world.
        raise ValueError(f"unknown header flag bits set: 0x{flags:04x}")

    basins = None
    if flags & FLAG_BASINS_PRESENT:
        if SECTION_BASIN_TABLE not in sections:
            raise ValueError("basin flag set but SECTION_BASIN_TABLE is missing")
        basins = decode_basin_table(sections[SECTION_BASIN_TABLE])
        for b in basins:
            if not (0 <= b.bbox_px[0] <= b.bbox_px[2] < size
                    and 0 <= b.bbox_px[1] <= b.bbox_px[3] < size):
                raise ValueError(f"basin {b.basin_id} bbox is outside the tile")
    elif SECTION_BASIN_TABLE in sections:
        raise ValueError("SECTION_BASIN_TABLE present but the basin flag is clear")

    flow = None
    if flags & FLAG_FLOW_PRESENT:
        if SECTION_FLOW_INDEX not in sections or SECTION_FLOW_DATA not in sections:
            raise ValueError("flow flag set but flow sections are missing")
        flow = _decode_plane(
            sections[SECTION_FLOW_INDEX], sections[SECTION_FLOW_DATA],
            size=size, block_log2=block_log2, codec=codec,
            elem_dtype="u1", out_dtype=np.uint8, decompressor=decompressor,
        )

    return TileV2(
        seed=seed, x=x, y=y, size=size,
        elevation_cp=elevation_cp,
        base_offset_mm=base_offset_mm,
        quant=quant, codec=codec, bake_ver=bake_ver,
        block_log2=block_log2, flow=flow, basins=basins,
        scale=scale, predictor=predictor, parent_scale=parent_scale,
    )


# --------------------------------------------------- bake surface -> v2 bytes
#
# `encode_v2` above takes a `TileV2`, i.e. it starts from control points that
# someone else already produced. The bake does NOT produce control points: a
# `BakeResult.elevation_m` is a field of SAMPLES (bake/pipeline.py's own
# docstring on that field says so, and points here for the prefilter). Between
# the two sits the step this function is:
#
#     samples (float m)  ->  prefilter  ->  cp (float m)  ->  datum + quantise
#                        ->  int16 cp   ->  encode_v2
#
# Without it there is no path at all from a bake to shippable bytes --
# `pregen._encode_fine` probes for exactly this name first, finds `encode_v2`
# instead, and dies because it cannot synthesise a `TileV2` argument. That is
# why no fine tile existed end-to-end before 2026-07-29.
#
# Skipping the prefilter and writing the samples straight into the cp plane is
# the one tempting shortcut and it is wrong in a way no round-trip test would
# catch: the file still decodes, but the client's spline then evaluates a
# LOW-PASSED version of the bake (docs/vxtl-v2-format.md §2: detrended H
# degrades 0.83 -> 1.47), quietly throwing away the fine band the whole bake
# exists to create.

#: Elevation datum granularity, mm. `base_offset_mm` is snapped to a multiple
#: of this so that a tile's datum is a legible round number in a hex dump and
#: two neighbouring tiles of similar relief usually share one, which costs
#: nothing (the int16 range is 3.2 km either way at 100 mm) and makes a
#: mis-set datum obvious rather than plausible.
DATUM_STEP_MM = 100_000  # 100 m


def choose_datum(cp_mm_min: int, cp_mm_max: int) -> tuple[int, int]:
    """Pick `(base_offset_mm, quant)` covering [cp_mm_min, cp_mm_max].

    Prefers `QUANT_100MM` (one voxel per LSB, the format's intent) and falls
    back to `QUANT_250MM` only when the tile's control-point range genuinely
    exceeds the int16 span at 100 mm -- 6553.4 m, which real terrain reaches
    only where an alpine tile also contains deep bathymetry. Raises ValueError
    if even 250 mm cannot hold it, rather than silently clipping mountains.
    """
    mid = (int(cp_mm_min) + int(cp_mm_max)) // 2
    base = int(round(mid / DATUM_STEP_MM)) * DATUM_STEP_MM
    for quant in (QUANT_100MM, QUANT_250MM):
        q = QUANT_MM[quant]
        lo = (int(cp_mm_min) - base) / q
        hi = (int(cp_mm_max) - base) / q
        if lo >= -32768 and hi <= 32767:
            return base, quant
    raise ValueError(
        f"control-point range [{cp_mm_min / 1000:.1f}, {cp_mm_max / 1000:.1f}] m "
        f"({(cp_mm_max - cp_mm_min) / 1000:.1f} m of relief) does not fit int16 at "
        "either quant; the format cannot represent this tile"
    )


def elevation_control_points(
    elevation_m: np.ndarray,
    *,
    base_offset_mm: int | None = None,
    quant: int | None = None,
) -> tuple[np.ndarray, int, int]:
    """Sample field in metres -> `(int16 cp, base_offset_mm, quant)`.

    Runs the §2 prefilter (`bake.noise.prefilter`, the same operator the B0
    carrier uses, imported lazily because it needs scipy and this module must
    import on a bare CI box), then picks a datum and quantises.

    `base_offset_mm` / `quant` may be pinned by the caller -- a seam test wants
    two tiles on ONE datum so their cp planes are directly comparable -- and are
    otherwise chosen per tile by `choose_datum`.
    """
    from .bake.noise import prefilter  # scipy; lazy on purpose

    z = np.asarray(elevation_m)
    if z.ndim != 2 or z.shape[0] != z.shape[1]:
        raise ValueError(f"elevation_m must be a square 2-D array, got {z.shape}")

    cp_mm = np.rint(prefilter(z) * 1000.0)
    lo, hi = int(cp_mm.min()), int(cp_mm.max())
    if base_offset_mm is None or quant is None:
        auto_base, auto_quant = choose_datum(lo, hi)
        base_offset_mm = auto_base if base_offset_mm is None else int(base_offset_mm)
        quant = auto_quant if quant is None else int(quant)
    cp = mm_to_control_points(cp_mm.astype(np.int64), int(base_offset_mm), int(quant))
    return cp, int(base_offset_mm), int(quant)


def encode_fine(
    *,
    seed: int,
    x: int,
    y: int,
    elevation_m: np.ndarray,
    flow: np.ndarray | None = None,
    basins: "list | tuple | None" = None,
    bake_ver: int | None = None,
    codec: int = CODEC_RAW,
    block_log2: int = DEFAULT_BLOCK_LOG2,
    base_offset_mm: int | None = None,
    quant: int | None = None,
) -> bytes:
    """Encode one baked fine tier straight from the bake's SAMPLE field.

    This is the entry point `pregen._encode_fine` looks for, and the only
    sanctioned way to turn a `BakeResult` into `.vxtl` v2 bytes.

    `bake_ver=None` stamps `bake.pipeline.BAKE_VERSION`, so a tile carries the
    identity of the bake that made it without every caller having to remember
    to pass it; pass an int to override (a fixture generator wants a fixed one).
    """
    if bake_ver is None:
        try:
            from .bake.pipeline import BAKE_VERSION

            bake_ver = int(BAKE_VERSION)
        except ImportError:  # pragma: no cover - bake package always ships
            bake_ver = 0

    cp, base_offset_mm, quant = elevation_control_points(
        elevation_m, base_offset_mm=base_offset_mm, quant=quant
    )
    flow_arr = None
    if flow is not None:
        flow_arr = np.ascontiguousarray(flow, dtype=np.uint8)
        if flow_arr.shape != cp.shape:
            raise ValueError(
                f"flow plane {flow_arr.shape} does not match elevation {cp.shape}"
            )
    basin_rows = None
    if basins is not None:
        # A tuple() of zero basins is NOT None: a tile that was surveyed and
        # holds nothing must say so, or a client cannot tell it apart from a
        # tile baked before the registry existed.
        basin_rows = [
            b if isinstance(b, BasinEntry) else BasinEntry.from_record(b)
            for b in basins
        ]
    return encode_v2(
        TileV2(
            seed=int(seed) & 0xFFFFFFFFFFFFFFFF,
            x=int(x),
            y=int(y),
            size=int(cp.shape[0]),
            elevation_cp=cp,
            base_offset_mm=int(base_offset_mm),
            quant=int(quant),
            codec=int(codec),
            bake_ver=int(bake_ver),
            block_log2=int(block_log2),
            flow=flow_arr,
            basins=basin_rows,
        )
    )
