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

FLAG_FLOW_PRESENT = 1 << 0

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

def _compress(payload: bytes, codec: int) -> bytes:
    if codec == CODEC_RAW:
        return payload
    if codec == CODEC_ZSTD:
        if not HAVE_ZSTD:
            raise RuntimeError(
                "zstandard is not installed; encode with codec=CODEC_RAW instead"
            )
        return zstandard.ZstdCompressor().compress(payload)
    raise ValueError(f"unsupported codec {codec}")


def _decompress(payload: bytes, codec: int) -> bytes:
    if codec == CODEC_RAW:
        return payload
    if codec == CODEC_ZSTD:
        if not HAVE_ZSTD:
            raise RuntimeError("zstandard is not installed; cannot decode CODEC_ZSTD")
        return zstandard.ZstdDecompressor().decompress(payload)
    raise ValueError(f"unsupported codec {codec}")


# ------------------------------------------------------------- block plane (§4)

def _encode_plane(
    values: np.ndarray,
    *,
    block_log2: int,
    codec: int,
    elem_dtype: str,
    force_raw_blocks: set[tuple[int, int]] | None = None,
) -> tuple[bytes, bytes]:
    """Encode one plane (elevation or flow) into (index_bytes, data_bytes).

    `values` is (size, size), already in control-point / flow-byte space,
    int64 for headroom. Blocks are independent (§4): each is predicted,
    zigzagged, and compressed on its own, with no state carried across a
    block boundary.

    `force_raw_blocks` is a set of (bx, by) block coordinates to force to
    MODE_RAW regardless of size. This reference encoder does NOT pick RAW on
    its own: under CODEC_RAW, RAW is only ever byte-cheaper than CODED when
    CODED needed resid_bits=32 (both cost 2 bytes/px at resid_bits=16, a
    tie broken to CODED), so a size-minimising auto-selector would make
    resid_bits=32 unreachable through the public encoder — exactly the code
    path the spec says must be tested (§5, §9 item 1). RAW is kept in the
    format and reachable via this explicit opt-in instead.
    """
    size = values.shape[0]
    bs = 1 << block_log2
    assert values.shape == (size, size)
    assert size % bs == 0
    nb = size // bs
    force_raw_blocks = force_raw_blocks or set()

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
                payload = _compress(blk.astype(elem_dtype).tobytes(), codec)
                entries.append((data_offset, len(payload), MODE_RAW, 0, 0, _ZERO_PAD4))
            else:
                resid = _med_residual(blk)
                # resid_bits is REQUIRED, not an optimisation (§5): a single
                # steep step forces this, not just pathological input.
                resid_bits = 16 if (resid.min() >= -32768 and resid.max() <= 32767) else 32
                payload = _compress(_pack_residuals(resid, resid_bits), codec)
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
        raw = _decompress(data_bytes[offset:offset + comp_len], codec)

        if mode == MODE_CODED:
            if resid_bits not in (16, 32):
                raise ValueError(f"bad resid_bits {resid_bits}")
            n = bs * bs
            expected = n * (2 if resid_bits == 16 else 4)
            if len(raw) != expected:
                raise ValueError("residual payload has the wrong length")
            resid = _unpack_residuals(raw, n, resid_bits).reshape(bs, bs)
            out[y0:y0 + bs, x0:x0 + bs] = _med_reconstruct(resid).astype(out_dtype)
        elif mode == MODE_RAW:
            n = bs * bs
            if len(raw) != n * elem_size:
                raise ValueError("RAW payload has the wrong length")
            out[y0:y0 + bs, x0:x0 + bs] = np.frombuffer(
                raw, dtype=elem_dtype, count=n
            ).reshape(bs, bs)
        else:
            raise ValueError(f"bad block mode {mode}")
    return out


# ---------------------------------------------------------------- top level

def encode_v2(
    tile: TileV2,
    *,
    raw_blocks: set[tuple[int, int]] | None = None,
    flow_raw_blocks: set[tuple[int, int]] | None = None,
) -> bytes:
    """Encode a v2 fine tile (docs/vxtl-v2-format.md §3-§6).

    `raw_blocks` / `flow_raw_blocks` force the named (bx, by) elevation /
    flow blocks to MODE_RAW — see _encode_plane's docstring for why this is
    an explicit opt-in rather than a size-minimising heuristic. They are
    independent: forcing an elevation block RAW says nothing about the flow
    block at the same coordinate, and vice versa.
    """
    flags = FLAG_FLOW_PRESENT if tile.flow is not None else 0

    elev_index, elev_data = _encode_plane(
        tile.elevation_cp.astype(np.int64),
        block_log2=tile.block_log2,
        codec=tile.codec,
        elem_dtype="<i2",
        force_raw_blocks=raw_blocks,
    )
    sections = [(SECTION_ELEV_INDEX, elev_index), (SECTION_ELEV_DATA, elev_data)]

    if tile.flow is not None:
        flow_index, flow_data = _encode_plane(
            tile.flow.astype(np.int64),
            block_log2=tile.block_log2,
            codec=tile.codec,
            elem_dtype="u1",
            force_raw_blocks=flow_raw_blocks,
        )
        sections += [(SECTION_FLOW_INDEX, flow_index), (SECTION_FLOW_DATA, flow_data)]

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


def decode_v2(data: bytes) -> TileV2:
    """Decode a v2 fine tile. Raises ValueError on any truncation, magic/
    version/field mismatch, or internal inconsistency — never lets a raw
    struct.error or an out-of-bounds slice through silently (§9 item 4)."""
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
        elem_dtype="<i2", out_dtype=np.int16,
    )

    flow = None
    if flags & FLAG_FLOW_PRESENT:
        if SECTION_FLOW_INDEX not in sections or SECTION_FLOW_DATA not in sections:
            raise ValueError("flow flag set but flow sections are missing")
        flow = _decode_plane(
            sections[SECTION_FLOW_INDEX], sections[SECTION_FLOW_DATA],
            size=size, block_log2=block_log2, codec=codec,
            elem_dtype="u1", out_dtype=np.uint8,
        )

    return TileV2(
        seed=seed, x=x, y=y, size=size,
        elevation_cp=elevation_cp,
        base_offset_mm=base_offset_mm,
        quant=quant, codec=codec, bake_ver=bake_ver,
        block_log2=block_log2, flow=flow,
        scale=scale, predictor=predictor, parent_scale=parent_scale,
    )
